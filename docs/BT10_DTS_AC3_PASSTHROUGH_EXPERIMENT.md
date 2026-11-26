# BT10 DTS / AC3 Bitstream 实验记录

日期：2026-11-25

## 结论

**主机侧完成，待物理听音。** DTS IEC61937 位流已经通过 CoreAudio 非混音
`kAudioFormat60958AC3` 路径进入 HAL，并从 alt 2 端点稳定提交。设备端解码
和 DAC 出声仍无法由主机侧证明。

10 秒 DTS 测试：2,502 个 CoreAudio callback，480,384 帧，`underruns=0`；
驱动端 1,246 个 Transfer、478,464 帧，`usb_err=0`、`frame_err=0`、
`under=0`、`over=0`、`drops=0`、`dups=0`。

原有 PCM 路径同时正常：3 秒 Start/Stop，`usb_err=0`、`frame_err=0`、
`under=0`、`over=0`。

## 已有证据

- 网页资料包含 5.1 DTS 测试文件和“数字输出”播放设置。
- Windows INF：`cm108.ini` 中 `DisableSpdifOut=0`，INF 定义 SPDIF UDA 节点。
- USB 描述符：流接口 alt 2、端点 `0x01` OUT、`bInterval=4`、最大包 200 字节，
  `bFormatType=3`（IEC61937）。
- 测试 DTS：`51音频6CH声道DTS测试.dts`，每帧 2048 字节，512 samples，
  DTS core size 2013 字节。

REA 的 Ghidra 反编译仍未完成；本轮结论来自设备描述符、现有 INF/网页资料
以及 GStreamer/VLC 的公开 CoreAudio passthrough 实现，没有依赖新的驱动反编译。

## USB alt 2 形状探测

使用 `usbalt2probe` 实测 `WriteIsochPipeAsync`：

| 形状 | 结果 |
| --- | --- |
| `8 × 192`，每项 192 字节 | `kIOReturnSuccess`，完成成功 |
| `64 × 192`，每项 192 字节 | 首次成功，但持续提交出现带宽/权限错误 |
| `64 × 192`，每 8 项填充 | `kIOReturnBadArgument` |
| `8 × 192`，每 8 项填充 | `kIOReturnBadArgument` |
| `64 × 24` | `kIOReturnNotPermitted` |

结论：对于 `bInterval=4`，主机应使用 **8 个 frame-list 元素 × 192 字节**，
每次 Transfer 共 1536 字节；这与 48 kHz 双声道 16-bit 的 8 ms 数据量一致。

驱动已按此形状提交，日志实测 4 秒 497 transfers / 190,848 frames，
`usb_err=0`、`frame_err=0`。这只证明主机侧 USB 传输成功，不证明设备解码
或出声。

## 正确的 CoreAudio passthrough 契约

GStreamer 的 `osxaudio` 实现给出了可工作的格式：

```
mFormatID          = kAudioFormat60958AC3
mSampleRate        = 48000
mChannelsPerFrame  = 2
mFormatFlags       = SignedInteger | Packed | NonMixable
mBytesPerFrame     = 0
mBytesPerPacket    = 6144
mFramesPerPacket   = 1536
mBitsPerChannel    = 16
```

同时必须在设置 physical format 前取得 `kAudioDevicePropertyHogMode`，并把
`kAudioDevicePropertySupportsMixing` 设为 false。测试确认 IOProc 每次提供
192 个 packet-frame；客户端按 `kAudioDevicePropertyBufferFrameSize` 填前
768 字节，剩余容量不能重复消费。

## 下一步

- 用同一测试播放 AC3 文件，验证 `kAudioFormat60958AC3` 对 AC3 burst 的支持。
- 将 DTS 测试改为可长期运行的播放器，并验证 Stop/Start、设备重连和错误恢复。
- 完成一次真实听音：下游解码器切到 PC-USB，播放 DTS 5.1 测试文件。
- 物理听音通过前，默认输出仍保持内置扬声器，不自动切换 BT10。

## 代码与测试文件

- `barrot-hal/usbalt2probe.c`：alt 2 frame-list 形状探测。
- `barrot-hal/coreaudio_bitstream_test.c`：DTS → IEC61937 → CoreAudio 实验。
- `barrot-hal/BarrotAudio.c`：alt 2、非混音 `cac3` 格式和 8×192 提交。

测试日志属于本机内部产物，未纳入公开仓库。
