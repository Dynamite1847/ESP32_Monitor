# AI 页功能重规划：参考资料

> 记录 AI 控制中心的参考来源、已实施范围与后续方向。

## 参考仓库

- **[codex-micro-waveshare](https://github.com/Douiuiuiz/codex-micro-waveshare)** —— 微雪（Waveshare）硬件上的 codex-micro（与本项目 ESP32-S3 微雪板同族，硬件最接近）。
- **[codex-micro-4-core2](https://github.com/imliubo/codex-micro-4-core2)** —— M5Stack Core2 上的 codex-micro。
- **[openmicrokbd](https://github.com/conol-ai/openmicrokbd)** —— Codex Micro 宏键盘的开源复刻，包含设备固件、桌面伴侣应用、按键配置、设备状态和编程代理活动反馈，可在后续讨论 AI 页与快捷控制的关系时参考。

## 2026-08-21 已实施方案

- AI 总览页用两张大卡片展示 Codex 和 Claude Code，点击进入各自二级页。
- Codex 页显示周额度剩余比例、准确刷新时刻、六个最近任务标题与状态，以及打开应用按钮。短周期区域已移除。
- 六个任务槽位同时兼容 Codex Micro 原生任务键；单击切换任务，双击由 Codex 桌面端解释为聚焦任务。
- “快捷控制”二级页提供快速模式、批准、拒绝、续开新对话、按住说话和发送。批准、拒绝必须长按。
- “导航与旋钮”三级页提供四向键、旋钮正反转和按下/释放，可在 Codex 的控制设备设置中重映射到计划模式、历史、侧栏、滚动、推理强度或自定义技能。
- Claude Code 页只显示本机会话数和六个通用会话入口，不采集额度。当前入口聚焦 Warp。
- 数据来源：macOS 助手采集后经 BLE 推送。
  - Codex：`account/rateLimits/read` 读额度与 `resetsAt`，`thread/list` 读最近任务标识、标题和运行状态。
  - Claude Code：通过本地进程统计会话数，过滤 `bg-*` 后台辅助进程。
- 相关代码：固件 `app_main.c` 的 `apply_ai_state_payload`、`ui.c` 的 AI 页；助手 `AIStatusMonitor.swift`。
- 隐私约束：断连时整页 AI 状态清空；只发送任务标题和状态，不发送提示词、回复、预览、工作目录或项目路径。

## 控制能力取舍

官方 Codex Micro 与两个兼容固件已经验证了六任务切换、快速模式、批准、拒绝、续开新任务、语音输入和发送；方向与旋钮还可分配计划模式、前进后退、侧栏、推理强度、对话滚动、技能及其他桌面命令。参考：[Codex Micro 官方说明](https://learn.chatgpt.com/docs/features/codex-micro)、[Core2 控制表](https://github.com/imliubo/codex-micro-4-core2#controls)、[Waveshare 控制表](https://github.com/Douiuiuiz/codex-micro-waveshare#controls)。

本项目按误触风险分两组实现：

- 任务标题、额度和隐私数据继续通过有应用认证的自有低功耗蓝牙服务传输。
- 任务状态灯、任务切换、命令、方向和旋钮使用 Codex Micro 原生 HID 协议，由 Codex 桌面端直接解释。
- 批准与拒绝采用长按，语音和旋钮按键发送真实的按下、释放事件，避免触摸取消后卡在按住状态。
- 设备使用 `Codex Micro` 名称及兼容的设备信息；固件主动发布 GATT 服务变更，并保持原有私人服务句柄顺序，现有助手可以平滑升级。
- 屏幕只有在私人助手完成应用认证后才解锁。蓝牙断开、心跳超时或 Mac 锁定时立即熄屏并清除任务标题、媒体和系统状态。

## 后续方向

- 当前使用 `thread/list` 的运行状态；独立 App Server 连接报告 `notLoaded` 时，用对应会话文件最近 20 秒的写入状态补足“运行中”。等待权限可由 `activeFlags` 识别，完成未读仍需后续接入桌面端事件。
- 接入 Claude Code 生命周期事件，给每个 Warp 会话分配稳定槽位，实现精确聚焦。
- Claude Code 继续保持状态与 Warp 入口；若将来接入生命周期钩子，再增加稳定会话槽位与终端精确聚焦。
- Codex 桌面端的方向键、旋钮和技能映射属于用户配置，可随使用习惯调整，无需重刷固件。
- 任务正文、提示词、回复和完整路径继续不进入设备。
