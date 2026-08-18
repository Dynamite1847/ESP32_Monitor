import Foundation

struct DeskAIProviderSnapshot: Encodable {
    let id: String
    let usageAvailable: Bool
    let secondaryAvailable: Bool
    let primary: UInt8
    let secondary: UInt8
    let primaryWindow: UInt16
    let secondaryWindow: UInt16
    let tasks: UInt8
    let status: String
    let elapsed: UInt32
}

struct DeskAIStateSnapshot: Encodable {
    let providers: [DeskAIProviderSnapshot]
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
    private var pendingRateLimitRequestIDs = Set<Int>()

    private var codexUsageAvailable = false
    private var codexSecondaryAvailable = false
    private var codexPrimary: UInt8 = 0
    private var codexSecondary: UInt8 = 0
    private var codexPrimaryWindow: UInt16 = 0
    private var codexSecondaryWindow: UInt16 = 0
    private var codexTaskCount: UInt8 = 0
    private var claudeTaskCount: UInt8 = 0
    private var claudeUsageAvailable = false
    private var claudeSecondaryAvailable = false
    private var claudePrimary: UInt8 = 0
    private var claudeSecondary: UInt8 = 0

    func start() {
        guard rateLimitTimer == nil else {
            return
        }
        startAppServer()
        refreshTaskCounts()
        rateLimitTimer = Timer.scheduledTimer(withTimeInterval: 60, repeats: true) { [weak self] _ in
            self?.requestRateLimits()
        }
        taskTimer = Timer.scheduledTimer(withTimeInterval: 5, repeats: true) { [weak self] _ in
            self?.refreshTaskCounts()
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
                    tasks: codexTaskCount,
                    status: codexTaskCount > 0 ? "running" : "idle",
                    elapsed: 0
                ),
                DeskAIProviderSnapshot(
                    id: "claude",
                    usageAvailable: claudeUsageAvailable,
                    secondaryAvailable: claudeSecondaryAvailable,
                    primary: claudePrimary,
                    secondary: claudeSecondary,
                    primaryWindow: claudeUsageAvailable ? 300 : 0,
                    secondaryWindow: claudeSecondaryAvailable ? 10080 : 0,
                    tasks: claudeTaskCount,
                    status: claudeTaskCount > 0 ? "running" : "idle",
                    elapsed: 0
                ),
            ]
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
        process.arguments = ["app-server"]
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
            sendJSONLine(["method": "initialized", "params": [:] as [String: Any]])
            requestRateLimits()
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
        appServerInput = nil
        appServerOutput = nil
        codexUsageAvailable = false
        codexSecondaryAvailable = false
        pendingRateLimitRequestIDs.removeAll()
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
        let requestID = nextRequestID
        nextRequestID += 1
        pendingRateLimitRequestIDs.insert(requestID)
        sendJSONLine([
            "method": "account/rateLimits/read",
            "id": requestID,
            "params": [:] as [String: Any],
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
        guard pendingRateLimitRequestIDs.remove(requestID) != nil,
              let result = object["result"] as? [String: Any],
              let limits = result["rateLimits"] as? [String: Any],
              let primary = limits["primary"] as? [String: Any] else {
            return
        }
        codexPrimary = percent(primary["usedPercent"])
        codexPrimaryWindow = minutes(primary["windowDurationMins"])
        codexUsageAvailable = true
        if let secondary = limits["secondary"] as? [String: Any] {
            codexSecondary = percent(secondary["usedPercent"])
            codexSecondaryWindow = minutes(secondary["windowDurationMins"])
            codexSecondaryAvailable = true
        } else {
            codexSecondary = 0
            codexSecondaryWindow = 0
            codexSecondaryAvailable = false
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

    private func refreshTaskCounts() {
        workQueue.async { [weak self] in
            guard let self else {
                return
            }
            let codex = self.countRecentlyUpdatedCodexSessions()
            let claude = self.countProcesses(named: "claude")
            let claudeUsage = self.readClaudeStatusSnapshot()
            DispatchQueue.main.async { [weak self] in
                self?.codexTaskCount = UInt8(min(codex, Int(UInt8.max)))
                self?.claudeTaskCount = UInt8(min(claude, Int(UInt8.max)))
                self?.claudeUsageAvailable = claudeUsage.primary != nil
                self?.claudeSecondaryAvailable = claudeUsage.secondary != nil
                self?.claudePrimary = claudeUsage.primary ?? 0
                self?.claudeSecondary = claudeUsage.secondary ?? 0
            }
        }
    }

    private func readClaudeStatusSnapshot() -> (primary: UInt8?, secondary: UInt8?) {
        let snapshot = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/DeskConsoleHelper/claude-status.json")
        guard let attributes = try? FileManager.default.attributesOfItem(atPath: snapshot.path),
              let modified = attributes[.modificationDate] as? Date,
              Date().timeIntervalSince(modified) >= 0,
              Date().timeIntervalSince(modified) <= 90,
              let data = try? Data(contentsOf: snapshot),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            return (nil, nil)
        }
        let primary = (object["fiveHour"] as? NSNumber).map { percent($0) }
        let secondary = (object["sevenDay"] as? NSNumber).map { percent($0) }
        return (primary, secondary)
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

    private func countProcesses(named name: String) -> Int {
        let process = Process()
        let output = Pipe()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/pgrep")
        process.arguments = ["-x", name]
        process.standardOutput = output
        process.standardError = FileHandle.nullDevice
        do {
            try process.run()
            process.waitUntilExit()
            let data = output.fileHandleForReading.readDataToEndOfFile()
            guard let text = String(data: data, encoding: .utf8) else {
                return 0
            }
            return text.split(whereSeparator: { $0.isNewline }).count
        } catch {
            return 0
        }
    }
}
