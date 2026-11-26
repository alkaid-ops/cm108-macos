# Barrot BT10 AudioServerPlugIn

独立的 macOS CoreAudio 输出设备驱动（AudioServerPlugIn），不依赖 BlackHole。

## 构建

```sh
./build.sh                   # 自动选择 Apple Development 身份
CODESIGN_IDENTITY="Apple Development: ..." ./build.sh   # 系统加载必需 Apple 证书
CODESIGN_IDENTITY="Developer ID Application: ..." ./build.sh
```

系统加载要求 Apple 签发的身份。免费 Apple ID 在 Xcode 中生成的
`Apple Development` 证书即可用于本机测试；首次构建前确认
`security find-identity -v -p codesigning` 能看到该身份。

## 非系统自测

```sh
./test.sh 6
```

`selftest` 用 `dlopen` 在用户进程中加载 bundle，不经过 `coreaudiod` 的 AMFI 检查，
因此可以在没有证书时验证对象模型、IOProc、Ring 和 64×24 USB 传输。

## 系统级 CoreAudio 测试

安装并枚举成功后，可定向 `Barrot BT10` 播放 440 Hz，不改变系统默认输出：

```sh
clang -std=c11 -O2 -o coreaudio-device-test coreaudio_device_test.c \
  -framework CoreAudio -framework AudioToolbox -framework CoreFoundation
./coreaudio-device-test 10          # 48 kHz
```

实际加载和测试结果见 `../docs/BT10_HAL_LOAD_TEST.md`。

设备 USB 输入最终只稳定支持 **48 kHz / 2ch / 16-bit PCM**。
96/192 kHz 在真机上会导致 BT10 固件停止响应并禁用 USB 端口，因此 HAL
只对外公布 48 kHz；多采样率代码保留给 `selftest` 诊断。24-bit 不在
UAC2 描述符中，详见 `../docs/BT10_192K_24BIT_AUDIT.md`。

## DTS / AC3 位流测试

安装并枚举成功后，可使用 non-mixable `cac3` 格式播放 IEC61937 位流：

```sh
clang -std=c11 -O2 -o coreaudio-bitstream-test coreaudio_bitstream_test.c \
  -framework CoreAudio -framework CoreFoundation
./coreaudio-bitstream-test '/path/to/test.dts' 10
```

测试会取得 BT10 的 hog mode、关闭 mixing，结束后恢复原格式和默认状态。
主机侧 DTS 传输已通过；设备实际出声仍需下游解码器切到 PC-USB 后听音。
详见 `../docs/BT10_DTS_AC3_PASSTHROUGH_EXPERIMENT.md`。

## 安装

```sh
./install.sh
```

脚本会拒绝 ad-hoc 和本地自签名；只接受 Apple Development 或
Developer ID Application，并执行 `codesign --verify --strict --deep`。

## 证据

- `../docs/BT10_MACOS_NATIVE_AUDIO_DEVICE_AUDIT.md`
- `../docs/BT10_MACOS_NATIVE_AUDIO_DEVICE_IMPLEMENTATION.md`
- `../docs/BT10_MACOS_NATIVE_AUDIO_DEVICE_TEST.md`
- `../docs/BT10_HAL_LOAD_TEST.md`
- `../docs/BT10_192K_24BIT_AUDIT.md`
