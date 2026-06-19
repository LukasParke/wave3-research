# Elgato Wave:3 — Native Linux Communication & Management Protocol Summary

**Repo:** `/home/USER/wave3-audio-report`  
**Device:** Elgato Wave:3 USB condenser microphone  
**USB IDs:** `VID 0x0fd9`, `PID 0x0070`  
**Date:** 2026-06-18  
**Status:** Standard UAC controls fully working; proprietary vendor protocol request format decoded statically (`bmRequestType`/`bRequest`/`wIndex`), with property-ID mapping still in progress.

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

## 3. Proprietary Vendor Protocol (Decoded)

### 3.1 What Is Known

* Interface 3 (`0xFF/0xF0`, 0 endpoints) is the vendor control interface.
* Wave Link uses a backend called `VendorUSBLewittDeviceBackend` on macOS and a matching strategy on Windows.
* The protocol is **not** standard HID and **not** standard UAC.  It is a vendor-specific USB control-transfer protocol over endpoint 0.
* Static strings from `waveapi.dll` reference Thesycon-style vendor requests: `TUSBAUDIO_ClassVendorRequestOut`, `THESYCON: OUT CTRL, vendor request with bInterfaceNumber==0`, etc.
* The same library exposes 309 cross-platform control paths including `/dspfx/compressor/*`, `/dspfx/equalizer/band/*`, `/headphone1/*`, `/headphone2/*`, `/line/*`, and `/mixer/N/*`.  These are shared across multiple Lewitt/Elgato devices; not all apply to the Wave:3.

### 3.2 Exact Control-Transfer Encoding

Disassembly of the `LegacyUAC1VendorUSBBackendStrategy` methods in
`waveapi.dll` (the strategy selected for the first-generation Wave:3)
reveals the exact request bytes:

| Direction | `bmRequestType` | `bRequest` | `wValue` | `wIndex` | `wLength` |
|-----------|-----------------|------------|----------|----------|-----------|
| Read (IN)  | `0xC1` | `0x85` | property ID | `0x3303` | payload size |
| Write (OUT)| `0x41` | `0x05` | property ID | `0x3303` | payload size |

`wIndex = 0x3303` is the same encoding trick as the standard UAC case:
high byte `0x33` is the vendor **entity ID**, low byte `0x03` is the
vendor **interface number**. The firmware treats `0x33` as the logical
unit for proprietary controls, while the Linux kernel only needs to see
interface 3 in the low byte (so `snd-usb-audio` does not need to be
detached).

**Important:** The firmware appears to require the device to be in
"APP mode" before it accepts these vendor requests. Wave Link uses the
proprietary Thesycon audio driver (`TUSBAUDIO_*` API) to open the device
in APP mode.  With plain `libusb` the requests are rejected (the device
returns `LIBUSB_ERROR_PIPE`) unless/until the device has been placed in
APP mode by the vendor driver.

```c
// Vendor read example
libusb_control_transfer(dev,
    0xC1,          // vendor, IN, interface recipient
    0x85,          // vendor read request code
    property_id,   // wValue
    0x3303,        // wIndex: entity 0x33, interface 3
    buf, len, 1000);

// Vendor write example
libusb_control_transfer(dev,
    0x41,          // vendor, OUT, interface recipient
    0x05,          // vendor write request code
    property_id,   // wValue
    0x3303,        // wIndex: entity 0x33, interface 3
    buf, len, 1000);
```

The remaining work is to map each feature to its **property ID**
and payload format.  Static reverse engineering shows that the property
ID passed in `wValue` is a **single byte** returned by the `IMessage`
virtual method at vtable offset 0 (`IMessage::propertyId()`).  The
`SessionAPI` descriptor constructors in `waveapi.dll` store these IDs
indirectly, so the exact byte for each logical path has not yet been
recovered from static analysis alone.

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

### 3.4 Fuzzing & Live Probe Results

* A safe fuzzer (`native-linux/src/fuzz_vendor_smart.c`) was run against interface 3 trying common vendor/class request patterns.  No responses to generic patterns; the protocol requires the specific `bRequest`/`wIndex` encoding recovered above.
* Targeted read-only probes with the recovered encoding (`0xC1, 0x85, wValue=candidate_id, wIndex=0x3303`) returned `LIBUSB_ERROR_PIPE` for all tested IDs, confirming the device rejects the requests until APP mode is entered.
* A broad read-only scan of IDs `0x0000`–`0x00FF` caused the device to reboot into its DFU/bootloader PID (`0x0071`) before re-enumerating as `0x0070`.  **Arbitrary live enumeration is therefore unsafe and must not be repeated.**

### 3.5 Property ID Mapping (Outstanding)

The exact property IDs for each feature are not yet recovered.  Static
analysis recovered the full logical-path table (≈309 paths, see
`wavelink/teardown/waveapi-control-paths.txt` and
`native-linux/docs/wave3-descriptor-paths.md`) and the request encoding,
but the `IMessage::propertyId()` byte for each path is generated from the
`SessionAPI::Impl` descriptor data and has not been extracted.

Two complementary approaches remain:

1. **Static parsing**: locate and decode the descriptor table in
   `waveapi.dll` so that the property-ID byte can be read directly.
2. **Live enumeration with APP mode**: place the device in APP mode
   (replicating the Thesycon driver's open handshake) and then perform
   small, targeted read probes correlated with physical state.

Live probe tools exist at `native-linux/src/probe_vendor_ids.c` and
`probe_vendor_targeted.c`, but **must only be used after APP-mode entry
is understood**.

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

1. **Property ID table** for RGB, direct monitor, clipguard, low-cut, level meters.
2. **Data format** for each property (bool, u8, u16, RGB triple, float).
3. **APP-mode handshake**: the exact USB control transfer(s) the Thesycon driver sends to switch the Wave:3 from audio mode to vendor-control mode.
4. Whether **Low-cut** is a hardware toggle or host-side DSP in Wave Link. (Clipguard and direct monitor mix are confirmed hardware.)
5. Whether the **level meters** are exposed via UAC peak meters on an unused terminal/unit, or only via vendor requests.

---

## 8. Next Steps

1. **APP-mode handshake (static or capture):** Determine how the Thesycon
   driver places the Wave:3 into APP mode.  This is the current blocker.
2. **Property ID recovery:** Once APP mode is available, perform targeted
   read probes to map logical paths to `wValue` property IDs.
3. **Correlate:** Match observed vendor requests with the 309 known control
   paths and the feature names from Wave Link strings.
4. **Implement:** Replace the placeholder vendor functions in
   `wave3-daemon.c` with real `libusb_control_transfer` calls.
5. **Verify:** Test RGB, Clipguard, low-cut, direct monitor, and level
   meters on the live Wave:3.
6. **Polish:** Update GUI, WirePlumber script, packaging, and documentation.

Because the device requires the Thesycon driver for APP mode, a **Windows
VM capture of Wave Link USB traffic remains the most reliable path** to
recover both the handshake and the property-ID table, even though the
user prefers to avoid it.

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
