# ESP32-S3 桌面控制台：蓝牙协议 v1

> 状态：传输、认证、心跳、系统状态、控制、AI、媒体和联网配置均已实装
> 更新日期：2026-08-18

## 1. 设计目标

- Mac 与设备有关的所有数据和操作都通过低功耗蓝牙传输。
- 断开、心跳超时、Mac 锁屏或休眠时，设备在 1 秒内锁定并熄屏。
- 帧有明确版本、类型、序号、长度和校验，方便排查丢包、重复和兼容问题。
- 系统、AI、媒体和控制条信息只存在内存中，锁定时立即清除。
- 通知标题、正文、AI 提示词、回答和完整项目路径不进入协议。

## 2. GATT 服务

| 用途 | UUID | 方向 | 属性 |
|---|---|---|---|
| 桌面控制台服务 | `7a1d0001-5d1f-4b42-9e8d-3b2c8c1a4f00` | - | Primary Service |
| Mac 写入 | `7a1d0002-5d1f-4b42-9e8d-3b2c8c1a4f00` | Mac → ESP32 | Write / Write Without Response |
| 设备事件 | `7a1d0003-5d1f-4b42-9e8d-3b2c8c1a4f00` | ESP32 → Mac | Notify |
| 设备状态 | `7a1d0004-5d1f-4b42-9e8d-3b2c8c1a4f00` | ESP32 → Mac | Read / Notify |

连接后请求 MTU 247。协议不依赖单包达到 247；MTU 较小时仍可通过分片传输。

当前固件与 Mac 助手已经实现上述服务、特征、加密写入、通知、状态读取和双向分片重组。链路加密成功后仍需通过应用认证，设备才会进入可信活动状态。

## 3. 安全与配对

### 3.1 链路层

- 启用 BLE Secure Connections 和绑定。
- 设备已绑定后关闭新 Mac 配对。
- 联调配置允许首台 Mac 自动配对和登记密钥。
- 办公室配置需要物理按键或触屏确认开启 60 秒配对窗口；此入口仍待实现。
- 清除绑定将要求已认证 Mac 确认，或长按物理按键 10 秒；此入口仍待实现。

### 3.2 应用层

蓝牙链路建立后设备仍保持熄屏，完成以下认证才进入活动状态：

1. Mac 发送 `HELLO`，包含认证负载版本、登记请求标志和 16 字节客户端随机数。
2. ESP32 返回 `AUTH_CHALLENGE`，包含 16 字节设备随机数和 8 字节会话编号。首次登记时还包含一枚 32 字节共享密钥；该消息只允许在加密链路上发送。
3. Mac 返回 `AUTH_RESPONSE`，值为 `HMAC-SHA256(共享密钥, "desk-console-auth-v1" || 客户端随机数 || 设备随机数 || 会话编号)`。
4. ESP32 返回 `AUTH_RESULT`。成功后亮屏、启用触摸并进入首页。
5. Mac 每 2 秒发送心跳，连续 6 秒没有收到有效心跳时立即锁定。

共享密钥在首次安全配对时生成：ESP32 保存在 NVS，macOS 保存在钥匙串。开发阶段不开启会熔断 eFuse 的安全启动或 Flash 加密；最终加固前单独评审和备份，避免开发板因不可逆配置无法恢复。

认证负载采用以下固定格式：

| 消息 | 长度 | 字段 |
|---|---:|---|
| `HELLO` | 18 | 版本 1 字节、标志 1 字节、客户端随机数 16 字节 |
| `AUTH_CHALLENGE` | 26 / 58 | 版本、标志、设备随机数 16 字节、会话编号 8 字节、首次登记密钥可选 32 字节 |
| `AUTH_RESPONSE` | 34 | 版本、保留字节、HMAC-SHA256 32 字节 |
| `AUTH_RESULT` | 3 | 版本、结果码、是否完成首次登记 |

结果码 `1` 表示成功；`0` 表示 HMAC 失败；`2` 表示需要首次登记；`3` 表示登记窗口关闭；`4` 表示请求格式无效；`5` 表示密钥保存失败。

## 4. 逻辑帧

```text
偏移  长度  字段
0     2     魔数：0x44 0x43（DC）
2     1     协议版本：1
3     1     标志
4     2     消息类型，小端
6     2     序号，小端
8     2     负载长度，小端
10    N     负载，最大 512 字节
10+N  2     CRC-16/CCITT-FALSE，小端
```

标志位：

- `0x01`：响应
- `0x02`：错误
- `0x04`：需要确认

认证消息使用固定二进制负载。业务状态首版使用 UTF-8 JSON，便于 ESP32 日志和 Mac 助手调试。负载超过 512 字节时需精简字段，不扩大上限。

## 5. GATT 分片

每个 GATT 数据包在逻辑帧前增加 4 字节分片头：

```text
frame_id       uint16 小端
fragment_index uint8  从 0 开始
fragment_count uint8
data           逻辑帧片段
```

- 同一方向同时只重组一个帧。
- 分片缺失、顺序错误或超过 2 秒未完成时丢弃整帧。
- 高频系统状态可覆盖旧帧，不追赶过期更新。
- 控制动作需要确认，超时后只允许用新序号重试。

## 6. 消息类型

| 编号 | 名称 | 方向 | 说明 |
|---:|---|---|---|
| `0x0001` | `HELLO` | Mac → ESP32 | 开始应用层认证 |
| `0x0002` | `AUTH_CHALLENGE` | ESP32 → Mac | 随机挑战 |
| `0x0003` | `AUTH_RESPONSE` | Mac → ESP32 | HMAC 结果 |
| `0x0004` | `AUTH_RESULT` | ESP32 → Mac | 认证结果 |
| `0x0005` | `HEARTBEAT` | Mac → ESP32 | 2 秒周期心跳 |
| `0x0006` | `LOCK` | Mac → ESP32 | Mac 锁屏、休眠或注销 |
| `0x0010` | `DEVICE_STATUS` | ESP32 → Mac | 版本、内存和连接状态 |
| `0x0100` | `SYSTEM_STATE` | Mac → ESP32 | CPU、内存、磁盘、网络和电量 |
| `0x0110` | `CONTROL_LAYOUT` | Mac → ESP32 | 当前应用与可用动作 |
| `0x0120` | `AI_STATE` | Mac → ESP32 | Codex/Claude Code 用量与任务状态 |
| `0x0130` | `MEDIA_STATE` | Mac → ESP32 | 播放、进度、音量和可选标题 |
| `0x0200` | `ACTION_TRIGGER` | ESP32 → Mac | 点击或长按动作 |
| `0x0201` | `SLIDER_UPDATE` | ESP32 → Mac | 音量或亮度等连续值 |
| `0x0202` | `OPEN_APP` | ESP32 → Mac | 激活 Codex、Claude Code 或媒体应用 |
| `0x0300` | `WIFI_PROVISION` | Mac → ESP32 | 在已认证链路中配置 Wi-Fi |
| `0x0301` | `WIFI_RESULT` | ESP32 → Mac | Wi-Fi 配置结果 |
| `0x0310` | `WEATHER_CONFIG` | Mac → ESP32 | 配置和风天气主机、密钥和坐标 |
| `0x0311` | `WEATHER_CONFIG_RESULT` | ESP32 → Mac | 天气配置结果 |

## 7. 业务负载

### 7.1 系统状态

```json
{
  "cpu10": 184,
  "memory10": 632,
  "diskFreeGB": 412,
  "upKbps": 320,
  "downKbps": 2450,
  "battery": 86
}
```

`cpu10` 和 `memory10` 是百分比乘 10，避免固件必须解析浮点数。

### 7.2 控制页布局

```json
{
  "activeApp": "Safari",
  "actionCount": 6
}
```

Mac 只在前台应用变化时重新发送。`activeApp` 最多 31 个 UTF-8 字节，`actionCount` 范围为 0–6。

### 7.3 AI 状态

```json
{
  "providers": [
    {
      "id":"codex",
      "usageAvailable":true,
      "secondaryAvailable":true,
      "primary":72,
      "secondary":28,
      "primaryWindow":300,
      "secondaryWindow":10080,
      "tasks":1,
      "status":"running",
      "elapsed":1122
    },
    {
      "id":"claude",
      "usageAvailable":false,
      "secondaryAvailable":false,
      "primary":0,
      "secondary":0,
      "primaryWindow":0,
      "secondaryWindow":0,
      "tasks":0,
      "status":"idle",
      "elapsed":0
    }
  ]
}
```

百分比表示当前额度窗口的已用比例，窗口长度单位为分钟。任务状态只允许：`idle`、`running`、`waiting_permission`、`waiting_input`、`completed`、`failed`。Mac 助手只发送任务数量和阶段，不发送任务名称、提示词、回答或项目路径。

### 7.4 媒体状态

```json
{
  "valid": true,
  "playing": false,
  "titleHidden": false,
  "volume": 42,
  "position": 0,
  "duration": 0,
  "title": "Mac 当前媒体"
}
```

首版使用通用标题和系统音量；播放器元数据接入前不读取歌名。`position` 与 `duration` 为秒，无法读取时均为 0。

### 7.5 控制动作

```json
{
  "actionId": 3,
  "gesture": "tap"
}
```

动作编号如下：

| 编号 | 动作 |
|---:|---|
| 1–6 | 后退、前进、刷新、新标签、系统截屏、终端 |
| 7–9 | 上一首、播放/暂停、下一首 |
| 10–12 | 静音、音量降低、音量升高 |

Mac 助手会校验认证状态、手势和动作范围；过期、未认证或序号重复的动作直接拒绝。前四项需要 macOS 辅助功能权限，未授权时不主动弹出系统提示。

### 7.6 Wi-Fi 配置

```json
{"ssid":"Office-2.4G","password":"example-password"}
```

SSID 为 1–32 个 UTF-8 字节。密码允许为空，或为 8–63 个 UTF-8 字节。固件保存配置后开始连接，响应格式为 `{"ok":true,"code":0}`。凭据只允许在链路加密且应用认证成功后传输，日志不记录字段原文。

### 7.7 天气配置

```json
{
  "provider":"qweather",
  "host":"abcxyz.re.qweatherapi.com",
  "apiKey":"example-key",
  "longitude":121.4737,
  "latitude":31.2304
}
```

`host` 使用和风天气控制台分配的专用 API Host，不含协议和路径。经纬度采用十进制度。固件保存后立即请求当前天气、未来 6 小时、今明两天和有效预警，响应格式与 Wi-Fi 配置一致。

## 8. 兼容策略

- 包络版本不兼容时拒绝认证，并向 Mac 返回设备支持的版本。
- JSON 解析忽略未识别字段，缺失必填字段时拒绝整条消息。
- 新功能优先新增消息类型，不改变已发布字段的含义。
- 固件和 Mac 助手都保留最近 32 个序号，拒绝会导致重复操作的旧帧。
