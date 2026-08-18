import CryptoKit
import Foundation
import Security

enum DeskAuthenticationError: LocalizedError {
    case invalidChallenge
    case missingSharedKey
    case invalidResult
    case rejected(UInt8)
    case randomGenerationFailed(OSStatus)
    case keychain(OSStatus)

    var errorDescription: String? {
        switch self {
        case .invalidChallenge:
            return "设备认证挑战无效"
        case .missingSharedKey:
            return "Mac 钥匙串中没有设备密钥"
        case .invalidResult:
            return "设备认证结果无效"
        case let .rejected(code):
            return "设备拒绝认证，错误码 \(code)"
        case let .randomGenerationFailed(status):
            return "生成认证随机数失败：\(status)"
        case let .keychain(status):
            return "访问 macOS 钥匙串失败：\(status)"
        }
    }
}

final class DeskAuthenticator {
    enum State {
        case idle
        case helloSent(clientNonce: Data, account: String)
        case responseSent(account: String, enrollmentKey: Data?)
        case authenticated
    }

    private static let payloadVersion: UInt8 = 1
    private static let enrollmentFlag: UInt8 = 1
    private static let hmacDomain = Data("desk-console-auth-v1".utf8)
    private var state: State = .idle

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
        let existingKey = try DeskKeychain.load(account: account)
        let clientNonce = try Self.randomData(count: 16)

        var payload = Data([Self.payloadVersion, existingKey == nil ? Self.enrollmentFlag : 0])
        payload.append(clientNonce)
        state = .helloSent(clientNonce: clientNonce, account: account)
        return payload
    }

    func handleChallenge(_ payload: Data) throws -> Data {
        guard case let .helloSent(clientNonce, account) = state,
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
        let storedKey = try DeskKeychain.load(account: account)
        guard let key = enrollmentKey ?? storedKey else {
            throw DeskAuthenticationError.missingSharedKey
        }

        if let enrollmentKey {
            try DeskKeychain.save(enrollmentKey, account: account)
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
            try? DeskKeychain.delete(account: account)
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

private enum DeskKeychain {
    private static let service = "com.dongyu.desk-console-helper.auth"

    static func load(account: String) throws -> Data? {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne,
        ]
        var item: CFTypeRef?
        let status = SecItemCopyMatching(query as CFDictionary, &item)
        if status == errSecItemNotFound {
            return nil
        }
        guard status == errSecSuccess, let data = item as? Data, data.count == 32 else {
            throw DeskAuthenticationError.keychain(status)
        }
        return data
    }

    static func save(_ key: Data, account: String) throws {
        guard key.count == 32 else {
            throw DeskAuthenticationError.keychain(errSecParam)
        }
        try? delete(account: account)
        let item: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
            kSecAttrAccessible as String: kSecAttrAccessibleAfterFirstUnlock,
            kSecValueData as String: key,
        ]
        let status = SecItemAdd(item as CFDictionary, nil)
        guard status == errSecSuccess else {
            throw DeskAuthenticationError.keychain(status)
        }
    }

    static func delete(account: String) throws {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
        ]
        let status = SecItemDelete(query as CFDictionary)
        guard status == errSecSuccess || status == errSecItemNotFound else {
            throw DeskAuthenticationError.keychain(status)
        }
    }
}
