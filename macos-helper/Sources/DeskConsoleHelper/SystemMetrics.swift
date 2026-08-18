import Darwin
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
    let valid = true
    let playing = false
    let titleHidden = false
    let volume: UInt8
    let position: UInt32 = 0
    let duration: UInt32 = 0
    let title = "Mac 当前媒体"
}

final class DeskMediaStateSampler {
    func sample() -> DeskMediaSnapshot {
        DeskMediaSnapshot(volume: outputVolumePercent())
    }

    private func outputVolumePercent() -> UInt8 {
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
            return 50
        }

        var scalar = Float32(0.5)
        propertySize = UInt32(MemoryLayout<Float32>.size)
        address = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyVolumeScalar,
            mScope: kAudioDevicePropertyScopeOutput,
            mElement: kAudioObjectPropertyElementMain
        )
        guard AudioObjectGetPropertyData(
            device,
            &address,
            0,
            nil,
            &propertySize,
            &scalar
        ) == noErr else {
            return 50
        }
        return UInt8(max(0, min(100, Int((scalar * 100).rounded()))))
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
        let unavailablePages = UInt64(statistics.free_count) + UInt64(statistics.speculative_count)
        let unavailableBytes = min(totalBytes, unavailablePages * UInt64(pageSize))
        return clampedX10Percent(Double(totalBytes - unavailableBytes) / Double(totalBytes))
    }

    private func sampleDiskFreeGB() -> UInt32 {
        guard let attributes = try? FileManager.default.attributesOfFileSystem(forPath: "/"),
              let freeBytes = attributes[.systemFreeSize] as? NSNumber else {
            return 0
        }
        let gigabytes = freeBytes.uint64Value / 1_000_000_000
        return UInt32(min(gigabytes, UInt64(UInt32.max)))
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
        let receivedKbps = Double(counters.received - previousNetwork.received) * 8 / 1_000 / elapsed
        let sentKbps = Double(counters.sent - previousNetwork.sent) * 8 / 1_000 / elapsed
        return (clampedUInt32(sentKbps), clampedUInt32(receivedKbps))
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
                if !name.hasPrefix("awdl") && !name.hasPrefix("llw") && !name.hasPrefix("utun") {
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
