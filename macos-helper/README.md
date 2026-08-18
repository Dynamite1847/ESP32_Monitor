# macOS 桌面控制台助手

助手常驻在 macOS 菜单栏，主动扫描并连接 `Desk Console 4.3`。当前包括：

- 按专用服务 UUID 扫描，避免误连其他设备。
- 自动连接并发现写入、事件和状态三条特征。
- 订阅设备事件和状态，读取协议版本、MTU、加密与绑定状态。
- 完成随机挑战和 HMAC-SHA256 认证，共享密钥保存在 macOS 钥匙串。
- 认证成功后每 2 秒发送心跳，保持设备处于可信活动状态。
- 每 2 秒采集 Mac 的 CPU、内存、剩余磁盘、网络速率和电量，同步到系统页。
- 识别当前前台应用，并执行后退、前进、刷新、新标签、截屏和打开终端。
- 发送系统媒体状态，执行上一首、播放/暂停、下一首、静音和音量调节。
- 通过 Codex App Server 只读获取当前额度；通过最近更新的会话文件数量估算运行中任务数，不读取任务正文。
- 检测 Claude Code 进程数量；可选状态行采集器可接入 5 小时与 7 天额度。
- 菜单中提供设备 Wi-Fi 与和风天气接口配置，凭据只在已加密且应用认证成功的蓝牙链路上传输。
- Mac 锁屏、屏幕休眠或整机睡眠时发送锁定命令并断开连接，解锁唤醒后自动恢复。
- 单次连接超过 10 秒会主动取消；失败后按 1、2、4、8 秒退避，之后每 15 秒重新扫描。
- 菜单栏显示当前连接状态，并提供立即重连和退出操作。

## 构建与运行

```bash
cd macos-helper
./scripts/build-app.sh
open .build/桌面控制台助手.app
```

首次启动时 macOS 会询问蓝牙权限，需要选择允许。状态栏出现“控制台 ●”后，ESP32 首页的蓝牙状态会同步为“已连”。固定应用标识下的后续普通重编译会沿用这项授权。

开发构建使用固定应用标识签名，减少重新编译后重复授权。正式发布时将改用 Apple 开发者签名。

## AI 状态来源

Codex 用量通过官方 App Server 的 `account/rateLimits/read` 获取。助手单独启动一个只读连接，不创建任务、不读取任务内容；官方字段说明见 [Codex App Server](https://developers.openai.com/codex/app-server/)。若 Codex 未登录或当前认证方式不提供额度，屏幕会显示用量暂不可读。

Claude Code 的官方状态行输入包含 `rate_limits.five_hour` 和 `rate_limits.seven_day`。项目已提供 [`scripts/capture-claude-status.sh`](scripts/capture-claude-status.sh)，但不会自动修改现有 Claude Code 设置。需要启用时，在 `~/.claude/settings.json` 顶层加入：

```json
{
  "statusLine": {
    "type": "command",
    "command": "<仓库绝对路径>/macos-helper/scripts/capture-claude-status.sh",
    "refreshInterval": 5
  }
}
```

若配置文件已有其他内容，只合并 `statusLine` 字段，不要覆盖整个文件。使用自定义接口时，服务端可能不返回订阅额度；这时任务数量仍可显示，用量保持“接口待接入”。字段来源见 [Claude Code 状态行文档](https://code.claude.com/docs/en/statusline)。

## Wi-Fi 与天气配置

设备完成蓝牙认证后，菜单会开放：

- “配置设备 Wi-Fi…”：支持 2.4GHz 个人网络和开放网络。
- “配置天气数据…”：填写和风天气专用 API Host、API Key、经度和纬度。

助手不把这些凭据写入自身日志。ESP32 将 Wi-Fi 和天气配置保存在 NVS，公共天气、预警与 A 股指数随后由设备通过 HTTPS 直接获取。

## 联调时清理旧绑定

若 macOS 已保存过开发板的低功耗蓝牙绑定，而开发板端的绑定密钥发生变化，系统日志可能出现连接阻止或“设备已不再与本机配对”。处理方式：

1. 退出菜单栏中的桌面控制台助手。
2. 打开“系统设置 → 蓝牙”，找到 `Desk Console 4.3`。
3. 点击设备右侧的详情按钮，选择“忽略此设备”。
4. 重新启动助手，等待系统重新建立加密绑定。

这项操作只清理目标设备的旧绑定，不会撤销助手的蓝牙权限。开发联调固件还会在收到重复配对请求时替换开发板端的旧记录；办公室版本将改为需要设备端人工确认的恢复流程。
