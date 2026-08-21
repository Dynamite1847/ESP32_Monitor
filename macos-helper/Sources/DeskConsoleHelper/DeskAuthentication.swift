import CryptoKit
import Foundation
import Security

enum DeskAuthenticationError: LocalizedError {
    case invalidChallenge
    case missingSharedKey
    case invalidResult
    case rejected(UInt8)
    case randomGenerationFailed(OSStatus)
    case storage(Error)

    var errorDescription: String? {
        switch self {
        case .invalidChallenge:
            return "设备认证挑战无效"
        case .missingSharedKey:
            return "Mac 本地没有设备认证密钥"
        case .invalidResult:
            return "设备认证结果无效"
        case let .rejected(code):
            return "设备拒绝认证，错误码 \(code)"
        case let .randomGenerationFailed(status):
            return "生成认证随机数失败：\(status)"
        case let .storage(error):
            return "访问本地认证密钥失败：\(error.localizedDescription)"
        }
    }
}

final class DeskAuthenticator {
    enum State {
        case idle
        case helloSent(clientNonce: Data, account: String, storedKey: Data?)
        case responseSent(account: String, enrollmentKey: Data?)
        case authenticated
    }

    private static let payloadVersion: UInt8 = 1
    private static let enrollmentFlag: UInt8 = 1
    private static let hmacDomain = Data("desk-console-auth-v1".utf8)
    private var state: State = .idle
    private var cachedKeys: [String: Data] = [:]

    var isAuthenticated: Bool {
        if case .authenticated = state {
            return true
        }
        return false
    }

    func reset() {
        state = .idle
    }

    func makeHello(peripheralID: UUID) throws -> Data {
        let account = peripheralID.uuidString
        let existingKey: Data?
        if let cachedKey = cachedKeys[account] {
            existingKey = cachedKey
        } else {
            existingKey = try DeskLocalKeyStore.load(account: account)
            if let existingKey {
                cachedKeys[account] = existingKey
            }
        }
        let clientNonce = try Self.randomData(count: 16)

        var payload = Data([Self.payloadVersion, existingKey == nil ? Self.enrollmentFlag : 0])
        payload.append(clientNonce)
        state = .helloSent(clientNonce: clientNonce, account: account, storedKey: existingKey)
        return payload
    }

    func handleChallenge(_ payload: Data) throws -> Data {
        guard case let .helloSent(clientNonce, account, storedKey) = state,
              payload.count == 26 || payload.count == 58,
              payload[0] == Self.payloadVersion else {
            throw DeskAuthenticationError.invalidChallenge
        }

        let containsEnrollmentKey = payload[1] & Self.enrollmentFlag != 0
        guard containsEnrollmentKey == (payload.count == 58) else {
            throw DeskAuthenticationError.invalidChallenge
        }

        let deviceNonce = payload.subdata(in: 2..<18)
        let sessionID = payload.subdata(in: 18..<26)
        let enrollmentKey = containsEnrollmentKey ? payload.subdata(in: 26..<58) : nil
        guard let key = enrollmentKey ?? storedKey else {
            throw DeskAuthenticationError.missingSharedKey
        }

        if let enrollmentKey {
            try DeskLocalKeyStore.save(enrollmentKey, account: account)
            cachedKeys[account] = enrollmentKey
        }

        var authenticatedData = Self.hmacDomain
        authenticatedData.append(clientNonce)
        authenticatedData.append(deviceNonce)
        authenticatedData.append(sessionID)
        let authenticationCode = HMAC<SHA256>.authenticationCode(
            for: authenticatedData,
            using: SymmetricKey(data: key)
        )

        var response = Data([Self.payloadVersion, 0])
        response.append(contentsOf: authenticationCode)
        state = .responseSent(account: account, enrollmentKey: enrollmentKey)
        return response
    }

    func handleResult(_ payload: Data) throws -> Bool {
        guard case let .responseSent(account, enrollmentKey) = state,
              payload.count == 3,
              payload[0] == Self.payloadVersion else {
            throw DeskAuthenticationError.invalidResult
        }

        let resultCode = payload[1]
        if resultCode == 1 {
            state = .authenticated
            return payload[2] != 0
        }

        if enrollmentKey != nil || resultCode == 2 {
            try? DeskLocalKeyStore.delete(account: account)
            cachedKeys.removeValue(forKey: account)
        }
        state = .idle
        throw DeskAuthenticationError.rejected(resultCode)
    }

    private static func randomData(count: Int) throws -> Data {
        var data = Data(count: count)
        let status = data.withUnsafeMutableBytes { bytes in
            SecRandomCopyBytes(kSecRandomDefault, count, bytes.baseAddress!)
        }
        guard status == errSecSuccess else {
            throw DeskAuthenticationError.randomGenerationFailed(status)
        }
        return data
    }
}

private enum DeskLocalKeyStore {
    private static let directoryName = "com.dongyu.desk-console-helper"

    static func load(account: String) throws -> Data? {
        do {
            let url = try keyURL(account: account, createDirectory: false)
            guard FileManager.default.fileExists(atPath: url.path) else {
                return nil
            }
            let data = try Data(contentsOf: url, options: [.mappedIfSafe])
            guard data.count == 32 else {
                try? FileManager.default.removeItem(at: url)
                return nil
            }
            try protect(url: url, permissions: 0o600)
            return data
        } catch let error as CocoaError where error.code == .fileNoSuchFile {
            return nil
        } catch {
            throw DeskAuthenticationError.storage(error)
        }
    }

    static func save(_ key: Data, account: String) throws {
        guard key.count == 32 else {
            throw DeskAuthenticationError.storage(
                CocoaError(.fileWriteInvalidFileName, userInfo: [NSLocalizedDescriptionKey: "认证密钥长度无效"])
            )
        }
        do {
            let url = try keyURL(account: account, createDirectory: true)
            // macOS 26 上应用外部启动和重签名后，FileProtection 文件可能持续
            // 返回 EPERM。目录 0700 + 文件 0600 已限定为当前用户，避免再加
            // 会导致重启后无法读取的 Data Protection 类别。
            try key.write(to: url, options: [.atomic])
            try protect(url: url, permissions: 0o600)
        } catch {
            throw DeskAuthenticationError.storage(error)
        }
    }

    static func delete(account: String) throws {
        do {
            let url = try keyURL(account: account, createDirectory: false)
            if FileManager.default.fileExists(atPath: url.path) {
                try FileManager.default.removeItem(at: url)
            }
        } catch let error as CocoaError where error.code == .fileNoSuchFile {
            return
        } catch {
            throw DeskAuthenticationError.storage(error)
        }
    }

    private static func keyURL(account: String, createDirectory: Bool) throws -> URL {
        let base = try FileManager.default.url(
            for: .applicationSupportDirectory,
            in: .userDomainMask,
            appropriateFor: nil,
            create: createDirectory
        )
        let directory = base.appendingPathComponent(directoryName, isDirectory: true)
        if createDirectory {
            try FileManager.default.createDirectory(
                at: directory,
                withIntermediateDirectories: true,
                attributes: [.posixPermissions: 0o700]
            )
            try protect(url: directory, permissions: 0o700)
        }
        let safeAccount = account.lowercased().filter { $0.isHexDigit || $0 == "-" }
        guard !safeAccount.isEmpty else {
            throw CocoaError(.fileWriteInvalidFileName)
        }
        return directory.appendingPathComponent("auth-\(safeAccount).key", isDirectory: false)
    }

    private static func protect(url: URL, permissions: Int) throws {
        try FileManager.default.setAttributes(
            [.posixPermissions: permissions],
            ofItemAtPath: url.path
        )
    }
}
