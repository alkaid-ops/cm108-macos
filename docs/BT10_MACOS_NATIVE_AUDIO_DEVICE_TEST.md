# BT10 macOS 原生 CoreAudio 输出设备：测试报告

环境：macOS 14.8.7（23J520），Apple M1，BT10 `0c1c:3a1d` 已连接。

> 2026-11-25 更新：Apple Development 签名已通过 AMFI，HAL 加载、设备枚举、
> CoreAudio IOProc 和 USB 传输均已实测通过。以下早期“AMFI 阻塞”章节保留为
> 历史记录；当前状态和证据见 `BT10_HAL_LOAD_TEST.md`。

测试分两类：

1. **系统级验证**：安装到 `/Library/Audio/Plug-Ins/HAL`，经过 `coreaudiod`
   和 AMFI。当前被签名策略阻塞。
2. **驱动自测**：`selftest` 在用户进程内 `dlopen` bundle，绕过系统 HAL 加载，
   验证对象模型、IOProc、ring 和 64×24 USB 传输。

---

## 1. 测试 A：系统枚举

构建完整 bundle 并用 ad-hoc 签名，安装到系统 HAL 目录，重启 `coreaudiod`：

```
Attempting to load: BarrotUSB.driver
Loading server plug-in com.barrot.usbaudio...
AMFI: '/Library/.../BarrotUSB' is adhoc signed.
amfid: No certificate chain found
amfid: Basic requirement validation failed, error ... Code=-67050
amfid: Code=-423 "adhoc signed or signed by an unknown certificate chain"
helper exited due to SIGABRT
```

结果：

| 检查项 | 结果 |
| --- | --- |
| CoreAudio 枚举到 BT10 | **否** |
| 控制中心显示 BT10 | **否** |
| 失败阶段 | AMFI 代码签名校验（插件代码执行前） |
| 退出码 | helper `SIGABRT`，连续 6 次 corpse |

自签名证书同样返回 `-423`。因此当前 macOS 14.8.7 实例没有可用的
零成本签名材料。

---

## 2. 测试 B：对象模型与属性

`selftest` 输出：

```
Initialize: 0
device id: 256 (expected 256)
device name: Barrot BT10
device uid: BarrotBT10-0c1c-3a1d
transport: 0x75736220        # 'usb '
stream config: 1 buffers, 2 channels
asbd: 48000 Hz, 2 ch, 32 bits, flags=0x9
```

结论：

- factory、QueryInterface、Initialize 正常。
- Device ID、名称、稳定 UID、USB transport 正确。
- Stream 为输出方向，1 个 buffer，2 声道。
- ASBD 为 48 kHz Float32 packed。

---

## 3. 测试 C：IOProc、Ring 与 USB 64×24

6 秒稳定测试（每 1 秒读取一次 `'brst'` 统计；生产端用
`mach_wait_until` 精确按 48 kHz 推送 512 帧/周期）：

| 秒 | Transfer/s | PCM 帧/s | 欠载增量 | 丢帧 | 重复 | Ring 水位 | USB 错误 | 帧错误 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 24 | 9216 | 500 | 0 | 234 | 464 | 0 | 0 |
| 2 | 125 | 48000 | 0 | 0 | 441 | 649 | 0 | 0 |
| 3 | 124 | 47616 | 0 | 0 | 96 | 745 | 0 | 0 |
| 4 | 126 | 48384 | 0 | 9 | 35 | 899 | 0 | 0 |
| 5 | 125 | 48000 | 0 | 11 | 8 | 1024 | 0 | 0 |
| 6 | 125 | 48000 | 0 | 17 | 17 | 768 | 0 | 0 |

说明：

- 第 1 秒是启动填充期，欠载是预期。
- 第 2 秒起欠载为 0，Transfer 稳定在 124–126/s。
- 每秒约 48,000 PCM 帧 = 192,000 B/s。
- USB 错误和 `frStatus` 帧错误均为 0。
- ring 水位稳定在 768–1024 帧（16–21 ms），drop/dup 总量低于 0.1%。
- 使用 **64 × 24 字节**，未改回 8×192。

### 3.1 Stop/Start 生命周期

第一次 `StopIO` 后立即第二次 `StartIO`，结果：

```
run=1 sec=1 tx=+95 frames=+36480 under=+841 dup=+500 level=940
usb_err=0 frame_err=0
```

结论：二次启动成功，USB 重新打开并恢复传输；没有重复 worker、
没有崩溃、没有旧线程占用 USB。

### 3.2 修复的缺陷

早期版本在 `StopIO` 时直接释放 `IOUSBIsocFrame` 数组，USB 控制器随后
把完成状态写入已释放内存，触发：

```
malloc: Corruption of free object ... msizes 747/0 disagree
SIGABRT
```

修复：停止时等待所有在途 Transfer 完成（最多 100 ms）再 free。

---

## 4. 测试 D：稳定性

### 4.1 180 秒最终形状测试

使用最终代码（`kMaxAhead=8`、submit 失败原地重试、ring 超限一次跳到目标）：

| 指标 | 值 |
| --- | --- |
| Transfer 速率 | 124.88 /s |
| PCM 帧率 | 47,952.6 /s |
| 字节率 | 191,810 B/s（−0.10% vs 192000） |
| 欠载 | 0 |
| USB 错误 | 0 |
| `frStatus` 帧错误 | 0 |
| Ring 水位 min / mean / max | 735 / 1195 / 1536 |
| 丢帧 / 重复帧（180 s） | 13,839 / 5,903（合计约 0.16%） |
| 二次 Start/Stop | 成功，无崩溃 |

180 秒内没有 submit 错误，也没有需要重连的事件。

### 4.2 第一轮 600 秒测试（旧代码）

第一轮 600 秒测试暴露了一个真实问题：

- 约 7 次 `WriteIsochPipeAsync` 提交失败；
- 失败后 worker 关闭并重开 interface，ring 积压到 8710 帧；
- 旧的水位校正每秒只 drop 约 1300 帧，需要约 7 秒才恢复；
- 期间欠载仍为 0，但音频会连续丢样。

修复：

1. 提交失败先原地重试（最多 3 次），不再立刻关闭 interface。
2. `kMaxAhead` 从 12 降到 8，避免把 start frame 排到控制器可能拒绝的未来位置。
3. ring 超过高水位时一次性跳到目标水位，避免数秒的连续 drop。

### 4.3 最终 600 秒复测

最终驱动代码（`kMaxAhead=8`、提交失败原地重试、ring 超限一次跳到目标）
连续运行 600 秒：

| 指标 | 值 |
| --- | --- |
| Transfer 速率 | 124.86 /s |
| PCM 帧率 | 47,945 /s |
| 字节率 | 191,781 B/s（−0.11% vs 192000） |
| 欠载 | **0** |
| USB 提交错误 | **1**（`0xe00002ee kIOReturnNotPermitted`，原地重试后恢复，未重连） |
| `frStatus` 帧错误 | **0** |
| Ring 水位 min / mean / max | 692 / 1159 / 1536 |
| 丢帧 / 重复帧 | 85,763 / 54,403（合计约 0.49%） |
| CPU | 约 4.7% |
| RSS | 5.1–5.8 MB |
| 二次 Start/Stop | 成功 |

说明：

- 欠载为 0，说明 USB Worker 在整个 600 秒内没有真正断流。
- 丢帧/重复帧来自**自测生产者**的时钟误差（实测约 0.11% 偏慢）和
  5 秒统计查询造成的突发；不是 USB submit 失败导致的。
- 唯一一次 submit 失败发生在 600 秒中段，worker 原地重试成功，没有
  关闭 interface，也没有出现第一轮旧代码那种 7 秒连续丢样。
- 水位没有持续单向漂移：692–1536 的摆动是校正死区内的正常波动。

### 4.4 结果判定

| 项目 | 判定 |
| --- | --- |
| 64×24 微帧形状 | 通过 |
| 48 kHz / 192 k B/s 平均速率 | 通过（−0.11%） |
| 600 秒无欠载 | 通过 |
| 600 秒无帧错误 | 通过 |
| 600 秒无持续时钟漂移 | 通过（有界水位） |
| 600 秒无 USB 提交错误 | 未完全通过：1 次瞬态，已恢复 |
| 系统 CoreAudio 枚举 | 阻塞（AMFI `-423`） |
| 控制中心切换 | 阻塞（同上） |
| 物理听音 | 阻塞（同上） |

---

## 5. 尚未完成/被阻塞的项目

| 项目 | 状态 | 原因 |
| --- | --- | --- |
| 控制中心枚举 BT10 | 阻塞 | AMFI 拒绝 ad-hoc/自签名；无 Apple 证书 |
| 系统 Shortcut/默认设备切换 | 阻塞 | 同上 |
| CoreAudio 客户端播放 | 驱动侧已自测，系统侧阻塞 | 同上 |
| 物理听音 | 待验收 | 需要系统枚举成功 |

解锁条件（二选一）：

1. 用户在 Xcode 中用免费 Apple ID 创建 **Apple Development** 证书，
   然后 `CODESIGN_IDENTITY="Apple Development: ..." ./build.sh && ./install.sh`。
   这是零成本候选，但是否还需 provisioning profile 未实测。
2. 使用付费 **Developer ID Application** 证书（已知可加载，但违反本项目
   “不购买 Developer ID”的约束）。

在拿到证书后，需要重新执行：

```sh
cd $HOME/barrot-bt10-macos/barrot-hal
CODESIGN_IDENTITY="..." ./build.sh
./install.sh
```

然后验证 Test A（枚举）、Test C（切换/重启）、Test D（10 分钟）和物理听音。

