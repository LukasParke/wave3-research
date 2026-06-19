# Elgato Wave Link 3.0 — Application Tear-Down

**Report generated:** 2026-06-18  
**Packages analyzed:**
- Windows: `Elgato.WaveLink_3.0.0.2388_x64.msix` (SHA-256: `4772a5cc8c1924d0b0901edea3de1bb2e88064e69f16fb8cf3889609a58ce828`)
- macOS: `ElgatoWaveLink-3.0.0.1999.dmg` (SHA-256: `c85d30e09818f5b76b2c618b2c08e9d602bde364c3b57841da2dc8c1b08c83a9`)

Both installers were downloaded from Elgato’s official `edge.elgato.com` CDN.

> **Educational purpose only**
>
> The original installer packages (`.msix`, `.dmg`) and their extracted
> binary contents have been removed from this repository. Only
> independent research notes, string/path summaries, and file manifests
> are retained here for educational and interoperability research
> purposes. Elgato®, Wave Link®, and related marks are trademarks of
> Corsair Memory, Inc. / Elgato and are used solely for identification
> and commentary.

## 1. Package Structure

### 1.1 Windows (MSIX)

- **Format:** MSIX package, x64, .NET / WinUI 3 / Windows App SDK 1.8 application.
- **Entry point:** `Elgato.WaveLink.exe`
- **Core C# assemblies:**
  - `Elgato.WaveLink.dll` — main application
  - `Elgato.WaveLink.AppLogic.dll` — business logic / device models
  - `Elgato.BaseClasses.Core.dll`, `Elgato.BaseClasses.WinUI.dll` — shared Elgato UI/framework
- **Native/audio libraries:**
  - `waveapi.dll` — low-level Elgato/Lewitt device communication (USB vendor protocol)
  - `EWLWAudioEngine.dll` — audio engine, CoreAudio/WASAPI wrapper, device enumeration
  - `EWLWSoundCheck.dll` — Sound Check / room calibration
  - `EWLWPluginManager.dll` — VST3 plugin hosting
  - `tlusbdfuapi.dll` — USB DFU firmware-update API (Thesycon-based)
  - `libusb-1.0.dll` — libusb backend for some USB paths
- **Third-party:** ONNX Runtime, MSQuic, WebView2, Win2D, Sentry crash reporting.

### 1.2 macOS (DMG → HFS+ app bundle)

- **App bundle:** `Elgato Wave Link.app`
- **Executable:** `Contents/MacOS/WaveLinkMacOS` (Swift/SwiftUI app)
- **Bundle ID:** `com.elgato.WaveLink3`
- **Frameworks:**
  - `WaveAPI.framework` — macOS build of the same low-level device API (`waveapi`)
  - `tlusbdfuapi.framework` — DFU update helper
  - `WavePluginRack.framework` — VST3/AU effect hosting
  - `Sentry.framework`, `Sparkle.framework` — crash reporting and auto-updater
- **Swift bundles:** `WLEngine_WLEngine.bundle`, `WLMixer_WLMixer.bundle`, `ElgatoSwiftToolkit_*` bundles, `ElgatoMarketplace_*`, telemetry bundles.

## 2. How Wave Link Talks to the Hardware

### 2.1 Discovery and identity

Both platforms use a shared C++ library (`waveapi.dll` / `WaveAPI.framework`) internally branded `LWT` (Lewitt — Elgato’s audio OEM). The library:

1. Enumerates USB devices via native OS APIs (Windows: Thesycon `TUSBAUDIO_*` API / `libusb-1.0`; macOS: IOKit USB matching + Thesycon DFU API).
2. Matches VID `0x0fd9` (Elgato) and known PIDs, plus their DFU-mode PIDs (`kPIDWave3`, `kPIDWave3Dfu`, `kPIDWave3MK2`, etc.).
3. Identifies the exact device model via USB descriptors and a device-version table (`LewittDeviceConf`, `LewittDeviceVersionPair`).

### 2.2 Supported device models

The same binary supports the whole Wave/Stream Deck ecosystem:

- `Wave1Device`, `Wave3Device`, `Wave3MK2Device`
- `WaveNeoDevice`
- `WaveXLRDevice`, `WaveXLRMK2Device`, `WaveXLRProDevice`
- `WaveXLRDockDevice`, `WaveXLRDockMK2Device`

A “virtual” Wave device (`kPIDWaveVirtual_USB`) is also referenced, used when the app creates software-only mixes.

### 2.3 Communication stack

| Layer | Windows | macOS | Notes |
|-------|---------|-------|-------|
| USB vendor control | Thesycon `TUSBAUDIO_ClassVendorRequestIn/Out` + `libusb-1.0` | IOKit USB + `VendorUSBBackendStrategy` | Uses interface 3 (`0xFF/0xF0`) |
| UAC1 audio streaming | Thesycon driver (`tusbaudio`) or WASAPI shared/exclusive | CoreAudio via `AVAudioEngine` / `ElgatoCoreAudio` | 48/96 kHz, 24-bit |
| Low-level device model | `waveapi.dll` (`LWT::*`) | `WaveAPI.framework` | Shared logic, message protocol |
| Audio engine | `EWLWAudioEngine.dll` (WASAPI/MMDevice) | `WLEngine_WLEngine.bundle` + `WLMixer_WLMixer.bundle` | Mixing, routing, effects |
| Application UI | WinUI 3 / C# | SwiftUI / Swift | Settings, marketplace, onboarding |

The application does **not** rely on standard HID reports for the dial/mute button. The multi-function dial and capacitive mute surface are handled inside the firmware and exposed to the host as:

- Standard UAC1 feature-unit mute/volume changes on the audio control interrupt endpoint (`0x83`).
- Vendor-specific property reads/writes over the `0xFF/0xF0` interface for RGB colors, Clipguard, low-cut, etc.

## 3. Device Settings / Control Paths

Wave Link accesses hardware settings through a unified key-value path namespace. The same 309 control paths exist in both the Windows and macOS `waveapi` libraries, confirming the protocol is identical across platforms.

### 3.1 Core Wave:3 controls

| Setting | Control path(s) | Type | Notes |
|---------|-----------------|------|-------|
| Microphone gain | `/gain` or `/input_gain` | float dB | Hardware input gain |
| Microphone mute | `/input_mute`, `/mute` | bool | Also reflected in UAC1 feature unit |
| Headphone volume | `/headphone_volume`, `/headphone1/volume` | float dB | Hardware monitor output |
| Headphone mute | `/headphone_mute`, `/headphone1/mute` | bool | Mutes the headphone jack |
| Mic/PC monitor mix | `/direct_monitor` | float | Blend of mic live vs. PC playback in headphones |
| Low-cut filter | `/lowcut_enable`, `/lowcut_enabled`, `/lowcut1_enabled` | bool | Cuts low frequencies (≈80 Hz / 120 Hz depending on model) |
| Clipguard | `/clipguard_enable` | bool | Anti-clipping limiter |
| Limiter bypass | `/limiter_bypassed` | bool | Bypass hardware limiter |
| RGB mute color | `/microphone_color_rgb/red`, `/green`, `/blue` | uint8 | Mute-ring LED color |
| RGB headphone ring | `/headphone_color_rgb/{red,green,blue}` | uint8 | Some models support this |
| Firmware version | `/version/serial`, `FirmwareVersionMajor/Minor/Patch` | read-only | From device descriptors + version table |

### 3.2 Mixer / routing model

Wave Link 3.0 models the device as a 12×5 mixer matrix internally:

- **Inputs 0–7:** software/application mixes (`System`, `Music`, `Browser`, `Voice Chat`, `SFX`, `Game`, `Aux1`, `Aux2`).
- **Inputs 8–11:** hardware microphone inputs (`Wave Mic 1`–`Wave Mic 4`).
- **Mixes 0–4:** `Personal`, `Chat`, `Stream`, `Record`, `Aux`.

Each crosspoint has paths like:

```
/mixer/0/input_enabled/8     (bool: is Wave Mic 1 in Personal Mix)
/mixer/0/input_volume/8      (float dB: Wave Mic 1 level in Personal Mix)
/mixer/2/input_volume/8      (float dB: Wave Mic 1 level in Stream Mix)
```

The hardware outputs (USB streaming to the OS) are selected with:

```
/usb_outputs/0/source_select
/usb_outputs/1/source_select
...
```

### 3.3 DSP effects (Wave:3 MK.2 / Wave Next)

On newer devices the firmware runs DSP effects. Paths exposed:

```
/dspfx/compressor/attack_ms
/dspfx/compressor/threshold_dB
/dspfx/compressor/ratio
/dspfx/compressor/release_ms
/dspfx/compressor/makeup_gain_dB
/dspfx/equalizer/band/0/enabled
/dspfx/equalizer/band/0/frequency_Hz
/dspfx/equalizer/band/0/gain_dB
/dspfx/equalizer/band/0/quality_log10
/dspfx/equalizer/band/0/type
```

Legacy Wave:3 (PID `0x0070`) has no onboard DSP effects; Clipguard and low-cut are the main hardware DSP features.

## 4. Firmware Updates

- Windows uses `tlusbdfuapi.dll`; macOS uses `tlusbdfuapi.framework`. Both are based on **Thesycon DFU** technology.
- The device exposes a standard USB DFU interface (interface 4 in the descriptor dump).
- APIs referenced: `TUSBAUDIO_StartDfuDownload`, `TUSBAUDIO_StartDfuUpload`, `TUSBAUDIO_EndDfuProc`, `thesycon::DfuImageBase::LoadFromFile`.
- Firmware images are likely `.bin` or `.dfu` files with a Thesycon-specific suffix; the library can load raw binary or Thesycon-format images.

## 5. Windows Audio Architecture Details

### 5.1 Driver model

- Legacy Wave devices use the **Thesycon USB Audio driver** (`tusbaudio`). This installs a custom kernel streaming audio device with ASIO support.
- Wave Link also registers **virtual audio devices**:
  - `Wave Link Stream` (capture)
  - `Wave Link Microphone` (capture)
  - Possibly `Wave Link Virtual ASIO`
- These virtual devices let apps like OBS use a clean “Stream Mix” while the app internally mixes multiple sources.

### 5.2 OS audio APIs used

- **WASAPI** (`IAudioClient`, `IAudioRenderClient`, `IAudioCaptureClient`, `IMMDeviceEnumerator`) for normal Windows audio routing.
- **Thesycon TUSBAUDIO API** for low-level UAC1 control, ASIO, sample-rate selection, and firmware update.
- **ASIO** is available for pro-audio low-latency access.

### 5.3 App manifest capabilities

```xml
<rescap:Capability Name="runFullTrust" />
<rescap:Capability Name="packageManagement" />
<rescap:Capability Name="packageQuery" />
<DeviceCapability Name="microphone" />
```

`runFullTrust` is required to load native drivers and interact with USB/audio hardware.

## 6. macOS Audio Architecture Details

- The main binary links against `AVFoundation`, `CoreAudio`, and an internal `ElgatoCoreAudio` framework.
- It uses **App Intents / Siri Shortcuts** (`com.elgato.WaveLink3` URL schemes `wavelink` and `wavelink-v2`).
- No kernel driver is installed for normal UAC1 operation; macOS class driver is sufficient.
- The app is **not sandboxed** (string: `"App is not sandboxed, extending default cache directory with bundle identifier"`).

## 7. Configuration and Persistence

- The C# side exposes session/setting field keys in `Elgato.WaveLink.AppLogic.dll`.
- Examples: `kSessionFieldInputGain`, `kSessionFieldClipguard`, `kSessionFieldLowcut`, `kSessionFieldHeadphoneVolume`, `kSessionFieldDirectMonitor`, `kSessionFieldMuteColorRGBRed`, etc.
- These map through SWIG-generated C# wrappers to native `waveapi.dll` get/set calls:
  - `EWLWWaveDeviceProxy_GetFloatValueForKey`
  - `EWLWWaveDeviceProxy_SetBoolValueForKey`
  - `EWLWWaveDeviceProxy_SetFloatValueForKey`
  - `EWLWWaveDeviceProxy_SetIntValueForKey`

## 8. KVM / Cross-Platform Implications

Because Wave Link’s extra features are **not** standard UAC1 controls:

1. The device must be passed through as the **full composite USB device** (all 5 interfaces) so Wave Link can open the vendor interface and DFU interface.
2. On Windows, the Thesycon driver must also be installed inside the guest; otherwise Wave Link falls back to standard UAC1 and loses ASIO / advanced control.
3. macOS and Linux guests will see the UAC1 audio endpoints natively, but only macOS can run Wave Link 3.0; there is **no Linux Wave Link build**.
4. A KVM switch that strips vendor/DFU descriptors or emulates only HID/mass-storage will break Wave Link entirely.

## 9. Raw Evidence Files

All extracted strings, manifests, and file lists are in:

```
wavelink/windows/Elgato.WaveLink_3.0.0.2388_x64.msix   (installer)
wavelink/windows/msix_extracted/                        (full extracted app)
wavelink/mac/ElgatoWaveLink-3.0.0.1999.dmg             (installer)
wavelink/mac/dmg_extracted/                             (full extracted app bundle)
wavelink/teardown/                                      (string dumps, manifests, APIs)
wavelink/checksums.txt                                  (SHA-256 of installers)
```

Key teardown files:

- `teardown/win-tusbaudio-api-full.txt` — complete Thesycon API surface
- `teardown/win-os-audio-apis.txt` — WASAPI/CoreAudio interfaces used
- `teardown/win-asio-strings.txt` — ASIO device names and functions
- `teardown/applogic-session-fields-clean.txt` — Wave Link settings keys
- `teardown/applogic-pid-list.txt` — supported product PIDs
- `teardown/mac-waveapi-all-device-paths-full.txt` and `teardown/win-waveapi-all-device-paths-full.txt` — identical 309 control paths
- `teardown/AppxManifest-keylines.txt` — Windows package capabilities

## 10. Summary

Elgato Wave Link 3.0 is a **single shared C++ device backend** (`waveapi` / `LWT`) wrapped in platform-specific UI and audio-routing layers. It depends on:

- Standard UAC1 for audio streaming.
- Proprietary vendor USB requests on interface `0xFF/0xF0` for device settings.
- Standard USB DFU for firmware updates.
- Thesycon’s Windows USB audio driver (and optionally ASIO) for low-latency operation.
- Windows WASAPI / macOS CoreAudio for host audio routing and virtual mix devices.

For perfect KVM support, expose the complete composite device to the active guest and install the matching platform-specific driver/application stack.
