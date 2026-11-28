# Independent Bitstream Device Experiment

Date: 2026-11-28

## Base

| Item | Value |
| --- | --- |
| Base tag | `v0.1.5-local` (`c8d3cd0`) |
| Candidate binary | `53312d331f57ad090065c3398318f42563398e61acd25aa7f8ce8a752c15f2b2` |
| Installed | `/Library/Audio/Plug-Ins/HAL/RH899X.driver` (same hash) |

## Change

Second CoreAudio device in the same AudioServerPlugIn:

| Object | ID | Role |
| --- | --- | --- |
| `RH-899X` | device `0x100` / stream `0x101` | PCM, unchanged |
| `RH-899X Bitstream` | device `0x102` / stream `0x103` | IEC 60958 only |

The bitstream device advertises exactly one format:

```
cac3 48000 Hz  bytes/pkt=6144  frames/pkt=1536  bytes/frame=0  bits=16
```

No Float32 / 8-byte format is listed for it. PCM and bitstream keep
separate `pcmMode` and `bsBufferFrames`, so a bitstream run cannot change
the PCM device's reported format or buffer size.

## Result

```
CoreAudio devices:
  52 RH-899X            RH899X-0c1c-3a1d
  55 RH-899X Bitstream  RH899X-0c1c-3a1d-bitstream

PCM before bitstream run:
  buffer frames=192
  physical/virtual lpcm float32  8 bytes/frame

Bitstream run (0.5 s):
  buffer frames: requested=1536 actual=1536
  first IO buffer: 1 buffer, 6144 bytes, 2 channels
  6144 / 1536 = 4 bytes/frame
  calls=15 frames=23040 copied=23040 underruns=0

PCM after bitstream run:
  buffer frames=192
  physical/virtual lpcm float32  8 bytes/frame
```

The HAL consumes `inIOBufferFrameSize * 4` bytes per callback:
`1536 * 4 = 6144` bytes, i.e. the whole buffer.

## Gate

The required contract is met: **4 bytes per frame and 100% of the
callback buffer consumed**. The literal 768-byte figure (192 frames x 4)
was not observed because coreaudiod selected a 1536-frame buffer for the
new device; `first IO buffer bytes` is therefore 1536 x 4 = 6144.

## Next

Only now is a VLC test meaningful. Select `RH-899X Bitstream` (VLC will
also list it as "Encoded Output") and verify the same 4-byte-per-frame
path is used.
