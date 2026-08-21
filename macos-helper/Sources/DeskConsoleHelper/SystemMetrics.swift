import Darwin
import AppKit
import Foundation
import CoreAudio
import IOKit.ps

struct DeskSystemSnapshot: Encodable {
    let cpuX10Percent: UInt16
    let memoryX10Percent: UInt16
    let diskFreeGB: UInt32
    let networkUpKbps: UInt32
    let networkDownKbps: UInt32
    let batteryPercent: UInt8

    enum CodingKeys: String, CodingKey {
        case cpuX10Percent = "cpu10"
        case memoryX10Percent = "memory10"
        case diskFreeGB
        case networkUpKbps = "upKbps"
        case networkDownKbps = "downKbps"
        case batteryPercent = "battery"
    }
}

struct DeskControlLayoutSnapshot: Encodable {
    let activeApp: String
    let actionCount: UInt8
}

struct DeskMediaSnapshot: Encodable {
    let valid: Bool
    let metadataAvailable: Bool
    let playing: Bool
    let titleHidden: Bool
    let muted: Bool
    let volume: UInt8
    let position: UInt32
    let duration: UInt32
    let title: String
    let artist: String
    let source: String
}

private struct DeskMediaBridgePayload: Decodable {
    let title: String?
    let artist: String?
    let source: String?
    let playing: Bool?
    let position: Double?
    let duration: Double?
}

final class DeskMediaStateSampler {
    private static let hideTitlesKey = "desk.media.hideTitles"
    private let bridgeQueue = DispatchQueue(
        label: "com.dongyu.desk-console-helper.media-snapshot",
        qos: .utility
    )
    private let stateLock = NSLock()
    private var cachedMetadata: DeskMediaBridgePayload?
    private var cachedMetadataAt: TimeInterval = 0
    private var refreshInFlight = false
    private var lastRefreshAt: TimeInterval = 0
    private var volumeBeforeFallbackMute: Float32?

    func sample() -> DeskMediaSnapshot {
        requestMetadataRefreshIfNeeded()
        stateLock.lock()
        let metadata = cachedMetadata
        let metadataAt = cachedMetadataAt
        stateLock.unlock()

        let rawTitle = metadata?.title?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        let rawArtist = metadata?.artist?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        let hasMetadata = !rawTitle.isEmpty
        let titlesHidden = UserDefaults.standard.bool(forKey: Self.hideTitlesKey)
        let duration = clampedSeconds(metadata?.duration)
        var rawPosition = metadata?.position ?? 0
        if metadata?.playing == true, metadataAt > 0 {
            rawPosition += max(0, ProcessInfo.processInfo.systemUptime - metadataAt)
        }
        let position = min(clampedSeconds(rawPosition), duration > 0 ? duration : UInt32.max)
        return DeskMediaSnapshot(
            valid: true,
            metadataAvailable: hasMetadata,
            playing: metadata?.playing ?? false,
            titleHidden: titlesHidden,
            muted: outputMuted(),
            volume: outputVolumePercent(),
            position: position,
            duration: duration,
            title: titlesHidden ? "媒体标题已隐藏" :
                (hasMetadata ? limitedUTF8(rawTitle, maximumBytes: 95) : "当前没有播放内容"),
            artist: titlesHidden ? "" : limitedUTF8(rawArtist, maximumBytes: 63),
            source: displayName(for: metadata?.source)
        )
    }

    func toggleTitleVisibility() {
        let hidden = UserDefaults.standard.bool(forKey: Self.hideTitlesKey)
        UserDefaults.standard.set(!hidden, forKey: Self.hideTitlesKey)
    }

    @discardableResult
    func adjustVolume(by delta: Float32) -> Bool {
        guard let device = defaultOutputDevice(), var currentVolume = currentVolumeScalar(device: device) else {
            return false
        }

        stateLock.lock()
        if let volumeBeforeFallbackMute {
            currentVolume = volumeBeforeFallbackMute
            self.volumeBeforeFallbackMute = nil
        }
        stateLock.unlock()

        let target = max(0, min(1, currentVolume + delta))
        let changed = setVolumeScalar(device: device, value: target)
        if changed {
            _ = setMuted(device: device, muted: false)
        }
        return changed
    }

    @discardableResult
    func toggleMute() -> Bool {
        guard let device = defaultOutputDevice() else {
            return false
        }

        if let muted = currentHardwareMute(device: device), setMuted(device: device, muted: !muted) {
            return true
        }

        guard let currentVolume = currentVolumeScalar(device: device) else {
            return false
        }
        stateLock.lock()
        let savedVolume = volumeBeforeFallbackMute
        stateLock.unlock()

        if let savedVolume {
            guard setVolumeScalar(device: device, value: savedVolume) else {
                return false
            }
            stateLock.lock()
            volumeBeforeFallbackMute = nil
            stateLock.unlock()
            return true
        }

        guard setVolumeScalar(device: device, value: 0) else {
            return false
        }
        stateLock.lock()
        volumeBeforeFallbackMute = max(currentVolume, 0.5)
        stateLock.unlock()
        return true
    }

    private func requestMetadataRefreshIfNeeded() {
        let now = ProcessInfo.processInfo.systemUptime
        stateLock.lock()
        guard !refreshInFlight, now - lastRefreshAt >= 3.5 else {
            stateLock.unlock()
            return
        }
        refreshInFlight = true
        lastRefreshAt = now
        stateLock.unlock()

        bridgeQueue.async { [weak self] in
            let metadata = self?.readMediaBridge()
            guard let self else {
                return
            }
            self.stateLock.lock()
            if let metadata {
                self.cachedMetadata = metadata
                self.cachedMetadataAt = ProcessInfo.processInfo.systemUptime
            }
            self.refreshInFlight = false
            self.stateLock.unlock()
        }
    }

    private func readMediaBridge() -> DeskMediaBridgePayload? {
        guard let bridgeURL = Bundle.main.resourceURL?
            .appendingPathComponent("DeskMediaBridge.dylib"),
              FileManager.default.fileExists(atPath: bridgeURL.path) else {
            return nil
        }

        let process = Process()
        let output = Pipe()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/perl")
        process.standardOutput = output
        process.standardError = FileHandle.nullDevice
        process.arguments = [
            "-e",
            "use DynaLoader; my $h=DynaLoader::dl_load_file($ARGV[0], 1) or die DynaLoader::dl_error(); " +
            "my $s=DynaLoader::dl_find_symbol($h, 'desk_media_snapshot') or die DynaLoader::dl_error(); " +
            "DynaLoader::dl_install_xsub('DeskMedia::snapshot', $s); DeskMedia::snapshot();",
            bridgeURL.path,
        ]
        do {
            try process.run()
            process.waitUntilExit()
        } catch {
            return nil
        }
        guard process.terminationStatus == 0 else {
            return nil
        }
        let data = output.fileHandleForReading.readDataToEndOfFile()
        return try? JSONDecoder().decode(DeskMediaBridgePayload.self, from: data)
    }

    private func clampedSeconds(_ value: Double?) -> UInt32 {
        guard let value, value.isFinite, value > 0 else {
            return 0
        }
        return UInt32(min(Double(UInt32.max), value.rounded(.down)))
    }

    private func limitedUTF8(_ value: String, maximumBytes: Int) -> String {
        var result = ""
        for character in value {
            let candidate = result + String(character)
            if candidate.lengthOfBytes(using: .utf8) > maximumBytes {
                break
            }
            result = candidate
        }
        return result
    }

    private func displayName(for bundleIdentifier: String?) -> String {
        guard let bundleIdentifier, !bundleIdentifier.isEmpty else {
            return "Mac 媒体"
        }
        let knownSources = [
            "com.apple.Music": "音乐",
            "com.spotify.client": "Spotify",
            "com.apple.Safari": "Safari",
            "com.google.Chrome": "Chrome",
            "com.microsoft.edgemac": "Edge",
            "org.mozilla.firefox": "Firefox",
        ]
        return knownSources[bundleIdentifier] ??
            (NSRunningApplication.runningApplications(withBundleIdentifier: bundleIdentifier).first?.localizedName ?? "Mac 媒体")
    }

    private func outputVolumePercent() -> UInt8 {
        guard let device = defaultOutputDevice(), let scalar = currentVolumeScalar(device: device) else {
            return 50
        }
        return UInt8(max(0, min(100, Int((scalar * 100).rounded()))))
    }

    private func outputMuted() -> Bool {
        guard let device = defaultOutputDevice() else {
            return false
        }
        if currentHardwareMute(device: device) == true {
            return true
        }
        stateLock.lock()
        let fallbackMuted = volumeBeforeFallbackMute != nil
        stateLock.unlock()
        return fallbackMuted
    }

    private func currentHardwareMute(device: AudioObjectID) -> Bool? {
        var found = false
        for element in [kAudioObjectPropertyElementMain, 1, 2] {
            if let muted = readMuted(device: device, element: element) {
                found = true
                if muted {
                    return true
                }
            }
        }
        return found ? false : nil
    }

    private func currentVolumeScalar(device: AudioObjectID) -> Float32? {
        if let master = readVolumeScalar(device: device, element: kAudioObjectPropertyElementMain) {
            return master
        }
        let channels = [AudioObjectPropertyElement(1), AudioObjectPropertyElement(2)].compactMap {
            readVolumeScalar(device: device, element: $0)
        }
        guard !channels.isEmpty else {
            return nil
        }
        return channels.reduce(0, +) / Float32(channels.count)
    }

    private func setVolumeScalar(device: AudioObjectID, value: Float32) -> Bool {
        if writeVolumeScalar(device: device, element: kAudioObjectPropertyElementMain, value: value) {
            return true
        }
        var changed = false
        for element in [AudioObjectPropertyElement(1), AudioObjectPropertyElement(2)] {
            changed = writeVolumeScalar(device: device, element: element, value: value) || changed
        }
        return changed
    }

    private func setMuted(device: AudioObjectID, muted: Bool) -> Bool {
        if writeMuted(device: device, element: kAudioObjectPropertyElementMain, muted: muted) {
            return true
        }
        var changed = false
        for element in [AudioObjectPropertyElement(1), AudioObjectPropertyElement(2)] {
            changed = writeMuted(device: device, element: element, muted: muted) || changed
        }
        return changed
    }

    private func defaultOutputDevice() -> AudioObjectID? {
        var device = AudioObjectID(kAudioObjectUnknown)
        var propertySize = UInt32(MemoryLayout<AudioObjectID>.size)
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultOutputDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        guard AudioObjectGetPropertyData(
            AudioObjectID(kAudioObjectSystemObject),
            &address,
            0,
            nil,
            &propertySize,
            &device
        ) == noErr, device != kAudioObjectUnknown else {
            return nil
        }
        return device
    }

    private func readVolumeScalar(device: AudioObjectID, element: AudioObjectPropertyElement) -> Float32? {
        var scalar = Float32(0)
        var propertySize = UInt32(MemoryLayout<Float32>.size)
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyVolumeScalar,
            mScope: kAudioDevicePropertyScopeOutput,
            mElement: element
        )
        guard AudioObjectHasProperty(device, &address),
              AudioObjectGetPropertyData(device, &address, 0, nil, &propertySize, &scalar) == noErr else {
            return nil
        }
        return scalar
    }

    private func readMuted(device: AudioObjectID, element: AudioObjectPropertyElement) -> Bool? {
        var muted: UInt32 = 0
        var propertySize = UInt32(MemoryLayout<UInt32>.size)
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyMute,
            mScope: kAudioDevicePropertyScopeOutput,
            mElement: element
        )
        guard AudioObjectHasProperty(device, &address),
              AudioObjectGetPropertyData(device, &address, 0, nil, &propertySize, &muted) == noErr else {
            return nil
        }
        return muted != 0
    }

    private func writeVolumeScalar(
        device: AudioObjectID,
        element: AudioObjectPropertyElement,
        value: Float32
    ) -> Bool {
        var scalar = max(0, min(1, value))
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyVolumeScalar,
            mScope: kAudioDevicePropertyScopeOutput,
            mElement: element
        )
        guard AudioObjectHasProperty(device, &address) else {
            return false
        }
        return AudioObjectSetPropertyData(
            device,
            &address,
            0,
            nil,
            UInt32(MemoryLayout<Float32>.size),
            &scalar
        ) == noErr
    }

    private func writeMuted(
        device: AudioObjectID,
        element: AudioObjectPropertyElement,
        muted: Bool
    ) -> Bool {
        var rawValue: UInt32 = muted ? 1 : 0
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyMute,
            mScope: kAudioDevicePropertyScopeOutput,
            mElement: element
        )
        guard AudioObjectHasProperty(device, &address) else {
            return false
        }
        return AudioObjectSetPropertyData(
            device,
            &address,
            0,
            nil,
            UInt32(MemoryLayout<UInt32>.size),
            &rawValue
        ) == noErr
    }
}

final class DeskSystemMetricsSampler {
    private struct CPUTicks {
        let total: UInt64
        let idle: UInt64
    }

    private struct NetworkCounters {
        let received: UInt64
        let sent: UInt64
        let sampledAt: TimeInterval
    }

    private var previousCPU: CPUTicks?
    private var previousNetwork: NetworkCounters?

    func sample() -> DeskSystemSnapshot {
        let networkRates = sampleNetworkRates()
        return DeskSystemSnapshot(
            cpuX10Percent: sampleCPU(),
            memoryX10Percent: sampleMemory(),
            diskFreeGB: sampleDiskFreeGB(),
            networkUpKbps: networkRates.up,
            networkDownKbps: networkRates.down,
            batteryPercent: sampleBattery()
        )
    }

    private func sampleCPU() -> UInt16 {
        var load = host_cpu_load_info_data_t()
        var count = mach_msg_type_number_t(
            MemoryLayout<host_cpu_load_info_data_t>.size / MemoryLayout<integer_t>.size
        )
        let result = withUnsafeMutablePointer(to: &load) { pointer in
            pointer.withMemoryRebound(to: integer_t.self, capacity: Int(count)) { rebound in
                host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, rebound, &count)
            }
        }
        guard result == KERN_SUCCESS else {
            return 0
        }

        let ticks = withUnsafeBytes(of: load.cpu_ticks) { rawBuffer -> [UInt64] in
            rawBuffer.bindMemory(to: natural_t.self).map(UInt64.init)
        }
        guard ticks.count > Int(CPU_STATE_IDLE) else {
            return 0
        }
        let current = CPUTicks(
            total: ticks.reduce(0, +),
            idle: ticks[Int(CPU_STATE_IDLE)]
        )
        defer { previousCPU = current }

        let total: UInt64
        let idle: UInt64
        if let previousCPU,
           current.total >= previousCPU.total,
           current.idle >= previousCPU.idle {
            total = current.total - previousCPU.total
            idle = current.idle - previousCPU.idle
        } else {
            total = current.total
            idle = current.idle
        }
        guard total > 0 else {
            return 0
        }
        return clampedX10Percent(Double(total - min(idle, total)) / Double(total))
    }

    private func sampleMemory() -> UInt16 {
        var statistics = vm_statistics64_data_t()
        var count = mach_msg_type_number_t(
            MemoryLayout<vm_statistics64_data_t>.size / MemoryLayout<integer_t>.size
        )
        let result = withUnsafeMutablePointer(to: &statistics) { pointer in
            pointer.withMemoryRebound(to: integer_t.self, capacity: Int(count)) { rebound in
                host_statistics64(mach_host_self(), HOST_VM_INFO64, rebound, &count)
            }
        }
        guard result == KERN_SUCCESS else {
            return 0
        }

        var pageSize: vm_size_t = 0
        guard host_page_size(mach_host_self(), &pageSize) == KERN_SUCCESS else {
            return 0
        }
        let totalBytes = ProcessInfo.processInfo.physicalMemory
        guard totalBytes > 0 else {
            return 0
        }
        // 与「活动监视器」的“已用内存”同口径：App 内存 + 联动(wired) + 压缩内存。
        // App 内存 = 匿名页(internal) − 可清除页(purgeable)；不把 inactive、文件缓存、
        // 可清除页算作占用（旧实现按 free+speculative 反推，会把这些都当已用，虚高到 ~98%）。
        let internalPages = UInt64(statistics.internal_page_count)
        let purgeablePages = UInt64(statistics.purgeable_count)
        let appPages = internalPages > purgeablePages ? internalPages - purgeablePages : 0
        let usedPages = appPages + UInt64(statistics.wire_count) + UInt64(statistics.compressor_page_count)
        let usedBytes = min(totalBytes, usedPages * UInt64(pageSize))
        return clampedX10Percent(Double(usedBytes) / Double(totalBytes))
    }

    private func sampleDiskFreeGB() -> UInt32 {
        // 与 Finder 同口径：数据卷的「重要用途可用容量」，含系统可清除(purgeable)空间。
        // 旧实现用 `/`(只读系统卷) 的 systemFreeSize，不含可清除空间，比 Finder 少十几 GB。
        let dataVolume = FileManager.default.homeDirectoryForCurrentUser
        if let values = try? dataVolume.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]),
           let importantFree = values.volumeAvailableCapacityForImportantUsage, importantFree > 0 {
            return UInt32(min(UInt64(importantFree) / 1_000_000_000, UInt64(UInt32.max)))
        }
        // 回退：老系统或取值失败时用传统 statfs。
        guard let attributes = try? FileManager.default.attributesOfFileSystem(forPath: "/"),
              let freeBytes = attributes[.systemFreeSize] as? NSNumber else {
            return 0
        }
        return UInt32(min(freeBytes.uint64Value / 1_000_000_000, UInt64(UInt32.max)))
    }

    private func sampleNetworkRates() -> (up: UInt32, down: UInt32) {
        let now = ProcessInfo.processInfo.systemUptime
        let counters = readNetworkCounters(sampledAt: now)
        defer { previousNetwork = counters }
        guard let previousNetwork,
              counters.sampledAt > previousNetwork.sampledAt,
              counters.received >= previousNetwork.received,
              counters.sent >= previousNetwork.sent else {
            return (0, 0)
        }

        let elapsed = counters.sampledAt - previousNetwork.sampledAt
        // 字节/秒，与 iStat Menus 默认口径一致。旧实现用 ×8÷1000 换成比特/秒(Kbps)，
        // 比 iStat 的字节口径大约 8 倍，这就是“网速对不上、差很多”的根因。
        let receivedBytesPerSec = Double(counters.received - previousNetwork.received) / elapsed
        let sentBytesPerSec = Double(counters.sent - previousNetwork.sent) / elapsed
        return (clampedUInt32(sentBytesPerSec), clampedUInt32(receivedBytesPerSec))
    }

    private func readNetworkCounters(sampledAt: TimeInterval) -> NetworkCounters {
        var firstAddress: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&firstAddress) == 0, let firstAddress else {
            return NetworkCounters(received: 0, sent: 0, sampledAt: sampledAt)
        }
        defer { freeifaddrs(firstAddress) }

        var received: UInt64 = 0
        var sent: UInt64 = 0
        var cursor: UnsafeMutablePointer<ifaddrs>? = firstAddress
        while let address = cursor {
            let interface = address.pointee
            let flags = Int32(interface.ifa_flags)
            let isUsable = flags & IFF_UP != 0 && flags & IFF_LOOPBACK == 0
            if isUsable,
               interface.ifa_addr?.pointee.sa_family == UInt8(AF_LINK),
               let dataPointer = interface.ifa_data {
                let name = String(cString: interface.ifa_name)
                if !name.hasPrefix("awdl") && !name.hasPrefix("llw") && !name.hasPrefix("utun")
                    && !name.hasPrefix("bridge") && !name.hasPrefix("anpi") && !name.hasPrefix("ap") {
                    let data = dataPointer.assumingMemoryBound(to: if_data.self).pointee
                    received &+= UInt64(data.ifi_ibytes)
                    sent &+= UInt64(data.ifi_obytes)
                }
            }
            cursor = interface.ifa_next
        }
        return NetworkCounters(received: received, sent: sent, sampledAt: sampledAt)
    }

    private func sampleBattery() -> UInt8 {
        guard let information = IOPSCopyPowerSourcesInfo()?.takeRetainedValue(),
              let sources = IOPSCopyPowerSourcesList(information)?.takeRetainedValue() as? [CFTypeRef] else {
            return UInt8.max
        }
        for source in sources {
            guard let description = IOPSGetPowerSourceDescription(information, source)?.takeUnretainedValue()
                as? [String: Any],
                  let current = description[kIOPSCurrentCapacityKey] as? NSNumber,
                  let maximum = description[kIOPSMaxCapacityKey] as? NSNumber,
                  maximum.doubleValue > 0 else {
                continue
            }
            let percent = current.doubleValue / maximum.doubleValue
            return UInt8(min(100, max(0, Int((percent * 100).rounded()))))
        }
        return UInt8.max
    }

    private func clampedX10Percent(_ ratio: Double) -> UInt16 {
        UInt16(min(1000, max(0, Int((ratio * 1000).rounded()))))
    }

    private func clampedUInt32(_ value: Double) -> UInt32 {
        UInt32(min(Double(UInt32.max), max(0, value.rounded())))
    }
}
