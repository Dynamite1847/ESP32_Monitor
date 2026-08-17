# ESP32-S3 桌面控制台固件

当前为第一次硬件联调前的软件基线，包含：

- 4.3 和 4.3B 双开发板编译配置
- 800×480 RGB 屏、GT911 触摸和 CH422G 背光控制
- LVGL 9 横屏界面骨架
- 六个日常页面、页面库、底部程序坞和左右滑动
- 内置 Source Han Sans SC 16px 中文字体
- 页面统一数据模型和用于界面验收的模拟数据
- 蓝牙逻辑帧、CRC-16 校验和消息编号
- 启动、认证、心跳、断开与 Mac 锁屏隐私状态机
- 无需开发板即可执行的主机侧自动测试

编译：

```bash
source '/Users/dongyu/.espressif/tools/activate_idf_v5.5.5.sh'
cd '/Users/dongyu/ClaudeCode/ESP32/firmware'
idf.py set-target esp32s3
idf.py build
```

一键执行主机测试和完整编译：

```bash
cd '/Users/dongyu/ClaudeCode/ESP32/firmware'
./scripts/verify-preflight.sh
```

此脚本只检查本机代码和构建结果，不连接、不擦除、不烧录开发板。

当前固件镜像约 1.14MB，最小应用分区为 5MB，剩余 78%。完整中文字体是只读数据占用的主要增量，当前容量仍然充足。

默认配置为当前已在手的 `ESP32-S3-Touch-LCD-4.3`。4.3B 到货后再在 `menuconfig` 中切换开发板并做实机验证。

`DESK_BRINGUP_DISPLAY_ON` 在首次屏幕联调期间默认开启，因此没有蓝牙连接时也会亮屏。蓝牙 GATT 服务接入隐私状态机后，办公室版本必须关闭该选项。

协议细节见 [`protocol/BLE协议.md`](../protocol/BLE协议.md)，接板顺序见 [`docs/接板联调清单.md`](../docs/接板联调清单.md)。
