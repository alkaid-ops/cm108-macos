# BlackHole → RH-899X 桥接

## 1. 现有 USB 接口审计

依据 `/Users/alkaidhe/Documents/mac/cm108/usbiso.c` 实测：

| 项目 | 实际实现 |
| --- | --- |
| 设备查找 | `IOServiceMatching("IOUSBHostDevice")` + 比较 `idVendor`/`idProduct` |
| 打开设备 | `USBDeviceOpen`，失败时 `USBDeviceOpenSeize` |
| 音频接口 | `CreateInterfaceIterator` 取 `bInterfaceNumber == 1` |
| 占用接口 | `USBInterfaceOpenSeize` |
| 异步端口 | `CreateInterfaceAsyncPort` + `CreateInterfaceAsyncEventSource` |
| 传输模式 | `SetAlternateInterface(1)`，端点 `0x02` OUT，8 个 `IOUSBIsocFrame` 元素，每元素 192 字节 |
| 采样率 | `DeviceRequestTO` 设置 clock source 1 为 48000 |
| 写数据 | `WriteIsochPipeAsync`，必须在该线程的 `CFRunLoop` 中处理完成回调 |
| 释放 | `SetAlternateInterface(0)` → `USBInterfaceClose` → `USBDeviceClose` |

已验证：8 个等时元素、每元素 192 字节、全部返回 `frStatus = 0`。
注意：实测每个元素是 125 μs 微帧，因此基线在 1 ms 总线时间内发送了 8 ms 音频；
24 B/微帧的修正方案见 `RH899X_HS_ISOCHRONOUS_MICROFRAME_AUDIT.md`。

## 2. 桥接架构

```
macOS 应用
  → BlackHole 2ch（系统输出设备）
  → AudioDeviceIOProc（Float32 48k 立体声）
  → SPSC Ring Buffer（65536 帧，无锁）
  → USB Worker Thread（Float32 → S16LE）
  → IOUSBLib 等时写入 alt 1
  → RH-899X
```

实时线程约束：IOProc 只写环形缓冲，不做 USB IO、文件 IO、日志或动态分配。

## 3. 测试结果

环境：macOS 14.8.7，Apple M1，BlackHole 2ch 0.7.1，RH-899X `0c1c:3a1d`。

M1（仅采集）：

- 设备：`BlackHole 2ch`，UID `BlackHole2ch_UID`，48000 Hz，2 声道，Float32。
- 4 秒采集 196608 帧，无欠载，无 USB 写入。

M3（采集 + USB）：

- 8 秒采集 393216 帧（约 8.19 秒 @48k）。
- USB 传输错误 0。
- 环形缓冲稳定在 384–896 帧（约 8–19 ms）。
- 启动阶段欠载 2816 帧（缓冲尚未填满），之后无持续欠载。

M4（1 分钟连续，含每分钟统计）：

- 采集 2887680 帧，USB 传输错误 0。
- 暖机后欠载 0、溢出 0。
- 环形水位 128–1494 帧（约 2.7–31 ms），不再持续增长。
- 进程 RSS 9.6 MB，CPU 约 3.1%。
- 漂移校正：60 秒内丢弃 1408 帧、重复 6 帧，约合 0.05% 的微调。

未加校正前的 1 分钟对照：水位从 128 帧涨到 4736 帧，说明 BlackHole 与 USB
总线时钟存在约 0.28% 的漂移。若不处理，长时间运行会逐步涨满或欠载。

## 4. 已知限制

- 未实现热插拔自动重连；运行中断开设备需要重启程序。
- 未做动态重采样，输入必须是 48k 双声道。
- 水位校正是整帧丢弃/重复，不是采样率转换；在 0.05% 量级下听感可忽略。
- USB 侧固定 16-bit 小端 PCM；音量/静音由上游应用控制。
- IEC61937 直通未实现。

## 5. 无声排查（诊断版）

新增每秒统计：`cap` / `cap_nonzero` / `cap_peak` / `cap_rms` / `ring_w` /
`ring_r` / `level` / `usb_w` / `usb_err` / `usb_peak` / `usb_rms` /
`under` / `over` / `dup` / `drop` / `rss` / `cpu`。

测试流程：内置扬声器下启动 → 2 秒后切到 BlackHole → 播放 12 秒 440Hz。

| 时段 | cap_peak | cap_rms | usb_peak | usb_rms | usb_err | level |
| --- | --- | --- | --- | --- | --- | --- |
| 未播放（1–4 秒） | 0.0000 | 0.0000 | 0.0000 | 0.0000 | 0 | 640–835 |
| 播放 440Hz（6–12 秒） | 0.3662 | 0.2590 | 0.3662 | 0.2589 | 0 | 383–835 |

结论：CoreAudio 采集、Ring Buffer、USB 提交三段都携带非零 PCM，且数值一致。
切换输出后采集自动恢复，无需重启桥接。

进一步检查设备端 Feature Unit（实体 10）：标准 UAC 请求
`GET/SET_CUR`（Mute、Volume）均返回 `0xe000404f`（pipe stalled），
因此无法通过标准控制请求确认或解除设备静音。

在这一层之上未再发现软件层面的无声原因；剩余变量位于设备与下游解码器
（PC-USB 输入模式、解码器开机状态、扬声器连接）。
