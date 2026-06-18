#!/usr/bin/env python3
"""
wave3-applet — GTK4 tray applet for the Elgato Wave:3 native daemon.

Shows mute status, gain, headphone volume, and provides toggles for
Clipguard, low-cut, direct monitor mix, and RGB mute color.
"""

import gi
gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
from gi.repository import Gtk, Gdk, GLib

import dbus
import dbus.mainloop.glib
import sys


def get_proxy():
    bus = dbus.SessionBus()
    proxy = bus.get_object("org.wave3.Daemon", "/org/wave3/Daemon")
    return dbus.Interface(proxy, "org.wave3.Daemon")


class Wave3Applet(Gtk.Application):
    def __init__(self):
        super().__init__(application_id="org.wave3.applet")
        self.proxy = None
        self.window = None
        self.menu = None
        self.tray_icon = None

    def do_activate(self):
        if not self.proxy:
            try:
                self.proxy = get_proxy()
            except Exception as e:
                self._error_dialog(f"Cannot connect to wave3-daemon:\n{e}")
                return

        if not self.tray_icon:
            # GTK4 removed system-tray widgets; use a small persistent window
            # or rely on desktop extension.  Provide a simple window.
            self._build_main_window()
        else:
            self.tray_icon.present()

    def _error_dialog(self, text):
        dlg = Gtk.MessageDialog(
            transient_for=None,
            modal=True,
            message_type=Gtk.MessageType.ERROR,
            buttons=Gtk.ButtonsType.OK,
            text=text,
        )
        dlg.connect("response", lambda *_: self.quit())
        dlg.present()

    def _build_main_window(self):
        win = Gtk.ApplicationWindow(application=self, title="Wave:3 Applet")
        win.set_default_size(360, 380)

        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12)
        box.set_margin_top(12)
        box.set_margin_bottom(12)
        box.set_margin_start(12)
        box.set_margin_end(12)
        win.set_child(box)

        self.status_label = Gtk.Label(label="Wave:3 — connecting…")
        self.status_label.set_xalign(0)
        box.append(self.status_label)

        box.append(Gtk.Separator())

        # Mic mute
        self.mute_btn = Gtk.Button(label="Toggle Mic Mute")
        self.mute_btn.connect("clicked", self._on_toggle_mute)
        box.append(self.mute_btn)

        # Headphone volume
        box.append(Gtk.Label(label="Headphone Volume"))
        self.hp_scale = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 0, 100, 1)
        self.hp_scale.set_digits(0)
        self.hp_scale.set_hexpand(True)
        self.hp_scale.connect("value-changed", self._on_hp_volume_changed)
        box.append(self.hp_scale)

        # Clipguard
        h = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12)
        h.append(Gtk.Label(label="Clipguard"))
        self.clip_switch = Gtk.Switch()
        self.clip_switch.connect("state-set", self._on_clipguard_changed)
        h.append(self.clip_switch)
        box.append(h)

        # Low-cut
        h = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12)
        h.append(Gtk.Label(label="Low-cut"))
        self.lowcut_switch = Gtk.Switch()
        self.lowcut_switch.connect("state-set", self._on_lowcut_changed)
        h.append(self.lowcut_switch)
        box.append(h)

        # Direct monitor
        box.append(Gtk.Label(label="Direct Monitor Mix"))
        self.dm_scale = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 0.0, 1.0, 0.01)
        self.dm_scale.set_digits(2)
        self.dm_scale.set_hexpand(True)
        self.dm_scale.connect("value-changed", self._on_dm_changed)
        box.append(self.dm_scale)

        # Mute color
        h = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12)
        h.append(Gtk.Label(label="Mute-ring color"))
        self.color_btn = Gtk.ColorButton()
        self.color_btn.connect("color-set", self._on_color_set)
        h.append(self.color_btn)
        box.append(h)

        self.window = win
        win.present()

        self._poll()
        GLib.timeout_add_seconds(1, self._poll)

    def _on_toggle_mute(self, _btn):
        try:
            self.proxy.ToggleMicMute()
            self._poll()
        except Exception as e:
            print(f"ToggleMicMute failed: {e}")

    def _on_hp_volume_changed(self, scale):
        try:
            self.proxy.SetHpVolume(int(scale.get_value()))
        except Exception as e:
            print(f"SetHpVolume failed: {e}")

    def _on_clipguard_changed(self, _switch, state):
        try:
            self.proxy.SetClipguard(bool(state))
        except Exception as e:
            print(f"SetClipguard failed: {e}")
        return False

    def _on_lowcut_changed(self, _switch, state):
        try:
            self.proxy.SetLowCut(bool(state))
        except Exception as e:
            print(f"SetLowCut failed: {e}")
        return False

    def _on_dm_changed(self, scale):
        try:
            self.proxy.SetDirectMonitor(scale.get_value())
        except Exception as e:
            print(f"SetDirectMonitor failed: {e}")

    def _on_color_set(self, btn):
        rgba = btn.get_rgba()
        rgb = (int(rgba.red * 255) << 16) | (int(rgba.green * 255) << 8) | int(rgba.blue * 255)
        try:
            self.proxy.SetMuteColor(rgb)
        except Exception as e:
            print(f"SetMuteColor failed: {e}")

    def _poll(self):
        try:
            state = self.proxy.GetState()
            mic_mute, hp_mute, mic_gain_pct, hp_vol_pct, mic_gain_db, hp_vol_db, \
                clipguard, lowcut, direct_monitor, mute_rgb, hp_rgb, in_level, pb_level = state

            status = (
                f"Mic: {'MUTED' if mic_mute else 'LIVE'} ({mic_gain_pct}% / {mic_gain_db} dB)\n"
                f"HP: {'MUTED' if hp_mute else 'ON'} ({hp_vol_pct}% / {hp_vol_db} dB)\n"
                f"Clipguard: {'ON' if clipguard else 'OFF'} | Low-cut: {'ON' if lowcut else 'OFF'}\n"
                f"Direct monitor: {direct_monitor:.2f}"
            )
            self.status_label.set_text(status)

            self.hp_scale.set_value(hp_vol_pct)
            self.clip_switch.set_state(clipguard)
            self.lowcut_switch.set_state(lowcut)
            self.dm_scale.set_value(direct_monitor)

            r = ((mute_rgb >> 16) & 0xff) / 255.0
            g = ((mute_rgb >> 8) & 0xff) / 255.0
            b = (mute_rgb & 0xff) / 255.0
            self.color_btn.set_rgba(Gdk.RGBA(red=r, green=g, blue=b, alpha=1.0))
        except Exception as e:
            print(f"poll failed: {e}")
        return True


if __name__ == "__main__":
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    app = Wave3Applet()
    sys.exit(app.run())
