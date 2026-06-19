# Elgato Wave:3 — Native Linux Communication & Management Protocol Summary

**Repo:** `/home/USER/wave3-audio-report`  
**Device:** Elgato Wave:3 USB condenser microphone  
**USB IDs:** `VID 0x0fd9`, `PID 0x0070`  
**Date:** 2026-06-19  
**Status:** Standard UAC controls and proprietary class config block fully working. All hardware-controllable features on the first-gen Wave:3 are mapped; only reserved bytes remain.

> **Educational and interoperability purpose**
>
> This document is research output from controlling a device I physically
> own on a Linux host. It is published for educational and interoperability
> purposes only. No proprietary firmware, driver binaries, or copyrighted
> source code are distributed alongside it. Elgato®, Wave:3®, Wave Link®,
> and related marks are trademarks of Corsair Memory, Inc. / Elgato and
> are used here solely for identification and commentary.

---

## 1. Device Overview

### 1.1 USB Configuration

The Wave:3 exposes five interfaces:

| Interface | `bInterfaceClass` | `bInterfaceSubClass` | `bInterfaceProtocol` | Endpoints | Purpose |
|-----------|-------------------|----------------------|----------------------|-----------|---------|
| 0         | `0x01` (Audio)    | `0x01` (Control)     | `0x00`               | 0         | USB Audio Class control (standard) |
| 1         | `0x01` (Audio)    | `0x02` (Streaming)   | `0x00`               | 1 IN      | Microphone 24-bit 48 kHz capture |
| 2         | `0x01` (Audio)    | `0x02` (Streaming)   | `0x00`               | 1 OUT     | Headphone 24-bit 48 kHz playback |
| 3         | `0xFF` (Vendor)   | `0xF0`               | `0x00`               | **0**     | **Vendor-specific control** |
| 4         | `0xFE` (App/DFU)  | `0x02`               | `0x01`               | 0         | Device Firmware Upgrade |

Key facts:

* Interface 3 has **no endpoints**.  All vendor protocol traffic is carried on **control transfers to endpoint 0**.
* Interface 0 is claimed by the kernel driver `snd-usb-audio`, so direct `libusb` control transfers to the standard UAC feature units fail with `LIBUSB_ERROR_BUSY`/`LIBUSB_ERROR_NOT_FOUND` unless we detach the kernel driver (undesirable).
* A workaround was discovered: standard UAC `GET_CUR`/`SET_CUR` requests succeed when the low byte of `wIndex` is set to **interface 3** (`0x03`) instead of interface 0, while the high byte still carries the UAC entity ID.  This allows userspace control without detaching `snd-usb-audio`.

### 1.2 ALSA / PipeWire Topology

* ALSA sees the Wave:3 as a single card with one capture PCM (`pcm0c`) and one playback PCM (`pcm0p`).
* PipeWire currently exposes only the capture source.  The playback PCM (headphones) is not automatically presented as a sink, which is why a custom WirePlumber script is required to create a `wave3-sink` node.
* Sample rate is fixed at **48 kHz**, 24-bit.

---

## 2. Standard USB Audio Class Controls (Working)

All standard UAC controls verified live on the device from Linux using `libusb` with the `wIndex = (entity << 8) | 3` trick.

### 2.1 Control Transfer Encoding

```c
bmRequestType = 0xA1 for GET, 0x21 for SET
bRequest      = 0x81 GET_CUR, 0x01 SET_CUR, 0x82 GET_MIN, 0x83 GET_MAX, 0x84 GET_RES
wValue        = (selector << 8) | channel
wIndex        = (entity << 8) | 3      // 3 = vendor interface workaround
wLength       = 1 for mute, 2 for volume
```

### 2.2 Entity Map

| Entity | Function | Selector 1 | Selector 2 |
|--------|----------|------------|------------|
| 5      | Headphone output | Mute | Volume |
| 6      | Microphone input | Mute | Volume (physical dial) |

### 2.3 Verified Ranges

| Control | Read/Write | Range | Notes |
|---------|------------|-------|-------|
| Mic mute | **RW** | `0`/`1` | Works instantly; LED ring also reflects state |
| Mic gain | **RO** via UAC | **0.0 dB … 40.0 dB** | Physical gain dial on the mic; UAC reports current position but writes are ignored or fail |
| Headphone mute | **RW** | `0`/`1` | Works |
| Headphone volume | **RW** | **-60.0 dB … 0.0 dB** | Works; 0.0 dB is unity |

### 2.4 Implementation

The reference implementation is `native-linux/src/wave3-daemon.c`.  It polls the UAC feature units every 100 ms and exposes state over D-Bus as `org.wave3.Daemon`.

---

## 3. Proprietary Class Config Protocol (Decoded)

### 3.1 What Is Known

* Interface 3 (`0xFF/0xF0`, 0 endpoints) is the vendor control interface.
* Wave Link uses a backend called `VendorUSBLewittDeviceBackend` on macOS and a matching strategy on Windows.
* The protocol is **not** standard HID and **not** standard UAC.  It is a vendor-specific USB control-transfer protocol over endpoint 0.
* Static strings from `waveapi.dll` reference Thesycon-style vendor requests: `TUSBAUDIO_ClassVendorRequestOut`, `THESYCON: OUT CTRL, vendor request with bInterfaceNumber==0`, etc.
* The same library exposes 309 cross-platform control paths including `/dspfx/compressor/*`, `/dspfx/equalizer/band/*`, `/headphone1/*`, `/headphone2/*`, `/line/*`, and `/mixer/N/*`.  These are shared across multiple Lewitt/Elgato devices; not all apply to the Wave:3.

### 3.2 Exact Control-Transfer Encoding

Live probing and the `rikkichy/openwave` Wave XLR project revealed that
the actual request type is **class**, not vendor. The first-generation
Wave:3 implements only three `wValue` IDs:

| Direction | `bmRequestType` | `bRequest` | `wValue` | `wIndex` | `wLength` | Purpose |
|-----------|-----------------|------------|----------|----------|-----------|---------|
| Read (IN)  | `0xA1` | `0x85` | `0x0000` | `0x3303` | 16 | Config block |
| Write (OUT)| `0x21` | `0x05` | `0x0000` | `0x3303` | 16 | Config block |
| Read (IN)  | `0xA1` | `0x85` | `0x0001` | `0x3303` | 8  | Level meter |
| Read (IN)  | `0xA1` | `0x85` | `0x000A` | `0x3303` | 51 | Device info |

`wIndex = 0x3303` is the same encoding trick as the standard UAC case:
high byte `0x33` is the vendor **entity ID**, low byte `0x03` is the
vendor **interface number**. The firmware treats `0x33` as the logical
unit for proprietary controls, while the Linux kernel only needs to see
interface 3 in the low byte (so `snd-usb-audio` does not need to be
detached).

```c
// Config read
libusb_control_transfer(dev, 0xA1, 0x85, 0x0000, 0x3303, cfg, 16, 1000);

// Config write
libusb_control_transfer(dev, 0x21, 0x05, 0x0000, 0x3303, cfg, 16, 1000);

// Meter read
libusb_control_transfer(dev, 0xA1, 0x85, 0x0001, 0x3303, meter, 8, 1000);

// Device info read
libusb_control_transfer(dev, 0xA1, 0x85, 0x000A, 0x3303, info, 51, 1000);
```

**Important:** A broad read-only scan of IDs `0x0000`–`0x00FF` using the
*vendor* request type (`0xC1`) caused the device to reboot into its
DFU/bootloader PID (`0x0071`) before re-enumerating as `0x0070`.
**Always use class requests (`0xA1`/`0x21`) and only probe the known
IDs (`0x0000`, `0x0001`, `0x000A`).**

### 3.3 Config Block Layout (16 bytes)

| Offset | Size | Field | Notes |
|--------|------|-------|-------|
| 0 | u8 | **Dial value low byte** | current dial position in the active mode |
| 1 | u8 | **Dial value high byte** | little-endian with offset 0; mic gain 0–40 dB, HP volume 0 to -128 dB, monitor mix 0–100 |
| 2 | u8 | **unknown / reserved** | writable, no visible effect |
| 3 | u8 | **unknown / reserved** | writable, no visible effect |
| 4 | u8 | **Mic mute** | `0x00` = live, `0x01` = muted |
| 5 | u8 | **Clipguard** | `0x00` = off, `0x01` = on |
| 6 | u8 | **unknown / reserved** | writable, no visible effect |
| 7 | u8 | **Dial flag** | toggles `0x00 <-> 0x80` while adjusting HP volume; sign/fraction flag for displayed value |
| 8 | s8 | **Headphone volume** | signed dB attenuation (`0x00` = 0 dB, `0xF7` ≈ -9 dB, `0xC4` ≈ -60 dB) |
| 9 | u8 | **Headphone mute** | `0x00` = on, `0x01` = muted |
| 10 | u8 | **Indicator / mute-ring R** | RGB red channel for ring feedback |
| 11 | u8 | **Indicator / mute-ring G** | RGB green channel; also physical monitor-mix value (0–100) in mix mode |
| 12 | u8 | **Dial mode** | `0x01` = mic gain, `0x02` = headphone volume, `0x03` = monitor mix |
| 13 | u8 | **Indicator / mute-ring B** | RGB blue channel |
| 14 | u8 | **Software direct monitor mix** | `0x00` = microphone only, `0xFF` = PC playback only, linear scale; independent of the dial |
| 15 | u8 | **LED brightness** | `0x00` = off, `0xFF` = maximum |

**Hardware controls implemented:**

* Mic mute (offset 4)
* Headphone mute (offset 9)
* Headphone volume (offset 8)
* Clipguard (offset 5)
* Mute-ring / indicator RGB color (offsets 10/11/13)
* LED brightness (offset 15)
* Software direct monitor mix (offset 14)

**Physical dial state (read-only):**

* Dial mode (offset 12)
* Dial value (offsets 0/1)
* Indicator RGB (offsets 10/11/13)

**Host-side only:**

* **Low-cut filter** — not present in the Wave:3 config block; Wave Link applies this in software DSP.
* **Headphone color LED** — first-gen Wave:3 has no such LED; `SetHeadphoneColor` returns `G_IO_ERROR_NOT_SUPPORTED`.

**Still unknown:** offsets 2, 3, 6. They accept arbitrary writes but produce no observable change; likely reserved for other firmware variants. Headphone connection state is not exposed in this config block.

### 3.4 Meter Block (`wValue = 0x0001`)

Eight bytes, two little-endian `uint32` values:

```c
uint32_t input_level    = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
uint32_t playback_level = buf[4] | (buf[5] << 8) | (buf[6] << 16) | (buf[7] << 24);
```

The numeric scale is not yet calibrated; the daemon reports
`20 * log10(level / 0x80000000)` as a relative dBFS value.

### 3.5 Device Info Block (`wValue = 0x000A`)

51 bytes. Known fields:

* `data[0]`.`data[1]` — API version (observed `5.3`)
* `data[6]`..`data[8]` — firmware version (observed `0.3.7`)
* `data[27]`..`data[46]` — serial number as ASCII

### 3.6 App-Level Feature Names (from Wave Link binary strings)

These fields exist in Wave Link's session/settings layer. They tell us what the UI exposes, but **not** which ones are sent to the device vs processed in host software:

| Feature | Type | Location |
|---------|------|----------|
| `Clipguard` | boolean | **Hardware** (offset 5) |
| `LowCut` / `LowCut1Enabled` / `LowCut2Enabled` | boolean | **Host DSP** (Wave Link) |
| `MuteColorRGB` | RGB | **Device LED** (offsets 10/11/13) |
| `MicrophoneColorRGB` | RGB | Device LED |
| `HeadphoneColorRGB` | RGB | Not present on first-gen Wave:3 |
| `MixColorRGB` | RGB | UI-only or device LED |
| `GRColorRGB` | RGB | UI-only (gain-reduction meter color) |
| `IndicatorBrightness` / `MuteBrightness` / `BackgroundBrightness` | scalar | Device LED (offset 15) |
| `LedFlip` | boolean | Device LED orientation |
| `InputEnabled` | boolean | Mixer routing |
| `P48Enabled` | boolean | Wave XLR only (phantom power) |
| `LowImpedanceEnabled` | boolean | Wave XLR only |

### 3.7 Fuzzing & Live Probe Results

* A safe fuzzer (`native-linux/src/fuzz_vendor_smart.c`) was run against interface 3 trying common vendor/class request patterns.  No responses to generic patterns; the protocol requires the specific `bRequest`/`wIndex` encoding recovered above.
* Targeted read-only probes using the *vendor* encoding (`0xC1, 0x85,
wValue=candidate_id, wIndex=0x3303`) returned `LIBUSB_ERROR_PIPE` for all
tested IDs.  The same scan using the *class* encoding (`0xA1, 0x85`) only
returned data for IDs `0x0000`, `0x0001`, and `0x000A`.
* A broad read-only scan of IDs `0x0000`–`0x00FF` using the **vendor**
request type (`0xC1`) caused the device to reboot into its DFU/bootloader
PID (`0x0071`) before re-enumerating as `0x0070`.  **Always use class
requests and avoid arbitrary ID scans.**
* An automated byte-probe (`native-linux/src/auto_probe.c`) wrote ten
values to every config offset and read them back. Results produced the
layout table above.
* A physical observatory script (`native-linux/src/poll_observatory.c`)
  logged the config block at 20 Hz while the user cycled dial modes and
  adjusted values. It confirmed:
  * offsets 0/1 form a 16-bit little-endian dial value
  * offset 12 is dial mode (`1`, `2`, `3`)
  * offset 7 toggles `0x80` during HP-volume adjustment
  * offset 11 doubles as the monitor-mix value (0–100)
  * offset 4 toggles with the capacitive mute pad
  * offset 9 auto-sets when HP volume reaches minimum

---

## 4. PipeWire / WirePlumber Integration

### 4.1 Current Status

* The daemon does **not** create or manage PipeWire nodes.  It only controls the Wave:3 hardware.
* PipeWire/WirePlumber configuration is shipped under `native-linux/pipewire/` and `native-linux/wireplumber/`.
* The related project **Undertone** (`https://github.com/polariscli/Undertone`) implements a full PipeWire mixer in Rust and has a working WirePlumber script that creates:
  * `wave3-source` — renamed ALSA capture node
  * `wave3-sink` — a playback sink created from the same ALSA device
  * `wave3-null-sink` — keeps the microphone source active when no app is recording

### 4.2 Recommended Topology

For Wave Link parity on Linux, the following PipeWire graph is recommended (derived from Undertone and extended with hardware controls):

```
App audio ──► wave3-channel-sink (e.g. "System", "Music", "Voice") ──┬──► stream-vol-filter ──► wave3-stream-mix ──► recording apps / OBS
                                                                      └──► monitor-vol-filter ──► wave3-monitor-mix ──► wave3-sink (headphones)

Wave:3 mic ──► wave3-source ──► wave3-mic-mix ──► wave3-stream-mix + wave3-monitor-mix
```

Hardware controls (daemon):

* Mic mute/gain — class config block (offset 4) and UAC
* Headphone mute/volume — class config block (offsets 8 and 9)
* Input/playback level meters — class meter block (`wValue=0x0001`)
* Clipguard — class config block (offset 5)
* Direct monitor mix — class config block (offset 14)
* Mute-ring RGB / brightness — class config block (offsets 10/11/13/15)

### 4.3 Key PipeWire Properties

From Undertone's implementation:

* `factory.name = support.null-audio-sink` for virtual channel/mix sinks
* `node.autoconnect = false` to prevent WirePlumber from auto-linking managed nodes
* `session.suspend-timeout-seconds = 0` to prevent auto-suspend
* Volume control on null sinks uses SPA properties `monitorVolumes` / `monitorMute`

---

## 5. Related Work & References

| Project | Language | What It Provides | Relevance |
|---------|----------|------------------|-----------|
| `wave3ctl` (upstream kernel module) | C + Python | `snd-usb-audio` patch + `amixer`-based CLI | Fallback; requires patching the kernel. Our daemon avoids kernel changes. |
| **openwave** | Python | Wave XLR Linux control app | Discovered the class-based `0xA1/0x21` control encoding and the config/meter/device-info IDs (`0x0000`, `0x0001`, `0x000A`). |
| **Undertone** | Rust | Full PipeWire mixer, app routing, profiles, Qt6 UI | Excellent PipeWire topology reference; HID/vendor protocol is also stubbed/ALSA fallback. |
| `wavelink-ts` docs | TypeScript docs | Wave Link JSON-RPC app protocol | Confirms logical feature names only; not the USB encoding. |
| Wave Link 3.0 (Windows/macOS) | C++/C#/Swift | Official driver/application | Static source of 309 control paths and backend strategy names. |

---

## 6. Implementation Deliverables

| Component | Path | Status |
|-----------|------|--------|
| UAC daemon (C/GDBus) | `native-linux/src/wave3-daemon.c` | Working |
| Automated config probe | `native-linux/src/auto_probe.c` | Working |
| Config probe helper | `native-linux/src/cfg_probe.c` | Working |
| Live observatory logger | `native-linux/src/poll_observatory.c` | Working |
| Config probe helper | `native-linux/src/cfg_probe.c` | Working |
| Live state watcher | `native-linux/src/watch_state.c` | Working |
| Device info reader | `native-linux/src/device_info.c` | Working |
| Observatory logger | `native-linux/src/poll_observatory.c` | Working |
| Shell CLI | `native-linux/bin/wave3ctl` | Working |
| udev rules | `native-linux/udev/50-elgato-wave3.rules` | Installed |
| systemd service | `native-linux/systemd/wave3-daemon.service` | Installed |
| D-Bus session activation | `native-linux/dbus/org.wave3.Daemon.service` | Installed |
| D-Bus system policy (optional) | `native-linux/dbus/org.wave3.Daemon-system.conf` | Created |
| ALSA UCM profile | `native-linux/alsa-ucm/Elgato Wave 3.conf` + `HiFi.conf` | Created |
| PipeWire virtual sinks | `native-linux/pipewire/pipewire.conf.d/wave3-mix-sinks.conf` | Created |
| WirePlumber auto-start rule | `native-linux/wireplumber/wave3.lua` | Created |
| GTK4 GUI applet | `native-linux/gui/wave3-applet.py` | Created |
| Integration tests | `native-linux/tests/test-dbus.py` | Passing |
| Arch package | `native-linux/pkg/PKGBUILD` | Created |
| Upstream `wave3ctl` vendored fallback | `native-linux/wave3ctl/` | Vendored |
| Protocol notes | `native-linux/docs/protocol-notes.md` | Updated |
| **This summary** | `WAVE3_PROTOCOL_SUMMARY.md` | Updated |

---

## 7. Remaining Unknowns

1. **Offsets 2, 3, 6** are writable but unused on this firmware; they may be
   reserved for other device variants.
2. **Meter scale** — the full-scale reference for the `uint32` input/playback
   level values has not been calibrated.

These unknowns do not block any user-facing feature; all hardware controls
on the first-generation Wave:3 are implemented.

---

## 8. License

The native-linux implementation is released under the same license as the upstream `wave3ctl` project (see `native-linux/LICENSE`).  Static analysis was performed on locally downloaded, publicly released Elgato software for interoperability purposes.
