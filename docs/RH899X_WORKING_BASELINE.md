# RH-899X Working Baseline (2026-11-27 22:29)

Local-only reference for the last driver build that was verified by
listening and by DTS playback.

## Artifact

| Property | Value |
| --- | --- |
| Build time | 2026-11-27 22:29:32 (+0800) |
| Architecture | arm64 |
| SHA-256 | `34967ef296d1d440c926c7b117f815e940cff5ab4f325dbcb32caeddbfdd79d0` |
| Code signature | Apple Development, identifier `com.ranhe.rh899x.audio` |
| Archived copy | `barrot-hal/reference/RH899X-working-20261127-2229.driver` |
| Installed copy | `/Library/Audio/Plug-Ins/HAL/RH899X.driver` (restored 2026-11-28) |

The archived bundle is not committed: `.gitignore` excludes `*.driver`.
Identify it by the SHA-256 above.

## Verified behaviour

- PCM on `RH-899X` is audible.
- DTS through `coreaudio-bitstream-test`:
  `mode=2 frames=141696 usb_err=0 under=0 drops=0`.
- CoreAudio registers `52 RH-899X (RH899X-0c1c-3a1d)`.
- `codesign --verify --strict --deep` passes.

## Hard rule for alternate settings

Start selects alt 1 (PCM) or alt 2 (IEC61937) **directly**. Teardown
returns the interface to alt 0. Do not insert an `alt 0 -> alt 1` step at
start: that sequence silenced PCM on this firmware (tested 2026-11-28).

## Source caveat

The source tree at this tag is the aligned snapshot used for later work.
It is **not bit-identical** to the archived binary: the tag adds logging,
presence-monitor bookkeeping and the universal build. Any rebuild is a new
artifact and must be re-verified by listening before replacing this
baseline.
