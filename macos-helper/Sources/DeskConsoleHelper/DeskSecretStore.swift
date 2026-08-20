import Foundation
import Security

/// 通用钥匙串字符串存储：用于 Wi-Fi 密码、天气 API Key 等敏感项，避免明文写入
/// UserDefaults（会进 Time Machine 备份、同用户任意进程可读）。非敏感项（SSID、
/// 天气 Host / 经纬度）仍可放 UserDefaults 用于界面预填。
///
/// 与认证共享密钥一样使用 `...ThisDeviceOnly`（不同步 iCloud）。钥匙串项绑定构建的
/// 代码签名：用稳定签名身份（build-app.sh 里的自签证书）后可跨重建复用。
enum DeskSecretStore {
    private static let service = "com.dongyu.desk-console-helper.secrets"

    static func set(_ value: String, for account: String) {
        delete(account)
        guard !value.isEmpty, let data = value.data(using: .utf8) else {
            return
        }
        let item: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
            kSecAttrAccessible as String: kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly,
            kSecValueData as String: data,
        ]
        let status = SecItemAdd(item as CFDictionary, nil)
        if status != errSecSuccess {
            NSLog("DeskConsoleHelper: keychain set failed for %@ (%d)", account, status)
        }
    }

    static func get(_ account: String) -> String? {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne,
        ]
        var item: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &item) == errSecSuccess,
              let data = item as? Data,
              let value = String(data: data, encoding: .utf8) else {
            return nil
        }
        return value
    }

    static func delete(_ account: String) {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
        ]
        SecItemDelete(query as CFDictionary)
    }
}
