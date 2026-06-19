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
settings. Most offsets are now mapped:

| Offset | Size | Field | Notes |
|--------|------|-------|-------|
| 0 | u8 | **checksum / flags** | writable, but likely part of a firmware checksum or validation pair with offset 1 |
| 1 | u8 | **checksum / validation** | firmware normalizes this value; invalid writes cause offset 0 to reset to `0x00` |
| 2 | u8 | **unused / reserved** | writable, no visible effect |
| 3 | u8 | **unused / reserved** | writable, no visible effect |
| 4 | u8 | **Mic mute** | `0x00` = live, `0x01` = muted |
| 5 | u8 | **Clipguard** | `0x00` = off, `0x01` = on (same offset as Wave XLR) |
| 6 | u8 | **unused / reserved** | writable, no visible effect |
| 7 | u8 | **unused / reserved** | writable, no visible effect; not low-cut |
| 8 | s8 | **Headphone volume** | signed dB attenuation (`0x00` = 0 dB, `0xF7` ≈ -9 dB, `0xC4` ≈ -60 dB) |
| 9 | u8 | **Headphone mute** | `0x00` = on, `0x01` = muted |
| 10 | u8 | **Mute color R** | RGB red channel for the mute-ring LED |
| 11 | u8 | **Mute color G** | RGB green channel; also appears in monitor-mix indicator |
| 12 | u8 | **Device state** | **read-only**; accepts only `0x01`, `0x02`, `0x03`; observed `0x03` (likely headphone connected + dial status) |
| 13 | u8 | **Mute color B** | RGB blue channel |
| 14 | u8 | **Direct monitor mix** | `0x00` = microphone only, `0xFF` = PC playback only, linear in between |
| 15 | u8 | **LED/indicator brightness** | `0x00` = off, `0xFF` = maximum |

**Confirmed hardware controls:**

* Mic mute (offset 4)
* Headphone mute (offset 9)
* Headphone volume (offset 8, signed dB attenuation)
* Clipguard (offset 5)
* Direct monitor mix (offset 14, 0–255)
* Mute-ring RGB color (offsets 10/11/13)
* LED brightness (offset 15)

**Host-side only:**

* **Low-cut filter** — not present in the Wave:3 config block. This is a
  software DSP effect applied by Wave Link (confirmed by Elgato
  documentation and the absence of any byte that toggles it).

**Checksum/validation:** offsets 0 and 1 behave like a firmware
validation pair. Offset 1 is normalized by the device, and invalid
values cause offset 0 to reset to `0x00`. They are not user features.

**Unused/reserved:** offsets 2, 3, 6, 7 accept arbitrary writes but
produce no observable change. They may be used by other firmware
variants or future devices.

**Read-only state:** offset 12 reflects device state (likely headphone
connection + dial mode); exact bit meanings are not decoded.

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
 brightness, playback_level_db)
```

Methods include `SetMicMute`, `ToggleMicMute`, `SetHpMute`,
`SetHpVolume`, `SetClipguard`, `SetDirectMonitor`, `SetMuteColor`,
`SetBrightness`, `GetInputLevel`, `GetPlaybackLevel`, plus getters for
all fields. `SetLowCut` and `SetHeadphoneColor` return
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
