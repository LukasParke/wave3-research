# Native Linux support for the Elgato Wave:3

This directory contains a fully native, userspace D-Bus daemon for the
Elgato Wave:3 microphone. It requires **no kernel module**, **no driver
detach**, and **no Windows/macOS software** for the core controls.

## Key discovery

The Wave:3 exposes standard USB Audio Class 1.0 feature units for the
microphone gain dial (Feature Unit entity 6).  All other hardware
controls live in a 16-byte proprietary **config block** accessed through
endpoint-0 class control transfers on the unclaimed vendor interface 3:

| Request | `bmRequestType` | `bRequest` | `wValue` | `wIndex` | `wLength` | Purpose |
|---------|-----------------|------------|----------|----------|-----------|---------|
| Read config | `0xA1` | `0x85` | `0x0000` | `0x3303` | 16 | Config block |
| Write config | `0x21` | `0x05` | `0x0000` | `0x3303` | 16 | Config block |
| Read meter | `0xA1` | `0x85` | `0x0001` | `0x3303` | 8 | Level meter |
| Read info | `0xA1` | `0x85` | `0x000A` | `0x3303` | 51 | Device info |

**The `wIndex` trick:** `0x3303` routes the request through the
unclaimed vendor interface 3 in the kernel's eyes, while the firmware
sees entity `0x33`.  This avoids detaching `snd-usb-audio`.

This trick was first published for the Elgato Wave XLR by
[rikkichy/openwave](https://github.com/rikkichy/openwave); the Wave:3
uses the same encoding.

### Config block layout (16 bytes)

| Offset | Field | Notes |
|--------|-------|-------|
| 4 | **Mic mute** | `0x00` live, `0x01` muted |
| 8 | **Headphone volume** | signed dB attenuation (`0x00` = 0 dB) |
| 9 | **Headphone mute** | `0x00` on, `0x01` muted |
| 14 | **Dial mode / volume select** | writable; meaning still being mapped |

The remaining bytes are unknown or read-only.  See
`docs/protocol-notes.md` for details.

## What is implemented

* `wave3-daemon` — C/GDBus service
  * Owns `org.wave3.Daemon` on the session bus
  * Polls hardware at 10 Hz and emits `StateChanged`
  * Methods: `GetState`, `SetMicMute`, `ToggleMicMute`, `SetHpMute`,
    `SetHpVolume`, plus vendor-feature stubs for
    `SetClipguard`, `SetLowCut`, `SetDirectMonitor`,
    `SetMuteColor`, `SetHeadphoneColor`, and level readouts.
* `wave3ctl` — shell CLI wrapper around `gdbus`
* GTK4 GUI applet (`gui/wave3-applet.py`)
* PipeWire/WirePlumber integration files
  * Virtual mix sinks: `Wave:3 Stream Mix`, `Wave:3 Chat Mix`,
    `Wave:3 Monitor Mix`
  * WirePlumber script that creates `wave3-source`, `wave3-sink`, and
    `wave3-null-sink` (keeps the mic awake)
* ALSA UCM profile for cleaner port naming
* Upstream `wave3ctl` kernel-module + Python CLI vendored under
  `wave3ctl/` as a fallback / reference.
* Arch PKGBUILD and systemd user service

## Build requirements

On Arch Linux:

```bash
sudo pacman -S base-devel libusb glib2 gtk4 python-dbus pipewire wireplumber
```

## Build

```bash
cd native-linux/src
make
```

This produces `wave3-daemon`. To build the diagnostic probes:

```bash
make probes
```

## Install (user)

```bash
cd native-linux
./install.sh
```

`install.sh` installs binaries, the systemd user service, D-Bus session
activation, and WirePlumber configuration under `$HOME/.local` and
`$HOME/.config`. It prints the remaining root-only commands (udev rule,
system D-Bus policy).

Make sure `~/.local/bin` is in your `$PATH`.

## Udev rule

The daemon needs permission to claim interface 3 of the Wave:3. A rule
is provided in `udev/50-elgato-wave3.rules`:

```bash
sudo install -Dm644 udev/50-elgato-wave3.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=usb --attr-match=idVendor=0fd9 --attr-match=idProduct=0070
```

The rule grants the `audio` group read/write access to the device node.
Ensure your user is in the `audio` group, then log out and back in.

## Usage

Start the daemon manually:

```bash
wave3-daemon &
```

Or as a user service:

```bash
systemctl --user daemon-reload
systemctl --user enable --now wave3-daemon
```

Then use the CLI:

```bash
wave3ctl status
wave3ctl mute toggle
wave3ctl volume 75
wave3ctl hpmute on
wave3ctl monitor
wave3ctl clipguard on     # requires decoded vendor protocol
wave3ctl lowcut on        # requires decoded vendor protocol
wave3ctl directmonitor 0.5 # requires decoded vendor protocol
wave3ctl mute-color 0xff0000 # requires decoded vendor protocol
```

GUI:

```bash
wave3-applet
```

Raw D-Bus examples:

```bash
gdbus call --session --dest org.wave3.Daemon \
    --object-path /org/wave3/Daemon --method org.wave3.Daemon.GetState

gdbus call --session --dest org.wave3.Daemon \
    --object-path /org/wave3/Daemon --method org.wave3.Daemon.SetHpVolume 60
```

## PipeWire / WirePlumber integration

The install script copies the WirePlumber config automatically. To do it
manually:

```bash
# WirePlumber 0.5+ (Arch, Fedora 43+, Ubuntu 24.10+)
install -Dm644 wireplumber/wireplumber.conf.d/50-elgato-wave3.conf \
    "$HOME/.config/wireplumber/wireplumber.conf.d/50-elgato-wave3.conf"
install -Dm644 wireplumber/scripts/elgato-wave3.lua \
    "$HOME/.local/share/wireplumber/scripts/elgato-wave3.lua"

# Static virtual mix sinks
sudo install -Dm644 pipewire/pipewire.conf.d/wave3-mix-sinks.conf \
    /etc/pipewire/pipewire.conf.d/

systemctl --user restart pipewire wireplumber
systemctl --user enable --now wave3-daemon
```

The WirePlumber script renames the ALSA capture node to `wave3-source`,
disables the default ALSA playback node, and creates a proper
`wave3-sink` node for the headphone output. It also keeps the mic source
awake with a `wave3-null-sink`. This pattern is derived from the
[Undertone](https://github.com/polariscli/Undertone) project.

## ALSA UCM profile

```bash
sudo install -Dm644 "alsa-ucm/Elgato Wave 3.conf" \
    "/usr/share/alsa/ucm2/conf.d/Elgato Wave 3/Elgato Wave 3.conf"
sudo install -Dm644 alsa-ucm/HiFi.conf \
    "/usr/share/alsa/ucm2/conf.d/Elgato Wave 3/HiFi.conf"
```

## Verified hardware state

From a live test on the connected Wave:3:

* HP volume range: **-60.0 dB … 0.0 dB**
* Mic gain range: **0.0 dB … 40.0 dB**
* Mic gain is read-only via UAC (hardware dial controls it)
* Mute, headphone mute and headphone volume can be set from software

## What requires further mapping

The same class-based config block on interface 3 is used by Elgato Wave
Link for advanced features.  The logical paths are known from static
analysis:

* RGB/mute-light color (`/leds/*`)
* Clipguard (`/config/clipguard_enable`)
* Low-cut filter (`/config/lowcut_enable`)
* Direct monitor mix (`/config/direct_monitor`, `/moninor_mix/level/*`)
* Mixer routing (`/mixer/*`)
* Indicator/background brightness

Static analysis suggests that **low-cut filter**, **compressor**, and
**EQ** may be host-side DSP in Wave Link rather than hardware controls.
LED colors, direct monitor mix, and clipguard are almost certainly
hardware controls stored in the remaining config bytes.

The D-Bus API and GUI already expose these as methods, but the
underlying byte offsets are not yet confirmed.  To complete them, either
physically observe each config byte or capture Wave Link in a Windows VM
with `usbmon`.  See `docs/protocol-notes.md` and
`docs/wave3-descriptor-paths.md`.

## Related projects

* [rikkichy/openwave](https://github.com/rikkichy/openwave) — Wave XLR
  Linux control app; source of the `wIndex` trick.
* [x4ndr0m3d4x/wave3ctl](https://git.4ndr0m3d4.me/4ndr0m3d4/wave3ctl) —
  upstream kernel-module + Python CLI for the Wave:3 (vendored under
  `wave3ctl/`).
* [polariscli/Undertone](https://github.com/polariscli/Undertone) —
  Rust PipeWire mixer for the Wave:3. Its WirePlumber topology was
  adapted here.
* [Raphiiko/wavelink-ts](https://github.com/Raphiiko/wavelink-ts) —
  documents Wave Link's JSON-RPC app protocol (logical names only).

## Tests

```bash
cd native-linux
./src/wave3-daemon &
python3 tests/test-dbus.py
```

## Files

```
native-linux/
├── alsa-ucm/                          ALSA UCM profile
├── bin/wave3ctl                       shell CLI
├── dbus/
│   ├── org.wave3.Daemon.service       D-Bus session activation
│   └── org.wave3.Daemon-system.conf   D-Bus system policy (optional)
├── docs/protocol-notes.md             protocol documentation
├── gui/wave3-applet.py                GTK4 GUI applet
├── pkg/PKGBUILD                       Arch package recipe
├── pipewire/                          static virtual mix sinks
├── src/
│   ├── wave3-daemon.c                 main daemon
│   ├── probe_wave3.c                  descriptor / interface probe
│   ├── restore_defaults.c             restores safe defaults
│   └── Makefile
├── systemd/wave3-daemon.service       user systemd unit
├── tests/test-dbus.py                 integration tests
├── udev/50-elgato-wave3.rules         permission rule
├── wave3ctl/                          vendored upstream kernel-module solution
├── wireplumber/                       WirePlumber 0.5 config + script
├── install.sh                         user install script
├── LICENSE
└── README.md                          this file
```

## License

`wave3-daemon`, `wave3ctl`, and the probe programs are provided under
the MIT license. The vendored `wave3ctl/` directory is GPL-2.0-only;
see `wave3ctl/LICENSE`.
