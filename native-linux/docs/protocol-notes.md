# Wave:3 Protocol Notes

## USB Audio Class controls (implemented)

The Wave:3 first-generation (PID `0x0070`) exposes two standard UAC 1.0
Feature Units:

| Entity | Purpose | Mute selector | Volume selector |
|--------|---------|---------------|-----------------|
| 5 | Headphone output | `0x01` | `0x02` |
| 6 | Microphone input | `0x01` | `0x02` |

UAC request format:

* `bmRequestType` — `0xa1` for GET, `0x21` for SET (Class | Interface)
* `bRequest` — `0x81` GET_CUR, `0x01` SET_CUR, `0x82` GET_MIN, `0x83` GET_MAX, `0x84` GET_RES
* `wValue` — `(selector << 8) | channel` (channel is normally `0` for master)
* `wIndex` — `(entity << 8) | interface_number`
* `wLength` — `1` for mute, `2` for volume

### The `wIndex` interface-number trick

`snd-usb-audio` claims AudioControl interface 0, so a userspace control
transfer with `wIndex = (entity << 8) | 0` is rejected. The Wave:3 also
has an unclaimed vendor-specific interface 3. Using
`wIndex = (entity << 8) | 3` routes the request through interface 3 in
the kernel's eyes while the firmware still sees the correct entity/selector.

This was verified live on the connected device:

```c
libusb_control_transfer(dev, 0xa1, 0x81,
                        (0x02 << 8),   /* VOLUME selector */
                        (0x06 << 8) | 3, /* MIC_FU entity, interface 3 */
                        buf, 2, 1000);
```

### What works

| Setting | Entity | Selector | Read | Write |
|---------|--------|----------|------|-------|
| Mic mute | 6 | 1 | yes | yes |
| Mic gain | 6 | 2 | yes | **no** (physical dial) |
| HP mute | 5 | 1 | yes | yes |
| HP volume | 5 | 2 | yes | yes |

Observed ranges:

* HP volume: `-60.0 dB … 0.0 dB` (raw `-15360 … 0`, step `64` = 0.25 dB)
* Mic gain: `0.0 dB … 40.0 dB` (raw `0 … 10240`, step `64` = 0.25 dB)

Raw values are signed 16-bit fixed point with 1/256 dB resolution, i.e.
`dB = raw / 256.0`.

## Vendor/class control interface (decoded)

Interface 3 (`bInterfaceClass = 0xFF`, `bInterfaceSubClass = 0xF0`,
`bNumEndpoints = 0`) is used by Elgato Wave Link for proprietary features.
Because it has no endpoints, the protocol runs over **endpoint-0 control
transfers**.

### Request format recovered by static reverse engineering + live probing

Wave Link ships three internal "vendor backend strategy" classes:
`LegacyUAC1VendorUSBBackendStrategy`, `LegacyUAC2VendorUSBBackendStrategy`,
and `MK2VendorUSBBackendStrategy`. The first-generation Wave:3 uses the
**LegacyUAC1** strategy.

Live probing (and the related Wave XLR project `rikkichy/openwave`)
revealed that the actual request type is **class**, not vendor, and that
only a handful of `wValue` IDs are implemented on the Wave:3:

| Direction | `bmRequestType` | `bRequest` | `wValue` | `wIndex` | `wLength` | Purpose |
|-----------|-----------------|------------|----------|----------|-----------|---------|
| Read (IN)  | `0xA1` | `0x85` | `0x0000` | `0x3303` | 16 | Config block |
| Write (OUT)| `0x21` | `0x05` | `0x0000` | `0x3303` | 16 | Config block |
| Read (IN)  | `0xA1` | `0x85` | `0x0001` | `0x3303` | 8  | Level meter |
| Read (IN)  | `0xA1` | `0x85` | `0x000A` | `0x3303` | 51 | Device info |

`wIndex = 0x3303` means: high byte `0x33` is the UAC-like **entity ID**
and low byte `0x03` is the vendor **interface number**. This is the same
encoding as the standard UAC `wIndex` trick.

The earlier reverse-engineering guess of `bmRequestType = 0xC1/0x41`
matched the Thesycon API wrapper but not the actual USB bus: those
vendor-type requests are rejected by the firmware with `LIBUSB_ERROR_PIPE`
and an ID scan in that mode triggered a DFU reset. **Always use the class
request type (`0xA1`/`0x21`) for live control.**

### Config block layout (16 bytes)

The Wave:3 config block (`wValue = 0x0000`) contains the hardware
settings. The full layout is now mapped:

| Offset | Size | Field | Notes |
|--------|------|-------|-------|
| 0 | u8 | **Dial value low byte** | reflects the current dial position in the active mode |
| 1 | u8 | **Dial value high byte** | little-endian with offset 0; mic gain 0–40 dB, HP volume 0 to -128 dB, monitor mix 0–100 |
| 2 | u8 | **unknown / reserved** | writable, no visible effect |
| 3 | u8 | **unknown / reserved** | writable, no visible effect |
| 4 | u8 | **Mic mute** | `0x00` = live, `0x01` = muted |
| 5 | u8 | **Clipguard** | `0x00` = off, `0x01` = on |
| 6 | u8 | **unknown / reserved** | writable, no visible effect |
| 7 | u8 | **Dial flag** | toggles `0x00 <-> 0x80` while adjusting HP volume; sign/fraction flag for the displayed value |
| 8 | s8 | **Headphone volume** | signed dB attenuation (`0x00` = 0 dB, `0xF7` ≈ -9 dB, `0xC4` ≈ -60 dB) |
| 9 | u8 | **Headphone mute** | `0x00` = on, `0x01` = muted |
| 10 | u8 | **Indicator / mute-ring R** | RGB red channel for ring feedback |
| 11 | u8 | **Indicator / mute-ring G** | RGB green channel; also the physical monitor-mix value (0–100) in mix mode |
| 12 | u8 | **Dial mode** | `0x01` = mic gain, `0x02` = headphone volume, `0x03` = monitor mix |
| 13 | u8 | **Indicator / mute-ring B** | RGB blue channel |
| 14 | u8 | **Software direct monitor mix** | `0x00` = microphone only, `0xFF` = PC playback only, linear in between; independent of the dial |
| 15 | u8 | **LED/indicator brightness** | `0x00` = off, `0xFF` = maximum |

**Hardware controls implemented:**

* Mic mute (offset 4)
* Headphone mute (offset 9)
* Headphone volume (offset 8, signed dB attenuation)
* Clipguard (offset 5)
* Mute-ring RGB color (offsets 10/11/13)
* LED brightness (offset 15)
* Direct monitor mix (offset 14) — writable from software

**Physical dial state (read-only):**

* Dial mode (offset 12)
* Dial value (offsets 0/1, little-endian)
* Indicator RGB (offsets 10/11/13) — temporary ring feedback

**Host-side only:**

* **Low-cut filter** — not present in the Wave:3 config block. This is a
  software DSP effect applied by Wave Link.

**Still unknown:** offsets 2, 3, 6. They accept arbitrary writes but
produce no observable change; likely reserved for other firmware
variants. Headphone connection state is not exposed in this config block.

### Meter block (`wValue = 0x0001`)

Eight bytes, two little-endian `uint32` values:

```c
uint32_t input_level    = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
uint32_t playback_level = buf[4] | (buf[5] << 8) | (buf[6] << 16) | (buf[7] << 24);
```

The numeric scale is not yet calibrated; the daemon currently reports
`20 * log10(level / 0x80000000)` as a relative dBFS value.

### Device info block (`wValue = 0x000A`)

51 bytes. Known fields:

* `data[0]`.`data[1]` — API version (observed `5.3`)
* `data[6]`..`data[8]` — firmware version (observed `0.3.7`)
* `data[27]`..`data[46]` — serial number as ASCII

### Live probe safety

A broad read-only scan of IDs `0x0000`–`0x00FF` using the *vendor*
request type (`0xC1`) caused the device to reboot into its DFU/bootloader
PID (`0x0071`) before re-enumerating as `0x0070`. **Always use the class
request type (`0xA1`/`0x21`) and only probe the known IDs (`0x0000`,
`0x0001`, `0x000A`).**

## D-Bus API

The daemon exposes `org.wave3.Daemon` on the session bus. State is
returned as the tuple:

```
(mic_mute, hp_mute,
 mic_gain_pct, hp_vol_pct, mic_gain_db, hp_vol_db,
 clipguard, lowcut,
 direct_monitor, mute_rgb, input_level_db,
 brightness, playback_level_db,
 indicator_rgb, dial_mode, dial_value)
```

Methods include `SetMicMute`, `ToggleMicMute`, `SetHpMute`,
`SetHpVolume`, `SetClipguard`, `SetDirectMonitor`, `SetMuteColor`,
`SetBrightness`, `GetInputLevel`, `GetPlaybackLevel`, `GetDialMode`,
`GetDialValue`, `GetIndicatorColor`, plus getters for all fields.
`SetLowCut` and `SetHeadphoneColor` return
`G_IO_ERROR_NOT_SUPPORTED` because the first-gen Wave:3 has neither
hardware low-cut nor a headphone-color LED.

## PipeWire / WirePlumber integration

The daemon only controls hardware; it does not manage PipeWire nodes.
Virtual sinks and routing are configured through WirePlumber/PipeWire.

The recommended topology (derived from the Undertone project, see
references) is:

```
App audio -> wave3-channel-sink -> stream-vol-filter -> wave3-stream-mix -> capture/recording
                                   -> monitor-vol-filter -> wave3-monitor-mix -> wave3-sink (headphones)

Wave:3 mic -> wave3-source -> wave3-mic-mix -> wave3-stream-mix + wave3-monitor-mix
```

Key PipeWire properties learned from Undertone:

* `factory.name = support.null-audio-sink` for virtual channel/mix sinks
* `node.autoconnect = false` to prevent WirePlumber auto-linking managed nodes
* `session.suspend-timeout-seconds = 0` to prevent auto-suspend
* Volume control on null sinks uses SPA `monitorVolumes` / `monitorMute`

## References

* USB Audio Class 1.0 specification
* [rikkichy/openwave](https://github.com/rikkichy/openwave) — Wave XLR
  Linux control app that discovered the `wIndex` interface-number trick
  and the class-based `0xA1/0x21` control encoding (`wValue` IDs
  `0x0000`, `0x0001`, `0x000A`).
* [x4ndr0m3d4x/wave3ctl](https://git.4ndr0m3d4.me/4ndr0m3d4/wave3ctl) —
  upstream kernel-module + Python CLI for the Wave:3.
* [polariscli/Undertone](https://github.com/polariscli/Undertone) —
  Rust PipeWire mixer for the Wave:3; excellent WirePlumber topology
  reference, but vendor protocol is also unresolved (uses ALSA fallback).
* [Raphiiko/wavelink-ts PROTOCOL.md](https://github.com/Raphiiko/wavelink-ts/blob/main/PROTOCOL.md) —
  documents Wave Link's JSON-RPC application protocol (logical names only).
