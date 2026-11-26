# BT10 USB Audio 协议审计

对象：`Barrot UPnP Dongle`，VID:PID `0c1c:3a1d`，厂商串 `BT10 / PC_USB Audio`。

证据来源：

- 本机原始描述符读取：`$HOME/barrot-bt10-macos/usbdesc 0x0c1c 0x3a1d`
- IOKit 注册信息：`ioreg -r -n 'Barrot UPnP Dongle' -l`
- 桥接实测统计（1 秒粒度）：`cap_peak`、`cap_rms`、`usb_peak`、`usb_rms`
- 标准 UAC 控制请求探测：`$HOME/barrot-bt10-macos/usbfu`

本阶段未发送任何厂商私有请求，未写 EEPROM，未刷写固件。

## 1. 配置描述符

| 字段 | 值 |
| --- | --- |
| bcdUSB | 0x0200（USB 2.0） |
| bDeviceClass/SubClass/Protocol | 0/0/0 |
| bDeviceSubClass | 0 |
| bNumConfigurations | 1 |
| 配置 wTotalLength | 191 字节 |
| bNumInterfaces | 2 |
| bmAttributes | 0x80（总线供电） |
| bMaxPower | 50 × 2mA = 100mA |
| 实际链路速率 | High Speed（480 Mbps） |

接口关联描述符（IAD）：`bFirstInterface=0`，`bInterfaceCount=2`，
`bFunctionClass=01`（Audio），`bFunctionSubClass=00`，`bFunctionProtocol=0x20`（UAC 2.0）。

## 2. AudioControl 拓扑

```
Clock Source 1   (Crystal / Internal, 属性 0x07，控制 0x07)
        |
        v
Input Terminal 3  (0x0101 USB Streaming, 2ch, FL|FR)
        |
        v
Feature Unit 10   (master: Mute+Volume 0x03；ch1/ch2: 0x0C)
        |
        v
Output Terminal 4 (0x0605, SourceID=10)
```

AC 头：`bcdADC=0x0200`，`bCategory=0x0A`，`wTotalLength=64`，`bmControls=0`。

Clock Source 1：`bmAttributes=0x07`，`bmControls=0x07`。

Feature Unit 10：`bSourceID=3`，`bmaControls[0]=0x00000003`，
`bmaControls[1]=0x0000000C`，`bmaControls[2]=0x0000000C`。

## 3. AudioStreaming 接口

| alt | 端点 | bmAttributes | wMaxPacketSize | bInterval |
| --- | --- | --- | --- | --- |
| 0 | 无 | — | — | — |
| 1 | 0x02 OUT | 0x01（Isochronous，无同步位置位） | 200 字节（`C8 00`，每微帧上限） | 1 |
| 2 | 0x01 OUT | 0x01（Isochronous，无同步位置位） | 200 字节 | 4 |

两个 alt 的接口类均为 `1/2/0x20`（Audio / AudioStreaming / UAC 2.0）。

### alt 1（PCM）

| 字段 | 值 |
| --- | --- |
| bTerminalLink | 3 |
| bFormatType | 1（FORMAT_TYPE_I） |
| bmFormats | 0x00000001（PCM） |
| bNrChannels | 2 |
| bmChannelConfig | 0x00000003（FL + FR） |
| bSubslotSize | 2 字节 |
| bBitResolution | 16 bit |
| 标称采样率 | 无显式列表，由 Clock Source 1 决定 |
| CS Endpoint | 0x01（EP_GENERAL），`bmControls=0x00`，`wLockDelay=2` |

### alt 2（压缩直通）

| 字段 | 值 |
| --- | --- |
| bTerminalLink | 3 |
| bFormatType | 3（IEC61937） |
| bmFormats | 0x00001381 |
| bNrChannels | 2 |
| bmChannelConfig | 0x00000003 |
| bSubslotSize | 2 字节 |
| bBitResolution | 16 bit |
| 端点 | 0x01 OUT，`bInterval=4` |

`bmFormats` 的位含义按 UAC 2.0 表解释，包含 DTS 家族相关位；该解释属于有依据但未实测。
未向 alt 2 提交任何数据。

## 4. 主机侧传输参数（实测）

依据 bridge 阶段的实测统计（bridge 源码未包含在本仓库）：

| 项目 | 实测 |
| --- | --- |
| alt 1 设置 | 成功 |
| 端点 | `0x02` OUT，等时 |
| 每次提交 | 8 个 IOUSBIsocFrame 元素，每元素 192 字节 |
| 元素单位 | 125 μs 微帧（实测，见微帧审计） |
| 提交频率 | 实测 124.9–125.0 次/秒 |
| 每次 Transfer 总线时间 | 1 ms（8 × 125 μs） |
| 每次 Transfer 音频量 | 1536 字节 = 8 ms PCM |
| 字节率 | 125 × 1536 = 192000 B/s（40 s 实测 191869 B/s） |
| 每微帧负载 | **192 字节**，而 UAC2 48k/2ch/16bit 应为 24 字节 |
| USB 错误 | 0 |
| 暖机后欠载/溢出 | 0 / 0 |

“约 128 次/秒 / 196608 B/s”是早期把 125 近似为 128 的估算误差：
`196608 / 192000 = 128 / 125 = 1.024`，不是真实过速。

## 5. 标准 UAC 请求审计

| 请求 | bmRequestType | bRequest | wValue | wIndex | 结果 |
| --- | --- | --- | --- | --- | --- |
| SET Clock Sampling Freq = 48000 | 0x21 | 0x01 | 0x0100 | 1 | 成功（0x0） |
| GET Clock Sampling Freq | 0xA1 | 0x81 | 0x0100 | 1 | STALL（0xe000404f） |
| GET Feature Unit Mute | 0xA1 | 0x81 | 0x0100 | 10 | STALL（0xe000404f） |
| GET Feature Unit Volume | 0xA1 | 0x81 | 0x0200 | 10 | STALL（0xe000404f） |
| SET Feature Unit Mute=0 | 0x21 | 0x01 | 0x0100 | 10 | STALL（0xe000404f） |
| SET Feature Unit Volume=0dB | 0x21 | 0x01 | 0x0200 | 10 | STALL（0xe000404f） |

事实：标准请求被 STALL。含义仅限“设备未响应这些控制请求”，
不能据此判定设备静音，也不能判定播放路径损坏。

## 6. 初始化序列

已检索：本机现有代码、`usbiso`/`usbfu` 探针、公开仓库与驱动资料。

结论：

- 未找到 BT10 该 VID:PID 的公开厂商初始化序列或固件。
- 未发现主机在枚举阶段发出非标准控制请求的证据。
- 唯一成功发出的控制请求是标准 clock SET_CUR 48000。

因此“设备需要特定初始化序列”目前属于未证实假设。

## 7. 待验证假设（按可疑度）

| 假设 | 证据等级 | 说明 |
| --- | --- | --- |
| 基线每微帧发送 192 字节（8 个微帧的音频塞进 1 个微帧），随后空档约 7 ms | **已实测** | `IOUSBIsocFrame` 元素为 125 μs；192 B/元素；Transfer 占 1 ms 总线时间但含 8 ms 音频 |
| 24 B/微帧的 64×24 形状可被主机稳定发送 | **已实测** | 40 s：125 Transfer/s、192000 B/s、0 帧错误、0 丢帧/重复帧 |
| `SetPipePolicy(24)` 可限制每微帧包长 | **已否定** | 本机所有组合均返回 `kIOReturnBadArgument 0xe00002c2` |
| 设备对 alt 1 PCM 的播放路径存在额外使能条件 | 推测 | 无公开资料，无厂商请求证据 |
| 下游解码器未处于 PC-USB 输入或未开机 | 推测 | 属于设备外部状态，主机不可观测 |
| Feature Unit 默认静音 | 推测 | 设备对标准音量/静音请求 STALL，无法确认 |

## 8. 证据边界

已确认：主机提交了非零 PCM，USB 传输完成且无错误。

未确认：设备是否实际消费这些等时包；设备内部是否完成解码；DAC/数字输出是否出声；
下游解码器是否选择了 PC-USB 输入。

等时传输没有逐包 ACK，`usb_err=0` 只代表主机侧调度完成。

## 9. 下一步最小验证实验

1. 已完成：`SetPipePolicy` 不可用；改用 `frReqCount=24` 表达微帧长度。
2. 已完成：64×24（8 ms）与 8×24（1 ms）两种形状均在主机侧通过 20–40 s 测试。
   详见 `docs/BT10_HS_ISOCHRONOUS_MICROFRAME_AUDIT.md`。
3. 待验收：用 `barrot-bridge-mf` 做一次实际听音，确认下游解码器 PC-USB 输入。
4. 若仍无声：在 Windows 侧抓取厂商驱动的 USB 控制传输，作为初始化序列来源。

以上实验均不涉及厂商私有请求、EEPROM 写入或固件刷写。
