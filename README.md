# Elgato Wave:3 Audio System Enumeration Report

**Host:** `HOST`  
**User:** `USER`  
**Report generated:** 2026-06-18  
**Sound server:** PulseAudio on PipeWire 1.6.6

## Executive Summary

A single **Elgato Wave:3** USB audio device is connected to this host. It is the current default playback sink and default recording source. The device enumerates as a USB Audio Class 1.0 full-speed device with one stereo playback stream, one mono capture stream, a vendor-specific control interface, and a Device Firmware Upgrade (DFU) interface.

A co-located Elgato Facecam MK.2 (USB video device) shares the same USB hub chain but is not part of the audio system.

## 1. Device Identity

| Attribute | Value |
|-----------|-------|
| Product | Elgato Wave:3 |
| Vendor | Elgato Systems GmbH |
| USB VID:PID | `0fd9:0070` |
| USB bcdDevice (firmware revision) | `1.22` |
| Serial number | `WAVE3_SERIAL_REDACTED` |
| USB bus:device | `005:072` |
| USB device path | `usb-0000:10:00.3-2.4.4.1.3` |
| sysfs path | `/sys/bus/usb/devices/5-2.4.4.1.3` |
| USB speed | Full Speed (12 Mbps) |
| Power | Bus powered, 100 mA |
| Runtime PM | `on`, currently `active`, autosuspend delay 2000 ms |
| ALSA card index | `3` (name `Wave3`) |
| Kernel driver | `snd-usb-audio` |

**Raw files:**
- [`devices/lsusb-elgato-wave3-verbose.txt`](devices/lsusb-elgato-wave3-verbose.txt) — full USB descriptor dump
- [`devices/sysfs-ids.txt`](devices/sysfs-ids.txt) — sysfs identity strings
- [`devices/udevadm-wave3.txt`](devices/udevadm-wave3.txt) — udev attributes
- [`devices/lsusb-all-elgato.txt`](devices/lsusb-all-elgato.txt) — all connected Elgato devices

## 2. USB Topology

The Wave:3 is attached behind a chain of hubs on bus 5:

```
Bus 005.Port 001: Dev 001, root_hub
  Port 002: Dev 002, Hub
    Port 004: Dev 003, Hub
      Port 004: Dev 067, Hub
        Port 001: Dev 068, Hub
          Port 002: Dev 071, Elgato Facecam MK.2 (video)
          Port 003: Dev 072, Elgato Wave:3 (audio)
          Port 004: Dev 073, Hub
            Port 003: Dev 074, smartcard reader
```

**Raw file:** [`devices/lsusb-topology.txt`](devices/lsusb-topology.txt)

## 3. USB Interface Enumeration

The device exposes 5 interfaces in its single configuration:

| Interface | Class | Subclass | Driver | Purpose |
|-----------|-------|----------|--------|---------|
| 0 | Audio | Control Device | `snd-usb-audio` | Audio control (clocks, mixers, feature units) |
| 1 | Audio | Streaming | `snd-usb-audio` | Stereo playback (headphone output) |
| 2 | Audio | Streaming | `snd-usb-audio` | Mono capture (microphone input) |
| 3 | Vendor Specific (`0xFF/0xF0`) | none | none | Proprietary Elgato/Wave Link control |
| 4 | Application Specific | DFU (`0x01/0x01`) | none | Device Firmware Upgrade interface |

Key endpoints:

| Endpoint | Direction | Type | Max packet | Notes |
|----------|-----------|------|------------|-------|
| `0x01` | OUT | Isochronous async | 582 bytes | Playback audio data |
| `0x81` | IN | Isochronous feedback | 4 bytes | Playback rate feedback |
| `0x82` | IN | Isochronous async | 291 bytes | Capture audio data |
| `0x83` | IN | Interrupt | 2 bytes | Audio-control status/mute events |

The DFU interface advertises: download supported, upload unsupported, manifestation tolerant, will detach, 240 ms detach timeout, 64-byte transfer size, DFU version 1.10.

**Raw file:** [`devices/lsusb-elgato-wave3-verbose.txt`](devices/lsusb-elgato-wave3-verbose.txt)

## 4. Audio Capabilities

### 4.1 Playback (headphone output)

| Capability | Value |
|------------|-------|
| Channels | 2 (stereo) |
| Channel map | Front Left, Front Right |
| Sample formats | `S24_3LE` (24-bit stored in 3 bytes) |
| Sample rates | 48000 Hz, 96000 Hz |
| Endpoint mode | Asynchronous OUT with feedback IN |
| ALSA device | `hw:3,0` (`pcmC3D0p`) |

### 4.2 Capture (microphone input)

| Capability | Value |
|------------|-------|
| Channels | 1 (mono) |
| Channel map | MONO |
| Sample formats | `S24_3LE` (24-bit stored in 3 bytes) |
| Sample rates | 48000 Hz, 96000 Hz |
| Endpoint mode | Asynchronous IN |
| ALSA device | `hw:3,0` (`pcmC3D0c`) |

### 4.3 Current runtime configuration

Both subdevices are active and owned by PipeWire (PID 3659) at 48000 Hz:

| Stream | State | Format | Channels | Rate | Period | Buffer |
|--------|-------|--------|----------|------|--------|--------|
| Playback | RUNNING | `S24_3LE` | 2 | 48000 Hz | 512 | 32768 |
| Capture | RUNNING | `S24_3LE` | 1 | 48000 Hz | 512 | 32768 |

**Raw files:**
- [`alsa/card3-stream0.txt`](alsa/card3-stream0.txt) — USB audio streaming descriptors
- [`alsa/pcm0p_hw_params.txt`](alsa/pcm0p_hw_params.txt) / [`alsa/pcm0c_hw_params.txt`](alsa/pcm0c_hw_params.txt)
- [`alsa/pcm0p_status.txt`](alsa/pcm0p_status.txt) / [`alsa/pcm0c_status.txt`](alsa/pcm0c_status.txt)
- [`alsa/aplay-l.txt`](alsa/aplay-l.txt), [`alsa/arecord-l.txt`](alsa/arecord-l.txt)
- [`alsa/aplay-L.txt`](alsa/aplay-L.txt), [`alsa/arecord-L.txt`](alsa/arecord-L.txt)

## 5. ALSA Mixer Controls

| Numid | Name | Type | Access | Current | Range / dB |
|-------|------|------|--------|---------|------------|
| 1 | Playback Channel Map | INTEGER | read-only | FL, FR | fixed |
| 2 | Capture Channel Map | INTEGER | read-only | MONO | fixed |
| 3 | PCM Playback Switch | BOOLEAN | rw | `on` | — |
| 4 | PCM Playback Volume | INTEGER | rw | 102 / 85% / -9.00 dB | 0–120, -60 to 0 dB |
| 5 | Mic Capture Switch | BOOLEAN | rw | `on` | — |
| 6 | Mic Capture Volume | INTEGER | rw | 24 / 30% / +12.00 dB | 0–80, 0 to +40 dB |

**Raw file:** [`alsa/amixer-contents.txt`](alsa/amixer-contents.txt)

## 6. PipeWire / PulseAudio View

- **Default sink:** `alsa_output.usb-Elgato_Systems_Elgato_Wave_3_WAVE3_SERIAL_REDACTED-00.iec958-stereo`
- **Default source:** `alsa_input.usb-Elgato_Systems_Elgato_Wave_3_WAVE3_SERIAL_REDACTED-00.mono-fallback`

### 6.1 Card

| Property | Value |
|----------|-------|
| Name | `alsa_card.usb-Elgato_Systems_Elgato_Wave_3_WAVE3_SERIAL_REDACTED-00` |
| API | ALSA / udev |
| ALSA path | `hw:3` |
| Object serial | 5361 |
| Active profile | `output:iec958-stereo+input:mono-fallback` |
| Available profiles | `off`, `output:analog-stereo+input:mono-fallback`, `output:analog-stereo`, `output:iec958-stereo+input:mono-fallback`, `output:iec958-stereo`, `pro-audio`, `input:mono-fallback` |

### 6.1a Card ports

| Port | Type | Priority | Profiles |
|------|------|----------|----------|
| `analog-input-mic` | Microphone | 8700 | all input-capable profiles |
| `analog-output` | Analog Output | 9900 | `output:analog-stereo`, `output:analog-stereo+input:mono-fallback` |
| `iec958-stereo-output` | Digital Output (S/PDIF) | 0 | `output:iec958-stereo`, `output:iec958-stereo+input:mono-fallback` |

### 6.2 Sink (playback)

| Property | Value |
|----------|-------|
| Name | `alsa_output.usb-Elgato_Systems_Elgato_Wave_3_WAVE3_SERIAL_REDACTED-00.iec958-stereo` |
| Description | Wave3 |
| State | RUNNING |
| Sample spec | `s24le 2ch 48000Hz` |
| Channel map | front-left, front-right |
| Volume | 40% / -23.87 dB (both channels) |
| Node serial | 5616 |
| Period size / count | 512 / 64 |
| Max latency | 16384/48000 |

### 6.3 Source (capture)

| Property | Value |
|----------|-------|
| Name | `alsa_input.usb-Elgato_Systems_Elgato_Wave_3_WAVE3_SERIAL_REDACTED-00.mono-fallback` |
| Description | Wave3 Mic |
| State | IDLE |
| Sample spec | `s24le 1ch 48000Hz` |
| Channel map | mono |
| Volume | 35% / -27.72 dB (base volume -40 dB) |
| Flags | HARDWARE, HW_MUTE_CTRL, HW_VOLUME_CTRL, DECIBEL_VOLUME, LATENCY |
| Node serial | 5617 |

### 6.4 Monitor source (software loopback of sink)

| Property | Value |
|----------|-------|
| Name | `alsa_output.usb-Elgato_Systems_Elgato_Wave_3_WAVE3_SERIAL_REDACTED-00.iec958-stereo.monitor` |
| Description | Monitor of Wave3 |
| State | RUNNING |
| Sample spec | `s24le 2ch 48000Hz` |
| Channel map | front-left, front-right |
| Volume | 100% / 0.00 dB |
| Monitored sink | `alsa_output.usb-Elgato_Systems_Elgato_Wave_3_WAVE3_SERIAL_REDACTED-00.iec958-stereo` |

**Raw file:** [`pipewire/pactl-source-monitor-wave3.txt`](pipewire/pactl-source-monitor-wave3.txt)

**Raw files:**
- [`pipewire/pactl-info.txt`](pipewire/pactl-info.txt)
- [`pipewire/pactl-card-wave3.txt`](pipewire/pactl-card-wave3.txt)
- [`pipewire/pactl-sink-wave3.txt`](pipewire/pactl-sink-wave3.txt)
- [`pipewire/pactl-source-wave3.txt`](pipewire/pactl-source-wave3.txt)
- [`pipewire/pw-cli-wave3-nodes.txt`](pipewire/pw-cli-wave3-nodes.txt)
- [`pipewire/wpctl-status.txt`](pipewire/wpctl-status.txt)

## 7. Kernel / Driver Details

| Item | Value |
|------|-------|
| Audio driver | `snd-usb-audio` |
| ALSA card module mapping | `3 snd_usb_audio` |
| Related loaded modules | `snd_usb_audio`, `snd_usbmidi_lib`, `snd_ump`, `snd_hwdep`, `snd_pcm` |
| USB audio class revision | 1.00 (UAC 1.0) |
| Device enumeration path in kernel log | `usb 5-2.4.4.1.3: Product: Elgato Wave:3` |

Kernel access to `dmesg` is restricted on this host; equivalent enumeration evidence was captured from the systemd journal.

**Raw files:**
- [`kernel/journalctl-wave3.txt`](kernel/journalctl-wave3.txt)
- [`kernel/journalctl-elgato.txt`](kernel/journalctl-elgato.txt)
- [`kernel/sound-modules.txt`](kernel/sound-modules.txt)

## 8. Device Nodes and Permissions

```
crw-rw----+ root audio 116, 17  /dev/snd/pcmC3D0p   (playback)
crw-rw----+ root audio 116, 18  /dev/snd/pcmC3D0c   (capture)
crw-rw----+ root audio 116, 19  /dev/snd/controlC3  (control)
```

**Raw file:** [`alsa/dev-snd.txt`](alsa/dev-snd.txt)

## 9. Observations and Notes

- Only **one** Wave:3 is connected. No Wave Link virtual mixer devices are present; the proprietary vendor interface (`0xFF/0xF0`) has no Linux driver loaded.
- The microphone signal path is **mono**, while the headphone monitor path is **stereo**.
- The hardware supports **24-bit/48 kHz and 96 kHz** operation but is currently running at **48 kHz** through PipeWire.
- Both capture and playback subdevices are currently open and RUNNING, driven by PipeWire.
- Although the hardware output terminal is a headphone output, the currently active PipeWire profile is `output:iec958-stereo+input:mono-fallback`; the `analog-output` port is also available if an analog-stereo profile is preferred.
- The device includes a **DFU interface**, meaning firmware updates are possible over USB using a DFU tool.

## 10. Raw Data Inventory

All collected evidence lives under this repository root:

```
alsa/              ALSA card, device, mixer, and PCM state files
devices/           USB descriptors, topology, udev, sysfs, power
kernel/            Kernel journal entries and loaded module list
pipewire/          PipeWire/PulseAudio card, sink, source, and node dumps
report/            File manifest
```

See [`report/file-manifest.txt`](report/file-manifest.txt) for the complete list.

## 11. Native Linux Control (new)

A working native control stack has been added under [`native-linux/`](native-linux/):

* `wave3-daemon` — C/GDBus daemon that controls the Wave:3 from userspace
  using only `libusb` and the session bus. No kernel module or driver
  detach is required.
* `wave3ctl` — shell CLI that talks to the daemon.

### How it works

The Wave:3's microphone mute, microphone gain, headphone mute and
headphone volume are exposed through standard USB Audio Class 1.0
feature units:

| Control | Feature Unit entity | Selector |
|---------|---------------------|----------|
| Mic mute | 6 | 1 (MUTE) |
| Mic gain | 6 | 2 (VOLUME) — read-only, dial-controlled |
| HP mute | 5 | 1 (MUTE) |
| HP volume | 5 | 2 (VOLUME) |

`snd-usb-audio` owns AudioControl interface 0, so userspace UAC requests
with `wIndex = (entity << 8) | 0` are rejected. By routing the same
requests through the unclaimed vendor interface 3,
`wIndex = (entity << 8) | 3`, the kernel allows the transfer and the
firmware still accepts it.

### Verified on this device

* HP volume range: **-60.0 dB … 0.0 dB**
* Mic gain range: **0.0 dB … 40.0 dB**
* Software control works for: mic mute, headphone mute, headphone volume
* Mic gain is read-only via UAC (physical dial)
* Level meters (input/playback) read via class control `wValue=0x0001`
* Device info (serial, firmware/API version) read via `wValue=0x000A`

### Quick start

```bash
cd native-linux
./install.sh
sudo install -Dm644 udev/50-elgato-wave3.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=usb --attr-match=idVendor=0fd9 --attr-match=idProduct=0070
systemctl --user enable --now wave3-daemon
wave3ctl status
wave3ctl mute toggle
wave3ctl volume 75
```

See [`native-linux/README.md`](native-linux/README.md) for full details.

## 12. Reverse-Engineering Summary

A separate protocol investigation produced the following findings; see
[`WAVE3_PROTOCOL_SUMMARY.md`](WAVE3_PROTOCOL_SUMMARY.md) for the complete
reference.

* **Standard UAC controls are fully working** from Linux via `libusb`
  using `wIndex = (entity << 8) | 3` through the unclaimed vendor
  interface 3. Verified controls: mic mute, mic gain (read-only dial),
  headphone mute, headphone volume.
* **Proprietary class-based control** on interface 3 is implemented
  through endpoint-0 control transfers (`bmRequestType=0xA1/0x21`,
  `bRequest=0x85/0x05`, `wIndex=0x3303`). The Wave:3 exposes three live
  IDs: `wValue=0x0000` (16-byte read/write config block),
  `wValue=0x0001` (8-byte meter), and `wValue=0x000A` (51-byte device
  info). The config block bytes for mic mute (offset 4), headphone
  volume (offset 8), and headphone mute (offset 9) are confirmed.
* **Static analysis** of Wave Link 3.0 identified 309 logical control
  paths and app-level session fields (Clipguard, LowCut, MuteColorRGB,
  HeadphoneColorRGB, etc.). Low-cut/EQ/compressor appear to be host-side
  DSP in Wave Link; LED colors, direct monitor mix, and clipguard likely
  live in the remaining config bytes.
* **Fuzzing** of interface 3 with vendor-type requests produced no
  responses and a dangerous DFU reset; class-type requests work for the
  three known IDs. A live `usbmon` capture from Wave Link in a Windows VM
  (or physical byte correlation) is required to decode the remaining
  config bytes.
* **PipeWire topology** for Wave Link parity was improved using patterns
  from the [Undertone](https://github.com/polariscli/Undertone) project:
  `wave3-source` renamed ALSA capture node, custom `wave3-sink` for
  headphones, and a `wave3-null-sink` to keep the mic awake.

The native control implementation lives under
[`native-linux/`](native-linux/).

## 13. Reproduction Commands

The data above was gathered with the following commands (run as the local user):

```bash
lsusb -d 0fd9:0070
lsusb -v -d 0fd9:0070
lsusb -t
aplay -l ; arecord -l
amixer -c 3 contents
cat /proc/asound/card3/stream0
cat /proc/asound/Wave3/pcm0p/sub0/hw_params
cat /proc/asound/Wave3/pcm0c/sub0/hw_params
pactl info
pactl list cards
pactl list sinks
pactl list sources
pw-cli info all
journalctl -k --grep='Wave:3' --no-pager
lsmod | grep -E 'snd|usb'
```
