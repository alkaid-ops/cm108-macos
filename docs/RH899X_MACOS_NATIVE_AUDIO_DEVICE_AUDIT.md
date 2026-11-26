# RH-899X macOS 原生 CoreAudio 输出设备：可行性与阻塞审计

环境：macOS **14.8.7**（Build `23J520`），Apple M1，Xcode SDK 15.2。

对象：`RH-899X`，VID:PID `0c1c:3a1d`，UAC 2.0，High Speed。

本审计只读取系统状态、安装/移除本项目自己的测试 bundle，并与 Apple 官方
`AudioServerPlugIn` 契约、当前 SDK 头文件和实际 `amfid` 日志对照。
没有修改 SIP、AMFI、系统安全配置，没有发送厂商私有请求。

---

## 1. 原 HAL 插件失败的根因

### 1.1 第一个缺陷：bundle 不完整

原 `barrot-hal/BarrotUSB.driver/Contents/` 只有 `Info.plist`，没有
`Contents/MacOS/BarrotUSB`。`CFBundleExecutable` 指向的文件不存在，
因此这个 bundle 在结构上就不是可加载的 CFPlugIn。

这解释了为什么早期尝试没有进入任何驱动代码，但它只是次要缺陷。

### 1.2 第二个缺陷：代码签名

把 bundle 重新构建为完整结构：

```
BarrotUSB.driver/
  Contents/Info.plist
  Contents/MacOS/BarrotUSB   (Mach-O arm64 MH_BUNDLE)
```

并用 ad-hoc 方式签名后安装到 `/Library/Audio/Plug-Ins/HAL`，重启
`coreaudiod`。实际日志（`sudo log show --last 3m --info --debug`）：

```
coreaudiod    HALS_RemotePlugInRegistrar.mm:394  Attempting to load: BarrotUSB.driver
helper        Core_Audio_Driver_Service.mm:279   Loading server plug-in com.barrot.usbaudio...
kernel        (AppleMobileFileIntegrity) AMFI: '/Library/.../BarrotUSB' is adhoc signed.
amfid         No certificate chain found
amfid         Failure getting cert chain
amfid         Basic requirement validation failed, error ... Code=-67050
amfid         ... not valid: AppleMobileFileIntegrityError Code=-423
              "The file is adhoc signed or signed by an unknown certificate chain"
launchd       helper exited due to SIGABRT
kernel        Core Audio Driver (BarrotUSB.dri... Corpse allowed 1 of 5
```

关键点：

- 拒绝发生在 **amfid 的基本要求校验**，早于插件工厂和 `Initialize` 执行。
- 失败后 Core Audio Driver Service helper 连续崩溃 6 次，最终
  `Corpse failure, too many`。
- 这不是 bundle 结构问题，也不是插件属性问题；完整 bundle 仍然被拒绝。

### 1.3 自签名证书同样失败

本机存在一个早期创建的本地自签名身份：

```
CN=Barrot Local Code Signing
issuer=CN=Barrot Local Code Signing
```

用它签名并安装后，`amfid` 日志：

```
Config profiles result: { success = 0; }
Failure validating against provisioning profiles
Basic requirement validation failed, error: ... Code=-67050
... Code=-423 "adhoc signed or signed by an unknown certificate chain"
```

因此：**ad-hoc 与本地自签名都不满足 macOS 14.8.7 对 HAL 插件的
Apple 信任链要求。**

### 1.4 不是 library validation 的问题

加载插件的 helper：

```
/System/Library/Frameworks/CoreAudio.framework/Versions/A/XPCServices/
  com.apple.audio.Core-Audio-Driver-Service.helper.xpc
```

其 entitlements 明确包含：

```
com.apple.security.cs.disable-library-validation = true
com.apple.private.audio.driver-host = true
```

所以不是“helper 的 library validation 只允许同 Team ID”这个常见误判。
AMFI 要求的是插件**自身**具有合法的 Apple 签发证书链。

### 1.5 对照：已安装的第三方 HAL 插件

- `BlackHole2ch.driver`：`Authority=Developer ID Application: Existential Audio Inc. (Q5C99V536K)`，正常加载。
- `ToDeskOutputDriver.driver`：`Authority=Developer ID Application: Hainan Youqu Technology Co., Ltd. (KM56KD59W4)`，签名有效。

即：**Developer ID 签名可以加载；ad-hoc/本地自签名不可以。**

### 1.6 本机可用的签名身份

```
security find-identity -v -p codesigning
  "Barrot Local Code Signing"   (self-signed, 不是 Apple 链)
```

Xcode 账户列表当时为空；`~/Library/Developer/Xcode/UserData/Provisioning Profiles`
与 `~/Library/MobileDevice/Provisioning Profiles` 均不存在。

**后续更新（2026-11-25）**：已通过 Xcode 免费 Personal Team 生成并关联
Apple Development 私钥。具体证书标识不公开。

---

## 2. macOS 14.8.7 对第三方 AudioServerPlugIn 的实际要求

依据本机实测（不是概念文章）：

1. 必须是结构完整的 CFPlugIn bundle，`CFBundleExecutable` 指向真实
   Mach-O bundle。
2. 必须用 **Apple 签发**的证书签名（Apple Development 或 Developer ID）；
   仅有本地信任的自签名证书仍然返回 `-423`。
3. 插件由 `Core-Audio-Driver-Service.helper` 加载，加载失败会导致 helper
   `SIGABRT` 并触发 corpse 限制。
4. `coreaudiod` 只加载 `/Library/Audio/Plug-Ins/HAL` 下的系统级 bundle；
   用户级 `~/Library/Audio/Plug-Ins/HAL` 在本机重启后没有产生设备。
5. 设备注册、枚举、IOProc 生命周期都是标准 AudioServerPlugIn 契约，不需要
   DriverKit，也不需要关闭 SIP/AMFI。

---

## 3. 零成本路线分析

| 路线 | 能否产生独立 CoreAudio 输出设备 | 零成本可行性 | 结论 |
| --- | --- | --- | --- |
| AudioServerPlugIn + Developer ID | 是 | **否**（Developer Program 付费） | 已知可行，但违反“不购买 Developer ID” |
| AudioServerPlugIn + Apple Development（免费 Apple ID） | 是 | **可能**，需要用户登录 Apple ID 并创建证书；是否还需 provisioning profile 未实测 | 唯一未验证的零成本候选 |
| AudioServerPlugIn + ad-hoc / 自签名 | 是（架构上） | 否 | 本机实测 AMFI `-423` |
| DriverKit `IOUserAudioDriver` | 是 | 否 | 需要付费 Developer Program 与 DriverKit entitlement |
| CoreAudio Aggregate / Multi-Output Device | 否 | 是 | 只能组合已有设备，不能把 RH-899X 注册成新设备 |
| BlackHole + Bridge | 否 | 是 | 只是转发进程，控制中心看到的是 BlackHole |
| 旧 IOAudioFamily kext | 是（旧系统） | 否 | Apple Silicon 上已淘汰，且需要 kext 签名/SIP 例外 |

### 3.1 AudioServerPlugIn 是正确架构

本项目的 HAL 驱动已经实现完整对象模型、属性契约、IOProc、ZeroTimeStamp、
ring、USB Worker 和 64×24 微帧传输。它**只差可加载的 Apple 证书**。

### 3.2 Aggregate / Multi-Output 不能冒充独立设备

`AudioHardwareCreateAggregateDevice` 只能聚合 CoreAudio 已经识别的设备。
RH-899X 当前不是 CoreAudio 设备，因此无法被聚合。即使把 BlackHole 聚合成一个
名为 “RH-899X” 的设备，底层仍然是 BlackHole + 转发进程，不满足最终验收标准。

### 3.3 DriverKit 不比 HAL 更便宜

`IOUserAudioDriver` 需要 `com.apple.developer.driverkit` entitlement，
该 entitlement 只对付费 Developer Program 成员开放，并且还要经过系统扩展
审批。它不能绕开“没有付费账号”的约束。

---

## 4. 明确结论

1. **原 HAL 失败根因已得到本机日志证据支持**：
   完整 bundle + ad-hoc 签名 → `amfid -423`；
   完整 bundle + 本地自签名 → 同样 `-423`；
   失败发生在代码执行之前。
2. **macOS 14.8.7 没有允许 ad-hoc HAL 插件加载的官方开关**；
   能工作的第三方插件都是 Developer ID 签名。
3. **免费 Apple Development 身份已经验证可用**（Apple 链、Code Signing EKU、
   私钥匹配、`codesign --verify --strict` 和 `spctl` 均通过）。
4. 系统级 HAL 加载仍**未验证**：尚未安装/重启 CoreAudio；helper 是否额外要求
   provisioning profile 仍待实测。
5. 本项目已经完成所有不依赖证书的部分：驱动源码、bundle、构建脚本、
   非系统 selftest、64×24 USB 传输验证。见
   `RH899X_MACOS_NATIVE_AUDIO_DEVICE_IMPLEMENTATION.md` 与
   `RH899X_MACOS_NATIVE_AUDIO_DEVICE_TEST.md`。

