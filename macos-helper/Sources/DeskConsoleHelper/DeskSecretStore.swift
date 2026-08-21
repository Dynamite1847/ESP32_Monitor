import CryptoKit
import Foundation

/// 助手私有字符串存储。所有文件都位于当前用户的 Application Support，
/// 目录权限 0700、文件权限 0600，运行期间不访问 macOS 钥匙串。
enum DeskSecretStore {
    private static let directoryName = "com.dongyu.desk-console-helper/secrets"

    static func set(_ value: String, for account: String) {
        guard !value.isEmpty, let data = value.data(using: .utf8) else {
            delete(account)
            return
        }
        do {
            let url = try secretURL(account: account, createDirectory: true)
            try data.write(to: url, options: [.atomic])
            try FileManager.default.setAttributes(
                [.posixPermissions: 0o600],
                ofItemAtPath: url.path
            )
        } catch {
            NSLog("DeskConsoleHelper: local secret set failed for %@ (%@)", account, error.localizedDescription)
        }
    }

    static func get(_ account: String) -> String? {
        do {
            let url = try secretURL(account: account, createDirectory: false)
            guard FileManager.default.fileExists(atPath: url.path) else {
                return nil
            }
            try FileManager.default.setAttributes(
                [.posixPermissions: 0o600],
                ofItemAtPath: url.path
            )
            return String(data: try Data(contentsOf: url), encoding: .utf8)
        } catch let error as CocoaError where error.code == .fileNoSuchFile {
            return nil
        } catch {
            NSLog("DeskConsoleHelper: local secret read failed for %@ (%@)", account, error.localizedDescription)
            return nil
        }
    }

    static func delete(_ account: String) {
        do {
            let url = try secretURL(account: account, createDirectory: false)
            if FileManager.default.fileExists(atPath: url.path) {
                try FileManager.default.removeItem(at: url)
            }
        } catch let error as CocoaError where error.code == .fileNoSuchFile {
            return
        } catch {
            NSLog("DeskConsoleHelper: local secret delete failed for %@ (%@)", account, error.localizedDescription)
        }
    }

    private static func secretURL(account: String, createDirectory: Bool) throws -> URL {
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
            try FileManager.default.setAttributes(
                [.posixPermissions: 0o700],
                ofItemAtPath: directory.path
            )
        }
        let digest = SHA256.hash(data: Data(account.utf8))
        let fileName = digest.map { String(format: "%02x", $0) }.joined() + ".secret"
        return directory.appendingPathComponent(fileName, isDirectory: false)
    }
}
