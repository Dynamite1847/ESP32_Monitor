# AI 控制中心收尾说明

> 完成日期：2026-08-21  
> 适用设备：ESP32-S3-Touch-LCD-4.3B，横屏 800×480

## 1. 本轮完成范围

AI 控制中心已经形成四层清晰路径：

```text
底部程序坞 AI
├── Codex
│   ├── 周额度与最近任务
│   └── 快捷控制
│       └── 导航与旋钮
└── Claude Code
    └── 本机会话状态与 Warp 入口
```

Codex 页面现在提供：

- 周额度**剩余比例**，与 Codex 桌面端语义一致。
- 官方返回的准确刷新日期、时刻与倒计时。
- 六个最近任务的真实标题与逐项状态：运行中、等待权限、等待输入、已完成、失败、空闲。
- 点击任务槽位切换任务；原生控制暂不可用时，自动回退到私人助手打开对应任务。
- 快速模式、长按批准、长按拒绝、续开新对话、按住说话和发送。
- 四向导航、旋钮正转、反转和按下，可在 Codex 中重映射到计划模式、历史、侧栏、滚动、推理强度、自定义命令或技能。

Claude Code 页面现在展示**真实的最近会话卡片**（2×2 共 4 张）：每张卡显示**会话名**（会话自定义标题，缺省回退到项目名）与运行/空闲状态，按最近活跃排序；轻触卡片由助手 `claude --resume <该会话>` 直接续接对话。运行/空闲由 **Claude Code hook 事件**精确判定（发消息即运行、答完即空闲），不再用时间窗口猜。当前使用中转服务，因此仍不采集、不展示额度，也不把终端内容发送到设备。

## 2. 交互与防误触

- AI 总览、Codex、快捷控制、导航与旋钮均有明确的逐级返回路径，底部程序坞始终可直接切页。
- 每个页面只承担一到两个主要任务，六个任务槽位采用两行三列，控制命令采用两行三列。
- 批准和拒绝必须长按；轻触不会触发。
- 语音键发送真实按下、释放事件，手指滑出按钮也会释放。
- 旋钮按键同样发送按下、释放事件；旋转使用独立步进事件。
- 所有命令都会显示成功或未连接反馈。

Codex 桌面端支持为方向键、命令键和旋钮重新分配功能。推荐先保留当前默认映射：上为计划模式、左右为历史切换、下为侧栏、旋钮为滚动或推理强度。需要某个高频技能时，再将一个方向分配给该技能，无需重新烧录固件。

## 3. 蓝牙结构与隐私

设备以 `Codex Micro` 名称广播，并在同一连接中提供两套能力：

| 通道 | 用途 | 数据范围 |
|---|---|---|
| 私人认证通道 | 屏幕解锁、心跳、额度、任务标题、系统状态、媒体、Mac 快捷动作 | 需要链路加密和应用认证 |
| Codex Micro 原生 HID | 六任务切换、任务灯、命令键、方向与旋钮 | 由 Codex 桌面端直接解释 |

固件保持原有私人服务的句柄顺序，并主动发布蓝牙服务变更。此次实机升级已经完成旧绑定迁移，Mac 助手随后以原密钥重新认证成功；没有重新构建或重新签名助手，也没有新增钥匙串访问。

隐私策略保持不变：

- 蓝牙断开、心跳超时、Mac 锁屏或休眠后，设备锁定、熄屏并清除私人状态。
- 任务同步仅包含最多 36 个 UTF-8 字节的标题和状态。
- 提示词、回答、任务预览、文件内容、工作目录和完整项目路径不进入蓝牙协议。
- 屏幕未通过私人助手认证时，触摸控制不可用。
- 周额度通过 Mac 本地 Codex App Server 读取；ESP32 不持有 Codex 账户凭据。

## 4. 实机验证结果

最终固件已经烧录到 `/dev/cu.usbmodem101`，验证结果如下：

- ESP32-S3 启动稳定，无崩溃、栈溢出、看门狗复位或内存不足。
- 私人蓝牙订阅和 Codex 原生订阅同时启用。
- Mac 应用认证成功，日志为 `enrolled=0`，说明沿用已有应用密钥。
- Codex 已向设备发送原生状态数据，设备正常回传 HID 报告。
- Wi-Fi、天气、A 股指数、microSD 和动态中文后备字库正常。
- 固件镜像 2,114,816 字节；6MB 应用分区剩余 66%。
- 主机协议、隐私检查、界面字形覆盖和 ESP-IDF 完整编译通过。

## 5. 使用方式

1. 从底部程序坞进入“AI”，点击 Codex 卡片。
2. 查看周额度剩余比例、刷新时间和最近任务状态。
3. 点击任务槽位切换任务；需要命令时进入“快捷控制”。
4. 批准或拒绝时长按对应按钮；语音输入保持按住。
5. 进入“导航与旋钮”使用方向和旋钮映射。
6. 若要改变映射，在 Codex 桌面端的控制设备设置中选择 `Codex Micro` 后调整。

本轮无需再次输入钥匙串密码。若未来重装 macOS、主动忽略蓝牙设备或清除 ESP32 绑定，系统会要求重新建立蓝牙连接；当前固件升级不需要再做这一步。

## 6. 参考与实现边界

本轮对照了以下资料：

- [Codex Micro 官方说明](https://learn.chatgpt.com/docs/features/codex-micro)
- [codex-micro-waveshare](https://github.com/Douiuiuiz/codex-micro-waveshare)
- [codex-micro-4-core2](https://github.com/imliubo/codex-micro-4-core2)
- [openmicrokbd](https://github.com/conol-ai/openmicrokbd)

官方控制器的六任务键、快速模式、批准、拒绝、续开、语音、发送、方向和旋钮能力均已纳入。会议、通知、自动化页面属于已约定的二期范围，不影响当前 AI 控制中心使用。

## 7. Claude Code 会话卡片（2026-08-21，边界：Claude 侧由此接管）

> 协调约定：AI 页的 **Claude Code 部分**已交接，独立于 Codex 路径实现，避免相互覆盖。

### 7.1 展示与交互
- **数据来源**：macOS 助手枚举全部 `~/.claude/projects/<项目>/*.jsonl`，按最近修改时间排序取**前 4 个会话**，2×2 卡片布局。
- **会话名**：读会话文件里的 `custom-title` > `ai-title`，都没有才回退到项目名；带缓存（`titleCache`），不重复解析整份 jsonl。
- **轻触续接**：卡片动作 `DESK_UI_ACTION_CLAUDE_SESSION_1..4`（42..45）→ 助手执行 `claude --resume <该卡 sessionId>` 打开对话。当前走 **Terminal**（`osascript`）；用户若常用 Warp，可改 `openClaudeSession` 的启动方式。

### 7.2 运行/空闲判定——靠 hook，不靠时间窗口
早期版本用“N 秒内有写入”猜运行/空闲，被否掉（思考时 jsonl 可能不写入，且窗口宽窄都不对）。现改为 **Claude Code hook 事件**精确驱动：

| Hook 事件 | 含义 | 动作 |
|---|---|---|
| `UserPromptSubmit` | 用户发消息、开始思考 | 写 `running` |
| `Stop` | 本轮回答结束 | 写 `idle` |
| `SessionEnd` | 会话结束 | 删除状态文件 |

- **仓库外依赖(环境配置,不入库)**：
  - 脚本 `~/.claude/desk-console-session-hook.sh`：从 hook 的 JSON stdin 解析 `session_id`，把 `{"status","ts"}` 写入 `~/.claude/desk-console/state/<sessionId>`（`end` 则删除）。
  - `~/.claude/settings.json` 的 `hooks` 段注册上述三事件（已用脚本**只增 hooks 键**合并，`env`/密钥/其它键原样保留，备份在 `settings.json.bak-desk`）。
- **助手侧**：`hookSessionRunning(sessionId)` 读状态文件——`running` 且 20 分钟内未过期→运行中；否则空闲；**无状态文件的旧会话**回退到“10 秒内有写入”启发式。
- **重要时效**：hook 在**会话启动时**加载，只对**加载后新开的会话**生效；已在跑的会话（含安装 hook 时的当前会话）需重启一次才开始写状态。
- **卸载**：删掉 `settings.json` 的 `hooks` 键 + 那个脚本即可，不影响其它功能。

### 7.3 有意不做的
- **“关掉 terminal 就从设备消失”的进程存活检测**：评估过（hook 记 claude 进程 pid + 助手查 pid 存活）后**放弃**——Codex 侧也只是列最近会话、不做开/关检测，保持一致、不过度工程化。设备展示的是“最近 4 个会话”，语义与 Codex 对齐。

### 7.4 通道与文件
- **独立通道,不碰 Codex**：新增消息 `DESK_MESSAGE_AI_CLAUDE_TASKS = 0x0122`（与 Codex 的 `AI_TASKS = 0x0121` 同格式，负载 `{"t":[{"n","s","d"}]}`，`d` 为副标题如“刚刚活动”）；固件新增 `desk_ai_state_t.claude_tasks[4]`、`desk_ai_task_slot_t.detail[24]` 与 `apply_claude_tasks_payload`，Codex 的 `codex_tasks` / `apply_ai_tasks_payload` 完全未改动。
- **防闪修复**：`apply_ai_state_payload` 整体覆盖 `app_state.ai` 时，显式 `memcpy` 保留 `claude_tasks`/`codex_tasks` 与两个 `available_task_slots`，避免每 2 秒同步把对方数据清零导致闪烁。
- **隐私**：只发送会话名（≤36 UTF-8 字节）与运行/空闲；**绝不发送完整路径、提示词、回复或会话内容**。额度不采集（中转服务下官方额度不可得，与 Codex 结论一致）。
- **改动文件**：`AIStatusMonitor.swift`、`main.swift`、`DeskProtocol.swift`、`ble_protocol.h`、`app_model.h`、`ui.h`、`ui.c`、`app_main.c`；仓库外 `~/.claude/desk-console-session-hook.sh`、`~/.claude/settings.json`。
- **生效**：固件已烧录、助手已重启（采集端生效）。
