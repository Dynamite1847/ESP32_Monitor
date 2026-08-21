import AppKit
import Foundation

struct DeskAIProviderSnapshot: Encodable {
    let id: String
    let usageAvailable: Bool
    let secondaryAvailable: Bool
    let primary: UInt8
    let secondary: UInt8
    let primaryWindow: UInt16
    let secondaryWindow: UInt16
    let primaryReset: UInt32
    let secondaryReset: UInt32
    let tasks: UInt8
    let slots: UInt8
    let status: String
    let elapsed: UInt32
}

struct DeskAIStateSnapshot: Encodable {
    let providers: [DeskAIProviderSnapshot]
}

struct DeskAITaskSnapshot: Encodable {
    // 蓝牙任务消息使用短键名，把六个 UTF-8 标题稳定控制在 512 字节协议上限内。
    let n: String
    let s: String
}

struct DeskAITaskStateSnapshot: Encodable {
    let t: [DeskAITaskSnapshot]
}

// Claude 会话任务：额外带一个副文本 d（如“3 分钟前”），仅项目名/状态/时间，不含内容。
struct DeskAIClaudeTaskSnapshot: Encodable {
    let n: String
    let s: String
    let d: String
}

struct DeskAIClaudeTaskStateSnapshot: Encodable {
    let t: [DeskAIClaudeTaskSnapshot]
}

private struct DeskCodexThread {
    let id: String
    let name: String
    let status: String
}

private struct DeskClaudeSession {
    let sessionId: String
    let name: String
    let status: String
    let detail: String
    let modified: Date
}

final class DeskAIStatusMonitor {
    private let workQueue = DispatchQueue(
        label: "com.dongyu.desk-console-helper.ai-status",
        qos: .utility
    )
    private var appServer: Process?
    private var appServerInput: FileHandle?
    private var appServerOutput: FileHandle?
    private var outputBuffer = Data()
    private var rateLimitTimer: Timer?
    private var taskTimer: Timer?
    private var restartWorkItem: DispatchWorkItem?
    private var nextRequestID = 10
    private var appServerInitialized = false
    private var pendingRateLimitRequestIDs = Set<Int>()
    private var pendingThreadListRequestIDs = Set<Int>()

    private var codexUsageAvailable = false
    private var codexSecondaryAvailable = false
    private var codexPrimary: UInt8 = 0
    private var codexSecondary: UInt8 = 0
    private var codexPrimaryWindow: UInt16 = 0
    private var codexSecondaryWindow: UInt16 = 0
    private var codexPrimaryResetsAt: Date?
    private var codexSecondaryResetsAt: Date?
    private var codexTaskCount: UInt8 = 0
    private var codexThreads: [DeskCodexThread] = []
    private var claudeTaskCount: UInt8 = 0
    private var claudeSessions: [DeskClaudeSession] = []
    private var titleCache: [String: String] = [:]

    func start() {
        guard rateLimitTimer == nil else {
            return
        }
        startAppServer()
        refreshTaskCounts()
        rateLimitTimer = Timer.scheduledTimer(withTimeInterval: 60, repeats: true) { [weak self] _ in
            self?.requestRateLimits()
        }
        taskTimer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            self?.refreshTaskCounts()
            self?.requestThreadList()
        }
    }

    func snapshot() -> DeskAIStateSnapshot {
        DeskAIStateSnapshot(
            providers: [
                DeskAIProviderSnapshot(
                    id: "codex",
                    usageAvailable: codexUsageAvailable,
                    secondaryAvailable: codexSecondaryAvailable,
                    primary: codexPrimary,
                    secondary: codexSecondary,
                    primaryWindow: codexPrimaryWindow,
                    secondaryWindow: codexSecondaryWindow,
                    primaryReset: remainingSeconds(until: codexPrimaryResetsAt),
                    secondaryReset: remainingSeconds(until: codexSecondaryResetsAt),
                    tasks: codexTaskCount,
                    slots: UInt8(min(codexThreads.count, 6)),
                    status: codexTaskCount > 0 ? "running" : "idle",
                    elapsed: 0
                ),
                DeskAIProviderSnapshot(
                    id: "claude",
                    usageAvailable: false,
                    secondaryAvailable: false,
                    primary: 0,
                    secondary: 0,
                    primaryWindow: 0,
                    secondaryWindow: 0,
                    primaryReset: 0,
                    secondaryReset: 0,
                    tasks: UInt8(min(claudeSessions.count, 6)),
                    slots: UInt8(min(claudeSessions.count, 6)),
                    status: claudeTaskCount > 0 ? "running" : "idle",
                    elapsed: 0
                ),
            ]
        )
    }

    func taskSnapshot() -> DeskAITaskStateSnapshot {
        DeskAITaskStateSnapshot(
            t: codexThreads.prefix(6).map {
                DeskAITaskSnapshot(n: $0.name, s: $0.status)
            }
        )
    }

    func claudeTaskSnapshot() -> DeskAIClaudeTaskStateSnapshot {
        DeskAIClaudeTaskStateSnapshot(
            t: claudeSessions.prefix(4).map {
                DeskAIClaudeTaskSnapshot(n: $0.name, s: $0.status, d: $0.detail)
            }
        )
    }

    private func codexExecutableURL() -> URL? {
        let candidates = [
            "/Applications/ChatGPT.app/Contents/Resources/codex",
            "/opt/homebrew/bin/codex",
            "/usr/local/bin/codex",
        ]
        return candidates.first(where: { FileManager.default.isExecutableFile(atPath: $0) })
            .map { URL(fileURLWithPath: $0) }
    }

    private func startAppServer() {
        guard appServer == nil, let executable = codexExecutableURL() else {
            return
        }
        let process = Process()
        let input = Pipe()
        let output = Pipe()
        process.executableURL = executable
        // Desktop 内置 CLI 在被 macOS Process 管道启动时需要显式指定
        // stdio 传输；仅使用默认值时会一直等待且不返回初始化结果。
        process.arguments = ["app-server", "--stdio"]
        process.standardInput = input
        process.standardOutput = output
        process.standardError = FileHandle.nullDevice
        output.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard !data.isEmpty else {
                return
            }
            self?.workQueue.async { [weak self] in
                self?.consumeAppServerOutput(data)
            }
        }
        process.terminationHandler = { [weak self] _ in
            DispatchQueue.main.async {
                self?.appServerDidTerminate()
            }
        }
        do {
            try process.run()
            appServer = process
            appServerInitialized = false
            appServerInput = input.fileHandleForWriting
            appServerOutput = output.fileHandleForReading
            sendJSONLine([
                "method": "initialize",
                "id": 1,
                "params": [
                    "clientInfo": [
                        "name": "desk-console-helper",
                        "title": "Desk Console Helper",
                        "version": "0.1.0",
                    ],
                    "capabilities": [:] as [String: Any],
                ],
            ])
            NSLog("DeskConsoleHelper: Codex App Server started; waiting for initialization")
        } catch {
            NSLog("DeskConsoleHelper: Codex usage monitor unavailable: %@", error.localizedDescription)
            appServer = nil
            appServerInput = nil
            appServerOutput = nil
            scheduleRestart()
        }
    }

    private func appServerDidTerminate() {
        appServerOutput?.readabilityHandler = nil
        appServer = nil
        appServerInitialized = false
        appServerInput = nil
        appServerOutput = nil
        codexUsageAvailable = false
        codexSecondaryAvailable = false
        pendingRateLimitRequestIDs.removeAll()
        pendingThreadListRequestIDs.removeAll()
        scheduleRestart()
    }

    private func scheduleRestart() {
        restartWorkItem?.cancel()
        let workItem = DispatchWorkItem { [weak self] in
            self?.startAppServer()
        }
        restartWorkItem = workItem
        DispatchQueue.main.asyncAfter(deadline: .now() + 30, execute: workItem)
    }

    private func requestRateLimits() {
        guard appServer?.isRunning == true else {
            startAppServer()
            return
        }
        guard appServerInitialized else {
            return
        }
        let requestID = nextRequestID
        nextRequestID += 1
        pendingRateLimitRequestIDs.insert(requestID)
        sendJSONLine([
            "method": "account/rateLimits/read",
            "id": requestID,
            "params": [:] as [String: Any],
        ])
    }

    private func requestThreadList() {
        guard appServer?.isRunning == true, appServerInitialized else {
            return
        }
        let requestID = nextRequestID
        nextRequestID += 1
        pendingThreadListRequestIDs.insert(requestID)
        sendJSONLine([
            "method": "thread/list",
            "id": requestID,
            "params": [
                "limit": 6,
                "sortKey": "recency_at",
                "sortDirection": "desc",
                "archived": false,
                "sourceKinds": ["appServer", "cli", "vscode", "exec"],
            ] as [String: Any],
        ])
    }

    private func sendJSONLine(_ object: [String: Any]) {
        guard let input = appServerInput,
              var data = try? JSONSerialization.data(withJSONObject: object) else {
            return
        }
        data.append(0x0A)
        do {
            try input.write(contentsOf: data)
        } catch {
            NSLog("DeskConsoleHelper: Codex monitor write failed: %@", error.localizedDescription)
        }
    }

    private func consumeAppServerOutput(_ data: Data) {
        outputBuffer.append(data)
        while let newline = outputBuffer.firstIndex(of: 0x0A) {
            let line = outputBuffer[..<newline]
            outputBuffer.removeSubrange(...newline)
            guard !line.isEmpty,
                  let object = try? JSONSerialization.jsonObject(with: Data(line)) as? [String: Any],
                  let requestID = (object["id"] as? NSNumber)?.intValue else {
                continue
            }
            DispatchQueue.main.async { [weak self] in
                self?.handleAppServerResponse(object, requestID: requestID)
            }
        }
        if outputBuffer.count > 256 * 1024 {
            outputBuffer.removeAll(keepingCapacity: true)
        }
    }

    private func handleAppServerResponse(_ object: [String: Any], requestID: Int) {
        if requestID == 1, !appServerInitialized {
            guard object["result"] != nil else {
                NSLog("DeskConsoleHelper: Codex App Server initialization failed")
                return
            }
            appServerInitialized = true
            sendJSONLine(["method": "initialized", "params": [:] as [String: Any]])
            NSLog("DeskConsoleHelper: Codex App Server initialized")
            requestRateLimits()
            requestThreadList()
        } else if pendingRateLimitRequestIDs.remove(requestID) != nil {
            handleRateLimitResponse(object)
        } else if pendingThreadListRequestIDs.remove(requestID) != nil {
            handleThreadListResponse(object)
        }
    }

    private func handleRateLimitResponse(_ object: [String: Any]) {
        if object["error"] != nil {
            NSLog("DeskConsoleHelper: Codex rate-limit request returned an error")
            return
        }
        guard let result = object["result"] as? [String: Any],
              let limits = result["rateLimits"] as? [String: Any],
              let primary = limits["primary"] as? [String: Any] else {
            NSLog("DeskConsoleHelper: Codex rate-limit response was missing expected fields")
            return
        }
        let windows = [primary, limits["secondary"] as? [String: Any]].compactMap { item -> (UInt8, UInt16, Date?)? in
            guard let item else {
                return nil
            }
            let duration = minutes(item["windowDurationMins"])
            guard duration > 0 else {
                return nil
            }
            return (percent(item["usedPercent"]), duration, date(item["resetsAt"]))
        }

        // App Server 的 primary / secondary 是接口顺序，并不保证分别代表
        // 短周期与周额度。按实际窗口时长归一化，避免将 7 天窗口误标。
        let shortWindow = windows
            .filter { $0.1 < 24 * 60 }
            .min { $0.1 < $1.1 }
        let weeklyWindow = windows
            .filter { $0.1 >= 24 * 60 }
            .max { $0.1 < $1.1 }

        codexUsageAvailable = shortWindow != nil
        codexPrimary = shortWindow?.0 ?? 0
        codexPrimaryWindow = shortWindow?.1 ?? 0
        codexPrimaryResetsAt = shortWindow?.2
        codexSecondaryAvailable = weeklyWindow != nil
        codexSecondary = weeklyWindow?.0 ?? 0
        codexSecondaryWindow = weeklyWindow?.1 ?? 0
        codexSecondaryResetsAt = weeklyWindow?.2
        NSLog(
            "DeskConsoleHelper: Codex limits updated; short=%d weekly=%d weeklyUsed=%d%%",
            codexUsageAvailable ? 1 : 0,
            codexSecondaryAvailable ? 1 : 0,
            codexSecondary
        )
    }

    private func handleThreadListResponse(_ object: [String: Any]) {
        guard let result = object["result"] as? [String: Any],
              let threads = result["data"] as? [[String: Any]] else {
            return
        }
        codexThreads = threads.prefix(6).compactMap { thread in
            guard let id = thread["id"] as? String else {
                return nil
            }
            let rawName = (thread["name"] as? String)?.trimmingCharacters(in: .whitespacesAndNewlines)
            let name = limitedUTF8Title(
                rawName?.isEmpty == false ? rawName! : "未命名任务",
                maximumBytes: 36
            )
            return DeskCodexThread(
                id: id,
                name: name,
                status: taskStatus(thread)
            )
        }
    }

    private func percent(_ value: Any?) -> UInt8 {
        guard let number = value as? NSNumber else {
            return 0
        }
        return UInt8(max(0, min(100, number.intValue)))
    }

    private func minutes(_ value: Any?) -> UInt16 {
        guard let number = value as? NSNumber else {
            return 0
        }
        return UInt16(max(0, min(Int(UInt16.max), number.intValue)))
    }

    private func date(_ value: Any?) -> Date? {
        guard let number = value as? NSNumber, number.doubleValue > 0 else {
            return nil
        }
        return Date(timeIntervalSince1970: number.doubleValue)
    }

    private func remainingSeconds(until date: Date?) -> UInt32 {
        guard let date else {
            return 0
        }
        return UInt32(max(0, min(Double(UInt32.max), date.timeIntervalSinceNow.rounded(.up))))
    }

    private func taskStatus(_ thread: [String: Any]) -> String {
        let status = thread["status"] as? [String: Any]
        let type = status?["type"] as? String
        let flags = status?["activeFlags"] as? [String] ?? []
        if type == "active" {
            if flags.contains("waitingOnApproval") {
                return "waiting_permission"
            }
            return "running"
        }
        if type == "systemError" {
            return "failed"
        }

        // 桌面端与助手分别使用 App Server 连接。桌面端正在运行的任务在助手连接里
        // 可能仍报告 notLoaded，因此用该任务会话文件的近期写入补足“运行中”状态。
        if let path = thread["path"] as? String,
           let attributes = try? FileManager.default.attributesOfItem(atPath: path),
           let modified = attributes[.modificationDate] as? Date,
           Date().timeIntervalSince(modified) >= 0,
           Date().timeIntervalSince(modified) <= 20 {
            return "running"
        }
        return "idle"
    }

    private func limitedUTF8Title(_ value: String, maximumBytes: Int) -> String {
        guard value.utf8.count > maximumBytes else {
            return value
        }
        var result = ""
        for character in value {
            let candidate = result + String(character)
            guard candidate.utf8.count <= maximumBytes else {
                break
            }
            result = candidate
        }
        return result.isEmpty ? "未命名任务" : result
    }

    @discardableResult
    func openCodexTask(slot: Int) -> Bool {
        guard codexThreads.indices.contains(slot),
              let encodedID = codexThreads[slot].id.addingPercentEncoding(withAllowedCharacters: .urlPathAllowed),
              let url = URL(string: "codex://threads/\(encodedID)") else {
            return false
        }
        return NSWorkspace.shared.open(url)
    }

    // 点击 Claude 会话卡片：用 Terminal 运行 `claude --resume <sessionId>` 跳回该对话。
    @discardableResult
    func openClaudeSession(slot: Int) -> Bool {
        guard claudeSessions.indices.contains(slot) else {
            return false
        }
        let sessionId = claudeSessions[slot].sessionId  // UUID，无 shell 注入风险
        let script = """
        tell application "Terminal"
            activate
            do script "claude --resume \(sessionId)"
        end tell
        """
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/osascript")
        process.arguments = ["-e", script]
        do {
            try process.run()
            return true
        } catch {
            NSLog("DeskConsoleHelper: unable to resume Claude session: %@", error.localizedDescription)
            return false
        }
    }

    private func refreshTaskCounts() {
        workQueue.async { [weak self] in
            guard let self else {
                return
            }
            let codex = self.countRecentlyUpdatedCodexSessions()
            let claudeSessions = self.buildClaudeSessions()
            let claudeRunning = claudeSessions.filter { $0.status == "running" }.count
            DispatchQueue.main.async { [weak self] in
                self?.codexTaskCount = UInt8(min(codex, Int(UInt8.max)))
                self?.claudeSessions = claudeSessions
                self?.claudeTaskCount = UInt8(min(claudeRunning, Int(UInt8.max)))
            }
        }
    }

    private func countRecentlyUpdatedCodexSessions() -> Int {
        let home = FileManager.default.homeDirectoryForCurrentUser
        let calendar = Calendar(identifier: .gregorian)
        let now = Date()
        var count = 0
        for dayOffset in [0, -1] {
            guard let date = calendar.date(byAdding: .day, value: dayOffset, to: now) else {
                continue
            }
            let components = calendar.dateComponents([.year, .month, .day], from: date)
            guard let year = components.year, let month = components.month, let day = components.day else {
                continue
            }
            let directory = home
                .appendingPathComponent(".codex/sessions")
                .appendingPathComponent(String(format: "%04d/%02d/%02d", year, month, day))
            let files = (try? FileManager.default.contentsOfDirectory(
                at: directory,
                includingPropertiesForKeys: [.contentModificationDateKey, .isRegularFileKey],
                options: [.skipsHiddenFiles]
            )) ?? []
            for file in files where file.pathExtension == "jsonl" {
                guard let values = try? file.resourceValues(forKeys: [.contentModificationDateKey, .isRegularFileKey]),
                      values.isRegularFile == true,
                      let modified = values.contentModificationDate,
                      now.timeIntervalSince(modified) >= 0,
                      now.timeIntervalSince(modified) <= 20 else {
                    continue
                }
                count += 1
            }
        }
        return count
    }

    // 逐个会话（每个 .jsonl = 一个 Claude 会话）枚举，按最近写入排序取前 4。
    // “运行中”= 会话文件在最近 CLAUDE_RUNNING_WINDOW 秒内被写（用户发消息/Claude 处理时会持续追加记录）；
    // 否则“空闲”。名称取会话标题（自定义 > AI 生成 > 项目名）。绝不外发完整路径或会话正文。
    private func buildClaudeSessions() -> [DeskClaudeSession] {
        let projectsDir = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent(".claude/projects")
        let projectDirs = (try? FileManager.default.contentsOfDirectory(
            at: projectsDir,
            includingPropertiesForKeys: [.isDirectoryKey],
            options: [.skipsHiddenFiles]
        )) ?? []
        var files: [(url: URL, sessionId: String, project: String, modified: Date)] = []
        for dir in projectDirs {
            guard (try? dir.resourceValues(forKeys: [.isDirectoryKey]))?.isDirectory == true else {
                continue
            }
            let items = (try? FileManager.default.contentsOfDirectory(
                at: dir,
                includingPropertiesForKeys: [.contentModificationDateKey],
                options: [.skipsHiddenFiles]
            )) ?? []
            for item in items where item.pathExtension == "jsonl" {
                guard let modified = (try? item.resourceValues(forKeys: [.contentModificationDateKey]))?.contentModificationDate else {
                    continue
                }
                files.append((item, item.deletingPathExtension().lastPathComponent, dir.lastPathComponent, modified))
            }
        }
        // 先按写入时间排序、只取前 4，再读标题（读取会话文件较重，仅对显示的少数做）。
        let now = Date()
        return files.sorted { $0.modified > $1.modified }.prefix(4).map { entry in
            let elapsed = now.timeIntervalSince(entry.modified)
            // 优先用 hook 精确状态；旧会话无 hook 文件时回退到“最近 10 秒有写入”。
            let running = hookSessionRunning(entry.sessionId) ?? (elapsed >= 0 && elapsed <= 10)
            return DeskClaudeSession(
                sessionId: entry.sessionId,
                name: claudeSessionTitle(file: entry.url, sessionId: entry.sessionId, project: entry.project),
                status: running ? "running" : "idle",
                detail: relativeTimeDetail(elapsed),
                modified: entry.modified
            )
        }
    }

    // 读 hook 写的会话状态文件（UserPromptSubmit→running / Stop→idle，精确）。
    // 返回 nil 表示该会话没有 hook 状态（回退到写入启发式）。
    private func hookSessionRunning(_ sessionId: String) -> Bool? {
        let file = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent(".claude/desk-console/state/\(sessionId)")
        guard let data = try? Data(contentsOf: file),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            return nil
        }
        // running 且未过期（防会话崩溃未触发 Stop 导致永久“运行中”，20 分钟为界）。
        if (object["status"] as? String) == "running",
           let ts = object["ts"] as? NSNumber,
           Date().timeIntervalSince1970 - ts.doubleValue < 1200 {
            return true
        }
        return false
    }

    // 会话标题：自定义标题 > AI 生成标题 > 项目名末段。按 sessionId 缓存（标题稳定，避免重复读大文件）。
    private func claudeSessionTitle(file: URL, sessionId: String, project: String) -> String {
        if let cached = titleCache[sessionId] {
            return cached
        }
        var customTitle: String?
        var aiTitle: String?
        if let content = try? String(contentsOf: file, encoding: .utf8) {
            for line in content.split(separator: "\n") {
                guard let data = line.data(using: .utf8),
                      let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
                    continue
                }
                switch object["type"] as? String {
                case "custom-title": customTitle = object["customTitle"] as? String ?? customTitle
                case "ai-title": aiTitle = object["aiTitle"] as? String ?? aiTitle
                default: break
                }
            }
        }
        let fallback = claudeProjectName(fromSlug: project)
        let raw = customTitle ?? aiTitle ?? fallback
        let name = limitedUTF8Title(raw.isEmpty ? fallback : raw, maximumBytes: 36)
        titleCache[sessionId] = name
        return name
    }

    private func claudeProjectName(fromSlug slug: String) -> String {
        // slug 形如 -Users-<user>-<路径…>；隐私红线：只取末段项目名，绝不发送完整路径。
        let parts = slug.split(separator: "-", omittingEmptySubsequences: true).map(String.init)
        let name = parts.last ?? slug
        return limitedUTF8Title(name.isEmpty ? "会话" : name, maximumBytes: 36)
    }

    private func relativeTimeDetail(_ elapsed: TimeInterval) -> String {
        let seconds = max(0, Int(elapsed))
        if seconds < 60 {
            return "刚刚活动"
        }
        let minutes = seconds / 60
        if minutes < 60 {
            return "\(minutes) 分钟前"
        }
        let hours = minutes / 60
        if hours < 24 {
            return "\(hours) 小时前"
        }
        return "\(hours / 24) 天前"
    }
}
