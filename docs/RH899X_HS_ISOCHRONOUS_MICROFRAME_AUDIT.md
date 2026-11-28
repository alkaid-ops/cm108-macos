# RH-899X High-Speed 等时传输微帧审计

对象：`RH-899X`，VID:PID `0c1c:3a1d`，UAC 2.0，链路 480 Mbps。

测试环境：macOS 14.8.7，Apple M1，BlackHole 2ch 0.7.1。

本审计只做主机侧只读测量和等时数据写入，没有发送任何厂商私有请求，
没有修改 EEPROM/固件/描述符，没有改动 BlackHole。

新增工具（均与稳定版隔离）：

- `bridge/tests/iso_diag.c`：总线时钟采样、单次 Transfer 形状探测、长流统计。
- `bridge/barrot-bridge-mf.c`：64×24 字节微帧桥接（8 ms/Transfer，流水线提交）。
- `bridge/barrot-bridge-m8.c`：8×24 字节微帧桥接（1 ms/Transfer，流水线提交）。

基线稳定版 `bridge/barrot-bridge.c` / `barrot-bridge` 保留未覆盖。

## 1. USB API 实现审计

实际使用的是 **IOUSBLib 用户态接口**（`IOUSBInterfaceInterface`），
不是 IOUSBHost / DriverKit。

| 项目 | 实现 |
| --- | --- |
| 设备查找 | `IOServiceMatching("IOUSBHostDevice")` + `idVendor`/`idProduct` |
| 打开设备 | `USBDeviceOpen`，失败时 `USBDeviceOpenSeize` |
| 音频接口 | `bInterfaceNumber == 1`，`USBInterfaceOpenSeize` |
| 异步端口 | `CreateInterfaceAsyncPort` + `CreateInterfaceAsyncEventSource` |
| alt 设置 | `SetAlternateInterface(1)` |
| 采样率 | 标准 UAC2 `SET_CUR` clock source 1 = 48000 |
| 数据传输 | `WriteIsochPipeAsync(pipe, buf, frameStart, numFrames, frameList, cb, refcon)` |
| 总线时钟 | `GetBusFrameNumber`（1 ms），`GetBusMicroFrameNumber`（125 μs） |
| 管道策略 | `SetPipePolicy(pipe, maxPacketSize, maxInterval)` |

`IOUSBIsocFrame`：

```c
typedef struct IOUSBIsocFrame {
    IOReturn frStatus;
    UInt16   frReqCount;   /* 本帧请求字节数 */
    UInt16   frActCount;   /* 本帧实际完成字节数 */
} IOUSBIsocFrame;
```

头文件把数组元素称为 "frame"。本机实测表明：对该 High-Speed 等时管道，
**每个数组元素对应一个 125 μs 微帧**，而 `frameStart` 仍以 1 ms 总线帧为单位。

### 1.1 `SetPipePolicy` 的真实语义

SDK 头文件说明：

- 它修改等时/中断管道的**带宽预留**，不是“每个包的长度”。
- 对非 High-Bandwidth 的 High-Speed 端点，`maxPacketSize` 表示
  **每微帧字节数**，合法范围 0 到端点描述符的 `wMaxPacketSize`。
- `0` 表示释放带宽但保留管道。

本机实测：对该端点调用 `SetPipePolicy`，无论 `(24,0)`、`(24,1)`、`(48,1)`、
`(192,1)`、`(200,1)`、`(0,1)`，全部返回 `0xe00002c2`（`kIOReturnBadArgument`），
`GetBandwidthAvailable` 始终保持 `6656 B/microframe`。

结论：当前 IOUSBLib 用户态路径不能用 `SetPipePolicy` 控制本管道带宽，
也不能用它表达 24 字节微帧。微帧长度必须由 `IOUSBIsocFrame.frReqCount` 表达。

### 1.2 端点与带宽

`GetPipeProperties(pipe=1)` 实测：

| 项目 | 值 |
| --- | --- |
| 方向/端点 | OUT / `0x02` |
| 类型 | isochronous |
| `wMaxPacketSize` | **200 字节**（描述符 `C8 00`，不是 512） |
| `bInterval` | 1 |
| 可用带宽（策略前） | 6656 B/microframe |
| 设备速度 | `kUSBDeviceSpeedHigh = 2` |

200 字节是**每个微帧**允许的最大包长。48 kHz/2ch/16bit 只需要 24 B/微帧。

## 2. 当前 Transfer 的实际结构

稳定版 `barrot-bridge.c` / `usbiso.c` 的实际提交：

| 参数 | 值 |
| --- | --- |
| `numFrames` | 8 |
| 每个 `frReqCount` | 192 字节 |
| Transfer 总长度 | 8 × 192 = 1536 字节 |
| 每个元素对应总线时间 | 125 μs |
| Transfer 占用总线时间 | **1 ms** |
| 音频内容 | 1536 字节 = 384 PCM 帧 = **8 ms** 的 48k 音频 |
| 提交节奏 | 约每 8 ms 一次（约 125 Transfer/s） |

因此基线在每 1 ms 总线时间里塞入 8 ms 音频，占空比约 12.5%：
每个微帧发送 192 字节（48 个 PCM 帧），随后约 7 ms 总线空档。

平均字节率仍然正确，但设备端看到的包形状是 `192 B / 125 μs`，
而不是 UAC2 音频所需的 `24 B / 125 μs`。

## 3. High-Speed 微帧数学模型

| 量 | 公式 | 结果 |
| --- | --- | --- |
| PCM 字节率 | 48000 × 2 × 2 | **192000 B/s** |
| 微帧周期 | USB 2.0 HS | **125 μs** |
| 每微帧 PCM 帧 | 48000 / 8000 | **6** |
| 每微帧字节 | 6 × 2 × 2 | **24 B** |
| 每 1 ms 微帧数 | 1000 / 125 | **8** |
| 每 1 ms 字节 | 8 × 24 | **192 B** |
| 每秒字节 | 8000 × 24 | **192000 B/s** |
| 每秒 8 微帧 Transfer 数 | 1000 / 8 | **125** |

## 4. 192 字节与 24 字节的关系

数值上 `192 = 8 × 24`，但两者在当前代码里的含义不同：

- 24 字节 = 一个微帧应有的 PCM 包。
- 192 字节 = 一个微帧**实际发送的** PCM 包（基线）。
- 1536 字节 = 一个 Transfer 总量，也是 8 个微帧按 24 字节发送的总量。

基线的错误不是“总量 1536 不对”，而是把 8 个微帧的量在 1 个微帧里一次发完，
然后在另外 7 个微帧里不发。正确形状是把同样的 1536 字节分摊到 8 个连续微帧。

- 8 个元素 × 24 字节 = 192 字节 = 1 ms 音频，必须以 1 ms 节奏提交。
- 64 个元素 × 24 字节 = 1536 字节 = 8 ms 音频，可以保持 8 ms 节奏。

本审计同时验证了这两种 24 字节形状（见第 8 节）。

## 5. 2.4% 数据率差异的解释

旧记录：`192 × 8 × 128 = 196608 B/s`，比 192000 高 2.4%。

`196608 / 192000 = 1.024 = 128 / 125`。差异完全来自“128 Transfer/s”这个估算值：

- 实测基线 Transfer 节奏是 **124.91–125.0 次/秒**（40 秒统计）。
- 125 × 1536 = **192000 B/s**。
- 40 秒实测为 **191869 B/s**（−0.07%，属于统计窗口与时钟噪声）。

2.4% 不是传输协议过速，而是把 125 近似成 128 造成的算术误差。
“有效采样率 49152”同样是 `48000 × 1.024` 的同一误差。

## 6. 每个等时元素的实际长度与时间统计

### 6.1 总线时钟

2 秒共 1579 个样本：

| 量 | 实测 |
| --- | --- |
| 总线帧周期 | **1.0006 ms** |
| 微帧周期 | **125.0688 μs** |
| 微帧/帧 | **8.0000** |
| 边界不一致样本 | 6 / 1579（两次读取之间的正常跨越） |

### 6.2 元素单位判定

单次 Transfer 在 `frameStart + 10`（约 10 ms 后）启动，
记录完成回调时刻的总线帧/微帧差值：

| 形状 | 元素数 | 每元素请求 | 总量 | 完成帧增量 | 微帧增量 | 实际完成 | 帧错误 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 × 192 | 1 | 192 | 192 | 10 | 80 | 192 | 0 |
| 8 × 24 | 8 | 24 | 192 | 10 | 80 | 192 | 0 |
| 8 × 192 | 8 | 192 | 1536 | 10 | 80 | 1536 | 0 |
| 9 × 24 | 9 | 24 | 216 | 11 | 88 | 216 | 0 |
| 16 × 24 | 16 | 24 | 384 | 11 | 88 | 384 | 0 |
| 32 × 24 | 32 | 24 | 768 | 13 | 104–112 | 768 | 0 |
| 64 × 24 | 64 | 24 | 1536 | 17–18 | 136–144 | 1536 | 0 |
| 16 × 192 | 16 | 192 | 3072 | 11–12 | 88–96 | 3072 | 0 |

完成时刻满足 `完成帧 ≈ start + floor((元素数 − 1) / 8)`，即每个元素前进 125 μs。
64 个 24 字节元素的实际跨度约 **8 ms**，不是 64 ms。

由此确认：`IOUSBIsocFrame` 数组元素 = 125 μs 微帧；`frameStart` = 1 ms 总线帧。

另一个关键推论：8×192 的 8 个元素在 1 ms 内完成。如果每个 192 字节元素被
控制器拆成 8 个微帧发送，跨度应为 8 ms；实测只有 1 ms，且 `frActCount=192`。
因此 192 字节是**单个微帧内的一个包**，刚好低于描述符的 200 字节上限。

### 6.3 长流明细

`iso_diag --mode stream` 对每次 Transfer 写入独立提交/完成时间戳，
并记录每个元素的 `frReqCount / frActCount / frStatus`。

基线 20 秒（2494 次 Transfer）：

| 指标 | 值 |
| --- | --- |
| 元素数/Transfer | 8 |
| 每元素平均实际字节 | 192 |
| 帧错误总数 | 0 |
| 状态码集合 | `{0x0}` |
| 提交间隔 min / p50 / mean / max | 3.909 / 7.700 / 8.022 / 23.143 ms |
| 完成延迟 min / mean / max | 3.893 / 7.465 / 23.108 ms |

## 7. 基线组实验结果（8 × 192 字节）

40 秒 440 Hz 连续测试（系统输出切到 BlackHole，`afplay` 循环播放，
桥接 `--duration 40`）：

| 指标 | 值 |
| --- | --- |
| Transfer 速率 | 124.91 /s |
| 微帧元素数 | 999.3 /s（8 × 124.91） |
| 每元素平均字节 | 192 |
| 每秒实际字节 | **191869 B/s**（−0.07% vs 192000） |
| USB 提交错误 | 0 |
| 帧状态错误 | 0 |
| Ring 水位 min / mean / max | 128 / 592.4 / 1762 帧 |
| 欠载 / 溢出 | 0 / 0 |
| 丢帧 / 重复帧 | 896 / 0 |
| `usb_peak` 均值 | 0.3662 |
| `usb_rms` 均值 | 0.2525 |
| CPU | 约 3.3% |

波形峰值与采集端一致，说明主机侧数据路径没有数据丢失；
丢帧 896（约 0.05%）来自既有的水位校正。

## 8. 微帧组实验结果

### 8.1 64 × 24 字节，8 ms/Transfer（流水线）

同一条件 40 秒：

| 指标 | 值 |
| --- | --- |
| Transfer 速率 | 124.99 /s |
| 微帧元素数 | 7999 /s（64 × 124.99） |
| 每元素平均字节 | 24（日志中全部为 `24/24/0x0`） |
| 每秒实际字节 | **191985 B/s**（−0.01% vs 192000） |
| USB 提交错误 | 0 |
| 帧状态错误 | 0 |
| Ring 水位 min / mean / max | 669 / 1049.4 / 1437 帧 |
| 欠载 / 溢出 | 0 / 0 |
| 丢帧 / 重复帧 | 0 / 0 |
| `usb_peak` 均值 | 0.3662 |
| `usb_rms` 均值 | 0.2525 |
| CPU | 约 5.4% |
| 在途 Transfer | 2–3 |

### 8.2 8 × 24 字节，1 ms/Transfer（流水线）

30 秒测试（8 个元素只覆盖 1 ms，因此必须 1 ms 提交一次）：

| 指标 | 值 |
| --- | --- |
| Transfer 速率 | 1000.0 /s |
| 微帧元素数 | 8000 /s |
| 每元素平均字节 | 24 |
| 每秒实际字节 | **191993 B/s** |
| USB 提交错误 | 0 |
| 帧状态错误 | 0 |
| Ring 水位 min / mean / max | 560 / 803.7 / 1056 帧 |
| 欠载 / 溢出 | 0 / 0 |
| 丢帧 / 重复帧 | 0 / 0 |
| CPU | 约 7.5% |
| 在途 Transfer | 7–8 |

早期使用 12 ms 前瞻时，启动阶段曾出现一次 `kIOReturnNotPermitted 0xe00002ee`；
把前瞻缩到 8 ms（8 个槽）后，30 秒测试为 0 提交错误、0 丢帧。

结论：8 × 24 在 API 上可行，但提交频率高 8 倍、CPU 更高；
64 × 24 保持 8 ms 节奏，是更稳定的生产形状。

## 9. Ring Buffer 漂移对比

| 组 | 水位范围 | 均值 | 漂移/校正 |
| --- | --- | --- | --- |
| 基线 8×192 | 128–1762 | 592 | 丢 896，无持续漂移 |
| 微帧 64×24 | 669–1437 | 1049 | 不丢不重，无持续漂移 |
| 微帧 8×24 | 560–1056 | 804 | 不丢不重，无持续漂移 |

基线水位摆动更大（128–1762），与其“1 ms 内发 8 ms 数据”的突发形状一致。
64×24 每 8 ms 均匀消费一次，水位更平稳，且不需要丢帧校正。

## 10. USB 错误和 Frame 状态统计

| 组 | 提交错误 | 帧 `frStatus` 非 0 | `frActCount` 异常 |
| --- | --- | --- | --- |
| 基线 8×192（40 s） | 0 | 0 | 无 |
| 微帧 64×24（40 s） | 0 | 0 | 无（全部 24） |
| 微帧 8×24（30 s） | 0 | 0 | 无（全部 24） |

等时传输没有逐包 ACK，`frStatus=0` 只代表主机控制器完成了调度和写入，
不代表设备内部已经解码。

## 11. 设备端播放证据边界

已证实：

- 主机以 24 B/微帧、8 ms 连续覆盖的方式向 alt 1 写入了 40 秒非零 PCM。
- USB 提交和帧状态无错误；PCM 峰值/RMS 与 BlackHole 采集端一致。
- 设备端 Feature Unit 的标准 `GET/SET_CUR` 仍然 STALL，但这不是静音证据。

未证实：

- 设备是否真正消费这些微帧。
- 设备内部是否完成 PCM 解码。
- DAC/数字输出是否出声。
- 下游解码器是否选择了 PC-USB 输入。

因此不能把“微帧形状修复成功”写成“设备已出声”。

## 12. 最终结论

1. **协议问题已定位**：基线的 `numFrames=8 / frReqCount=192` 在 High-Speed
   等时管道上表示“8 个微帧，每微帧 192 字节”，单个 Transfer 在 1 ms 内发送
   8 ms 音频，随后空档约 7 ms。
2. **API 语义已确认**：`frameStart` 是 1 ms 总线帧；`IOUSBIsocFrame` 元素是
   125 μs 微帧；`SetPipePolicy` 在本机不可用（始终 `kIOReturnBadArgument`）。
3. **24 字节方案可行**：64×24（8 ms/Transfer）和 8×24（1 ms/Transfer）
   两种形状都在主机侧成功运行，`frActCount` 全为 24，平均字节率 192k。
4. **推荐形状**：64×24，8 ms 节奏流水线提交；CPU 约 5.4%，水位最稳，
   无丢帧/重复帧。
5. **播放结论**：证据不足。主机侧已排除数据形状和速率问题，但设备端
   解码与下游输入模式仍未验证。

## 13. 下一步最小实验

只做一次实际听音验收：

```sh
cd /Users/alkaidhe/Documents/mac/cm108/bridge/tests
./set_default BlackHole2ch_UID
cd ..
./barrot-bridge-mf
```

另一个终端：

```sh
afplay /tmp/tone440.wav
```

确认下游解码器已切到 PC-USB 输入，持续 30 秒。
若仍无声，下一步应转向设备端/Windows 侧控制传输抓取，而不是继续改主机数据形状。

## 14. 复现命令

```sh
cd /Users/alkaidhe/Documents/mac/cm108/bridge/tests
./iso_diag --mode clock --seconds 2
./iso_diag --mode probe --repeats 5
./iso_diag --mode policy
./iso_diag --mode stream --seconds 30 --log /tmp/iso_base.log
./iso_diag --mode stream --seconds 30 --shape micro64 --log /tmp/iso_micro64.log

cd ..
./barrot-bridge --duration 40
./barrot-bridge-mf --duration 40
./barrot-bridge-m8 --duration 20
```
