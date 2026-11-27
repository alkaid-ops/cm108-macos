# Ranhe RH-899X macOS AudioServerPlugIn

Repository: <https://github.com/alkaid-ops/cm108-macos>

An AudioServerPlugIn for the **Ranhe RH-899X / `0c1c:3a1d`** USB audio device.
It exposes RH-899X as a native CoreAudio output device, so it appears in
Control Center → Sound → Output without BlackHole or a helper process.

## Status — v0.1.3

- **48 kHz / 2ch / 16-bit PCM: stable.**
- **DTS / AC3 IEC61937 passthrough: working, including VLC.**
  `RH-899X (Encoded Output)` in VLC 3.0.24 plays 5.1 AC3 with zero
  `way too early` / `inserting zeroes` warnings and `under=0 over=0`.
  Physical output was confirmed by ear on the downstream decoder.
- **v0.1.3 fixes (important):**
  1. Bitstream transfers must be aligned to an **8-frame boundary**
     (`nextStart &= ~7`). Without it the alt-2 service points shift relative
     to the transfer and the downstream decoder hears a "telegraph" click.
  2. `mBytesPerFrame` default is back to `0` (the verified-working value).
- **Device state:** after a USB re-plug the RH-899X loses its S/PDIF output
  state. PCM keeps working but bitstream crackles until the device is
  **power-cycled** (unplug power, not just USB). This is a device behaviour,
  not a driver bug.
- **AC3 end-to-end host test:** the 30-second DTS 5.1 test file was decoded
  and re-encoded to 48 kHz / 448 kbps AC3; 10-second playback completed with
  `underruns=0` and the USB device stayed online.
- **96 / 192 kHz: not usable.** Both caused the RH-899X firmware to stop
  responding and macOS to disable the USB port. Only 48 kHz is advertised.
- **24-bit: not supported by this USB input.** The UAC2 descriptors declare
  a 2-byte subslot and 16-bit resolution; there is no 24-bit alternate setting.

## Requirements

- macOS with Xcode Command Line Tools (`clang`, `codesign`).
- An Apple-issued code-signing identity. A free Apple ID's
  `Apple Development` certificate is sufficient for local use.
- `security find-identity -v -p codesigning` must show that identity.

Ad-hoc and locally self-signed bundles are rejected by AMFI when loaded as a
system HAL plug-in. See `docs/RH899X_HAL_LOAD_TEST.md`.

## Build

```sh
cd barrot-hal
./build.sh
```

`build.sh` auto-selects an `Apple Development` identity. You can override it:

```sh
CODESIGN_IDENTITY="Apple Development: Your Name (TEAMID)" ./build.sh
```

## Install

```sh
cd barrot-hal
./install.sh
```

The installer verifies the Apple signature, backs up any previous bundle,
installs to `/Library/Audio/Plug-Ins/HAL/RH899X.driver`, restarts
`coreaudiod`, and rolls back if the built-in speaker disappears. If the
legacy `BarrotBT10.driver` is present, it is moved to a timestamped backup
before the new bundle is installed.

## Test

```sh
cd barrot-hal
./test.sh 6                         # in-process object model + 64x24 USB
clang -std=c11 -O2 -o coreaudio-device-test coreaudio_device_test.c \
  -framework CoreAudio -framework AudioToolbox -framework CoreFoundation
./coreaudio-device-test 10          # 48 kHz PCM, targeted at RH-899X
```

DTS / IEC61937 host-side test:

```sh
clang -std=c11 -O2 -o coreaudio-bitstream-test coreaudio_bitstream_test.c \
  -framework CoreAudio -framework CoreFoundation
./coreaudio-bitstream-test /path/to/test.dts 10
```

The bitstream test takes hog mode, switches the device to non-mixable
`kAudioFormat60958AC3`, and restores the previous format on exit.

## Architecture

```
CoreAudio client
  -> AudioServerPlugIn (barrot-hal/BarrotAudio.c)
  -> lock-free SPSC ring buffer
  -> USB isochronous worker
  -> RH-899X (0c1c:3a1d)
```

- PCM: 48 kHz, 64 microframes × 24 bytes per 8 ms transfer.
- IEC61937: non-mixable `cac3`, USB alt 2, 8 × 192 bytes per transfer.
- The real-time path performs no allocation, logging, or USB I/O; a separate
  worker thread owns the USB endpoint.

The bitstream test tool auto-detects DTS and AC3, supports DTS 512/1024/2048
sample bursts and AC3 48 kHz frames, and writes IEC61937 bursts of the correct
dynamic packet size.

## Limitations

- USB PCM is fixed at 48 kHz / 16-bit for this firmware.
- 96 kHz and 192 kHz are intentionally not exposed.
- 24-bit cannot be enabled from the host; it would require a device firmware
  format setting that is not present in the descriptors.
- No vendor-specific USB requests, EEPROM writes, or firmware flashing are
  performed by this project.

## Repository Layout

- `barrot-hal/` — plug-in source, bundle plist, build/install/test scripts.
- `docs/` — USB protocol, microframe, HAL load, passthrough, and rate audits.

The audit documents are in Chinese. Start with
`docs/RH899X_USB_AUDIO_PROTOCOL_AUDIT.md` and
`docs/RH899X_DTS_AC3_PASSTHROUGH_EXPERIMENT.md`.

