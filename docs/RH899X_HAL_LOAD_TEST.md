# RH-899X HAL 系统加载验证与自动回滚

日期：2026-11-25  
环境：macOS 14.8.7（23J520），Apple M1（arm64）

## 结论

**PARTIAL**

- Apple Development 签名已被当前机器的 AMFI 接受。
- Core Audio Driver Service 成功加载 `com.ranhe.rh899x.audio`。
- `RH-899X` 已出现在系统 CoreAudio 输出设备列表。
- HALOutput 客户端完成两次 Start/Stop，USB 传输无错误。
- 尚未进行物理听音，不能宣布完整播放链路通过。

重要限制：本机安装前即为 `System Integrity Protection status: disabled` 和
`assessments disabled`。本次没有修改 SIP、AMFI 或 Gatekeeper。因此本结果证明
Apple Development 证书在当前机器可用，但不能替代“标准 SIP/Gatekeeper 配置”
下的回归测试。

## 安装前审计

- Bundle：`com.ranhe.rh899x.audio`，可执行文件 `RH899X`，factory UUID
  `B7A1C2D3-4E5F-6789-ABCD-EF0123456789`。
- AudioServerPlugIn type UUID：`443ABAB8-E7B3-491A-B985-BEB9187030DB`。
- Device UID：`RH899X-0c1c-3a1d`；名称：`RH-899X`。
- 签名链：Apple Development → Apple WWDR → Apple Root CA；
  TeamIdentifier 与 Apple 证书一致。
- `codesign --verify --strict --deep` 通过，`spctl -a -vv` 返回 accepted。
- 无 entitlements、无 embedded provisioning profile。
- 三个已安装 HAL bundle 的 CFBundleIdentifier 均唯一。
- `install.sh` 只移动/替换 `RH899X.driver`，并检查内置扬声器仍然存在。

测试期间的备份目录与日志未纳入公开仓库。

## 首次未枚举的根因

11:40 安装的二进制构建于 11:34，而输入作用域的 `Latency`/`SafetyOffset`
修复写入源码时已是 11:49。日志显示 CoreAudio 查询 `saft scope=inpt` 后没有
继续 `CreateDevice`：旧二进制仍对该属性返回 unknown property。

重新构建当前源码后，查询通过并成功调用 `CreateDevice`。这是本次枚举失败的直接原因。

## 系统加载结果

关键日志：

```
Attempting to load: RH899X.driver
amfid: Entering OSX path for .../RH899X.driver/Contents/MacOS/RH899X
Loading server plug-in com.ranhe.rh899x.audio...
Done loading server plug-in com.ranhe.rh899x.audio...
```

没有 `-423`、`-67050` 或 provisioning profile 拒绝。当前测试不需要已嵌入 profile。

安装后设备：

```
54   RH-899X   RH899X-0c1c-3a1d
76   BlackHole 2ch BlackHole2ch_UID
106  MacBook Air扬声器 BuiltInSpeakerDevice
```

`system_profiler SPAudioDataType` 显示 2 输出声道、48 kHz、USB transport。
默认输出仍为 `MacBook Air扬声器`，未被测试修改。

## CoreAudio 功能测试

新增 `coreaudio_device_test.c`，通过 HALOutput 的 CurrentDevice 定向 RH-899X，
播放 440 Hz、48 kHz、双声道 Float32，不改变系统默认输出。

首次测试发现启动阶段提前提交了一个未填充的 Transfer，产生 575 帧启动欠载。
在 USB 提交前增加最多 80 ms、目标 1536 帧的预填充后复测：

| 项目 | 第一次启动 | 第二次启动 |
| --- | ---: | ---: |
| 时长 | 10 s | 2 s |
| Transfers | 1249 | 247 |
| PCM 帧 | 479,424 | 94,560 |
| 实际帧率 | 47,942/s | 47,280/s |
| underrun | 0 | 0 |
| overrun | 0 | 0 |
| USB errors | 0 | 0 |
| frame errors | 0 | 0 |
| drops / dups | 0 / 0 | 0 / 0 |
| 预填充 | 1536 帧 / 32.4 ms | 1536 帧 / 32.6 ms |

驱动 helper 进程在测试中约 12–14% CPU、8.3–8.6 MB RSS。10 秒短测不能替代
长时间稳定性验收；先前 in-process 自测已有 600 秒数据。

## 最终检查

1. Apple Development 被 AMFI 接受：**是**。
2. 需要 provisioning profile：**当前测试未发现需要**；无 profile、无受限 entitlements，加载成功。
3. Driver Service 加载插件：**是**。
4. 系统出现 `RH-899X`：**是**（CoreAudio 与 system_profiler 均确认；未单独截取控制中心 UI）。
5. IOProc：**正常**，两次 Start/Stop 成功。
6. USB 传输：**短测稳定**，0 错误、0 欠载、0 溢出。
7. 物理听音：**仍需用户完成**。
8. 回滚：**未触发**；CoreAudio、内置扬声器、ToDesk 均正常。BlackHole 在后续解除遗留依赖后移除。
9. 当前系统音频状态：**正常**，默认输出未改变。

## BlackHole 清理

`RH899X.driver` 本身不依赖 BlackHole。为避免当前 `install.sh` 的健康检查
误判，先将其对 `BlackHole2ch_UID` 的硬性要求移除，仅保留内置扬声器检查。

随后安全移除 BlackHole：

- 将 `BlackHole2ch.driver` 移至
  `backups/blackhole-uninstall-20261125-211032/`，未直接销毁。
- 执行 `pkgutil --forget audio.existential.BlackHole2ch`。
- 重启 `coreaudiod`，确认设备列表只剩 `RH-899X`、内置麦克风和内置扬声器。
- 删除后再次运行 CoreAudio 测试：两次 Start/Stop 均成功，`usb_err=0`、
  `frame_err=0`、`under=0`、`over=0`、`drops=0`、`dups=0`。

旧 `bridge/barrot-bridge*` 方案仍依赖 BlackHole，现已不可运行；这是删除的预期结果。

## 待用户完成的一次验收

打开控制中心 → 声音 → 输出，选择 `RH-899X`，播放任意音频并确认 RH-899X
实际出声；随后切回 `MacBook Air扬声器`，确认声音恢复。只有该步骤通过后，
才能把状态改为 `PASS`。

## 证据说明

测试日志、备份目录和证书标识属于本机内部产物，未纳入公开仓库。
