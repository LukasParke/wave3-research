#!/usr/bin/env python3
"""
Integration tests for the wave3-daemon D-Bus API.

Requires the daemon to be running and a Wave:3 connected.
This script restores the original headphone volume before exiting.
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
        clipguard, lowcut, direct_monitor, mute_rgb, hp_rgb, in_level, pb_level = state

    assert isinstance(mic_mute, (bool, dbus.Boolean)), "mic_mute must be bool"
    assert 0 <= mic_gain_pct <= 100, "mic gain percent out of range"
    assert 0 <= hp_vol_pct <= 100, "hp volume percent out of range"
    print("PASS: GetState returns sane values")

    original_hp_vol = hp_vol_pct
    proxy.SetHpVolume(42)
    state2 = proxy.GetState()
    assert state2[3] == 42, f"hp volume did not change: {state2[3]}"
    print("PASS: SetHpVolume works")

    proxy.ToggleMicMute()
    state3 = proxy.GetState()
    assert bool(state3[0]) != bool(mic_mute), "mic mute did not toggle"
    print("PASS: ToggleMicMute works")
    proxy.ToggleMicMute()  # restore

    # Vendor methods should report not-yet-decoded but not crash
    try:
        proxy.SetClipguard(True)
    except dbus.DBusException:
        print("PASS: SetClipguard reports unsupported as expected")
    else:
        print("FAIL: SetClipguard should report unsupported")
        return 1

    proxy.SetHpVolume(original_hp_vol)
    print(f"PASS: restored HP volume to {original_hp_vol}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
