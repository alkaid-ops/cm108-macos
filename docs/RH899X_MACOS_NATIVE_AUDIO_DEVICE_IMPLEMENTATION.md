# RH-899X macOS 原生 CoreAudio 输出设备：实现

目标架构：

```
Control Center / CoreAudio
        |
        v
AudioServerPlugIn (RH899X.driver)
        |
        |  DoIOOperation(WriteMix, Float32 48k stereo)
        v
lock-free SPSC ring (65536 frames)
        |
        v
USB Worker thread
        |
        |  64 x 24-byte IOUSBIsocFrame, 8 ms transfer, 2-3 in flight
        v
RH-899X USB HS isochronous OUT endpoint 0x02
```

不依赖 BlackHole，不依赖 Bridge 进程，不依赖 IEC61937。

## 1. 新增文件

| 文件 | 作用 |
| --- | --- |
| `barrot-hal/BarrotAudio.c` | 完整 AudioServerPlugIn 驱动 |
| `barrot-hal/RH899X.plist` | CFPlugIn bundle Info.plist |
| `barrot-hal/build.sh` | 构建 bundle（可选 Apple 证书签名） |
| `barrot-hal/install.sh` | 安装到系统 HAL 目录，拒绝 ad-hoc 签名，可回滚 |
| `barrot-hal/selftest.c` | 不经过 coreaudiod 的对象模型/IO/USB 自测 |
| `barrot-hal/test.sh` | 构建 + selftest |
| `barrot-hal/README.md` | 使用说明 |

旧 `BarrotUSB.c` / `BarrotUSB.driver` 保留未删除，作为历史记录。

## 2. HAL 对象模型

| 对象 | ID | 关键属性 |
| --- | --- | --- |
| PlugIn | `kAudioObjectPlugInObject` | `DeviceList={0x100}`, `TranslateUIDToDevice`, `ResourceBundle` |
| Device | `0x100` | UID `RH899X-0c1c-3a1d`, 名称 `RH-899X`, USB transport, 48 kHz, 2ch, volume/mute |
| Stream | `0x101` | output, speaker terminal, Float32 packed 48k 2ch |

设备属性包括：

- `DeviceUID`、`ModelUID`
- `TransportType = kAudioDeviceTransportTypeUSB`
- `NominalSampleRate = 48000`，可用范围仅 48000
- `BufferFrameSize` 默认 512，范围 128–4096
- `ZeroTimeStampPeriod = BufferFrameSize`
- `StreamConfiguration = 1 buffer / 2 channels`
- `VolumeScalar`（**Float32**，符合 HAL 契约）
- `Mute`（UInt32）
- `DeviceIsRunning` / `DeviceIsRunningSomewhere`
- `PreferredChannelsForStereo = {1, 2}`
- 自定义 `'brst'` 统计属性，供 selftest 读取

流属性包括：

- `Direction = 0`（输出）
- `TerminalType = kAudioStreamTerminalTypeSpeaker`
- `VirtualFormat = PhysicalFormat = Float32 packed 48k stereo`
- `AvailableVirtualFormats` / `AvailablePhysicalFormats` 返回同一格式

## 3. 实时数据路径

`DoIOOperation` 只做一件事：

```c
ringPush(d, ioMainBuffer, inIOBufferFrameSize);
```

实时回调内没有：

- 锁
- malloc/free
- 文件/日志
- USB IO
- CoreAudio 属性查询

Ring 是单生产者/单消费者：

- producer = HAL IO 线程（`DoIOOperation`）
- consumer = USB Worker
- `head`/`tail` 为 64 位原子计数
- 容量 65536 帧（约 1.36 秒）
- 满时丢弃新数据并记 `overruns`
- 空时由 USB Worker 补零并记 `underruns`

USB Worker 做：

- 每 8 ms 取 384 帧（6 帧 × 64 微帧）
- Float32 → S16LE，应用 volume/mute
- 64 个 `IOUSBIsocFrame`，每个 `frReqCount=24`
- 在途最多 2 个 Transfer，4 个 slot 轮转
- `GetBusFrameNumber` 节奏，`nextStart += 8`，`kMaxAhead=8`
- ring 水位 768–1536 帧时做单帧 drop/duplicate 校正
- submit 失败先原地重试 3 次，仍失败才关闭并重连 interface

没有改成 8×192 或其它形状；继续使用已验证的 **64×24**。

## 4. 生命周期与热插拔

```
Stopped -> Starting -> Running -> Stopping -> Stopped
```

- `StartIO`：首个 client 时清空 ring、重置统计、启动 USB 线程。
- `StopIO`：最后一个 client 时设置 `running=false`，等待在途 Transfer
  完成（最多 100 ms），再关闭 interface、join 线程。
- `DestroyDevice` / `Release`：调用同一个 `stopWorker`，避免重复 join。
- USB 打开失败或流中断：worker 关闭上下文，100 ms 后重试，直到停止；
  设备重新插入后可自动恢复。

关键修复：停止时先 **drain in-flight frame lists** 再 `free`，否则 USB
控制器会在已释放的 `IOUSBIsocFrame` 数组中写完成状态，导致 malloc 堆损坏。

## 5. 构建与签名

```sh
cd $HOME/cm108-macos/barrot-hal
./build.sh
```

默认 ad-hoc 签名**只能用于 selftest**，不能安装到系统。

系统加载必须使用 Apple 签发证书：

```sh
CODESIGN_IDENTITY="Apple Development: Your Name (TEAMID)" ./build.sh
# 或
CODESIGN_IDENTITY="Developer ID Application: Your Name (TEAMID)" ./build.sh
```

`install.sh` 会检查：

- 是否 `Signature=adhoc`
- 是否存在 `Authority=` Apple 链

不满足时拒绝安装，并打印 `-423` 原因。

安装是可回滚的：已存在的 `RH899X.driver` 先移动到
`RH899X.driver.backup-<timestamp>`。

## 6. 非系统自测

```sh
./test.sh 6
```

`selftest` 用 `dlopen` 在当前用户进程加载 bundle，绕过 `coreaudiod` 的 AMFI
检查，验证：

- factory / QueryInterface / Initialize
- device list、UID、名称、transport
- stream configuration 与 ASBD
- StartIO / DoIOOperation / StopIO
- 64×24 USB 实际传输、帧状态、ring 水位
- 二次 StartIO/StopIO 不崩溃、不泄漏 USB 占用

## 7. 当前限制

1. **系统加载被签名阻塞**。没有 Apple 签发证书时，`amfid` 在代码执行前
   返回 `-423`。详见审计文档。
2. 只支持 48 kHz / 双声道 / Float32 输入，USB 固定 16-bit PCM。
3. 未实现 IEC61937 / DTS 直通。
4. 设备时钟使用 host clock + ring 水位校正；未用 USB 总线时钟做
   `GetZeroTimeStamp` 的硬同步。
5. 热插拔恢复是 worker 重试，不是 CoreAudio 侧的设备移除/重建通知；
   设备在拔掉期间仍显示为“alive”但静音。

