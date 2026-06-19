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

## Vendor control interface (decoded from Wave Link)

Interface 3 (`bInterfaceClass = 0xFF`, `bInterfaceSubClass = 0xF0`,
`bNumEndpoints = 0`) is used by Elgato Wave Link for proprietary features.
Because it has no endpoints, the protocol runs over **endpoint-0 control
transfers**.

### Request format recovered by static reverse engineering

Wave Link ships three internal "vendor backend strategy" classes:
`LegacyUAC1VendorUSBBackendStrategy`, `LegacyUAC2VendorUSBBackendStrategy`,
and `MK2VendorUSBBackendStrategy`. The first-generation Wave:3 uses the
**LegacyUAC1** strategy.

For the Wave:3 the vendor strategy methods decompile to these exact
control transfers (verified by disassembly of `waveapi.dll`):

| Direction | `bmRequestType` | `bRequest` | `wValue` | `wIndex` | `wLength` |
|-----------|-----------------|------------|----------|----------|-----------|
| Read (IN)  | `0xC1` | `0x85` | property ID | `0x3303` | payload size |
| Write (OUT)| `0x41` | `0x05` | property ID | `0x3303` | payload size |

`wIndex = 0x3303` means: high byte `0x33` is the UAC-like **entity ID**
and low byte `0x03` is the vendor **interface number**. This is the same
encoding as the standard UAC `wIndex` trick, but with a vendor-specific
entity (`0x33`) and the vendor read/write request codes `0x85`/`0x05`.

Features expected to live here:

* RGB / LED ring control
* Direct monitor mix (mic ↔ playback blend in headphones) — **hardware**
* Clipguard anti-clip/limiter — **hardware**
* Possibly input/playback level meters

Elgato support documentation confirms that **Clipguard runs inside the
Wave microphone** and works independent of software, and that the
**Mic/PC monitor mix** can be adjusted from the hardware dial as well as
from Wave Link. Both must therefore be controllable over USB.

Features that appear to be **host-side DSP in Wave Link** rather than
hardware USB controls (based on `applogic` strings):

* Low-cut filter (`LowCutSettings`, frequency configurable)
* Compressor / EQ / expander (`DSPSettingsArray`)

This distinction is not yet fully confirmed; the exact **property IDs**
for each feature must still be mapped.

Static reverse engineering shows that the property ID carried in `wValue`
is a **single byte** returned by the `IMessage` virtual method at vtable
offset 0 (`IMessage::propertyId()`). The `SessionAPI::Impl` descriptor
constructors store the IDs indirectly, so the exact byte for each logical
path has not been recovered from static analysis alone.

### Static-analysis findings

#### Cross-platform backend names

From `waveapi.dll` / `WaveAPI.framework`:

* `VendorUSBLewittDeviceBackend`
* `LegacyUAC1VendorUSBBackendStrategy`
* `MK2VendorUSBBackendStrategy`
* Thesycon-style strings: `TUSBAUDIO_ClassVendorRequestOut`,
  `THESYCON: OUT CTRL, vendor request with bInterfaceNumber==0`

These confirm a vendor-specific control-transfer backend, but do not
expose the exact request bytes.

#### Control paths

Wave Link's shared audio engine exposes 309 logical control paths.
Top-level namespaces:

```
/dspfx
/headphone1
/headphone2
/line
/mixer
```

Examples:

```
/dspfx/compressor/attack_ms
/dspfx/compressor/threshold_dB
/dspfx/equalizer/band/0/enabled
/dspfx/equalizer/band/0/gain_dB
/headphone1/mute
/headphone1/volume
/headphone1/limiter_bypassed
/line/volume
/line/gain_lock
/mixer/0/input_enabled/0
/mixer/0/input_volume/0
```

These paths are shared across multiple Elgato/Lewitt devices and are
unlikely to all apply to the Wave:3.

#### App-level session fields

From Wave Link application strings (`applogic-session-fields.txt`):

| Field | Meaning |
|-------|---------|
| `Clipguard` | clip/limiter feature |
| `LowCut` / `LowCut1Enabled` / `LowCut2Enabled` | high-pass filter(s) |
| `MuteColorRGB*` | RGB color shown when muted |
| `MicrophoneColorRGB*` | RGB color for mic ring |
| `HeadphoneColorRGB*` | RGB color for headphone indicator |
| `MixColorRGB*` | mixer/channel color |
| `GRColorRGB*` | gain-reduction meter color (likely UI-only) |
| `IndicatorBrightness` / `MuteBrightness` / `BackgroundBrightness` | LED brightness |
| `LedFlip` | LED orientation |
| `InputEnabled` | mixer input routing |
| `P48Enabled` | phantom power (Wave XLR only) |
| `LowImpedanceEnabled` | low-impedance mode (Wave XLR only) |

No RGB/LED strings were found inside `waveapi.dll`, which suggests the
LED color logic lives in the platform-specific application layer and is
sent to the device through the vendor control interface.

No HID interface or HID report strings were found anywhere in the
binaries or device descriptors.

### Fuzzing results

`native-linux/src/fuzz_vendor_smart.c` was used to safely probe interface 3
with common vendor/class request patterns (`bmRequestType = 0x40/0xc0`,
`bRequest` 0x00..0xff, varied `wValue`/`wIndex`).  No responses were
observed.

Targeted read probes using the recovered encoding
(`0xC1, 0x85, wIndex=0x3303`) also returned `LIBUSB_ERROR_PIPE` for all
tested IDs, confirming that the device is not in APP mode.

A broad read-only scan of IDs `0x0000`–`0x00FF`
(`native-linux/src/probe_vendor_ids.c`) caused the device to reboot into
its DFU/bootloader PID (`0x0071`) before re-enumerating as `0x0070`.
**Arbitrary live enumeration is therefore unsafe and must not be
repeated.**

### Encoding summary

```c
// Vendor read
libusb_control_transfer(dev,
    0xC1,              // vendor, IN, interface recipient
    0x85,              // vendor read request code
    property_id,       // wValue: feature/property ID
    0x3303,            // wIndex: entity 0x33, interface 3
    buf, len, timeout);

// Vendor write
libusb_control_transfer(dev,
    0x41,              // vendor, OUT, interface recipient
    0x05,              // vendor write request code
    property_id,       // wValue: feature/property ID
    0x3303,            // wIndex: entity 0x33, interface 3
    buf, len, timeout);
```

`property_id` is the byte returned by `IMessage::propertyId()`. The
payload size and interpretation are feature-specific (boolean, uint8/16,
fixed-point, RGB triple, etc.). The ID table is stored in the Wave Link
`SessionAPI` descriptor data inside `waveapi.dll`; it can be recovered by
further static parsing **or** by targeted live enumeration once the
device is in APP mode.

### How to decode the vendor protocol

1. Run Wave Link in a Windows VM with the Wave:3 passed through.
2. Capture USB control traffic on the host with `usbmon` + `tshark`:

```bash
sudo modprobe usbmon
sudo tshark -i usbmonN -f "usb.idVendor == 0x0fd9 and usb.idProduct == 0x0070" \
    -w wave3-wavelink.pcapng -F pcapng
```

3. In the VM, toggle one feature at a time and annotate the timestamp.
4. Correlate each UI action with the exact `bmRequestType`, `bRequest`,
   `wValue`, `wIndex`, and payload.
5. Map property IDs to the logical feature names above.

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
  Linux control app that discovered the `wIndex` interface-number trick.
* [x4ndr0m3d4x/wave3ctl](https://git.4ndr0m3d4.me/4ndr0m3d4/wave3ctl) —
  upstream kernel-module + Python CLI for the Wave:3.
* [polariscli/Undertone](https://github.com/polariscli/Undertone) —
  Rust PipeWire mixer for the Wave:3; excellent WirePlumber topology
  reference, but vendor protocol is also unresolved (uses ALSA fallback).
* [Raphiiko/wavelink-ts PROTOCOL.md](https://github.com/Raphiiko/wavelink-ts/blob/main/PROTOCOL.md) —
  documents Wave Link's JSON-RPC application protocol (logical names only).
