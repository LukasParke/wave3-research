# Wave Link logical control paths recovered from `waveapi.dll`

This file lists the logical control-path namespaces extracted from the
Wave Link 3.0 Windows `waveapi.dll` (and matching macOS
`WaveAPI.framework`).  These paths are used internally by the
`SessionAPI::Impl` descriptor constructors; they describe the features
Wave Link can address, but **not all of them apply to the first-generation
Wave:3** (PID `0x0070`).

## What these paths tell us

* The cross-platform audio engine exposes a **uniform path API** across
  multiple Elgato/Lewitt devices.
* Many paths are for host-side DSP (compressor, EQ, mixer) and are not
  sent to the microphone firmware.
* Paths related to `/mute`, `/volume`, `/gain`, `/headphone1/*`,
  `/indicator_*`, `/background_*`, `/moninor_mix/*`, `*_color_rgb/*`,
  and `/clipguard_enable` are the most likely to correspond to hardware
  USB controls on the Wave:3.

## Namespaces

| Namespace | Likely scope |
|-----------|--------------|
| `/clipguard_enable` | hardware clipguard/limiter toggle |
| `/direct_monitor` | hardware mic/PC monitor mix |
| `/dspfx/*` | host-side DSP (compressor, EQ, expander, limiter) |
| `/error/*` | device/host error reporting |
| `/gain_*` | gain meter / indicator colors |
| `/headphone*/*` | headphone output state, volume, limiter, LED colors |
| `/hw_*` / `/hw_outputs/*` / `/hw_input/*` | hardware audio routing / levels |
| `/line/*` | line/gain-lock controls |
| `/mic_det_*` | microphone detection / clipping counters |
| `/microphone_color_rgb/*` | mic LED RGB components |
| `/mixer/*` | host-side mixer inputs/levels |
| `/moninor_mix/*` | direct monitor mix levels |
| `/post_dsp/*` | post-processing level meters |
| `/sample_rate_hz` | device sample rate |
| `/usb_outputs/*` | USB audio routing |

## Paths that probably matter for Wave:3 hardware

```
/clipguard_enable
/direct_monitor
/gain_meter_background
/gain_reduction_value_indicator
/gain_value_indicator
/headphone1/limiter_bypassed
/headphone1/mute
/headphone1/volume
/headphone2/limiter_bypassed
/headphone2/mute
/headphone2/volume
/headphone_color_rgb/blue
/headphone_color_rgb/green
/headphone_color_rgb/red
/headphone_detected
/headphone_meter_background
/headphone_mute
/headphone_value_indicator
/headphone_volume
/indicator_brightness_bleed
/indicator_brightness_main
/limiter_bypassed
/limiter_mode
/line/gain_lock
/line/limiter_bypassed
/line/volume
/lowcut_enable
/lowcut_enabled
/lowcut1_enabled
/lowcut2_enabled
/mic_clipping_counter
/mic_det_disabled
/mic_det_threshold
/mic_det_value
/microphone_color_rgb/blue
/microphone_color_rgb/green
/microphone_color_rgb/red
/mic_select/0
/mic_select/1
/mixer/0/input_enabled/0..11
/mixer/0/input_volume/0..11
/mixer/1/input_enabled/0..11
/mixer/1/input_volume/0..11
/mixer/2/input_enabled/0..11
/mixer/2/input_volume/0..11
/mixer/3/input_enabled/0..11
/mixer/3/input_volume/0..11
/mixer/crossfade
/mixer/mode
/mixer_inputs/source_select
/moninor_mix/level/0
/moninor_mix/level/1
/mute_background
/post_dsp/level/0
/post_dsp/level/1
/sample_rate_hz
/status_brightness
/structureborne_lowcut_enabled
/usb_outputs/source_select
```

## Complete path dump

The full, sorted path list is also stored in the repo:

* `wavelink/teardown/waveapi-control-paths.txt` — 120 top-level paths
* `wavelink/teardown/waveapi-all-control-paths.txt` — subset that
  includes Wave:3-specific paths
* `wavelink/teardown/waveapi-control-search.txt` — search-oriented list
  used during static analysis

These files were generated from `waveapi.dll` string references.

## Property IDs

The exact byte-sized USB property IDs for each path are **not** stored as
plain constants in `waveapi.dll`.  Static reverse engineering shows they
are generated from the `SessionAPI::Impl` descriptor table and returned by
`IMessage::propertyId()` (vtable offset 0) before being placed in the USB
`wValue` field.  Recovering the mapping requires either deeper static
parsing of the descriptor table or a live USB capture with the device in
APP mode.

See `protocol-notes.md` and `../../WAVE3_PROTOCOL_SUMMARY.md` for the
recovered request encoding and the APP-mode caveat.
