# RH-899X Vendor Init Audit

Date: 2026-11-28

## 1. Question

Does the vendor driver package contain an initialisation sequence (vendor
request or chip register write) that enables the S/PDIF output, and can that
sequence be replayed from the macOS HAL after every re-enumeration?

## 2. Package under audit

Source: `drivers/51PCusb_win10_11.zip` (identical payload to
`drivers/51Sounddriver_PCusb.rar`).

| File | Detail |
| --- | --- |
| `SoftwareDriver/driver/cm10864.sys` | PE32+ x86-64, 1,286,656 bytes, SHA-256 `bea8c63c…` |
| `SoftwareDriver/driver/cm108.sys` | PE32 x86, SHA-256 `f3918b78…` |
| `SoftwareDriver/driver/Cm108.INF` | `DriverVer=09/11/2008, 6.12.8.2104`, I386+AMD64 |
| `SoftwareDriver/INI/cm108.ini` | driver configuration, `DisableSpdifOut = 0x00000000` |

The INF hardware ID list contains only C-Media and OEM IDs:
`0D8C:000C/001F/0105/0107/010F/0115`, `1940:AC01-AC03`,
`1995:3202/3203`, `1460:0066/0088`. There is no `0C1C:3A1D` entry.

The package is the C-Media CM108 USB Audio Class 1 driver from 2008. It is
a generic package, as stated before; that alone does not invalidate it, so
the binary itself was audited instead of the INF.

## 3. S/PDIF-related code

UTF-16 strings in `cm10864.sys`:

```
DisableSpdifOut        DisableSpdifIn
DisableSpdifOut_0      MasterMuteSpdif
SpDigitalOut           InputSpdifVol
```

Cross-references (image base `0x10000`, code in the `PAGE` section):

| Address | Meaning |
| --- | --- |
| `0x175E71`, `0x179310`, `0x17EB92`, `0x182601`, `0x188278` | `DisableSpdifOut` used as a property/topology name |
| `0x175ECA`, `0x17BF8D`, `0x18266A` | `DisableSpdifIn` used as a property/topology name |
| `0x155316`, `0x15B216` | `SpDigitalOut` used in a get/set dispatcher with a node name |
| `0x18B73F` | `MasterMuteSpdif` used in a property set path |

At each site the string is passed as a *name* to the C-Media property
dispatcher (`0x14C25C` / `0x14C520`) together with a mixer node ID
(`0x02`, `0x03`, `0x35`). The surrounding code builds KS property tables.
None of these sites writes a USB setup packet or a chip register.

`DisableSpdifOut` is therefore a Windows mixer/topology switch, not a
hardware enable command. The same is true for `DisableSpdifIn`,
`SpDigitalOut` and `MasterMuteSpdif`.

## 4. USB request audit

`cm10864.sys` imports only `USBD_CreateConfigurationRequestEx`,
`USBD_ParseConfigurationDescriptorEx` and `USBD_QueryBusTime`; URBs are
submitted by hand through `IRP_MJ_INTERNAL_DEVICE_CONTROL` with
`IOCTL_INTERNAL_USB_SUBMIT_URB` (`0x00220003`, built at `0x2DD22` and
`0x34CFE`).

All URB function codes written by the driver:

| URB function | Value | Count |
| --- | --- | --- |
| `URB_FUNCTION_CONTROL_TRANSFER` | `0x08` | class/standard control path |
| `URB_FUNCTION_ISOCH_TRANSFER` | `0x0A` | 5 |
| `URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE` | `0x0B` | 1 |
| `URB_FUNCTION_SET_FEATURE_TO_DEVICE` | `0x0D` | 1 |
| `URB_FUNCTION_CLASS_DEVICE` | `0x1B` | 4 |
| `URB_FUNCTION_VENDOR_DEVICE/INTERFACE/ENDPOINT/OTHER` | `0x17`-`0x1A` | **0** |

The class requests at `0x2E3D2`, `0x30C1E`, `0x3369A`, `0x337AB` build
setup packets from descriptor bytes at `+0x54`-`+0x5A` and use `bRequest
= 0x01` (SET_CUR). These are UAC Feature Unit / clock requests, the same
family the HAL already issues.

Two private kernel IOCTLs exist:

| IOCTL | Device | Input |
| --- | --- | --- |
| `0x2F205C` | `\Device\XSFI` | 0x18 bytes |
| `0x2F2060` | `\Device\XSFI` | 4 bytes |

`\Device\XSFI` is C-Media's own filter device, created by this package's
filter driver. These IOCTLs never leave the Windows kernel stack and have
no USB equivalent.

## 5. Device-side reality

The RH-899X (0C1C:3A1D) descriptor has exactly one audio function:

```
IF 0  AudioControl  (class 1/1, protocol 0x20 = UAC2)
      Clock Source 0x01
      Input Terminal 0x03 (USB streaming, 2ch) -> Feature Unit 0x0A
      Output Terminal 0x04 (0x0605 = S/PDIF)
IF 1  AudioStreaming
      alt 1: EP 0x02, 200 B, interval 1, PCM 2ch/16-bit
      alt 2: EP 0x01, 200 B, interval 4, IEC61937
```

There is no vendor-specific interface, no HID interface and no vendor
request target. A Windows host would bind this device to the in-box
`usbaudio2.sys`, not to this 2008 package, because the INF does not list
`0C1C:3A1D`.

## 6. Conclusion

1. The generic package does contain S/PDIF controls, but they are
   Windows topology properties (`DisableSpdifOut` etc.), not chip
   initialisation commands.
2. It contains **no vendor-specific USB request** (`0x17`-`0x1A`) at all,
   so there is no vendor init sequence to transplant.
3. The RH-899X exposes no control endpoint for enabling S/PDIF beyond
   what UAC2 already defines; the output routing is selected by the
   alternate setting (alt 1 = PCM, alt 2 = IEC61937).
4. A HAL-side "send the vendor SPDIF-enable request" fix therefore has no
   evidence behind it and was not implemented.

## 7. Changes made from this audit

- `BarrotAudio.c`: teardown returns the streaming interface to alt 0
  (the 2026-11-27 22:29 build behaviour). Start selects alt 1/2 directly.
  An extra `alt 0 -> alt 1` step at start was tested on 2026-11-28 and
  silenced PCM on this firmware, so it must not be added.
- `build.sh`: universal binary (`arm64` + `x86_64`), verified with
  `lipo -info`. The universal build is not installed; the verified
  baseline is the arm64 build from 2026-11-27 22:29, preserved at
  `barrot-hal/reference/RH899X-working-20261127-2229.driver`.

## 8. Remaining evidence gap

The only way to settle the "PCM silent until port reset" behaviour is
external observation, because the device gives no status back:

1. Capture USB traffic on a Windows host with this dongle and
   `usbaudio2.sys` while PCM plays.
2. Compare that capture with the macOS alt 0 / alt 1 / clock sequence.
3. If the captures match, the fault is in the dongle's firmware or
   hardware, not in the macOS driver.

Until such a capture exists, the HAL must not invent control requests.
