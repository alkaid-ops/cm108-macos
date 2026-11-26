# Ranhe RH-899X Rename

Date: 2026-11-26

## Naming

| Item | New value |
| --- | --- |
| CoreAudio device name | `RH-899X` |
| CoreAudio UID | `RH899X-0c1c-3a1d` |
| Model UID | `com.ranhe.rh899x.model` |
| Bundle identifier | `com.ranhe.rh899x.audio` |
| Bundle / executable | `RH899X.driver` / `RH899X` |
| HAL install path | `/Library/Audio/Plug-Ins/HAL/RH899X.driver` |
| Repository | `https://github.com/alkaid-ops/cm108-macos` |

The source file `barrot-hal/BarrotAudio.c` and directory `barrot-hal/` keep
their existing names. The GitHub repository and on-disk snapshot are named
`cm108-macos`.

## Changed Files

- `README.md`
- `RENAME_RH899X.md`
- `barrot-hal/BarrotAudio.c`
- `barrot-hal/README.md`
- `barrot-hal/BarrotBT10.plist` -> `barrot-hal/RH899X.plist`
- `barrot-hal/build.sh`
- `barrot-hal/install.sh`
- `barrot-hal/test.sh`
- `barrot-hal/selftest.c`
- `barrot-hal/coreaudio_device_test.c`
- `barrot-hal/coreaudio_bitstream_test.c`
- `docs/BT10_192K_24BIT_AUDIT.md` -> `docs/RH899X_192K_24BIT_AUDIT.md`
- `docs/BT10_DTS_AC3_PASSTHROUGH_EXPERIMENT.md` -> `docs/RH899X_DTS_AC3_PASSTHROUGH_EXPERIMENT.md`
- `docs/BT10_HAL_LOAD_TEST.md` -> `docs/RH899X_HAL_LOAD_TEST.md`
- `docs/BT10_HS_ISOCHRONOUS_MICROFRAME_AUDIT.md` -> `docs/RH899X_HS_ISOCHRONOUS_MICROFRAME_AUDIT.md`
- `docs/BT10_MACOS_NATIVE_AUDIO_DEVICE_AUDIT.md` -> `docs/RH899X_MACOS_NATIVE_AUDIO_DEVICE_AUDIT.md`
- `docs/BT10_MACOS_NATIVE_AUDIO_DEVICE_IMPLEMENTATION.md` -> `docs/RH899X_MACOS_NATIVE_AUDIO_DEVICE_IMPLEMENTATION.md`
- `docs/BT10_MACOS_NATIVE_AUDIO_DEVICE_TEST.md` -> `docs/RH899X_MACOS_NATIVE_AUDIO_DEVICE_TEST.md`
- `docs/BT10_USB_AUDIO_PROTOCOL_AUDIT.md` -> `docs/RH899X_USB_AUDIO_PROTOCOL_AUDIT.md`

Historical references to `BarrotUSB`, `barrot-bridge`, and legacy signing
identities remain where they identify real historical artifacts. The install
script retains `/Library/Audio/Plug-Ins/HAL/BarrotBT10.driver` only as the
legacy migration source.

## Completed

- Renamed device strings, UIDs, bundle metadata, build outputs, test paths,
  documentation, and repository links.
- Changed the plug-in factory symbol to `Ranhe_CreatePlugin`.
- Updated both CoreAudio test tools to search for `RH899X-0c1c-3a1d`.
- Added legacy migration to `install.sh`: existing `RH899X.driver` and
  `BarrotBT10.driver` bundles are moved to timestamped backups, and rollback
  restores the previous bundle.
- Renamed the GitHub repository to
  `https://github.com/alkaid-ops/cm108-macos` and committed/pushed the rename.
- The target machine built and installed `RH899X.driver`; the legacy bundle
  remains at
  `/Library/Audio/Plug-Ins/HAL/BarrotBT10.driver.backup-20261126-111818`.
- CoreAudio enumerates `RH-899X` (`RH899X-0c1c-3a1d`); the built-in speaker
  remains present, and a 48 kHz PCM Start/Stop test passed.
- `v0.1.0` remains unchanged and no new release was created.

## Pending

- Re-run DTS/AC3 playback after installation if a fresh bitstream regression
  is required.
- The v0.1.0 tag and release notes are unchanged.

## Risk

- Any existing process or script that hard-codes the old UID must use
  `RH899X-0c1c-3a1d`.
