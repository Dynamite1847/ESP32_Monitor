import AppKit
import ApplicationServices
import CoreBluetooth
import Foundation

private enum DeskBluetoothUUID {
    static let service = CBUUID(string: "7a1d0001-5d1f-4b42-9e8d-3b2c8c1a4f00")
    static let receive = CBUUID(string: "7a1d0002-5d1f-4b42-9e8d-3b2c8c1a4f00")
    static let transmit = CBUUID(string: "7a1d0003-5d1f-4b42-9e8d-3b2c8c1a4f00")
    static let status = CBUUID(string: "7a1d0004-5d1f-4b42-9e8d-3b2c8c1a4f00")
}

private enum ConnectionState: Equatable {
    case starting
    case permissionDenied
    case bluetoothOff
    case scanning
    case connecting(String)
    case discovering
    case connected(DeviceStatus?)
    case authenticating(DeviceStatus?)
    case authenticated(DeviceStatus?)
    case retrying(Int)
    case failed(String)

    var menuTitle: String {
        switch self {
        case .starting:
            return "正在启动蓝牙…"
        case .permissionDenied:
            return "没有蓝牙权限"
        case .bluetoothOff:
            return "蓝牙已关闭"
        case .scanning:
            return "正在查找 Desk Console 4.3…"
        case let .connecting(name):
            return "正在连接 \(name)…"
        case .discovering:
            return "已连接，正在读取服务…"
        case let .connected(status):
            guard let status else {
                return "蓝牙已连接"
            }
            var details = ["MTU \(status.mtu)"]
            if status.encrypted {
                details.append("已加密")
            }
            if status.bonded {
                details.append("已绑定")
            }
            return "蓝牙已连接 · " + details.joined(separator: " · ")
        case .authenticating:
            return "蓝牙已连接 · 正在验证设备…"
        case let .authenticated(status):
            var details = ["已认证"]
            if let status {
                details.append("MTU \(status.mtu)")
                if status.encrypted {
                    details.append("已加密")
                }
            }
            return "控制台在线 · " + details.joined(separator: " · ")
        case let .retrying(seconds):
            return "连接断开，\(seconds) 秒后重试"
        case let .failed(message):
            return message
        }
    }

    var statusBarTitle: String {
        switch self {
        case .authenticated:
            return "控制台 ●"
        case .permissionDenied, .bluetoothOff, .failed:
            return "控制台 !"
        default:
            return "控制台 …"
        }
    }
}

private struct DeviceStatus: Equatable {
    let protocolVersion: UInt8
    let connected: Bool
    let encrypted: Bool
    let bonded: Bool
    let subscribed: Bool
    let mtu: UInt16
    let receiveErrors: UInt16

    init?(data: Data) {
        guard data.count >= 8 else {
            return nil
        }
        protocolVersion = data[0]
        let flags = data[1]
        connected = flags & 0x01 != 0
        encrypted = flags & 0x02 != 0
        bonded = flags & 0x04 != 0
        subscribed = flags & 0x08 != 0
        mtu = UInt16(data[2]) | UInt16(data[3]) << 8
        receiveErrors = UInt16(data[4]) | UInt16(data[5]) << 8
    }
}

private struct DeskActionRequest: Decodable {
    let actionId: UInt16
    let gesture: String
}

private struct DeskWiFiProvisionRequest: Encodable {
    let ssid: String
    let password: String
}

private struct DeskWiFiProvisionResult: Decodable {
    let ok: Bool
    let code: Int32
}

private struct DeskWeatherConfigurationRequest: Encodable {
    let provider = "qweather"
    let host: String
    let apiKey: String
    let longitude: Double
    let latitude: Double
}

private struct DeskWeatherConfigurationResult: Decodable {
    let ok: Bool
    let code: Int32
}

private enum DeskWiFiProvisionError: LocalizedError {
    case deviceUnavailable
    case invalidSSID
    case invalidPassword

    var errorDescription: String? {
        switch self {
        case .deviceUnavailable:
            return "设备尚未完成认证"
        case .invalidSSID:
            return "Wi-Fi 名称必须包含 1–32 个 UTF-8 字节"
        case .invalidPassword:
            return "Wi-Fi 密码需要为空，或包含 8–63 个 UTF-8 字节"
        }
    }
}

private enum DeskWeatherConfigurationError: LocalizedError {
    case deviceUnavailable
    case invalidHost
    case invalidAPIKey
    case invalidCoordinate

    var errorDescription: String? {
        switch self {
        case .deviceUnavailable:
            return "设备尚未完成认证"
        case .invalidHost:
            return "请填写和风天气控制台中的专用 API Host，不要包含 https:// 或路径"
        case .invalidAPIKey:
            return "请填写有效的和风天气 API Key"
        case .invalidCoordinate:
            return "经度需要介于 -180–180，纬度需要介于 -90–90"
        }
    }
}

private final class DeskBluetoothController: NSObject {
    var stateChanged: ((ConnectionState) -> Void)?
    var wifiProvisionCompleted: ((Bool, Int32) -> Void)?
    var weatherConfigurationCompleted: ((Bool, Int32) -> Void)?

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var receiveCharacteristic: CBCharacteristic?
    private var transmitCharacteristic: CBCharacteristic?
    private var statusCharacteristic: CBCharacteristic?
    private var retryWorkItem: DispatchWorkItem?
    private var connectionTimeoutWorkItem: DispatchWorkItem?
    private var heartbeatTimer: Timer?
    private var systemMetricsTimer: Timer?
    private var securityPollTimer: Timer?
    private var deviceStatus: DeviceStatus?
    private var lastSentActiveApplication = ""
    private var reassembler = DeskFragmentReassembler()
    private let authenticator = DeskAuthenticator()
    private let metricsSampler = DeskSystemMetricsSampler()
    private let mediaSampler = DeskMediaStateSampler()
    private let aiStatusMonitor = DeskAIStatusMonitor()
    private let telemetryQueue = DispatchQueue(
        label: "com.dongyu.desk-console-helper.telemetry",
        qos: .utility
    )
    private var nextSequence: UInt16 = 0
    private var nextFrameID: UInt16 = 0
    private var authenticationStarted = false
    private var connectionSuspended = false
    private var retryAttempt = 0
    private var state: ConnectionState = .starting {
        didSet {
            guard oldValue != state else {
                return
            }
            stateChanged?(state)
            NSLog("DeskConsoleHelper: %@", state.menuTitle)
        }
    }

    override init() {
        super.init()
    }

    func start() {
        guard central == nil else {
            return
        }
        aiStatusMonitor.start()
        central = CBCentralManager(delegate: self, queue: .main)
    }

    func reconnectNow() {
        connectionSuspended = false
        retryWorkItem?.cancel()
        retryWorkItem = nil
        retryAttempt = 0

        if let peripheral, peripheral.state != .disconnected {
            central.cancelPeripheralConnection(peripheral)
        } else {
            beginScanning()
        }
    }

    func stop() {
        connectionSuspended = true
        retryWorkItem?.cancel()
        connectionTimeoutWorkItem?.cancel()
        heartbeatTimer?.invalidate()
        systemMetricsTimer?.invalidate()
        securityPollTimer?.invalidate()
        guard central != nil else {
            return
        }
        central.stopScan()
        if let peripheral, peripheral.state != .disconnected {
            central.cancelPeripheralConnection(peripheral)
        }
    }

    private func beginScanning() {
        guard !connectionSuspended else {
            return
        }
        guard central.state == .poweredOn else {
            updateUnavailableState()
            return
        }
        central.stopScan()
        resetSession()
        clearCharacteristics()
        state = .scanning
        central.scanForPeripherals(
            withServices: [DeskBluetoothUUID.service],
            options: [CBCentralManagerScanOptionAllowDuplicatesKey: false]
        )
    }

    private func scheduleReconnect() {
        guard !connectionSuspended else {
            return
        }
        retryWorkItem?.cancel()
        retryAttempt += 1
        let delay = min(15, 1 << min(retryAttempt - 1, 3))
        state = .retrying(delay)

        let workItem = DispatchWorkItem { [weak self] in
            self?.beginScanning()
        }
        retryWorkItem = workItem
        DispatchQueue.main.asyncAfter(deadline: .now() + .seconds(delay), execute: workItem)
    }

    private func clearCharacteristics() {
        receiveCharacteristic = nil
        transmitCharacteristic = nil
        statusCharacteristic = nil
    }

    private func resetSession() {
        connectionTimeoutWorkItem?.cancel()
        connectionTimeoutWorkItem = nil
        heartbeatTimer?.invalidate()
        heartbeatTimer = nil
        systemMetricsTimer?.invalidate()
        systemMetricsTimer = nil
        securityPollTimer?.invalidate()
        securityPollTimer = nil
        deviceStatus = nil
        reassembler.reset()
        authenticator.reset()
        nextSequence = 0
        nextFrameID = 0
        authenticationStarted = false
        lastSentActiveApplication = ""
    }

    private func updateUnavailableState() {
        switch central.state {
        case .unauthorized:
            state = .permissionDenied
        case .poweredOff:
            state = .bluetoothOff
        case .unsupported:
            state = .failed("这台 Mac 不支持低功耗蓝牙")
        case .resetting:
            state = .failed("蓝牙正在重新初始化")
        case .unknown:
            state = .starting
        case .poweredOn:
            break
        @unknown default:
            state = .failed("遇到未知蓝牙状态")
        }
    }

    private func finishDiscoveryIfReady() {
        guard let peripheral,
              receiveCharacteristic != nil,
              let transmitCharacteristic,
              let statusCharacteristic else {
            return
        }

        peripheral.setNotifyValue(true, for: transmitCharacteristic)
        peripheral.setNotifyValue(true, for: statusCharacteristic)
        peripheral.readValue(for: statusCharacteristic)
        state = .connected(nil)
        startSecurityPolling()

        let writeLength = peripheral.maximumWriteValueLength(for: .withoutResponse)
        NSLog("DeskConsoleHelper: GATT ready, maximum write length %d", writeLength)
    }

    private func beginAuthenticationIfReady() {
        guard !authenticationStarted,
              let peripheral,
              peripheral.state == .connected,
              deviceStatus?.encrypted == true,
              transmitCharacteristic?.isNotifying == true else {
            return
        }

        do {
            authenticationStarted = true
            securityPollTimer?.invalidate()
            securityPollTimer = nil
            state = .authenticating(deviceStatus)
            let hello = try authenticator.makeHello(peripheralID: peripheral.identifier)
            try sendMessage(.hello, payload: hello)
        } catch {
            authenticationFailed(error)
        }
    }

    private func startSecurityPolling() {
        securityPollTimer?.invalidate()
        securityPollTimer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] timer in
            guard let self,
                  !self.authenticationStarted,
                  let peripheral = self.peripheral,
                  peripheral.state == .connected,
                  let statusCharacteristic = self.statusCharacteristic else {
                timer.invalidate()
                return
            }
            peripheral.readValue(for: statusCharacteristic)
        }
    }

    private func sendMessage(_ messageType: DeskMessageType, payload: Data = Data()) throws {
        guard let peripheral,
              let receiveCharacteristic,
              peripheral.state == .connected else {
            throw DeskProtocolError.invalidFrame
        }

        let frame = try DeskProtocolCodec.encode(
            DeskFrame(
                flags: 0,
                messageType: messageType,
                sequence: nextSequence,
                payload: payload
            )
        )
        nextSequence &+= 1

        let writeLength = peripheral.maximumWriteValueLength(for: .withResponse)
        let packets = try DeskProtocolCodec.fragment(
            frame: frame,
            frameID: nextFrameID,
            maximumWriteLength: writeLength
        )
        nextFrameID &+= 1
        for packet in packets {
            peripheral.writeValue(packet, for: receiveCharacteristic, type: .withResponse)
        }
    }

    private func handleDeviceFrame(_ frame: DeskFrame) {
        do {
            switch frame.messageType {
            case .authChallenge:
                let response = try authenticator.handleChallenge(frame.payload)
                try sendMessage(.authResponse, payload: response)

            case .authResult:
                let enrolled = try authenticator.handleResult(frame.payload)
                retryAttempt = 0
                state = .authenticated(deviceStatus)
                NSLog("DeskConsoleHelper: application authentication succeeded, enrolled=%d", enrolled ? 1 : 0)
                startHeartbeat()
                startSystemTelemetry()

            case .actionTrigger:
                try handleAction(frame.payload)

            case .wifiResult:
                let result = try JSONDecoder().decode(DeskWiFiProvisionResult.self, from: frame.payload)
                wifiProvisionCompleted?(result.ok, result.code)

            case .weatherConfigResult:
                let result = try JSONDecoder().decode(DeskWeatherConfigurationResult.self, from: frame.payload)
                weatherConfigurationCompleted?(result.ok, result.code)

            default:
                NSLog("DeskConsoleHelper: received message type 0x%04x", frame.messageType.rawValue)
            }
        } catch {
            authenticationFailed(error)
        }
    }

    func provisionWiFi(ssid: String, password: String) throws {
        guard authenticator.isAuthenticated else {
            throw DeskWiFiProvisionError.deviceUnavailable
        }
        let ssidLength = ssid.utf8.count
        let passwordLength = password.utf8.count
        guard (1...32).contains(ssidLength) else {
            throw DeskWiFiProvisionError.invalidSSID
        }
        guard passwordLength == 0 || (8...63).contains(passwordLength) else {
            throw DeskWiFiProvisionError.invalidPassword
        }
        let payload = try JSONEncoder().encode(
            DeskWiFiProvisionRequest(ssid: ssid, password: password)
        )
        try sendMessage(.wifiProvision, payload: payload)
    }

    func configureWeather(host: String, apiKey: String, longitude: Double, latitude: Double) throws {
        guard authenticator.isAuthenticated else {
            throw DeskWeatherConfigurationError.deviceUnavailable
        }
        let hostCharacters = CharacterSet(charactersIn: "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-")
        guard !host.isEmpty, host.utf8.count <= 127,
              host.unicodeScalars.allSatisfy({ hostCharacters.contains($0) }),
              !host.hasPrefix("."), !host.hasSuffix(".") else {
            throw DeskWeatherConfigurationError.invalidHost
        }
        let keyCharacters = CharacterSet(charactersIn: "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")
        guard !apiKey.isEmpty, apiKey.utf8.count <= 95,
              apiKey.unicodeScalars.allSatisfy({ keyCharacters.contains($0) }) else {
            throw DeskWeatherConfigurationError.invalidAPIKey
        }
        guard longitude.isFinite, latitude.isFinite,
              (-180...180).contains(longitude), (-90...90).contains(latitude) else {
            throw DeskWeatherConfigurationError.invalidCoordinate
        }
        let payload = try JSONEncoder().encode(
            DeskWeatherConfigurationRequest(
                host: host,
                apiKey: apiKey,
                longitude: longitude,
                latitude: latitude
            )
        )
        try sendMessage(.weatherConfig, payload: payload)
    }

    private func handleAction(_ payload: Data) throws {
        guard authenticator.isAuthenticated else {
            throw DeskProtocolError.invalidFrame
        }
        let request = try JSONDecoder().decode(DeskActionRequest.self, from: payload)
        guard request.gesture == "tap" else {
            throw DeskProtocolError.invalidFrame
        }

        switch request.actionId {
        case 1:
            sendKeyboardShortcut(keyCode: 33)
        case 2:
            sendKeyboardShortcut(keyCode: 30)
        case 3:
            sendKeyboardShortcut(keyCode: 15)
        case 4:
            sendKeyboardShortcut(keyCode: 17)
        case 5:
            launchScreenshotTool()
        case 6:
            openTerminal()
        case 7:
            sendMediaKey(20)
        case 8:
            sendMediaKey(16)
        case 9:
            sendMediaKey(19)
        case 10:
            sendMediaKey(7)
        case 11:
            sendMediaKey(1)
        case 12:
            sendMediaKey(0)
        default:
            throw DeskProtocolError.invalidFrame
        }
        if (7...12).contains(request.actionId) {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) { [weak self] in
                self?.sendMediaState()
            }
        }
    }

    private func sendMediaKey(_ keyCode: Int32) {
        for keyDown in [true, false] {
            let state: Int32 = keyDown ? 0xA : 0xB
            let event = NSEvent.otherEvent(
                with: .systemDefined,
                location: .zero,
                modifierFlags: NSEvent.ModifierFlags(rawValue: UInt(state << 8)),
                timestamp: 0,
                windowNumber: 0,
                context: nil,
                subtype: 8,
                data1: Int((keyCode << 16) | (state << 8)),
                data2: -1
            )
            event?.cgEvent?.post(tap: .cghidEventTap)
        }
    }

    private func sendKeyboardShortcut(keyCode: CGKeyCode) {
        guard AXIsProcessTrusted() else {
            NSLog("DeskConsoleHelper: shortcut ignored until Accessibility access is enabled")
            return
        }
        guard let keyDown = CGEvent(keyboardEventSource: nil, virtualKey: keyCode, keyDown: true),
              let keyUp = CGEvent(keyboardEventSource: nil, virtualKey: keyCode, keyDown: false) else {
            return
        }
        keyDown.flags = .maskCommand
        keyUp.flags = .maskCommand
        keyDown.post(tap: .cghidEventTap)
        keyUp.post(tap: .cghidEventTap)
    }

    private func launchScreenshotTool() {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/sbin/screencapture")
        process.arguments = ["-i"]
        do {
            try process.run()
        } catch {
            NSLog("DeskConsoleHelper: unable to start screenshot tool: %@", error.localizedDescription)
        }
    }

    private func openTerminal() {
        let terminalURL = URL(fileURLWithPath: "/System/Applications/Utilities/Terminal.app")
        NSWorkspace.shared.openApplication(
            at: terminalURL,
            configuration: NSWorkspace.OpenConfiguration()
        ) { _, error in
            if let error {
                NSLog("DeskConsoleHelper: unable to open Terminal: %@", error.localizedDescription)
            }
        }
    }

    private func authenticationFailed(_ error: Error) {
        NSLog("DeskConsoleHelper: application authentication failed: %@", error.localizedDescription)
        state = .failed("设备认证失败：\(error.localizedDescription)")
        heartbeatTimer?.invalidate()
        heartbeatTimer = nil
        systemMetricsTimer?.invalidate()
        systemMetricsTimer = nil
        securityPollTimer?.invalidate()
        securityPollTimer = nil
        if let peripheral, peripheral.state != .disconnected {
            central.cancelPeripheralConnection(peripheral)
        }
    }

    private func startHeartbeat() {
        heartbeatTimer?.invalidate()
        sendHeartbeat()
        heartbeatTimer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            self?.sendHeartbeat()
        }
    }

    private func sendHeartbeat() {
        guard authenticator.isAuthenticated else {
            return
        }
        do {
            try sendMessage(.heartbeat)
        } catch {
            authenticationFailed(error)
        }
    }

    private func startSystemTelemetry() {
        systemMetricsTimer?.invalidate()
        sendSystemMetrics()
        sendControlLayout(force: true)
        sendMediaState()
        sendAIState()
        systemMetricsTimer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            self?.sendSystemMetrics()
            self?.sendControlLayout()
            self?.sendMediaState()
            self?.sendAIState()
        }
    }

    private func sendAIState() {
        guard authenticator.isAuthenticated else {
            return
        }
        do {
            let payload = try JSONEncoder().encode(aiStatusMonitor.snapshot())
            try sendMessage(.aiState, payload: payload)
        } catch {
            authenticationFailed(error)
        }
    }

    private func sendMediaState() {
        guard authenticator.isAuthenticated else {
            return
        }
        do {
            let payload = try JSONEncoder().encode(mediaSampler.sample())
            try sendMessage(.mediaState, payload: payload)
        } catch {
            authenticationFailed(error)
        }
    }

    private func sendControlLayout(force: Bool = false) {
        guard authenticator.isAuthenticated else {
            return
        }
        let detectedName = NSWorkspace.shared.frontmostApplication?.localizedName ?? "Mac"
        let activeApplication = limitedUTF8String(detectedName, maximumBytes: 31)
        guard force || activeApplication != lastSentActiveApplication else {
            return
        }
        do {
            let payload = try JSONEncoder().encode(
                DeskControlLayoutSnapshot(activeApp: activeApplication, actionCount: 6)
            )
            try sendMessage(.controlLayout, payload: payload)
            lastSentActiveApplication = activeApplication
        } catch {
            authenticationFailed(error)
        }
    }

    private func limitedUTF8String(_ value: String, maximumBytes: Int) -> String {
        var result = ""
        for character in value {
            let candidate = result + String(character)
            if candidate.utf8.count > maximumBytes {
                break
            }
            result = candidate
        }
        return result.isEmpty ? "Mac" : result
    }

    private func sendSystemMetrics() {
        guard authenticator.isAuthenticated else {
            return
        }
        telemetryQueue.async { [weak self] in
            guard let self else {
                return
            }
            let snapshot = self.metricsSampler.sample()
            guard let payload = try? JSONEncoder().encode(snapshot) else {
                NSLog("DeskConsoleHelper: unable to encode system metrics")
                return
            }
            DispatchQueue.main.async { [weak self] in
                guard let self, self.authenticator.isAuthenticated else {
                    return
                }
                do {
                    try self.sendMessage(.systemState, payload: payload)
                } catch {
                    self.authenticationFailed(error)
                }
            }
        }
    }

    func macDidLockOrSleep() {
        connectionSuspended = true
        retryWorkItem?.cancel()
        heartbeatTimer?.invalidate()
        heartbeatTimer = nil
        systemMetricsTimer?.invalidate()
        systemMetricsTimer = nil
        if authenticator.isAuthenticated {
            try? sendMessage(.lock)
        }
        guard let peripheral, peripheral.state != .disconnected else {
            return
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.25) { [weak self, weak peripheral] in
            guard let self, let peripheral, peripheral.state != .disconnected else {
                return
            }
            self.central.cancelPeripheralConnection(peripheral)
        }
    }

    func macDidBecomeActive() {
        guard connectionSuspended else {
            return
        }
        connectionSuspended = false
        reconnectNow()
    }
}

extension DeskBluetoothController: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        if central.state == .poweredOn {
            beginScanning()
        } else {
            retryWorkItem?.cancel()
            central.stopScan()
            updateUnavailableState()
        }
    }

    func centralManager(
        _ central: CBCentralManager,
        didDiscover peripheral: CBPeripheral,
        advertisementData: [String: Any],
        rssi RSSI: NSNumber
    ) {
        guard self.peripheral == nil || self.peripheral?.state == .disconnected else {
            return
        }

        central.stopScan()
        self.peripheral = peripheral
        peripheral.delegate = self
        let name = peripheral.name
            ?? advertisementData[CBAdvertisementDataLocalNameKey] as? String
            ?? "Desk Console 4.3"
        state = .connecting(name)
        central.connect(peripheral, options: [CBConnectPeripheralOptionNotifyOnDisconnectionKey: true])

        let timeout = DispatchWorkItem { [weak self, weak peripheral] in
            guard let self,
                  let peripheral,
                  self.peripheral === peripheral,
                  peripheral.state == .connecting else {
                return
            }
            NSLog("DeskConsoleHelper: connection attempt timed out")
            self.state = .failed("连接超时，稍后自动重试")
            self.central.cancelPeripheralConnection(peripheral)
        }
        connectionTimeoutWorkItem = timeout
        DispatchQueue.main.asyncAfter(deadline: .now() + .seconds(10), execute: timeout)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        retryWorkItem?.cancel()
        connectionTimeoutWorkItem?.cancel()
        connectionTimeoutWorkItem = nil
        resetSession()
        state = .discovering
        peripheral.discoverServices([DeskBluetoothUUID.service])
    }

    func centralManager(
        _ central: CBCentralManager,
        didFailToConnect peripheral: CBPeripheral,
        error: Error?
    ) {
        NSLog("DeskConsoleHelper: connection failed: %@", error?.localizedDescription ?? "unknown")
        self.peripheral = nil
        resetSession()
        clearCharacteristics()
        scheduleReconnect()
    }

    func centralManager(
        _ central: CBCentralManager,
        didDisconnectPeripheral peripheral: CBPeripheral,
        error: Error?
    ) {
        if let error {
            NSLog("DeskConsoleHelper: disconnected: %@", error.localizedDescription)
        }
        self.peripheral = nil
        resetSession()
        clearCharacteristics()
        scheduleReconnect()
    }
}

extension DeskBluetoothController: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        if let error {
            state = .failed("读取设备服务失败：\(error.localizedDescription)")
            central.cancelPeripheralConnection(peripheral)
            return
        }

        guard let service = peripheral.services?.first(where: { $0.uuid == DeskBluetoothUUID.service }) else {
            state = .failed("设备缺少桌面控制台服务")
            central.cancelPeripheralConnection(peripheral)
            return
        }

        peripheral.discoverCharacteristics(
            [DeskBluetoothUUID.receive, DeskBluetoothUUID.transmit, DeskBluetoothUUID.status],
            for: service
        )
    }

    func peripheral(
        _ peripheral: CBPeripheral,
        didDiscoverCharacteristicsFor service: CBService,
        error: Error?
    ) {
        if let error {
            state = .failed("读取设备能力失败：\(error.localizedDescription)")
            central.cancelPeripheralConnection(peripheral)
            return
        }

        for characteristic in service.characteristics ?? [] {
            switch characteristic.uuid {
            case DeskBluetoothUUID.receive:
                receiveCharacteristic = characteristic
            case DeskBluetoothUUID.transmit:
                transmitCharacteristic = characteristic
            case DeskBluetoothUUID.status:
                statusCharacteristic = characteristic
            default:
                continue
            }
        }
        finishDiscoveryIfReady()
    }

    func peripheral(
        _ peripheral: CBPeripheral,
        didUpdateNotificationStateFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        if let error {
            NSLog(
                "DeskConsoleHelper: notification setup failed for %@: %@",
                characteristic.uuid.uuidString,
                error.localizedDescription
            )
            return
        }
        if characteristic.uuid == DeskBluetoothUUID.transmit,
           characteristic.isNotifying {
            beginAuthenticationIfReady()
        }
        if characteristic.uuid == DeskBluetoothUUID.status,
           characteristic.isNotifying,
           let statusCharacteristic {
            peripheral.readValue(for: statusCharacteristic)
        }
    }

    func peripheral(
        _ peripheral: CBPeripheral,
        didUpdateValueFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        if let error {
            NSLog(
                "DeskConsoleHelper: value update failed for %@: %@",
                characteristic.uuid.uuidString,
                error.localizedDescription
            )
            return
        }
        guard let data = characteristic.value else {
            return
        }

        if characteristic.uuid == DeskBluetoothUUID.status {
            if let deviceStatus = DeviceStatus(data: data) {
                self.deviceStatus = deviceStatus
                if authenticator.isAuthenticated {
                    state = .authenticated(deviceStatus)
                } else if authenticationStarted {
                    state = .authenticating(deviceStatus)
                } else {
                    state = .connected(deviceStatus)
                }
                beginAuthenticationIfReady()
                NSLog(
                    "DeskConsoleHelper: protocol v%d, encrypted=%d bonded=%d subscribed=%d rxErrors=%d",
                    deviceStatus.protocolVersion,
                    deviceStatus.encrypted ? 1 : 0,
                    deviceStatus.bonded ? 1 : 0,
                    deviceStatus.subscribed ? 1 : 0,
                    deviceStatus.receiveErrors
                )
            } else {
                NSLog("DeskConsoleHelper: ignored malformed device status (%d bytes)", data.count)
            }
        } else if characteristic.uuid == DeskBluetoothUUID.transmit {
            do {
                if let logicalFrame = try reassembler.accept(data) {
                    handleDeviceFrame(try DeskProtocolCodec.decode(logicalFrame))
                }
            } catch {
                authenticationFailed(error)
            }
        }
    }

    func peripheral(
        _ peripheral: CBPeripheral,
        didWriteValueFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        if let error {
            authenticationFailed(error)
        }
    }
}

private final class ApplicationDelegate: NSObject, NSApplicationDelegate {
    private let bluetoothController = DeskBluetoothController()
    private var statusItem: NSStatusItem!
    private let connectionItem = NSMenuItem(title: "正在启动蓝牙…", action: nil, keyEquivalent: "")
    private var wifiItem: NSMenuItem!
    private var weatherItem: NSMenuItem!
    private var workspaceObservers: [NSObjectProtocol] = []

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.accessory)
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)

        let menu = NSMenu()
        connectionItem.isEnabled = false
        menu.addItem(connectionItem)
        menu.addItem(.separator())

        wifiItem = NSMenuItem(
            title: "配置设备 Wi-Fi…",
            action: #selector(configureWiFi),
            keyEquivalent: ""
        )
        wifiItem.target = self
        wifiItem.isEnabled = false
        menu.addItem(wifiItem)

        weatherItem = NSMenuItem(
            title: "配置天气数据…",
            action: #selector(configureWeather),
            keyEquivalent: ""
        )
        weatherItem.target = self
        weatherItem.isEnabled = false
        menu.addItem(weatherItem)
        menu.addItem(.separator())

        let reconnectItem = NSMenuItem(
            title: "立即重新连接",
            action: #selector(reconnect),
            keyEquivalent: "r"
        )
        reconnectItem.target = self
        menu.addItem(reconnectItem)

        let quitItem = NSMenuItem(
            title: "退出桌面控制台助手",
            action: #selector(quit),
            keyEquivalent: "q"
        )
        quitItem.target = self
        menu.addItem(quitItem)

        statusItem.menu = menu
        updateStatus(.starting)
        bluetoothController.stateChanged = { [weak self] state in
            self?.updateStatus(state)
        }
        bluetoothController.wifiProvisionCompleted = { [weak self] succeeded, code in
            self?.showWiFiProvisionResult(succeeded: succeeded, code: code)
        }
        bluetoothController.weatherConfigurationCompleted = { [weak self] succeeded, code in
            self?.showWeatherConfigurationResult(succeeded: succeeded, code: code)
        }
        bluetoothController.start()
        observeWorkspaceState()
    }

    func applicationWillTerminate(_ notification: Notification) {
        bluetoothController.stop()
        for observer in workspaceObservers {
            NSWorkspace.shared.notificationCenter.removeObserver(observer)
        }
    }

    private func updateStatus(_ state: ConnectionState) {
        connectionItem.title = state.menuTitle
        statusItem.button?.title = state.statusBarTitle
        statusItem.button?.toolTip = state.menuTitle
        if case .authenticated = state {
            wifiItem?.isEnabled = true
            weatherItem?.isEnabled = true
        } else {
            wifiItem?.isEnabled = false
            weatherItem?.isEnabled = false
        }
    }

    @objc private func configureWiFi() {
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = "配置设备 Wi-Fi"
        alert.informativeText = "凭据只会通过已加密且已认证的连接发送给设备。"
        alert.addButton(withTitle: "保存并连接")
        alert.addButton(withTitle: "取消")

        let accessory = NSView(frame: NSRect(x: 0, y: 0, width: 340, height: 72))
        let ssidLabel = NSTextField(labelWithString: "Wi-Fi 名称")
        ssidLabel.frame = NSRect(x: 0, y: 45, width: 86, height: 20)
        let ssidField = NSTextField(frame: NSRect(x: 92, y: 42, width: 248, height: 24))
        let passwordLabel = NSTextField(labelWithString: "密码")
        passwordLabel.frame = NSRect(x: 0, y: 9, width: 86, height: 20)
        let passwordField = NSSecureTextField(frame: NSRect(x: 92, y: 6, width: 248, height: 24))
        accessory.addSubview(ssidLabel)
        accessory.addSubview(ssidField)
        accessory.addSubview(passwordLabel)
        accessory.addSubview(passwordField)
        alert.accessoryView = accessory
        alert.window.initialFirstResponder = ssidField

        guard alert.runModal() == .alertFirstButtonReturn else {
            return
        }
        do {
            try bluetoothController.provisionWiFi(
                ssid: ssidField.stringValue,
                password: passwordField.stringValue
            )
            wifiItem.title = "正在保存 Wi-Fi…"
            wifiItem.isEnabled = false
        } catch {
            showError(title: "Wi-Fi 配置未发送", message: error.localizedDescription)
        }
    }

    private func showWiFiProvisionResult(succeeded: Bool, code: Int32) {
        wifiItem.title = succeeded ? "Wi-Fi 配置已保存" : "Wi-Fi 配置失败（\(code)）"
        DispatchQueue.main.asyncAfter(deadline: .now() + 4) { [weak self] in
            self?.wifiItem.title = "配置设备 Wi-Fi…"
            self?.wifiItem.isEnabled = true
        }
    }

    @objc private func configureWeather() {
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = "配置天气数据"
        alert.informativeText = "使用和风天气专用 API Host 和 API Key。设备会自己通过 Wi-Fi 更新天气和预警。"
        alert.addButton(withTitle: "保存")
        alert.addButton(withTitle: "取消")

        let accessory = NSView(frame: NSRect(x: 0, y: 0, width: 390, height: 144))
        func addLabel(_ title: String, y: CGFloat) {
            let label = NSTextField(labelWithString: title)
            label.frame = NSRect(x: 0, y: y, width: 90, height: 20)
            accessory.addSubview(label)
        }
        addLabel("API Host", y: 117)
        addLabel("API Key", y: 81)
        addLabel("经度", y: 45)
        addLabel("纬度", y: 9)
        let hostField = NSTextField(frame: NSRect(x: 96, y: 114, width: 294, height: 24))
        hostField.placeholderString = "abc123.qweatherapi.com"
        let keyField = NSSecureTextField(frame: NSRect(x: 96, y: 78, width: 294, height: 24))
        let longitudeField = NSTextField(frame: NSRect(x: 96, y: 42, width: 294, height: 24))
        longitudeField.placeholderString = "121.47"
        let latitudeField = NSTextField(frame: NSRect(x: 96, y: 6, width: 294, height: 24))
        latitudeField.placeholderString = "31.23"
        accessory.addSubview(hostField)
        accessory.addSubview(keyField)
        accessory.addSubview(longitudeField)
        accessory.addSubview(latitudeField)
        alert.accessoryView = accessory
        alert.window.initialFirstResponder = hostField

        guard alert.runModal() == .alertFirstButtonReturn else {
            return
        }
        guard let longitude = Double(longitudeField.stringValue),
              let latitude = Double(latitudeField.stringValue) else {
            showError(title: "天气配置未发送", message: DeskWeatherConfigurationError.invalidCoordinate.localizedDescription)
            return
        }
        do {
            try bluetoothController.configureWeather(
                host: hostField.stringValue,
                apiKey: keyField.stringValue,
                longitude: longitude,
                latitude: latitude
            )
            weatherItem.title = "正在保存天气配置…"
            weatherItem.isEnabled = false
        } catch {
            showError(title: "天气配置未发送", message: error.localizedDescription)
        }
    }

    private func showWeatherConfigurationResult(succeeded: Bool, code: Int32) {
        weatherItem.title = succeeded ? "天气配置已保存" : "天气配置失败（\(code)）"
        DispatchQueue.main.asyncAfter(deadline: .now() + 4) { [weak self] in
            self?.weatherItem.title = "配置天气数据…"
            self?.weatherItem.isEnabled = true
        }
    }

    private func showError(title: String, message: String) {
        let alert = NSAlert()
        alert.alertStyle = .warning
        alert.messageText = title
        alert.informativeText = message
        alert.runModal()
    }

    @objc private func reconnect() {
        bluetoothController.reconnectNow()
    }

    @objc private func quit() {
        NSApp.terminate(nil)
    }

    private func observeWorkspaceState() {
        let center = NSWorkspace.shared.notificationCenter
        let lockNotifications: [Notification.Name] = [
            NSWorkspace.sessionDidResignActiveNotification,
            NSWorkspace.willSleepNotification,
            NSWorkspace.screensDidSleepNotification,
        ]
        let wakeNotifications: [Notification.Name] = [
            NSWorkspace.sessionDidBecomeActiveNotification,
            NSWorkspace.didWakeNotification,
            NSWorkspace.screensDidWakeNotification,
        ]

        for name in lockNotifications {
            workspaceObservers.append(
                center.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                    self?.bluetoothController.macDidLockOrSleep()
                }
            )
        }
        for name in wakeNotifications {
            workspaceObservers.append(
                center.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                    self?.bluetoothController.macDidBecomeActive()
                }
            )
        }
    }
}

private let application = NSApplication.shared
private let applicationDelegate = ApplicationDelegate()
application.delegate = applicationDelegate
application.run()
