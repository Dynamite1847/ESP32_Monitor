import Foundation

enum DeskMessageType: UInt16 {
    case hello = 0x0001
    case authChallenge = 0x0002
    case authResponse = 0x0003
    case authResult = 0x0004
    case heartbeat = 0x0005
    case lock = 0x0006
    case systemState = 0x0100
    case controlLayout = 0x0110
    case aiState = 0x0120
    case mediaState = 0x0130
    case actionTrigger = 0x0200
    case wifiProvision = 0x0300
    case wifiResult = 0x0301
    case weatherConfig = 0x0310
    case weatherConfigResult = 0x0311
}

struct DeskFrame {
    let flags: UInt8
    let messageType: DeskMessageType
    let sequence: UInt16
    let payload: Data
}

enum DeskProtocolError: LocalizedError {
    case invalidFrame
    case unsupportedVersion
    case invalidCRC
    case payloadTooLarge
    case invalidFragment
    case fragmentOutOfOrder

    var errorDescription: String? {
        switch self {
        case .invalidFrame:
            return "设备消息格式无效"
        case .unsupportedVersion:
            return "设备协议版本不兼容"
        case .invalidCRC:
            return "设备消息校验失败"
        case .payloadTooLarge:
            return "消息超过 512 字节上限"
        case .invalidFragment:
            return "蓝牙分片格式无效"
        case .fragmentOutOfOrder:
            return "蓝牙分片丢失或乱序"
        }
    }
}

enum DeskProtocolCodec {
    static let maximumPayloadLength = 512
    static let maximumFrameLength = 524

    static func encode(_ frame: DeskFrame) throws -> Data {
        guard frame.payload.count <= maximumPayloadLength else {
            throw DeskProtocolError.payloadTooLarge
        }

        var output = Data([0x44, 0x43, 0x01, frame.flags])
        output.appendLittleEndian(frame.messageType.rawValue)
        output.appendLittleEndian(frame.sequence)
        output.appendLittleEndian(UInt16(frame.payload.count))
        output.append(frame.payload)
        output.appendLittleEndian(crc16(output))
        return output
    }

    static func decode(_ data: Data) throws -> DeskFrame {
        guard data.count >= 12, data[0] == 0x44, data[1] == 0x43 else {
            throw DeskProtocolError.invalidFrame
        }
        guard data[2] == 0x01 else {
            throw DeskProtocolError.unsupportedVersion
        }

        let payloadLength = Int(data.uint16LittleEndian(at: 8))
        guard payloadLength <= maximumPayloadLength, data.count == 12 + payloadLength else {
            throw DeskProtocolError.invalidFrame
        }
        let expectedCRC = data.uint16LittleEndian(at: 10 + payloadLength)
        guard crc16(data.prefix(10 + payloadLength)) == expectedCRC else {
            throw DeskProtocolError.invalidCRC
        }
        guard let messageType = DeskMessageType(rawValue: data.uint16LittleEndian(at: 4)) else {
            throw DeskProtocolError.invalidFrame
        }

        return DeskFrame(
            flags: data[3],
            messageType: messageType,
            sequence: data.uint16LittleEndian(at: 6),
            payload: data.subdata(in: 10..<(10 + payloadLength))
        )
    }

    static func fragment(frame: Data, frameID: UInt16, maximumWriteLength: Int) throws -> [Data] {
        let capacity = maximumWriteLength - 4
        guard capacity > 0 else {
            throw DeskProtocolError.invalidFragment
        }
        let count = max(1, (frame.count + capacity - 1) / capacity)
        guard count <= Int(UInt8.max) else {
            throw DeskProtocolError.payloadTooLarge
        }

        return (0..<count).map { index in
            let start = index * capacity
            let end = min(frame.count, start + capacity)
            var packet = Data()
            packet.appendLittleEndian(frameID)
            packet.append(UInt8(index))
            packet.append(UInt8(count))
            packet.append(frame.subdata(in: start..<end))
            return packet
        }
    }

    static func crc16<T: DataProtocol>(_ bytes: T) -> UInt16 {
        var crc: UInt16 = 0xffff
        for byte in bytes {
            crc ^= UInt16(byte) << 8
            for _ in 0..<8 {
                crc = crc & 0x8000 != 0 ? (crc << 1) ^ 0x1021 : crc << 1
            }
        }
        return crc
    }
}

struct DeskFragmentReassembler {
    private var frameID: UInt16?
    private var fragmentCount: UInt8 = 0
    private var nextIndex: UInt8 = 0
    private var startedAt: TimeInterval = 0
    private var data = Data()

    mutating func reset() {
        frameID = nil
        fragmentCount = 0
        nextIndex = 0
        startedAt = 0
        data.removeAll(keepingCapacity: true)
    }

    mutating func accept(_ packet: Data) throws -> Data? {
        guard packet.count > 4 else {
            reset()
            throw DeskProtocolError.invalidFragment
        }

        let now = ProcessInfo.processInfo.systemUptime
        if frameID != nil, now - startedAt > 2 {
            reset()
        }

        let packetFrameID = packet.uint16LittleEndian(at: 0)
        let index = packet[2]
        let count = packet[3]
        guard count > 0, index < count else {
            reset()
            throw DeskProtocolError.invalidFragment
        }

        if index == 0 {
            reset()
            frameID = packetFrameID
            fragmentCount = count
            startedAt = now
        }
        guard frameID == packetFrameID, fragmentCount == count, nextIndex == index else {
            reset()
            throw DeskProtocolError.fragmentOutOfOrder
        }

        data.append(packet.subdata(in: 4..<packet.count))
        guard data.count <= DeskProtocolCodec.maximumFrameLength else {
            reset()
            throw DeskProtocolError.payloadTooLarge
        }
        nextIndex &+= 1
        guard nextIndex == fragmentCount else {
            return nil
        }

        let completed = data
        reset()
        return completed
    }
}

extension Data {
    mutating func appendLittleEndian(_ value: UInt16) {
        append(UInt8(value & 0xff))
        append(UInt8(value >> 8))
    }

    func uint16LittleEndian(at index: Int) -> UInt16 {
        UInt16(self[index]) | UInt16(self[index + 1]) << 8
    }
}
