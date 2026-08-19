import AppKit

/// Desk Console 管理窗口：蓝牙连接管理、多 Wi-Fi 配置、天气配置。
/// 菜单栏应用的控制台入口，替代零散的弹窗式配置。
final class ConsoleWindowController: NSWindowController, NSTableViewDataSource, NSTableViewDelegate {
    private let controller: DeskBluetoothController

    // 蓝牙连接区
    private let statusLabel = NSTextField(labelWithString: "正在启动…")
    private let connectButton = NSButton(title: "连接", target: nil, action: nil)
    private let disconnectButton = NSButton(title: "断开", target: nil, action: nil)

    // Wi-Fi 配置区
    private struct WiFiProfile: Codable {
        var ssid: String
        var password: String
    }
    private var profiles: [WiFiProfile] = []
    private let profileTable = NSTableView()
    private let ssidField = NSTextField()
    private let passwordField = NSSecureTextField()
    private let wifiHintLabel = NSTextField(labelWithString: "可保存多套网络（家里/公司），选择一套下发给设备。")

    // 天气配置区
    private let hostField = NSTextField()
    private let keyField = NSSecureTextField()
    private let longitudeField = NSTextField()
    private let latitudeField = NSTextField()

    private static let profilesKey = "desk.wifi.profiles"
    private static let lastUsedKey = "desk.wifi.lastUsedSSID"

    init(controller: DeskBluetoothController) {
        self.controller = controller
        super.init(window: nil)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not supported")
    }

    override func windowDidLoad() {
        super.windowDidLoad()
        controller.stateChanged = { [weak self] state in
            DispatchQueue.main.async {
                self?.statusLabel.stringValue = state.menuTitle
            }
        }
        statusLabel.stringValue = controller.currentStateTitle
        loadProfiles()
    }

    func showWindowIfNeeded() {
        if window == nil {
            buildWindow()
        }
        showWindow(nil)
        window?.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    // MARK: - UI 构建

    private func buildWindow() {
        let content = NSView(frame: NSRect(x: 0, y: 0, width: 480, height: 560))
        let window = NSWindow(
            contentRect: content.frame,
            styleMask: [.titled, .closable, .miniaturizable],
            backing: .buffered,
            defer: false
        )
        window.title = "Desk Console 控制台"
        window.contentView = content
        self.window = window

        var y: CGFloat = content.frame.height - 24

        // ── 蓝牙连接区 ──
        let connectionTitle = NSTextField(labelWithString: "蓝牙连接")
        connectionTitle.font = .boldSystemFont(ofSize: 13)
        connectionTitle.frame = NSRect(x: 16, y: y - 16, width: 200, height: 18)
        content.addSubview(connectionTitle)
        y -= 40

        statusLabel.frame = NSRect(x: 16, y: y - 18, width: 300, height: 20)
        content.addSubview(statusLabel)

        connectButton.target = self
        connectButton.action = #selector(connectTapped)
        connectButton.frame = NSRect(x: 330, y: y - 24, width: 60, height: 26)
        content.addSubview(connectButton)

        disconnectButton.target = self
        disconnectButton.action = #selector(disconnectTapped)
        disconnectButton.frame = NSRect(x: 398, y: y - 24, width: 60, height: 26)
        content.addSubview(disconnectButton)
        y -= 44

        // ── Wi-Fi 配置区 ──
        let wifiTitle = NSTextField(labelWithString: "Wi-Fi 配置")
        wifiTitle.font = .boldSystemFont(ofSize: 13)
        wifiTitle.frame = NSRect(x: 16, y: y - 16, width: 200, height: 18)
        content.addSubview(wifiTitle)
        y -= 36

        wifiHintLabel.textColor = .secondaryLabelColor
        wifiHintLabel.font = .systemFont(ofSize: 11)
        wifiHintLabel.frame = NSRect(x: 16, y: y - 14, width: 448, height: 16)
        content.addSubview(wifiHintLabel)
        y -= 26

        // 网络列表
        let tableColumn = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("ssid"))
        tableColumn.title = "网络名称"
        tableColumn.width = 400
        profileTable.addTableColumn(tableColumn)
        profileTable.headerView = nil
        profileTable.rowHeight = 22
        profileTable.dataSource = self
        profileTable.delegate = self
        profileTable.allowsEmptySelection = false

        let scroll = NSScrollView(frame: NSRect(x: 16, y: y - 110, width: 448, height: 110))
        scroll.hasVerticalScroller = true
        scroll.documentView = profileTable
        scroll.borderType = .bezelBorder
        content.addSubview(scroll)
        y -= 124

        // 添加行：SSID + 密码
        let ssidLabel = NSTextField(labelWithString: "SSID")
        ssidLabel.frame = NSRect(x: 16, y: y - 20, width: 40, height: 20)
        content.addSubview(ssidLabel)
        ssidField.placeholderString = "Wi-Fi 名称"
        ssidField.frame = NSRect(x: 60, y: y - 24, width: 140, height: 24)
        content.addSubview(ssidField)

        let pwLabel = NSTextField(labelWithString: "密码")
        pwLabel.frame = NSRect(x: 210, y: y - 20, width: 34, height: 20)
        content.addSubview(pwLabel)
        passwordField.placeholderString = "留空表示开放网络"
        passwordField.frame = NSRect(x: 248, y: y - 24, width: 140, height: 24)
        content.addSubview(passwordField)

        let addButton = NSButton(title: "保存到列表", target: self, action: #selector(addProfileTapped))
        addButton.frame = NSRect(x: 396, y: y - 26, width: 70, height: 26)
        content.addSubview(addButton)
        y -= 40

        let removeButton = NSButton(title: "删除所选", target: self, action: #selector(removeProfileTapped))
        removeButton.frame = NSRect(x: 16, y: y - 28, width: 80, height: 26)
        content.addSubview(removeButton)

        let provisionButton = NSButton(title: "下发所选并连接", target: self, action: #selector(provisionSelectedTapped))
        provisionButton.frame = NSRect(x: 330, y: y - 28, width: 134, height: 26)
        content.addSubview(provisionButton)
        y -= 44

        // ── 天气配置区 ──
        let weatherTitle = NSTextField(labelWithString: "天气配置（和风天气）")
        weatherTitle.font = .boldSystemFont(ofSize: 13)
        weatherTitle.frame = NSRect(x: 16, y: y - 16, width: 200, height: 18)
        content.addSubview(weatherTitle)
        y -= 36

        let weatherHint = NSTextField(labelWithString: "设备通过 Wi-Fi 直接请求天气，凭据只保存在设备 NVS 中。")
        weatherHint.textColor = .secondaryLabelColor
        weatherHint.font = .systemFont(ofSize: 11)
        weatherHint.frame = NSRect(x: 16, y: y - 14, width: 448, height: 16)
        content.addSubview(weatherHint)
        y -= 26

        func addField(_ label: String, _ field: NSTextField, width: CGFloat, x: CGFloat, y: CGFloat) {
            let textLabel = NSTextField(labelWithString: label)
            textLabel.frame = NSRect(x: x, y: y + 2, width: 70, height: 20)
            content.addSubview(textLabel)
            field.frame = NSRect(x: x + 74, y: y, width: width, height: 24)
            content.addSubview(field)
        }

        hostField.placeholderString = "devapi.qweather.com"
        addField("API Host", hostField, width: 200, x: 16, y: y - 26)
        keyField.placeholderString = "和风 API Key"
        addField("API Key", keyField, width: 170, x: 248, y: y - 26)
        y -= 40

        longitudeField.placeholderString = "经度，如 116.40"
        addField("经度", longitudeField, width: 170, x: 16, y: y - 26)
        latitudeField.placeholderString = "纬度，如 39.90"
        addField("纬度", latitudeField, width: 170, x: 248, y: y - 26)
        y -= 40

        let weatherSaveButton = NSButton(title: "保存并下发", target: self, action: #selector(weatherSaveTapped))
        weatherSaveButton.frame = NSRect(x: 330, y: y - 28, width: 134, height: 26)
        content.addSubview(weatherSaveButton)

        // 从上次选择恢复
        if let lastSSID = UserDefaults.standard.string(forKey: Self.lastUsedKey) {
            if let row = profiles.firstIndex(where: { $0.ssid == lastSSID }) {
                profileTable.selectRowIndexes(IndexSet(integer: row), byExtendingSelection: false)
            }
        }
    }

    // MARK: - 蓝牙操作

    @objc private func connectTapped() {
        controller.start()
    }

    @objc private func disconnectTapped() {
        controller.stop()
    }

    // MARK: - Wi-Fi 操作

    private func loadProfiles() {
        if let data = UserDefaults.standard.data(forKey: Self.profilesKey),
           let decoded = try? JSONDecoder().decode([WiFiProfile].self, from: data) {
            profiles = decoded
        }
    }

    private func saveProfiles() {
        if let data = try? JSONEncoder().encode(profiles) {
            UserDefaults.standard.set(data, forKey: Self.profilesKey)
        }
    }

    @objc private func addProfileTapped() {
        let ssid = ssidField.stringValue.trimmingCharacters(in: .whitespaces)
        let password = passwordField.stringValue
        guard !ssid.isEmpty, ssid.utf8.count <= 32, password.utf8.count <= 63 else {
            showMessage(title: "无法保存", message: "SSID 1–32 字节，密码不超过 63 字节。")
            return
        }
        if let index = profiles.firstIndex(where: { $0.ssid == ssid }) {
            profiles[index].password = password
        } else {
            profiles.append(WiFiProfile(ssid: ssid, password: password))
        }
        saveProfiles()
        profileTable.reloadData()
        if let index = profiles.firstIndex(where: { $0.ssid == ssid }) {
            profileTable.selectRowIndexes(IndexSet(integer: index), byExtendingSelection: false)
        }
        ssidField.stringValue = ""
        passwordField.stringValue = ""
    }

    @objc private func removeProfileTapped() {
        let row = profileTable.selectedRow
        guard row >= 0, row < profiles.count else { return }
        profiles.remove(at: row)
        saveProfiles()
        profileTable.reloadData()
    }

    @objc private func provisionSelectedTapped() {
        let row = profileTable.selectedRow
        guard row >= 0, row < profiles.count else {
            showMessage(title: "未选择网络", message: "请先在列表中选择要下发的网络。")
            return
        }
        let profile = profiles[row]
        do {
            try controller.provisionWiFi(ssid: profile.ssid, password: profile.password)
            UserDefaults.standard.set(profile.ssid, forKey: Self.lastUsedKey)
            // 同步到旧对话框预填
            UserDefaults.standard.set(profile.ssid, forKey: "desk.wifi.ssid")
            UserDefaults.standard.set(profile.password, forKey: "desk.wifi.password")
            showMessage(title: "已下发", message: "正在将「\(profile.ssid)」发送给设备…")
        } catch {
            showMessage(title: "下发失败", message: error.localizedDescription)
        }
    }

    // MARK: - 天气操作

    @objc private func weatherSaveTapped() {
        let host = hostField.stringValue.trimmingCharacters(in: .whitespaces)
        let key = keyField.stringValue.trimmingCharacters(in: .whitespaces)
        guard let longitude = Double(longitudeField.stringValue),
              let latitude = Double(latitudeField.stringValue) else {
            showMessage(title: "坐标无效", message: "请填写有效的经纬度数值。")
            return
        }
        do {
            try controller.configureWeather(host: host, apiKey: key, longitude: longitude, latitude: latitude)
            showMessage(title: "已下发", message: "天气配置已发送给设备。")
        } catch {
            showMessage(title: "下发失败", message: error.localizedDescription)
        }
    }

    // MARK: - NSTableViewDataSource / Delegate

    func numberOfRows(in tableView: NSTableView) -> Int {
        profiles.count
    }

    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
        let identifier = NSUserInterfaceItemIdentifier("cell")
        let cell: NSTableCellView
        if let reused = tableView.makeView(withIdentifier: identifier, owner: nil) as? NSTableCellView {
            cell = reused
        } else {
            cell = NSTableCellView()
            cell.identifier = identifier
            let text = NSTextField(labelWithString: "")
            text.translatesAutoresizingMaskIntoConstraints = false
            cell.addSubview(text)
            cell.textField = text
            NSLayoutConstraint.activate([
                text.leadingAnchor.constraint(equalTo: cell.leadingAnchor, constant: 4),
                text.trailingAnchor.constraint(equalTo: cell.trailingAnchor, constant: -4),
                text.centerYAnchor.constraint(equalTo: cell.centerYAnchor),
            ])
        }
        cell.textField?.stringValue = profiles[row].ssid
        return cell
    }

    // MARK: - 辅助

    private func showMessage(title: String, message: String) {
        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = message
        alert.alertStyle = .informational
        alert.runModal()
    }
}
