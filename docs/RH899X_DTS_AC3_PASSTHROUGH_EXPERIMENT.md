# RH-899X DTS / AC3 Bitstream 实验记录

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

## AC3 实测

- 用 ffmpeg 将 `51音频6CH声道DTS测试.dts`（48 kHz、5.1）解码，
  重编码为 48 kHz / 448 kbps AC3。
- `coreaudio-bitstream-test` 播放 10 秒：2,504 个 callback、480,768 帧，
  `underruns=0`，`usb_err`/`frame_err` 为 0，设备保持在线。
- 测试器同时修复了 Ring Buffer 生产端索引少乘声道数的问题；此前的预填
  数据正确，但主循环写入会错位，可能导致设备端听到损坏的位流。
- 测试器现在自动识别 DTS/AC3，按实际帧长生成 IEC61937：
  DTS 512/1024/2048 samples，AC3 48 kHz 6144-byte packet。

## 2026-11-26 复盘：真正导致“选编码输出没声音”的两个原因

### 原因一：错误的 128 ms 输出延迟声明（已回滚）

实验版本曾把 `kAudioDevicePropertyLatency` 报成 6144 帧（128 ms），
并在 `GetZeroTimeStamp` 的 host time 上叠加同样的 128 ms。结果 CoreAudio
在比特流模式下**完全不再调用 `DoIOOperation`**（`do_calls=0`），
USB 侧只剩空转（`under≈297600 frames / 6 s`），必然没有声音。
该改动已整体回滚，`kAudioDevicePropertyLatency` 与 `kAudioDevicePropertySafetyOffset`
恢复为 0。

### 原因二：cac3 的 `mBytesPerFrame` 必须为 4（正式修复）

原因是 `cac3` 的 ASBD 里 `mBytesPerFrame = 0`。CoreAudio 会因此向客户端
报告 `bytes/frame = 0`，而 VLC 的 SPDIF 路径用 `AOUT_SPDIF_SIZE(6144) / 1536`
推导 4 字节/帧。两者不一致时，VLC 每次 IO 回调都拿到 2 倍于真实位流长度的
缓冲区，drift 被推到阈值边缘，并稳定触发
`playback way too early (-128000)` → 每次插入 6144 个静音样本（128 ms），
形成死循环，输出几乎全是静音。

修复只有一处：

```c
asbd->mBytesPerPacket = 6144;
asbd->mFramesPerPacket = 1536;
asbd->mBytesPerFrame = 4;      /* 原来为 0 */
```

修复后 `physical/virtual format` 均报告 `bytes/frame=4`，VLC 的
`way too early` 与 `inserting zeroes` 归零。

### 修复后的实测

| 场景 | 结果 |
| --- | --- |
| PCM（`coreaudio-device-test` 3 s / 4 s） | `mode=0`，`under=0 over=0 drops=0`，`ring≈1152` |
| AC3 5.1 直通（`coreaudio-bitstream-test`） | `mode=2`，`under=0 over=0 drops=0` |
| AC3 5.1 直通（VLC `RH-899X (Encoded Output)` 30 s） | `tx≈3753`（≈125/s），`frames=1,441,152`，`usb_err=0`，`frame_err=0`，`under=0`，`over=0`，`drops=2435`，`dups=131`，`ring≈1152` |
| VLC 日志 | `way too early/late = 0`，`inserting zeroes = 0` |

USB 形状保持不变：alt 2、端点 `0x01`、`bInterval=4`、每次 Transfer
`8 × 192 = 1536` 字节、约 125 次/秒，等效 192 kB/s 的 16-bit IEC61937 位流。

## 下一步

- 将 DTS 测试改为可长期运行的播放器，并验证 Stop/Start、设备重连和错误恢复。
- 长时间（≥10 分钟）直通稳定性测试，记录 `drops/dups` 是否随时间增长。
- 下游解码器需切换到 PC-USB 输入；默认输出仍保持内置扬声器，不自动切换 RH-899X。

## 代码与测试文件

- `barrot-hal/usbalt2probe.c`：alt 2 frame-list 形状探测。
- `barrot-hal/coreaudio_bitstream_test.c`：DTS/AC3 → IEC61937 → CoreAudio 实验。
- `barrot-hal/BarrotAudio.c`：alt 2、非混音 `cac3` 格式和 8×192 提交。
- `barrot-hal/results/usbalt2_shape_probe.log`
- `barrot-hal/results/dts_bitstream_10s_final.log`
- `barrot-hal/results/pcm_regression_final.log`
