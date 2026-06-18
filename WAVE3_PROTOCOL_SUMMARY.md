# Elgato Wave:3 — Native Linux Communication & Management Protocol Summary

**Repo:** `/home/USER/wave3-audio-report`  
**Device:** Elgato Wave:3 USB condenser microphone  
**USB IDs:** `VID 0x0fd9`, `PID 0x0070`  
**Date:** 2026-06-18  
**Status:** Standard UAC controls fully working; proprietary vendor protocol partially understood and blocked on live `usbmon` capture from Wave Link in a Windows VM.

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

## 3. Proprietary Vendor Protocol (Partial / Blocked)

### 3.1 What Is Known

* Interface 3 (`0xFF/0xF0`, 0 endpoints) is the vendor control interface.
* Wave Link uses a backend called `VendorUSBLewittDeviceBackend` on macOS and a matching strategy on Windows.
* The protocol is **not** standard HID and **not** standard UAC.  It is almost certainly a vendor-specific USB control-transfer protocol over endpoint 0, likely using `bmRequestType = 0x40` (vendor, device-to-host, no data stage) or `0xC0` for reads, with a custom `bRequest` and a property ID encoded in `wValue`/`wIndex`.
* Static strings from `waveapi.dll` reference Thesycon-style vendor requests: `TUSBAUDIO_ClassVendorRequestOut`, `THESYCON: OUT CTRL, vendor request with bInterfaceNumber==0`, etc.
* The same library exposes 309 cross-platform control paths including `/dspfx/compressor/*`, `/dspfx/equalizer/band/*`, `/headphone1/*`, `/headphone2/*`, `/line/*`, and `/mixer/N/*`.  These are shared across multiple Lewitt/Elgato devices; not all apply to the Wave:3.

### 3.2 App-Level Feature Names (from Wave Link binary strings)

These fields exist in Wave Link's session/settings layer.  They tell us what the UI exposes, but **not** which ones are sent to the device vs processed in host software:

| Feature | Type | Likely Location |
|---------|------|-----------------|
| `Clipguard` | boolean | **Hardware** (Elgato confirms it runs inside the microphone) |
| `LowCut` / `LowCut1Enabled` / `LowCut2Enabled` | boolean | **Host DSP** (filter in Wave Link) |
| `MuteColorRGB` | RGB | **Device LED** (must be USB) |
| `MicrophoneColorRGB` | RGB | Device LED |
| `HeadphoneColorRGB` | RGB | Device LED |
| `MixColorRGB` | RGB | UI-only or device LED |
| `GRColorRGB` | RGB | UI-only (gain-reduction meter color) |
| `IndicatorBrightness` / `MuteBrightness` / `BackgroundBrightness` | scalar | Device LED |
| `LedFlip` | boolean | Device LED orientation |
| `InputEnabled` | boolean | Mixer routing |
| `P48Enabled` | boolean | Wave XLR only (phantom power) |
| `LowImpedanceEnabled` | boolean | Wave XLR only |

### 3.3 What Is NOT in the Binary

* **No HID interface** was found in the device descriptors or in Wave Link's `waveapi.dll` strings.
* **No RGB/LED strings** were found in `waveapi.dll`.  LED color handling appears to live in the platform-specific application layer (Wave Link UI), not the shared cross-platform audio backend.
* Therefore, the exact USB encoding for RGB and direct-monitor cannot be recovered from static analysis alone.

### 3.4 Fuzzing Results

A safe fuzzer (`native-linux/src/fuzz_vendor_smart.c`) was run against interface 3 trying common vendor/class request patterns:

* `bmRequestType = 0xC0 / 0x40` with `bRequest` from 0x00..0xFF
* `wValue`/`wIndex` combinations using guessed property IDs
* Class requests (`0xA1`/`0x21`) targeting interface 3

**Result:** no responses to generic patterns.  The protocol requires a specific encoding that cannot be brute-forced safely without a reference capture.

### 3.5 Guessed Encoding

Based on Thesycon conventions and the fact that the device has no bulk/interrupt endpoints, the most probable format is:

```c
// Vendor write (no data stage)
bmRequestType = 0x40;   // vendor, OUT, device
bRequest      = ??;     // unknown (likely 0x01..0x10)
wValue        = prop_id;
wIndex        = (sub-index << 8) | 3;
wLength       = 0 or small payload

// Vendor read
bmRequestType = 0xC0;   // vendor, IN, device
bRequest      = ??;
wValue        = prop_id;
wIndex        = (sub-index << 8) | 3;
wLength       = 1, 2, or 4 bytes
```

This is **only a hypothesis**.  The actual `bRequest` values and property IDs require a live `usbmon`/tshark capture.

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

* Mic mute/gain — UAC entity 6
* Headphone mute/volume — UAC entity 5
* Direct monitor mix / LED colors — vendor protocol (pending capture)

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
| **Undertone** | Rust | Full PipeWire mixer, app routing, profiles, Qt6 UI | Excellent PipeWire topology reference; HID/vendor protocol is also stubbed/ALSA fallback. |
| `wavelink-ts` docs | TypeScript docs | Wave Link JSON-RPC app protocol | Confirms logical feature names only; not the USB encoding. |
| Wave Link 3.0 (Windows/macOS) | C++/C#/Swift | Official driver/application | Static source of 309 control paths and backend strategy names. |

---

## 6. Implementation Deliverables

| Component | Path | Status |
|-----------|------|--------|
| UAC daemon (C/GDBus) | `native-linux/src/wave3-daemon.c` | Working |
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
| **This summary** | `WAVE3_PROTOCOL_SUMMARY.md` | **Created** |

---

## 7. Remaining Unknowns

1. **Vendor `bRequest` values** for reads/writes.
2. **Property ID table** for RGB, direct monitor, clipguard, low-cut, level meters.
3. **Data format** for each property (bool, u8, u16, RGB triple, float).
4. Whether **Low-cut** is a hardware toggle or host-side DSP in Wave Link. (Clipguard and direct monitor mix are confirmed hardware.)
5. Whether the **level meters** are exposed via UAC peak meters on an unused terminal/unit, or only via vendor requests.

---

## 8. Next Steps

1. **Capture:** Run Windows 11 VM with Wave:3 USB passthrough, install Wave Link, and record all USB control traffic with `usbmon` + `tshark` while toggling every feature.
2. **Correlate:** Match observed vendor requests with the 309 known control paths and the feature names from Wave Link strings.
3. **Implement:** Replace the placeholder vendor functions in `wave3-daemon.c` with real `libusb_control_transfer` calls.
4. **Verify:** Test RGB, Clipguard, low-cut, direct monitor, and level meters on the live Wave:3.
5. **Polish:** Update GUI, WirePlumber script, packaging, and documentation.

### 8.1 Exact Capture Commands for the User

```bash
# On the Linux host (requires root)
sudo modprobe usbmon
lsusb | grep -i elgato   # note Bus and Device numbers, e.g. 005/090
sudo tshark -i usbmon5 -f "usb.idVendor == 0x0fd9 and usb.idProduct == 0x0070" \
    -w wave3-wavelink.pcapng -F pcapng

# In the VM, run Wave Link and toggle, one at a time:
#   - Mic mute/unmute
#   - Headphone mute/unmute
#   - Headphone volume
#   - Mic gain dial
#   - Clipguard on/off
#   - Low-cut on/off
#   - Direct monitor / monitor mix
#   - Mute color / headphone color / brightness
#   - Start/stop audio playback
```

Upload the resulting `wave3-wavelink.pcapng` to `native-linux/captures/` for decoding.

---

## 9. License

The native-linux implementation is released under the same license as the upstream `wave3ctl` project (see `native-linux/LICENSE`).  Static analysis was performed on locally downloaded, publicly released Elgato software for interoperability purposes.
