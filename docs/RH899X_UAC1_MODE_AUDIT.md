# RH-899X UAC 模式调查

## 结论摘要

| 判断 | 内容 |
| --- | --- |
| 已证实 | 设备枚举为 UAC 2.0（`bcdUSB 0x0200`，AC 头 `bcdADC 0x0200`，接口协议 `0x20`） |
| 已证实 | 提供 AudioControl（接口 0）与 AudioStreaming（接口 1） |
| 已证实 | 流接口 alt 1 = PCM，alt 2 = IEC61937 |
| 已证实 | macOS 的 `AppleUSBAudioDevice` 会绑定，但系统没有生成音频引擎 |
| 有证据未验证 | 输出终端类型 `0x0605` 可能是 Apple 驱动未完成初始化的原因 |
| 推测 | 设备固件可能存在 UAC 1.0 兼容模式 |
| 不支持 | 本阶段未发送任何厂商控制请求，未修改固件 |

## 描述符要点

- 配置：1 个配置，191 字节，2 个接口，总线供电 100mA。
- 接口 0：AudioControl，`bInterfaceProtocol = 0x20`。
- 接口 1：AudioStreaming，alt 0 无端点；alt 1 端点 `0x02` OUT，`maxPacket 200`，`bInterval 1`。
- alt 1 格式：PCM Type I，2 声道，2 字节子帧，16 bit。
- alt 2 格式：`bFormatType = 3`（IEC61937），端点 `0x01` OUT，`bInterval 4`。
- AC 拓扑：Clock Source(1) → Input Terminal(3, `0x0101`) → Feature Unit(10) → Output Terminal(4, `0x0605`)。

完整描述符可通过 `/Users/alkaidhe/Documents/mac/cm108/usbdesc` 重新读取，未做任何写操作。

## 与 macOS 原生驱动的观察

- `AppleUSBAudioDevice` 已匹配接口 0，接口 1 上报 `AppleUSBAudioStreamPropertiesReady = Yes`。
- 系统中 `IOAudioEngine` 只有 1 个（内置扬声器）。
- CoreAudio 设备列表中没有该设备，因此 Aggregate Device 无法包含它。

这些是观测事实；“`0x0605` 导致引擎创建失败”目前只是最合理的解释，尚未通过修改描述符验证。

## 后续验证方法（未执行）

1. 在 Windows 侧抓取驱动初始化阶段的 USB 控制传输。
2. 取得厂商固件工具或文档，确认是否存在 UAC 1.0 模式切换。
3. 仅在厂商明确说明后，才测试模式切换请求。
