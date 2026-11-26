# BT10 192 kHz / 24-bit 能力审计

日期：2026-11-26

对象：`Barrot UPnP Dongle`，VID:PID `0c1c:3a1d`，UAC 2.0。

## 1. 描述符事实

原始配置描述符由本机 `usbdesc 0x0c1c 0x3a1d` 读取，完整配置长度 191 字节，
只有两个 AudioStreaming alternate setting：

| alt | 端点 | 格式 | bSubslotSize | bBitResolution |
| --- | --- | --- | --- | --- |
| 1 | `0x02` OUT | PCM | **2 字节** | **16 bit** |
| 2 | `0x01` OUT | IEC61937 | **2 字节** | **16 bit** |

alt 1 的 Format Type I 描述符为 `06 24 02 01 02 10`，即
`bFormatType=1, bSubslotSize=2, bBitResolution=0x10`。

**结论：设备没有公布任何 24-bit 子槽或 3 字节 PCM 格式。**
主机不能把 24-bit 样本塞进 2 字节子槽；这不是 HAL 层能绕过的限制。

## 2. Clock Source 能力

AudioControl 拓扑中只有 Clock Source 1：

- `bmAttributes=0x07`（内部、可编程、可读写采样频率）
- `bmControls=0x07`
- 描述符没有显式采样率列表

本机实测：

| 请求 | 结果 |
| --- | --- |
| UAC2 `GET CUR` (`0xA1/0x81`, `wValue=0x0101`, `wIndex=0`) | STALL `0xe000404f` |
| UAC2 `GET RANGE` (`0xA1/0x82`, `wValue=0x0101`, `wIndex=0`) | STALL `0xe000404f` |
| UAC1 风格 `GET CUR` (`wValue=0x0100`, `wIndex=1`) | STALL `0xe000404f` |
| `SET CUR` 48000 / 88200 / 96000 / 176400 / 192000 / 384000 | 全部返回 `0x0` |
| `SET CUR` 123456 / 0 / 1 | 也全部返回 `0x0` |

设备对任意值都返回成功，说明 `SET` 返回值不能证明采样率真的被接受。
`GET` 全部 STALL，主机也没有可靠的读回路径。

## 3. HAL 侧改动

`barrot-hal/BarrotAudio.c` 已从固定 48 kHz 改为运行时采样率：

- 当前对外只公布 `48000`；96000/192000 仅保留给 `selftest` 诊断，不在
  CoreAudio / Audio MIDI Setup 中暴露，避免复现固件掉线。
- USB 微帧包长按采样率计算：48k=24 B、96k=48 B、192k=96 B。
- Transfer 仍为 64 个微帧、8 ms；192k 时每个 Transfer 为 64 × 96 =
  6144 B，仍在端点 `wMaxPacketSize=200` 以内。
- Ring Buffer 水位按传输帧数缩放，保持约 21 ms 的时间深度。
- 时钟控制同时发送 UAC2 与 UAC1 两种寻址形式，避免因固件实现差异导致
  采样率未真正切换。
- IEC61937（DTS/AC3）仍固定 48 kHz。

## 4. 192 kHz 首次实测

使用 `coreaudio-device-test 10 192000`：

- CoreAudio 将标称采样率切到 192000 Hz，并成功启动 10 秒。
- 第二次 2 秒 Start/Stop 后，设备在 `06:31:07` 报
  `hardware connection lost`，随后从 USB 总线消失。
- 内核日志没有过流/过功率记录；随后出现 XHCI `setAddress` 超时，macOS
  `disable port`。
- 重新安装 HAL 后 CoreAudio 仍枚举 BT10，但 USB 设备必须物理拔插才会回来。

这次掉线发生在高采样率测试之后，因此当前不能把 192 kHz 视为已验证能力。
可能原因：

1. 固件不接受 192 kHz，收到 4 倍数据率后内部缓冲溢出并复位；
2. 首次测试使用的 UAC2 时钟寻址没有被固件识别，设备仍停在 48 kHz；
3. Start/Stop 时 alt setting 切换触发固件缺陷。

已修正时钟请求为“双形式发送”，但还没有在重新枚举的设备上复测。

### 4.1 96 kHz 复测

设备重新枚举后，依次跑 48k / 96k：

- 48k：5 秒 + 1 秒两次 Start/Stop，设备保持在线。
- 96k：5 秒运行正常，第二次 1 秒 Start/Stop 后设备再次掉线，USB 端口被禁用。

因此不是 192k 单点问题，而是 **高于 48k 的传输都会触发固件复位**。
主机侧统计本身正确：96k 时 503 个 Transfer / 4 秒，约 386,304 帧，
`usb_err=0`、`frame_err=0`、`under=0`，说明 HAL 的 96k 包长计算无误。

## 4.2 结论

这条 USB 输入的可用工作点是 **48 kHz / 2ch / 16-bit**。
Clock Source 描述符虽然标记为可编程，但：

- `GET CUR/RANGE` 全部 STALL；
- `SET CUR` 对任意值都返回成功；
- 实际发送 96k/192k 数据会导致固件停止响应。

这三点组合说明固件没有真正实现可编程采样率。设备宣传的
“192k/24bit”不能作为这条 UAC2 输入路径的能力依据。

## 5. 24-bit 结论

| 方案 | 可行性 | 原因 |
| --- | --- | --- |
| HAL 公布 24-bit PCM | **不可行** | USB alt 1 只有 2 字节子槽、16-bit 分辨率 |
| 把 24-bit 塞进 16-bit 子槽 | **不可行** | 数据宽度不足，设备端无法按 UAC2 解析 |
| 新增 3 字节子槽 alt | **不可行** | 描述符由设备固件提供，主机不能修改 |
| 厂商私有模式 | **未证实** | 没有公开协议，禁止猜测 Vendor Request |

因此当前只能保证 **16-bit PCM**。如果设备宣传“192k/24bit”，该规格更可能
指设备的下游数字/模拟输出，而不是这条 UAC2 输入路径。

## 6. 最终状态

96k/192k 均已在真机上确认会触发固件复位，因此 HAL 最终只对外公布
**48 kHz / 2ch / 16-bit**。

重新构建并安装后做了回归：

- 48k PCM：两次 Start/Stop，`usb_err=0`、`frame_err=0`、`under=0`，
  设备保持在线。
- DTS IEC61937：5 秒，1,252 个 callback、240,384 帧、`underruns=0`，
  设备保持在线。

后续只有在拿到设备固件确实支持 3 字节 PCM 子槽或厂商私有格式切换协议的
证据后，才值得继续研究 24-bit；不再用主机侧伪造格式做实验。

以上测试不发送未知 Vendor Request，不写 EEPROM，不刷写固件。
