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

## VLC acceptance (2026-11-28)

Environment: VLC 3.0.24 (`auhal`), candidate `382534b6...` (ring policy
scaled to the client IO buffer), commit `1d89e6e` plus the policy fix.

VLC log:

```
Trying to use stored audio device 55 (digital)
'RH-899X Bitstream' supports digital output
original stream format: [48000][3cac][76][6144][1536][0][2][16]
setting stream format:  [48000][3cac][76][6144][1536][0][2][16]
digital output successfully opened
using audio converter module "tospdif"
using audio decoder module "spdif"
```

First attempt passed the wrong decimal for the encoded device
(`15728711` = device 71); VLC correctly rejected it and fell back to the
built-in output. The correct value is `55 | 0xf00000 = 15728695`.

HAL:

```
stream format obj=259 mode=2
USB alt=2
StopIO mode=2 tx=743 frames=285312 usb_err=0 frame_err=0
        under=0 over=0 drops=0 dups=1173 ring=4245
```

Before scaling the ring policy, the same 1536-frame callbacks produced
`drops=25944` (~10% of the stream). With the policy scaled to
`4/6/2` IO buffers, drops went to 0.

A listening test then reported a "USB unplug"-style pop about 3 s into
playback. HAL counters showed `drops=0` but `dups=1173`: the low-water
corrector was repeating single frames inside the IEC61937 burst (it
started after the first 250 transfers, i.e. ~2 s). Duplicating a frame
inside a burst breaks the decoder's framing.

Fix (`4f9a621d...`): the water marks are recomputed from the real
`ioFrameSize` on every loop, and the duplicate-frame path is disabled for
bitstream. Re-run:

```
StopIO mode=2 tx=758 frames=291072 usb_err=0 frame_err=0
        under=0 over=0 drops=0 dups=0 ring=2304
```

Acoustic check (built-in mic, 6 s excerpt):

| Capture | mean | max |
| --- | --- | --- |
| ambient | -60.2 dB | -36.7 dB |
| playback | -50.1 dB | -22.2 dB |

PCM device after the run remains Float32 8 B/frame / 192 frames; system
default output stays on the built-in speaker.

Status: **PARTIAL** - device selection, format negotiation, data path and
audible output are verified; human confirmation that the decoded AC3
content is correct is still pending.
