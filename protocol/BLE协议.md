# ESP32-S3 桌面控制台：蓝牙协议 v1

> 状态：草案，等待实机 BLE 联调  
> 更新日期：2026-08-16

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

## 3. 安全与配对

### 3.1 链路层

- 启用 BLE Secure Connections 和绑定。
- 设备已绑定后关闭新 Mac 配对。
- 首次配对需要物理按键进入 60 秒配对窗口。
- 清除绑定需要已认证 Mac 确认，或长按物理按键 10 秒。

### 3.2 应用层

蓝牙链路建立后设备仍保持熄屏，完成以下认证才进入活动状态：

1. Mac 发送 `HELLO`，包含 16 字节客户端随机数。
2. ESP32 返回 `AUTH_CHALLENGE`，包含 16 字节设备随机数和 8 字节会话编号。
3. Mac 返回 `AUTH_RESPONSE`，值为 `HMAC-SHA256(共享密钥, 客户端随机数 || 设备随机数 || 会话编号)`。
4. ESP32 返回 `AUTH_RESULT`。成功后亮屏、启用触摸并进入首页。
5. Mac 每 2 秒发送心跳，连续 6 秒没有收到有效心跳时立即锁定。

共享密钥在首次安全配对时生成：ESP32 保存在 NVS，macOS 保存在钥匙串。开发阶段不开启会熔断 eFuse 的安全启动或 Flash 加密；最终加固前单独评审和备份，避免开发板因不可逆配置无法恢复。

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

### 7.2 AI 状态

```json
{
  "providers": [
    {"id":"codex","primary":72,"secondary":28,"tasks":1,"status":"running","elapsed":1122},
    {"id":"claude","primary":41,"secondary":63,"tasks":0,"status":"idle","elapsed":0}
  ]
}
```

任务状态只允许：`idle`、`running`、`waiting_permission`、`waiting_input`、`completed`、`failed`。

### 7.3 控制动作

```json
{
  "actionId": 120,
  "gesture": "long_press"
}
```

Mac 助手根据当前前台应用和最新控制布局校验 `actionId`。过期、未认证或序号重复的动作直接拒绝。

## 8. 兼容策略

- 包络版本不兼容时拒绝认证，并向 Mac 返回设备支持的版本。
- JSON 解析忽略未识别字段，缺失必填字段时拒绝整条消息。
- 新功能优先新增消息类型，不改变已发布字段的含义。
- 固件和 Mac 助手都保留最近 32 个序号，拒绝会导致重复操作的旧帧。
