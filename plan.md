# Elgato Wave:3 Native Linux Support — Investigation & Implementation Plan

**Status:** Core implementation complete and verified on the physical device.  
**Protocol summary:** See [`WAVE3_PROTOCOL_SUMMARY.md`](WAVE3_PROTOCOL_SUMMARY.md) for the complete, consolidated reference.  
**Host environment:** Linux only. All investigation, reverse engineering, and testing were done from the existing Linux host.  
**Goal:** Build a native, first-class Linux integration for the Elgato Wave:3.

## Results Summary

* The Wave:3's core controls (mic mute, headphone mute, headphone volume)
  are standard **USB Audio Class 1.0** feature units, not proprietary
  vendor commands.
* `snd-usb-audio` owns AudioControl interface 0 and blocks userspace UAC
  transfers, but the same transfers can be routed through the unclaimed
  vendor interface 3 (`wIndex = (entity << 8) | 3`) and the firmware
  accepts them.
* A native **C/GDBus daemon** (`wave3-daemon`) and **CLI** (`wave3ctl`)
  have been built and tested live on the connected Wave:3.
* Mic gain is read-only via UAC because it is controlled by the physical
  dial; headphone volume and both mutes can be set from software.
* PipeWire/WirePlumber integration was improved using patterns from the
  [Undertone](https://github.com/polariscli/Undertone) project:
  `wave3-source` rename, custom `wave3-sink`, and `wave3-null-sink` to
  keep the mic awake.
* The proprietary class-based protocol on interface 3 was decoded down to
  three live IDs (`wValue = 0x0000` config block, `0x0001` meter, `0x000A`
  device info) using the same trick discovered by `rikkichy/openwave`.
* Mic mute, headphone mute, headphone volume, and level meters are now
  implemented via the 16-byte config block.
* Advanced features (RGB, Clipguard, low-cut, direct monitor) still require
  mapping the remaining config bytes. This can be done by physically
  observing each byte or by a live Wave Link `usbmon` capture.

See [`native-linux/README.md`](native-linux/README.md) for the working
control stack.

---

**Host environment:** Linux only. No switching to Windows or macOS is required for testing; all reverse engineering, capture, and development will be done from the existing Linux host using VMs, static analysis, and Linux-native tools.  
**Goal:** Build a native, first-class Linux integration for the Elgato Wave:3 by extracting the vendor USB control protocol from the official Wave Link applications and bridging it into the standard Linux audio stack (ALSA / PipeWire).

This plan is derived from the evidence collected in this repo:

- `README.md` — full USB/ALSA/PipeWire enumeration of the connected Wave:3.
- `wavelink/README.md` — tear-down of Wave Link 3.0 for Windows and macOS.
- `wavelink/windows/msix_extracted/` — full extracted Windows application.
- `wavelink/mac/dmg_extracted/` — full extracted macOS application bundle.
- `wavelink/teardown/` — string dumps, API surfaces, and control-path lists.

The Wave:3 core audio path is standard USB Audio Class 1.0 and already works on Linux. The missing functionality lives entirely on the vendor-specific USB interface (`0xFF/0xF0`) and in Wave Link’s software mixer. All of these settings are observable because Wave Link applies them live to the microphone.

---

## 1. Investigation Objectives

### 1.1 Reverse-engineer the vendor USB protocol

The Wave Link applications use a shared low-level library (`waveapi.dll` / `WaveAPI.framework`, internal namespace `LWT`). It opens the Wave:3’s vendor-specific USB interface and reads/writes settings as a key-value protocol. We must determine:

1. **Physical transport layer**
   - Are settings sent as USB control transfers (`bmRequestType` = vendor) to endpoint 0?
   - Or as small bulk/interrupt messages on interface 3 endpoints?
   - Does the protocol use `libusb` control transfers (`libusb_control_transfer`) or bulk transfers?
   - What are the exact request type, request ID, value, index, and payload lengths?

2. **Message framing and encoding (resolved)**
   - The protocol uses standard USB class control transfers
     (`bmRequestType = 0xA1` for GET, `0x21` for SET,
     `bRequest = 0x85` / `0x05`).
   - Payloads are fixed-length little-endian binary blocks:
     16-byte config, 8-byte meter, 51-byte device info.
   - No header, CRC, or sequence number is used.

3. **Property addressing scheme (resolved for known IDs)**
   - The live Wave:3 only exposes three `wValue` IDs:
     `0x0000` config block, `0x0001` meter, `0x000A` device info.
   - The 309 logical paths map onto bytes inside the 16-byte config block
     (and possibly onto bits within those bytes). The exact byte for each
     path is still being mapped.

4. **Data types and scales (partially resolved)**
   - Booleans are single bytes (`0x00`/`0x01`).
   - Headphone volume is a signed 8-bit dB attenuation (0 dB = max,
     -60 dB = min, 1 dB steps).
   - Mic gain remains read-only via standard UAC (physical dial).
   - RGB colors and other scalar types are still unknown.

5. **Transaction semantics (resolved)**
   - Every `Set` returns the number of bytes written as the USB control
     transfer status; no separate response packet.
   - The daemon polls the config block at 10 Hz; no async interrupt or
     heartbeat message is required for the first-gen Wave:3.
   - Concurrent open from two hosts will fail at `libusb_claim_interface`.

6. **Device initialization / handshake**
   - No special handshake is required for the first-gen Wave:3. The class
     control transfers succeed immediately after claiming interface 3.
   - The device info request (`wValue=0x000A`) can be read at any time and
     returns API/firmware version and serial number.

### 1.2 Map every observable Wave:3 setting to its control path and USB payload

From the Wave Link tear-down, the following settings exist. Each must be exercised in the official app while capturing USB traffic, then correlated with the control path and final byte payload.

#### Input / microphone settings

| # | Setting | Control path hints | Type | Expected values |
|---|---------|-------------------|------|-----------------|
| 1 | Microphone gain | `/gain`, `/input_gain` | float dB | e.g. 0 dB to +40 dB |
| 2 | Microphone mute | `/mute`, `/input_mute` | bool | on / off |
| 3 | Clipguard anti-clip | `/clipguard_enable` | bool | on / off |
| 4 | Low-cut filter | `/lowcut_enable`, `/lowcut_enabled`, `/lowcut1_enabled` | bool | on / off |
| 5 | Limiter bypass | `/limiter_bypassed` | bool | on / off |
| 6 | Input VST insert (if MK.2) | `/input/vst_enabled` | bool | on / off |

#### Headphone / monitor settings

| # | Setting | Control path hints | Type | Expected values |
|---|---------|-------------------|------|-----------------|
| 7 | Headphone volume | `/headphone_volume`, `/headphone1/volume` | float dB | e.g. -60 dB to 0 dB |
| 8 | Headphone mute | `/headphone_mute`, `/headphone1/mute` | bool | on / off |
| 9 | Mic/PC monitor mix | `/direct_monitor` | float | 0.0 = mic only, 1.0 = PC only |
| 10 | Headphone RGB color | `/headphone_color_rgb/{red,green,blue}` | uint8 | 0–255 per channel |

#### LED / RGB settings

| # | Setting | Control path hints | Type | Expected values |
|---|---------|-------------------|------|-----------------|
| 11 | Mute-ring color | `/microphone_color_rgb/{red,green,blue}` | uint8 | 0–255 per channel |
| 12 | Mute background brightness | `/mute_background`, `/mute_brightness` | uint8 | 0–255 |
| 13 | Indicator brightness | `/indicator_brightness` | uint8 | 0–255 |
| 14 | LED flip / orientation | `/led_flip` | bool | on / off |

#### Firmware / device identity

| # | Setting | Control path hints | Type | Expected values |
|---|---------|-------------------|------|-----------------|
| 15 | Firmware version major | `/version/serial`, `FirmwareVersionMajor` | uint8 | read-only |
| 16 | Firmware version minor | `FirmwareVersionMinor` | uint8 | read-only |
| 17 | Firmware version patch | `FirmwareVersionPatch` | uint8 | read-only |
| 18 | Serial number | `/version/serial` | string | read-only |
| 19 | Hardware version board | `/hw_version_board` | string / uint | read-only |

#### Mixer / routing settings (Wave Link software mixer)

| # | Setting | Control path hints | Type | Expected values |
|---|---------|-------------------|------|-----------------|
| 20 | Mix input enable | `/mixer/<mix>/input_enabled/<input>` | bool | on / off |
| 21 | Mix input volume | `/mixer/<mix>/input_volume/<input>` | float dB | per-channel mix level |
| 22 | Mix FX select | `/mixer/<mix>/fx_select/<input>` | enum | effect rack selection |
| 23 | USB output source select | `/usb_outputs/<n>/source_select` | enum | mix source selection |
| 24 | USB output limiter bypass | `/usb_outputs/<n>/limiter_bypassed` | bool | on / off |
| 25 | Hardware output source select | `/hw_outputs/<n>/source_select` | enum | mix source selection |
| 26 | Hardware output level | `/hw_output/<n>/level_dB/<ch>` | float dB | read-only meter |

#### DSP effects (Wave:3 MK.2 / Wave Next only)

| # | Setting | Control path hints | Type | Expected values |
|---|---------|-------------------|------|-----------------|
| 27 | Equalizer band enable | `/dspfx/equalizer/band/<i>/enabled` | bool | on / off |
| 28 | Equalizer band frequency | `/dspfx/equalizer/band/<i>/frequency_Hz` | float Hz | 20–20000 |
| 29 | Equalizer band gain | `/dspfx/equalizer/band/<i>/gain_dB` | float dB | ±N dB |
| 30 | Equalizer band Q | `/dspfx/equalizer/band/<i>/quality_log10` | float | Q factor |
| 31 | Equalizer band type | `/dspfx/equalizer/band/<i>/type` | enum | low-shelf, peaking, high-shelf, etc. |
| 32 | Compressor threshold | `/dspfx/compressor/threshold_dB` | float dB | |
| 33 | Compressor ratio | `/dspfx/compressor/ratio` | float | |
| 34 | Compressor attack | `/dspfx/compressor/attack_ms` | float ms | |
| 35 | Compressor release | `/dspfx/compressor/release_ms` | float ms | |
| 36 | Compressor makeup gain | `/dspfx/compressor/makeup_gain_dB` | float dB | |

#### Level meters / telemetry

| # | Setting | Control path hints | Type | Expected values |
|---|---------|-------------------|------|-----------------|
| 37 | Input level dB | `/input/<n>/level_dB/<ch>` | float dB | read-only |
| 38 | Hardware input level | `/hw_input/level/<ch>` | float dB | read-only |
| 39 | USB host playback level | `/usb_host_playback/level/<ch>` | float dB | read-only |
| 40 | Clip / mic clipping counter | `/mic_clipping_counter` | uint | read-only |
| 41 | Microphone detect threshold | `/mic_det_threshold/<ch>` | float | |
| 42 | Microphone detect value | `/mic_det_value/<ch>` | float | read-only |

### 1.3 Understand the event / interrupt channel

The Wave:3 exposes an interrupt IN endpoint (`0x83`, 2 bytes, interval 128 ms) in the USB descriptor. Investigate:

1. What events are pushed over `0x83`?
   - Tap-to-mute toggle?
   - Dial rotation / button press?
   - Headphone plug/unplug?
   - Gain-change acknowledgments?
   - Async property-change notifications?

2. Is the interrupt payload a standard UAC1 interrupt message (`bStatus`, `bOriginator`) or a custom vendor event?

3. Does Wave Link poll the device at a fixed rate, or does it rely on async interrupts?

### 1.4 Understand firmware update protocol

1. What DFU mode does the Wave:3 enter? Standard USB DFU or Thesycon-specific?
2. What is the image format? `.bin`, `.dfu`, or Thesycon `.tlb` / `.raw`?
3. Can `dfu-util` flash the device, or is a Thesycon-specific tool required?
4. Map the `tlusbdfuapi` / `TUSBAUDIO_*DFU*` API calls to actual USB DFU requests.
5. Extract any public-key / signature / checksum verification logic from `tlusbdfuapi.dll`.

### 1.5 Replicate Wave Link’s software mixer in PipeWire

Wave Link creates virtual audio devices such as:

- `Wave Link Stream` (capture)
- `Wave Link Microphone` (capture)
- `Wave Link Chat` (capture)
- Possibly `Wave Link Virtual ASIO`

Investigate:

1. What exact audio formats, channel counts, and sample rates do these virtual devices expose?
2. How does Wave Link route application audio to these devices? (Windows audio session / macOS aggregate devices)
3. What latency and buffer sizes are used?
4. How is the hardware capture channel mixed into the stream?
5. Can the same behavior be reproduced with PipeWire null sinks, loopbacks, and filter chains?

### 1.6 Cross-platform protocol verification (from Linux)

1. Compare the disassembled protocol code in `waveapi.dll` (Windows) and `WaveAPI.framework` (macOS) from the Linux analysis environment.
2. If a macOS guest VM is available, capture the same setting change there; otherwise rely on binary cross-reference.
3. Verify that the USB payload encoding is byte-for-byte identical. (The 309 shared control paths strongly suggest it is.)
4. Document any OS-specific initialization differences found in the binaries.
5. Confirm that the protocol does not depend on Thesycon driver state — it should work through `libusb` on Linux.

---

## 2. Reverse-Engineering Environments (All From Linux)

All investigation will be performed from the existing Linux host. The official Windows and macOS Wave Link installers have already been downloaded and extracted into this repository, so no OS switching is required. Use the following approaches from Linux.

### 2.1 Live USB traffic capture from a Windows Wave Link guest (QEMU/KVM)

Because the Wave Link application is Windows-only for full hardware control, run it inside a Windows virtual machine on the Linux host and capture the USB traffic on the host side.

- **Hypervisor:** QEMU/KVM with SPICE or virt-manager.
- **USB passthrough:** Pass the entire Wave:3 composite device (all 5 interfaces) to the Windows guest. Use one of:
  - `virt-manager` device passthrough
  - QEMU command line: `-device usb-host,hostbus=5,hostaddr=72`
  - USB controller PCIe passthrough for cleaner timing
- **Capture tools on Linux host:**
  - **`usbmon`** kernel module + **Wireshark** for raw USB traffic on the host bus.
  - **`tshark`** for headless capture.
  - Save captures as `.pcapng`.
- **Procedure:**
  1. Load `usbmon`: `sudo modprobe usbmon`.
  2. Identify the Wave:3 bus/device from `lsusb` (e.g. `005:072`).
  3. Start Wireshark capture on `usbmon5` (or the appropriate bus).
  4. Start the Windows VM and open Wave Link.
  5. Change one setting at a time in Wave Link, wait 2–3 seconds, change back.
  6. Annotate the capture with timestamps and UI values.
  7. Stop capture and export the `.pcapng` into `native-linux/captures/`.

This approach keeps Linux as the host OS while still observing the official application’s real USB behavior.

### 2.2 Live USB traffic capture under Wine (experimental fallback)

As a Linux-native alternative to a VM, attempt to run the extracted `Elgato.WaveLink.exe` under **Wine** with USB device access.

- **Tools:** `wine`, `winetricks`, optional `wineusb`/`libusb` Wine USB support.
- **Capture:** `usbmon` + Wireshark, same as above.
- **Caveats:**
  - Wave Link is a WinUI 3 / Windows App SDK 1.8 application and may not launch under Wine.
  - Native USB access from Wine can be fragile.
  - Treat this as a convenience fallback; the QEMU/KVM path is authoritative.
- **If it works:** It avoids the VM overhead and gives cleaner captures.

### 2.3 Static binary analysis on Linux

The extracted Wave Link binaries are already present in the repo and can be analyzed directly from Linux.

- **Disassemblers / decompilers:**
  - **Ghidra** (Java-based, runs natively on Linux).
  - **rizin** / **radare2** / **Cutter** (native Linux).
  - **objdump** / `llvm-objdump` for quick PE/Mach-O inspection.
- **.NET assembly inspection:**
  - **ilspycmd** (.NET CLI decompiler, runs on Linux via `dotnet`).
  - **dnSpyEx** under Wine if a GUI is needed.
- **String and metadata extraction:**
  - `strings -n`, `file`, `binwalk`, `pextractor`.
- **Primary targets:**
  - `wavelink/windows/msix_extracted/waveapi.dll` — vendor protocol backend.
  - `wavelink/windows/msix_extracted/EWLWAudioEngine.dll` — audio engine and SWIG C# bindings.
  - `wavelink/windows/msix_extracted/tlusbdfuapi.dll` — DFU update API.
  - `wavelink/windows/msix_extracted/Elgato.WaveLink.AppLogic.dll` — C# setting/session field names and PID list.
  - `wavelink/mac/dmg_extracted/.../WaveAPI.framework/Versions/A/WaveAPI` — cross-reference.
  - `wavelink/mac/dmg_extracted/.../tlusbdfuapi.framework/Versions/A/tlusbdfuapi` — cross-reference.

### 2.4 Runtime instrumentation from Linux

If the protocol backend can be isolated:

- Build a tiny Linux `libusb` probe that claims interface 3 and sends candidate vendor requests.
- Use information from static analysis to predict valid request shapes.
- Fuzz small numeric property IDs and observe device responses (stalls, length changes, or state changes).
- **Safety:** Only perform fuzzing on a device whose firmware can be recovered via DFU. Avoid destructive writes until the protocol is understood.

### 2.5 Cross-reference captures with static findings

For every captured USB transaction:

1. Note the control-path string from Wave Link UI / static analysis.
2. Find the corresponding raw USB request in the `.pcapng`.
3. Decode the payload using the framing rules discovered in static analysis.
4. Build a mapping table: `control path → property ID → request bytes → response bytes`.
5. Verify the same mapping works on both Windows and macOS binaries by comparing their disassembled protocol code.

---

## 3. Implementation Plan

### Phase 0 — Foundation (no reverse engineering yet)

1. Create a clean project skeleton under the repo:
   - `native-linux/src/` — daemon source code
   - `native-linux/include/` — public headers
   - `native-linux/udev/` — udev rules
   - `native-linux/pipewire/` — PipeWire configuration snippets
   - `native-linux/docs/` — protocol notes
   - `native-linux/tests/` — unit and integration tests

2. Confirm the existing Linux audio path is sufficient:
   - Test capture at 48 kHz and 96 kHz.
   - Test playback at 48 kHz and 96 kHz.
   - Verify ALSA mixer controls behave correctly.
   - Measure round-trip latency with `alsa_delay` / `jack_iodelay`.

3. Write a minimal `libusb` program that:
   - Opens the Wave:3 by VID/PID.
   - Claims interface 3 (vendor).
   - Reads the USB configuration descriptor.
   - Dumps all string descriptors.
   - This becomes the basis for the daemon.

### Phase 1 — Protocol Reverse Engineering

1. Build a USB capture annotation tool/script to:
   - Read `.pcapng` files.
   - Filter control transfers to/from the Wave:3.
   - Correlate transactions with logged UI actions.
   - Output candidate request/response pairs.

2. Capture and document the following baseline traffic:
   - Device enumeration / open sequence.
   - Read of `/version/serial` and firmware version.
   - Read of all input/output level meters.

3. Capture per-setting traffic for at least:
   - Microphone gain (min, mid, max)
   - Microphone mute toggle
   - Headphone volume (min, mid, max)
   - Headphone mute toggle
   - Clipguard toggle
   - Low-cut toggle
   - RGB mute-ring color (red, green, blue, white, black)
   - Direct monitor mix (if exposed for Wave:3)

4. Derive the property-ID lookup table by:
   - Static analysis of `waveapi.dll` data sections.
   - Comparing captured payloads against the 309 known control paths.
   - Brute-forcing small numeric IDs if the table is sparse.

5. Write a decoding/encoding test harness that replays captured packets and verifies round-trip serialization.

### Phase 2 — Userspace Daemon

Create `wave3-daemon`:

1. **Responsibilities**
   - Open Wave:3 vendor interface via `libusb`.
   - Maintain a cache of device settings.
   - Expose a D-Bus service (`com.elgato.Wave3` or `org.elgato.Wave3`).
   - Poll or subscribe to async device events.
   - Handle device hotplug.

2. **D-Bus API surface** (initial)
   - `GetDeviceInfo()` → serial, firmware version, model
   - `GetGain()` / `SetGain(dB)`
   - `GetHeadphoneVolume()` / `SetHeadphoneVolume(dB)`
   - `GetMute()` / `SetMute(bool)`
   - `GetHeadphoneMute()` / `SetHeadphoneMute(bool)`
   - `GetClipguard()` / `SetClipguard(bool)`
   - `GetLowCut()` / `SetLowCut(bool)`
   - `GetMuteColor()` / `SetMuteColor(RGB)`
   - `GetDirectMonitor()` / `SetDirectMonitor(float)`
   - `GetLevelMeters()` → input/playback levels

3. **Hotplug / permissions**
   - Register `libusb_hotplug_register_callback`.
   - Ship `udev/50-elgato-wave3.rules` to grant `audio` group access.

4. **Safety / coexistence**
   - Ensure the daemon does not interfere with `snd-usb-audio` on interfaces 0–2.
   - Handle the case where Wave Link is running on another OS in a VM — only one host should open interface 3.

### Phase 3 — PipeWire / ALSA Integration

1. **ALSA UCM profile**
   - Create `ucm2/conf.d/Elgato Wave 3/Elgato Wave 3.conf`.
   - Rename confusing port descriptions (e.g. “Digital Stereo IEC958” → “Headphones”).
   - Map existing ALSA mixer controls.
   - Add custom verbs for streaming/recording.

2. **PipeWire properties**
   - Extend the card node with custom `device.props` for the daemon-controlled settings.
   - Example:
     ```
     device.props = {
       elgato.model = "Wave:3"
       elgato.serial = "WAVE3_SERIAL_REDACTED"
       elgato.clipguard = true
       elgato.lowcut = false
       elgato.mute-color = "0xff0000"
     }
     ```

3. **WirePlumber rule**
   - Auto-start `wave3-daemon` when a Wave:3 is plugged.
   - Set default sample rate to 48000 Hz unless 96000 Hz is explicitly requested.

4. **Virtual mix devices (optional but valuable)**
   - Define null sinks in PipeWire config:
     - `Wave:3 Stream Mix`
     - `Wave:3 Chat Mix`
   - Use `module-loopback` to route app audio and mic into the selected mix.

### Phase 4 — CLI / GUI Frontend

1. **CLI tool `wave3-ctl`**
   ```bash
   wave3-ctl list-devices
   wave3-ctl status
   wave3-ctl set gain 24dB
   wave3-ctl set headphone-volume -10dB
   wave3-ctl set clipguard on
   wave3-ctl set lowcut on
   wave3-ctl set rgb-mute ff00ff
   wave3-ctl set monitor-mix 0.5
   ```

2. **Optional GTK/Qt GUI**
   - Minimal tray applet showing mute status and gain.
   - Color picker for mute-ring RGB.
   - Toggle buttons for Clipguard and low-cut.

### Phase 5 — Firmware Updates

1. Implement a `wave3-fwupdate` tool that:
   - Reads Thesycon firmware image format.
   - Puts the device into DFU mode via vendor command.
   - Flashes the image using standard USB DFU or the Thesycon variant.
   - Verifies version after reboot.

2. Alternatively, document how to use `dfu-util` if the image format is plain DFU.

### Phase 6 — Testing & Validation

1. **Functional tests**
   - Each setting change from the CLI is reflected live on the microphone.
   - Each physical change (tap-to-mute, dial) is reflected in the daemon state.

2. **Audio quality tests**
   - Capture 1 kHz sine wave at 48 kHz and 96 kHz; verify bit depth and no sample-rate conversion.
   - Verify Clipguard prevents clipping at high SPL.
   - Verify low-cut filter attenuates sub-120 Hz content.

3. **Latency tests**
   - Measure hardware round-trip latency.
   - Compare with Wave Link on Windows/macOS.

4. **Regression tests**
   - Ensure `snd-usb-audio` continues to work when the daemon is not running.
   - Ensure unplugging/replugging recovers cleanly.
   - Ensure no audio glitching when changing RGB or Clipguard.

5. **Distribution packaging**
   - `.deb`, `.rpm`, Arch PKGBUILD, or Flatpak.
   - systemd user service for `wave3-daemon`.
   - udev rules package.

---

## 4. Deliverables

| Deliverable | Location | Description |
|-------------|----------|-------------|
| Protocol specification | `native-linux/docs/protocol.md` | Byte-level vendor USB protocol |
| Property ID table | `native-linux/docs/property-table.csv` | Path ↔ ID ↔ type ↔ range mapping |
| USB capture annotations | `native-linux/captures/` | Labeled `.pcapng` + notes |
| `wave3-daemon` | `native-linux/src/daemon/` | D-Bus daemon for device control |
| `wave3-ctl` | `native-linux/src/cli/` | Command-line tool |
| ALSA UCM profile | `native-linux/alsa-ucm/` | Clean profile naming |
| PipeWire config | `native-linux/pipewire/` | Rules and virtual devices |
| udev rules | `native-linux/udev/` | Permission rules |
| Tests | `native-linux/tests/` | Unit and integration tests |
| Firmware updater | `native-linux/src/fwupdate/` | Optional DFU tool |

---

## 5. Open Questions

1. Does the Wave:3 first-generation (PID `0x0070`) support **all** 309 control paths, or only a subset?
2. Is the tap-to-mute event delivered over the standard UAC1 interrupt endpoint, or over the vendor endpoint?
3. Does the firmware reject vendor commands if the device is actively streaming audio?
4. Is there a firmware-side mixer for the Wave:3 first-gen, or is the Wave Link mixer purely software?
5. What is the exact binary format of Thesycon firmware images?
6. Can the device be opened from a Linux guest while the same device is visible to a hypervisor host, or does `libusb` claim conflict?

---

## 6. Success Criteria

The project is complete when:

1. A user can plug a Wave:3 into a Linux system and have it work as a fully functional audio device **without** Wave Link.
2. All first-gen Wave:3 settings (gain, mute, headphone volume, headphone mute, Clipguard, low-cut, RGB mute color) are controllable from a native Linux tool.
3. Tap-to-mute and other hardware events are reported to the Linux audio stack or desktop.
4. The implementation does not require proprietary Windows/macOS drivers or applications.
5. Firmware updates are possible from Linux (or clearly documented if blocked).
6. The solution integrates cleanly with PipeWire and is packaged for at least one major distribution.

---

## 7. Completed

- [x] Enumerated the Wave:3 audio system.
- [x] Downloaded and statically tore down Wave Link 3.0 for Windows and macOS.
- [x] Identified the 309 shared cross-platform control paths and the UAC feature-unit layout.
- [x] Discovered and verified the `wIndex = (entity << 8) | 3` userspace workaround.
- [x] Built, compiled, and live-tested `wave3-daemon` and `wave3ctl`.
- [x] Documented install, udev, systemd, and D-Bus integration.
- [x] Vendored the upstream `wave3ctl` kernel-module solution as a fallback.
- [x] Created ALSA UCM profile, PipeWire virtual sinks, WirePlumber auto-start rule, GTK4 GUI, Arch PKGBUILD, and integration tests.
- [x] Reviewed the [Undertone](https://github.com/polariscli/Undertone) project and adopted its proven PipeWire topology (`wave3-source`, `wave3-sink`, `wave3-null-sink`).
- [x] Created a comprehensive protocol summary at [`WAVE3_PROTOCOL_SUMMARY.md`](WAVE3_PROTOCOL_SUMMARY.md).
- [x] Decoded the class-based vendor protocol on interface 3
  (`bmRequestType=0xA1/0x21`, `bRequest=0x85/0x05`, `wIndex=0x3303`).
- [x] Identified the three live `wValue` IDs (`0x0000` config, `0x0001` meter,
  `0x000A` device info) and the 16-byte config block layout.
- [x] Implemented mic mute, headphone mute, headphone volume, and level-meter
  polling through the config block in `wave3-daemon`.
- [x] Added `uac_set.c` helper for controlled live UAC set tests.

## 8. Blocked / Remaining Work

1. **Config byte mapping for advanced features**
   - Identify which byte(s) control clipguard, direct monitor mix, low-cut,
     RGB/LED brightness, and dial mode.
   - The fastest path is a Wave Link `usbmon` capture from a Windows VM;
     alternatively, physically toggle each byte and observe the device.

2. **Daemon enhancements** (pending byte mapping)
   - Implement real `SetClipguard`, `SetLowCut`, `SetDirectMonitor`,
     `SetMuteColor`, and `SetHeadphoneColor` by writing the correct config
     byte(s).
   - Add `libusb_hotplug_register_callback` for plug/unplug events.
   - Calibrate the input/playback meter scale.

3. **Validation of PipeWire/WirePlumber integration**
   - Restart WirePlumber with the new 0.5 config and verify `wave3-source`,
     `wave3-sink`, and `wave3-null-sink` appear.
   - Verify app routing through virtual mix sinks works end-to-end.
