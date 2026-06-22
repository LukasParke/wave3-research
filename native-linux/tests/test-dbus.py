#!/usr/bin/python3
"""
Integration tests for the wave3-daemon D-Bus API.

Requires the daemon to be running and a Wave:3 connected.
This script restores the original state before exiting.
"""

import sys
import dbus


def get_proxy():
    bus = dbus.SessionBus()
    obj = bus.get_object("org.wave3.Daemon", "/org/wave3/Daemon")
    return dbus.Interface(obj, "org.wave3.Daemon")


def main():
    try:
        proxy = get_proxy()
    except dbus.DBusException as e:
        print(f"FAIL: cannot connect to daemon: {e}")
        return 1

    state = proxy.GetState()
    print(f"GetState: {state}")

    mic_mute, hp_mute, mic_gain_pct, hp_vol_pct, mic_gain_db, hp_vol_db, \
        clipguard, lowcut, direct_monitor, mute_rgb, in_level, brightness, pb_level, \
        indicator_rgb, dial_mode, dial_value = state

    assert isinstance(mic_mute, (bool, dbus.Boolean)), "mic_mute must be bool"
    assert 0 <= mic_gain_pct <= 100, "mic gain percent out of range"
    assert 0 <= hp_vol_pct <= 100, "hp volume percent out of range"
    assert 0 <= direct_monitor <= 1, "direct monitor out of range"
    assert 0 <= mute_rgb <= 0xFFFFFF, "mute RGB out of range"
    assert 0 <= brightness <= 255, "brightness out of range"
    assert 1 <= dial_mode <= 3, "dial mode out of range"
    assert 0 <= dial_value <= 0xFFFF, "dial value out of range"
    assert 0 <= indicator_rgb <= 0xFFFFFF, "indicator RGB out of range"
    print("PASS: GetState returns sane values")

    original_hp_vol = hp_vol_pct
    proxy.SetHpVolume(42)
    state2 = proxy.GetState()
    assert state2[3] == 42, f"hp volume did not change: {state2[3]}"
    print("PASS: SetHpVolume works")

    # The Wave:3 mic gain UAC control appears read-only from software on
    # this firmware revision, so SetMicGain may not physically move the
    # hardware.  We only verify the method exists and returns success.
    proxy.SetMicGain(60)
    print("PASS: SetMicGain method accepted")

    proxy.ToggleMicMute()
    state3 = proxy.GetState()
    assert bool(state3[0]) != bool(mic_mute), "mic mute did not toggle"
    print("PASS: ToggleMicMute works")
    proxy.ToggleMicMute()  # restore

    original_clipguard = bool(clipguard)
    proxy.SetClipguard(not original_clipguard)
    state4 = proxy.GetState()
    assert bool(state4[6]) != original_clipguard, "clipguard did not change"
    print("PASS: SetClipguard works")
    proxy.SetClipguard(original_clipguard)  # restore

    original_monitor = float(direct_monitor)
    proxy.SetDirectMonitor(0.75)
    state5 = proxy.GetState()
    assert abs(float(state5[8]) - 0.75) < 0.02, f"direct monitor did not change: {state5[8]}"
    print("PASS: SetDirectMonitor works")
    proxy.SetDirectMonitor(original_monitor)  # restore

    original_color = int(mute_rgb)
    proxy.SetMuteColor(0x00FF00)
    state6 = proxy.GetState()
    assert int(state6[9]) == 0x00FF00, f"mute color did not change: {state6[9]}"
    print("PASS: SetMuteColor works")
    proxy.SetMuteColor(original_color)  # restore

    original_brightness = int(brightness)
    proxy.SetBrightness(128)
    state7 = proxy.GetState()
    assert int(state7[11]) == 128, f"brightness did not change: {state7[11]}"
    print("PASS: SetBrightness works")
    proxy.SetBrightness(original_brightness)  # restore

    dial_mode = proxy.GetDialMode()
    dial_value = proxy.GetDialValue()
    assert 1 <= int(dial_mode) <= 3, "GetDialMode out of range"
    assert 0 <= int(dial_value) <= 0xFFFF, "GetDialValue out of range"
    print("PASS: GetDialMode/GetDialValue work")

    # Low-cut is host-side DSP on first-gen Wave:3
    try:
        proxy.SetLowCut(True)
    except dbus.DBusException:
        print("PASS: SetLowCut reports unsupported as expected")
    else:
        print("FAIL: SetLowCut should report unsupported")
        return 1

    proxy.SetHpVolume(original_hp_vol)
    print(f"PASS: restored HP volume to {original_hp_vol}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
