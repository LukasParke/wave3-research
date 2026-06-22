/*
 * wave3-daemon — native D-Bus service for the Elgato Wave:3 on Linux
 *
 * Discovers the Wave:3, claims its unclaimed vendor interface (3), and
 * talks to the standard USB Audio Class feature units by routing control
 * transfers through interface 3.  This bypasses snd-usb-audio's ownership
 * of interface 0 without a kernel module.
 *
 * Exposes org.wave3.Daemon on the session bus.
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib-unix.h>
#include <glib.h>
#include <libusb-1.0/libusb.h>

#define WAVE3_VID 0x0fd9
#define WAVE3_PID 0x0070
#define WAVE3_IFACE 3

#define UAC_GET_CUR 0x81
#define UAC_SET_CUR 0x01
#define UAC_BM_IN   0xa1
#define UAC_BM_OUT  0x21
#define UAC_MUTE    0x01
#define UAC_VOLUME  0x02

#define HP_FU  5
#define MIC_FU 6

#define POLL_MS 100

/* Vendor/class control interface (interface 3, endpoint 0) */
#define WAVE3_CFG_WVALUE    0x0000   /* 16-byte config block */
#define WAVE3_METER_WVALUE  0x0001   /* 8-byte meter block */
#define WAVE3_INFO_WVALUE   0x000A   /* 51-byte device info */

/* Wave:3 config block layout (16 bytes) */
#define CFG_DIAL_VALUE_LO   0   /* current dial value, low byte */
#define CFG_DIAL_VALUE_HI   1   /* current dial value, high byte */
#define CFG_MIC_MUTE        4
#define CFG_CLIPGUARD       5
#define CFG_DIAL_FLAG       7   /* 0x80 toggles with HP volume changes */
#define CFG_HP_VOLUME       8   /* signed 8-bit dB attenuation */
#define CFG_HP_MUTE         9
#define CFG_INDICATOR_R     10  /* RGB ring feedback */
#define CFG_INDICATOR_G     11  /* also monitor mix value when in mix mode */
#define CFG_DIAL_MODE       12  /* 1=mic gain, 2=hp volume, 3=monitor mix */
#define CFG_INDICATOR_B     13
#define CFG_MUTE_R          CFG_INDICATOR_R
#define CFG_MUTE_G          CFG_INDICATOR_G
#define CFG_MUTE_B          CFG_INDICATOR_B
#define CFG_MONITOR_MIX     14  /* 0 = mic only, 255 = PC only (software mix) */
#define CFG_BRIGHTNESS      15

typedef struct {
    libusb_context *ctx;
    libusb_device_handle *dev;

    /* connection state */
    gboolean connected;
    gboolean notified_disconnected;

    /* cached state */
    gboolean mic_mute;
    gint16   mic_gain;   /* read-only on Wave:3 (physical dial) */
    gboolean hp_mute;
    gint16   hp_volume;

    gint16 hp_vol_min, hp_vol_max;
    gint16 mic_gain_min, mic_gain_max;

    /* vendor features (cached) */
    gboolean clipguard;
    gboolean lowcut;
    gdouble  direct_monitor;
    guint32  mute_rgb;
    guint32  indicator_rgb;
    guint32  brightness;
    guint    dial_mode;
    gint16   dial_value;
    gdouble  input_level_db;
    gdouble  playback_level_db;

    /* D-Bus */
    GDBusConnection *bus;
    guint owner_id;
    gchar *obj_path;
} Wave3Daemon;

static Wave3Daemon g_daemon;

/* forward declarations for config helpers */
static int wave3_cfg_read(Wave3Daemon *d, unsigned char *cfg);
static int wave3_cfg_write(Wave3Daemon *d, const unsigned char *cfg);
static int wave3_meter_read(Wave3Daemon *d, unsigned char *meter);
static gdouble level_to_db(uint32_t raw);
static gboolean wave3_open_device(Wave3Daemon *d);
static void wave3_close_device(Wave3Daemon *d);
static gboolean wave3_is_io_error(int libusb_err);
static int wave3_get_range(Wave3Daemon *d, int entity, gint16 *min, gint16 *max, gint16 *res);

/* ── device open/close ─────────────────────────────────────────────────── */

static void wave3_close_device(Wave3Daemon *d)
{
    if (!d->dev) return;
    g_message("Releasing Wave:3 USB interface and closing device");
    libusb_release_interface(d->dev, WAVE3_IFACE);
    libusb_close(d->dev);
    d->dev = NULL;
    d->connected = FALSE;
}

static gboolean wave3_open_device(Wave3Daemon *d)
{
    int r;

    if (d->dev) {
        d->connected = TRUE;
        return TRUE;
    }

    if (!d->ctx) {
        r = libusb_init(&d->ctx);
        if (r < 0) {
            g_warning("libusb_init failed: %s", libusb_error_name(r));
            return FALSE;
        }
    }

    d->dev = libusb_open_device_with_vid_pid(d->ctx, WAVE3_VID, WAVE3_PID);
    if (!d->dev) {
        /* Device not present right now; this is normal during reconnect. */
        return FALSE;
    }

    r = libusb_claim_interface(d->dev, WAVE3_IFACE);
    if (r < 0) {
        g_warning("claim interface %d failed: %s", WAVE3_IFACE, libusb_error_name(r));
        libusb_close(d->dev);
        d->dev = NULL;
        return FALSE;
    }

    if (wave3_get_range(d, HP_FU, &d->hp_vol_min, &d->hp_vol_max, &(gint16){0}) < 0) {
        d->hp_vol_min = -60 * 256;
        d->hp_vol_max = 0;
    }
    if (wave3_get_range(d, MIC_FU, &d->mic_gain_min, &d->mic_gain_max, &(gint16){0}) < 0) {
        d->mic_gain_min = 0;
        d->mic_gain_max = 40 * 256;
    }

    d->connected = TRUE;
    d->notified_disconnected = FALSE;
    g_message("Wave:3 connected and interface %d claimed", WAVE3_IFACE);
    g_message("  HP volume range: %.1f … %.1f dB",
              d->hp_vol_min / 256.0, d->hp_vol_max / 256.0);
    g_message("  Mic gain range:  %.1f … %.1f dB (read-only dial)",
              d->mic_gain_min / 256.0, d->mic_gain_max / 256.0);
    return TRUE;
}

static gboolean wave3_is_io_error(int libusb_err)
{
    return libusb_err == LIBUSB_ERROR_NO_DEVICE ||
           libusb_err == LIBUSB_ERROR_IO ||
           libusb_err == LIBUSB_ERROR_NOT_FOUND ||
           libusb_err == LIBUSB_ERROR_BUSY ||
           libusb_err == LIBUSB_ERROR_PIPE;
}

static gboolean wave3_ensure_open(Wave3Daemon *d)
{
    if (d->connected && d->dev) return TRUE;
    return wave3_open_device(d);
}

/* ── UAC helpers ───────────────────────────────────────────────────────── */

static int wave3_uac_get(Wave3Daemon *d, int entity, int selector,
                         int channel, unsigned char *out, int len)
{
    if (!d->dev) return LIBUSB_ERROR_NO_DEVICE;
    uint16_t wValue  = (uint16_t)((selector << 8) | channel);
    uint16_t wIndex3 = (uint16_t)((entity << 8) | WAVE3_IFACE);
    return libusb_control_transfer(d->dev, UAC_BM_IN, UAC_GET_CUR,
                                   wValue, wIndex3, out, len, 1000);
}

static int wave3_get_range(Wave3Daemon *d, int entity, gint16 *min, gint16 *max, gint16 *res)
{
    unsigned char buf[2];
    uint16_t wIndex3 = (uint16_t)((entity << 8) | WAVE3_IFACE);
    int r;

    r = libusb_control_transfer(d->dev, UAC_BM_IN, 0x82, /* GET_MIN */
                                (UAC_VOLUME << 8), wIndex3, buf, 2, 1000);
    if (r < 0) return r;
    *min = (gint16)((buf[1] << 8) | buf[0]);

    r = libusb_control_transfer(d->dev, UAC_BM_IN, 0x83, /* GET_MAX */
                                (UAC_VOLUME << 8), wIndex3, buf, 2, 1000);
    if (r < 0) return r;
    *max = (gint16)((buf[1] << 8) | buf[0]);

    r = libusb_control_transfer(d->dev, UAC_BM_IN, 0x84, /* GET_RES */
                                (UAC_VOLUME << 8), wIndex3, buf, 2, 1000);
    if (r < 0) return r;
    *res = (gint16)((buf[1] << 8) | buf[0]);

    return 0;
}

/* ── state helpers ─────────────────────────────────────────────────────── */

static int pct_from_raw(gint16 raw, gint16 lo, gint16 hi)
{
    if (hi <= lo) return 0;
    return (int)CLAMP(round((raw - lo) * 100.0 / (hi - lo)), 0.0, 100.0);
}

static gint16 pct_to_raw(int pct, gint16 lo, gint16 hi)
{
    pct = (int)CLAMP(pct, 0, 100);
    return (gint16)round(lo + (hi - lo) * pct / 100.0);
}

static GVariant *wave3_build_state(Wave3Daemon *d)
{
    return g_variant_new("(bbuuuubbdududduu)",
                         d->mic_mute,
                         d->hp_mute,
                         (guint)pct_from_raw(d->mic_gain, d->mic_gain_min, d->mic_gain_max),
                         (guint)pct_from_raw(d->hp_volume, d->hp_vol_min, d->hp_vol_max),
                         (guint)abs(d->mic_gain / 256),
                         (guint)abs(d->hp_volume / 256),
                         d->clipguard,
                         d->lowcut,
                         d->direct_monitor,
                         d->mute_rgb,
                         d->input_level_db,
                         d->brightness,
                         d->playback_level_db,
                         d->dial_mode,
                         (guint)d->dial_value);
}

static GVariant *wave3_build_state_outer(Wave3Daemon *d)
{
    return g_variant_new("((bbuuuubbdududduu))",
                         d->mic_mute,
                         d->hp_mute,
                         (guint)pct_from_raw(d->mic_gain, d->mic_gain_min, d->mic_gain_max),
                         (guint)pct_from_raw(d->hp_volume, d->hp_vol_min, d->hp_vol_max),
                         (guint)abs(d->mic_gain / 256),
                         (guint)abs(d->hp_volume / 256),
                         d->clipguard,
                         d->lowcut,
                         d->direct_monitor,
                         d->mute_rgb,
                         d->input_level_db,
                         d->brightness,
                         d->playback_level_db,
                         d->dial_mode,
                         (guint)d->dial_value);
}

static void wave3_log_changes(Wave3Daemon *d, gboolean old_mic_mute, gboolean old_hp_mute,
                              gint16 old_mic_gain, gint16 old_hp_volume,
                              gboolean old_clipguard, gdouble old_direct_monitor,
                              guint32 old_mute_rgb, guint32 old_indicator_rgb,
                              guint32 old_brightness, guint old_dial_mode,
                              gint16 old_dial_value)
{
    if (d->mic_mute != old_mic_mute)
        g_message("Mic mute changed: %s -> %s",
                  old_mic_mute ? "MUTED" : "LIVE",
                  d->mic_mute ? "MUTED" : "LIVE");
    if (d->hp_mute != old_hp_mute)
        g_message("Headphone mute changed: %s -> %s",
                  old_hp_mute ? "MUTED" : "ON",
                  d->hp_mute ? "MUTED" : "ON");
    if (d->mic_gain != old_mic_gain)
        g_message("Mic gain changed: %.1f dB (%d%%)",
                  d->mic_gain / 256.0,
                  pct_from_raw(d->mic_gain, d->mic_gain_min, d->mic_gain_max));
    if (d->hp_volume != old_hp_volume)
        g_message("Headphone volume changed: %.1f dB (%d%%)",
                  d->hp_volume / 256.0,
                  pct_from_raw(d->hp_volume, d->hp_vol_min, d->hp_vol_max));
    if (d->clipguard != old_clipguard)
        g_message("Clipguard changed: %s", d->clipguard ? "ON" : "OFF");
    if (fabs(d->direct_monitor - old_direct_monitor) > 0.01)
        g_message("Direct monitor mix changed: %.2f", d->direct_monitor);
    if (d->mute_rgb != old_mute_rgb)
        g_message("Mute color changed: 0x%06x", d->mute_rgb);
    if (d->indicator_rgb != old_indicator_rgb)
        g_message("Indicator color changed: 0x%06x", d->indicator_rgb);
    if (d->brightness != old_brightness)
        g_message("Brightness changed: %u", d->brightness);
    if (d->dial_mode != old_dial_mode)
        g_message("Dial mode changed: %u (%s)", d->dial_mode,
                  d->dial_mode == 1 ? "mic gain" :
                  d->dial_mode == 2 ? "hp volume" :
                  d->dial_mode == 3 ? "monitor mix" : "unknown");
    if (d->dial_value != old_dial_value)
        g_message("Dial value changed: %d", d->dial_value);
    /* Level meters are intentionally not logged here; they fluctuate
     * continuously at the noise floor and would flood the journal.
     * They are still emitted via StateChanged and readable via
     * GetInputLevel / GetPlaybackLevel.
     */
}

static gboolean wave3_refresh(Wave3Daemon *d)
{
    unsigned char buf[8];
    unsigned char cfg[16];
    gboolean changed = FALSE;
    int r;

    if (!wave3_ensure_open(d)) {
        if (!d->notified_disconnected) {
            d->notified_disconnected = TRUE;
            g_message("Wave:3 not connected; waiting for device");
        }
        return FALSE;
    }

    /* snapshot old values for logging */
    gboolean old_mic_mute = d->mic_mute;
    gboolean old_hp_mute = d->hp_mute;
    gint16 old_mic_gain = d->mic_gain;
    gint16 old_hp_volume = d->hp_volume;
    gboolean old_clipguard = d->clipguard;
    gdouble old_direct_monitor = d->direct_monitor;
    guint32 old_mute_rgb = d->mute_rgb;
    guint32 old_indicator_rgb = d->indicator_rgb;
    guint32 old_brightness = d->brightness;
    guint old_dial_mode = d->dial_mode;
    gint16 old_dial_value = d->dial_value;


    /* mic gain is still read via standard UAC (physical dial) */
    r = wave3_uac_get(d, MIC_FU, UAC_VOLUME, 0, buf, 2);
    if (r == 2) {
        gint16 v = (gint16)((buf[1] << 8) | buf[0]);
        if (v != d->mic_gain) { d->mic_gain = v; changed = TRUE; }
    } else if (wave3_is_io_error(r)) {
        g_warning("Mic gain read failed (%s); closing device", libusb_error_name(r));
        wave3_close_device(d);
        return FALSE;
    }

    r = wave3_cfg_read(d, cfg);
    if (r == 16) {
        gboolean mic_mute = cfg[CFG_MIC_MUTE] ? TRUE : FALSE;
        if (mic_mute != d->mic_mute) { d->mic_mute = mic_mute; changed = TRUE; }

        gboolean hp_mute = cfg[CFG_HP_MUTE] ? TRUE : FALSE;
        if (hp_mute != d->hp_mute) { d->hp_mute = hp_mute; changed = TRUE; }

        /* cfg[8] is signed dB attenuation; scale to 1/256 dB for the state API */
        gint16 hp_vol = (gint8)cfg[CFG_HP_VOLUME] * 256;
        if (hp_vol != d->hp_volume) { d->hp_volume = hp_vol; changed = TRUE; }

        gboolean clipguard = cfg[CFG_CLIPGUARD] ? TRUE : FALSE;
        if (clipguard != d->clipguard) { d->clipguard = clipguard; changed = TRUE; }

        gdouble monitor = cfg[CFG_MONITOR_MIX] / 255.0;
        if (fabs(monitor - d->direct_monitor) > 0.01) { d->direct_monitor = monitor; changed = TRUE; }

        guint32 mute_rgb = ((guint32)cfg[CFG_MUTE_R] << 16) |
                           ((guint32)cfg[CFG_MUTE_G] << 8) |
                           ((guint32)cfg[CFG_MUTE_B]);
        if (mute_rgb != d->mute_rgb) { d->mute_rgb = mute_rgb; changed = TRUE; }

        guint32 indicator_rgb = ((guint32)cfg[CFG_INDICATOR_R] << 16) |
                                ((guint32)cfg[CFG_INDICATOR_G] << 8) |
                                ((guint32)cfg[CFG_INDICATOR_B]);
        if (indicator_rgb != d->indicator_rgb) { d->indicator_rgb = indicator_rgb; changed = TRUE; }

        guint32 brightness = cfg[CFG_BRIGHTNESS];
        if (brightness != d->brightness) { d->brightness = brightness; changed = TRUE; }

        guint dial_mode = cfg[CFG_DIAL_MODE];
        if (dial_mode < 1) dial_mode = 1;
        if (dial_mode > 3) dial_mode = 3;
        if (dial_mode != d->dial_mode) { d->dial_mode = dial_mode; changed = TRUE; }

        gint16 dial_value = (gint16)((cfg[CFG_DIAL_VALUE_HI] << 8) | cfg[CFG_DIAL_VALUE_LO]);
        if (dial_value != d->dial_value) { d->dial_value = dial_value; changed = TRUE; }

        /* level meters */
        unsigned char meter[8];
        r = wave3_meter_read(d, meter);
        if (r == 8) {
            uint32_t left  = (uint32_t)(meter[0] | (meter[1] << 8) | (meter[2] << 16) | (meter[3] << 24));
            uint32_t right = (uint32_t)(meter[4] | (meter[5] << 8) | (meter[6] << 16) | (meter[7] << 24));
            gdouble in_db = level_to_db(left);
            if (fabs(in_db - d->input_level_db) > 0.5) { d->input_level_db = in_db; changed = TRUE; }
            gdouble pb_db = level_to_db(right);
            if (fabs(pb_db - d->playback_level_db) > 0.5) { d->playback_level_db = pb_db; changed = TRUE; }
        } else if (wave3_is_io_error(r)) {
            g_warning("Meter read failed (%s); closing device", libusb_error_name(r));
            wave3_close_device(d);
            return FALSE;
        }

        if (changed)
            wave3_log_changes(d, old_mic_mute, old_hp_mute, old_mic_gain, old_hp_volume,
                              old_clipguard, old_direct_monitor, old_mute_rgb,
                              old_indicator_rgb, old_brightness, old_dial_mode,
                              old_dial_value);
    } else if (wave3_is_io_error(r)) {
        g_warning("Config read failed (%s); closing device", libusb_error_name(r));
        wave3_close_device(d);
        return FALSE;
    } else {
        g_warning("Config read returned unexpected length %d", r);
    }

    return changed;
}

/* ── D-Bus emit ────────────────────────────────────────────────────────── */

static void wave3_dbus_emit_state(Wave3Daemon *d)
{
    if (!d->bus) return;
    g_dbus_connection_emit_signal(d->bus,
                                  NULL,
                                  "/org/wave3/Daemon",
                                  "org.wave3.Daemon",
                                  "StateChanged",
                                  wave3_build_state(d),
                                  NULL);
}

/* ── class config/meter helpers ───────────────────────────────────────── */

static int wave3_cfg_read(Wave3Daemon *d, unsigned char *cfg)
{
    if (!d->dev) return LIBUSB_ERROR_NO_DEVICE;
    return libusb_control_transfer(d->dev, 0xA1, 0x85, WAVE3_CFG_WVALUE,
                                   (0x33 << 8) | WAVE3_IFACE,
                                   cfg, 16, 1000);
}

static int wave3_cfg_write(Wave3Daemon *d, const unsigned char *cfg)
{
    if (!d->dev) return LIBUSB_ERROR_NO_DEVICE;
    return libusb_control_transfer(d->dev, 0x21, 0x05, WAVE3_CFG_WVALUE,
                                   (0x33 << 8) | WAVE3_IFACE,
                                   (unsigned char *)cfg, 16, 1000);
}

static int wave3_meter_read(Wave3Daemon *d, unsigned char *meter)
{
    if (!d->dev) return LIBUSB_ERROR_NO_DEVICE;
    return libusb_control_transfer(d->dev, 0xA1, 0x85, WAVE3_METER_WVALUE,
                                   (0x33 << 8) | WAVE3_IFACE,
                                   meter, 8, 1000);
}

static gdouble level_to_db(uint32_t raw)
{
    if (raw == 0) return -INFINITY;
    /* full-scale reference observed in firmware meter; may need calibration */
    double fs = 0x80000000u;
    return 20.0 * log10(raw / fs);
}

/* ── D-Bus method handlers ─────────────────────────────────────────────── */

static void handle_method_call(G_GNUC_UNUSED GDBusConnection *conn,
                               G_GNUC_UNUSED const gchar *sender,
                               G_GNUC_UNUSED const gchar *object_path,
                               G_GNUC_UNUSED const gchar *interface_name,
                               const gchar *method_name,
                               GVariant *parameters,
                               GDBusMethodInvocation *inv,
                               gpointer user_data)
{
    Wave3Daemon *d = user_data;
    int r;

    if (!wave3_ensure_open(d)) {
        g_dbus_method_invocation_return_error(inv, G_IO_ERROR,
                                              G_IO_ERROR_NOT_CONNECTED,
                                              "Wave:3 is not connected");
        return;
    }

    if (g_strcmp0(method_name, "GetState") == 0) {
        g_dbus_method_invocation_return_value(inv, wave3_build_state_outer(d));
        return;
    }

    if (g_strcmp0(method_name, "SetMicMute") == 0) {
        gboolean muted;
        g_variant_get(parameters, "(b)", &muted);
        unsigned char cfg[16];
        r = wave3_cfg_read(d, cfg);
        if (r != 16) goto usb_err;
        cfg[CFG_MIC_MUTE] = muted ? 1 : 0;
        r = wave3_cfg_write(d, cfg);
        if (r != 16) goto usb_err;
        d->mic_mute = muted;
        g_message("SetMicMute: %s", muted ? "MUTED" : "LIVE");
        wave3_dbus_emit_state(d);
        g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", TRUE));
        return;
    }

    if (g_strcmp0(method_name, "ToggleMicMute") == 0) {
        unsigned char cfg[16];
        r = wave3_cfg_read(d, cfg);
        if (r != 16) goto usb_err;
        cfg[CFG_MIC_MUTE] = d->mic_mute ? 0 : 1;
        r = wave3_cfg_write(d, cfg);
        if (r != 16) goto usb_err;
        d->mic_mute = !d->mic_mute;
        g_message("ToggleMicMute: now %s", d->mic_mute ? "MUTED" : "LIVE");
        wave3_dbus_emit_state(d);
        g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", d->mic_mute));
        return;
    }

    if (g_strcmp0(method_name, "SetHpMute") == 0) {
        gboolean muted;
        g_variant_get(parameters, "(b)", &muted);
        unsigned char cfg[16];
        r = wave3_cfg_read(d, cfg);
        if (r != 16) goto usb_err;
        cfg[CFG_HP_MUTE] = muted ? 1 : 0;
        r = wave3_cfg_write(d, cfg);
        if (r != 16) goto usb_err;
        d->hp_mute = muted;
        g_message("SetHpMute: %s", muted ? "MUTED" : "ON");
        wave3_dbus_emit_state(d);
        g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", TRUE));
        return;
    }

    if (g_strcmp0(method_name, "SetHpVolume") == 0) {
        guint pct;
        g_variant_get(parameters, "(u)", &pct);
        gint16 raw = pct_to_raw((int)pct, d->hp_vol_min, d->hp_vol_max);
        unsigned char cfg[16];
        r = wave3_cfg_read(d, cfg);
        if (r != 16) goto usb_err;
        cfg[CFG_HP_VOLUME] = (unsigned char)(raw / 256);   /* signed dB */
        r = wave3_cfg_write(d, cfg);
        if (r != 16) goto usb_err;
        d->hp_volume = raw;
        g_message("SetHpVolume: %d%% (%.1f dB)", pct, raw / 256.0);
        wave3_dbus_emit_state(d);
        g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", TRUE));
        return;
    }

    if (g_strcmp0(method_name, "GetClipguard") == 0) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", d->clipguard));
        return;
    }

    if (g_strcmp0(method_name, "SetClipguard") == 0) {
        gboolean v;
        g_variant_get(parameters, "(b)", &v);
        unsigned char cfg[16];
        r = wave3_cfg_read(d, cfg);
        if (r != 16) goto usb_err;
        cfg[CFG_CLIPGUARD] = v ? 1 : 0;
        r = wave3_cfg_write(d, cfg);
        if (r != 16) goto usb_err;
        d->clipguard = v;
        g_message("SetClipguard: %s", v ? "ON" : "OFF");
        wave3_dbus_emit_state(d);
        g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", TRUE));
        return;
    }

    if (g_strcmp0(method_name, "GetLowCut") == 0) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", d->lowcut));
        return;
    }

    if (g_strcmp0(method_name, "SetLowCut") == 0) {
        g_dbus_method_invocation_return_error(inv, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                              "Low-cut is host-side DSP in Wave Link");
        return;
    }

    if (g_strcmp0(method_name, "GetDirectMonitor") == 0) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(d)", d->direct_monitor));
        return;
    }

    if (g_strcmp0(method_name, "SetDirectMonitor") == 0) {
        gdouble v;
        g_variant_get(parameters, "(d)", &v);
        if (v < 0.0) v = 0.0;
        if (v > 1.0) v = 1.0;
        unsigned char cfg[16];
        r = wave3_cfg_read(d, cfg);
        if (r != 16) goto usb_err;
        cfg[CFG_MONITOR_MIX] = (unsigned char)round(v * 255.0);
        r = wave3_cfg_write(d, cfg);
        if (r != 16) goto usb_err;
        d->direct_monitor = v;
        g_message("SetDirectMonitor: %.2f", v);
        wave3_dbus_emit_state(d);
        g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", TRUE));
        return;
    }

    if (g_strcmp0(method_name, "GetMuteColor") == 0) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", d->mute_rgb));
        return;
    }

    if (g_strcmp0(method_name, "SetMuteColor") == 0) {
        guint v;
        g_variant_get(parameters, "(u)", &v);
        v &= 0xffffff;
        unsigned char cfg[16];
        r = wave3_cfg_read(d, cfg);
        if (r != 16) goto usb_err;
        cfg[CFG_MUTE_R] = (v >> 16) & 0xff;
        cfg[CFG_MUTE_G] = (v >> 8) & 0xff;
        cfg[CFG_MUTE_B] = v & 0xff;
        r = wave3_cfg_write(d, cfg);
        if (r != 16) goto usb_err;
        d->mute_rgb = v;
        g_message("SetMuteColor: 0x%06x", v);
        wave3_dbus_emit_state(d);
        g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", TRUE));
        return;
    }

    if (g_strcmp0(method_name, "GetHeadphoneColor") == 0) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", 0));
        return;
    }

    if (g_strcmp0(method_name, "SetHeadphoneColor") == 0) {
        g_dbus_method_invocation_return_error(inv, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                              "First-gen Wave:3 has no headphone color LED");
        return;
    }

    if (g_strcmp0(method_name, "GetBrightness") == 0) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", d->brightness));
        return;
    }

    if (g_strcmp0(method_name, "SetBrightness") == 0) {
        guint v;
        g_variant_get(parameters, "(u)", &v);
        if (v > 255) v = 255;
        unsigned char cfg[16];
        r = wave3_cfg_read(d, cfg);
        if (r != 16) goto usb_err;
        cfg[CFG_BRIGHTNESS] = (unsigned char)v;
        r = wave3_cfg_write(d, cfg);
        if (r != 16) goto usb_err;
        d->brightness = v;
        g_message("SetBrightness: %u", v);
        wave3_dbus_emit_state(d);
        g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", TRUE));
        return;
    }

    if (g_strcmp0(method_name, "GetInputLevel") == 0) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(d)", d->input_level_db));
        return;
    }

    if (g_strcmp0(method_name, "GetPlaybackLevel") == 0) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(d)", d->playback_level_db));
        return;
    }

    if (g_strcmp0(method_name, "GetDialMode") == 0) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", d->dial_mode));
        return;
    }

    if (g_strcmp0(method_name, "GetDialValue") == 0) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", (guint)d->dial_value));
        return;
    }

    if (g_strcmp0(method_name, "GetIndicatorColor") == 0) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", d->indicator_rgb));
        return;
    }

    g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
                                          G_DBUS_ERROR_UNKNOWN_METHOD,
                                          "Unknown method %s", method_name);
    return;

usb_err:
    if (d->dev && wave3_is_io_error(r)) {
        g_warning("USB I/O error during %s (%s); closing device", method_name, libusb_error_name(r));
        wave3_close_device(d);
        g_dbus_method_invocation_return_error(inv, G_IO_ERROR,
                                              G_IO_ERROR_NOT_CONNECTED,
                                              "Wave:3 disconnected during operation");
    } else {
        g_warning("USB error during %s: %s", method_name, libusb_error_name(r));
        g_dbus_method_invocation_return_error(inv, G_IO_ERROR,
                                              G_IO_ERROR_FAILED,
                                              "USB error during %s: %s", method_name, libusb_error_name(r));
    }
}

static GVariant *handle_get_property(G_GNUC_UNUSED GDBusConnection *conn,
                                     G_GNUC_UNUSED const gchar *sender,
                                     G_GNUC_UNUSED const gchar *object_path,
                                     G_GNUC_UNUSED const gchar *interface_name,
                                     const gchar *property_name,
                                     GError **error,
                                     gpointer user_data)
{
    Wave3Daemon *d = user_data;

    if (!wave3_ensure_open(d)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                    "Wave:3 is not connected");
        return NULL;
    }

    if (g_strcmp0(property_name, "MicMute") == 0)
        return g_variant_new_boolean(d->mic_mute);
    if (g_strcmp0(property_name, "HpMute") == 0)
        return g_variant_new_boolean(d->hp_mute);
    if (g_strcmp0(property_name, "HpVolumePercent") == 0)
        return g_variant_new_uint32(pct_from_raw(d->hp_volume, d->hp_vol_min, d->hp_vol_max));
    if (g_strcmp0(property_name, "MicGainPercent") == 0)
        return g_variant_new_uint32(pct_from_raw(d->mic_gain, d->mic_gain_min, d->mic_gain_max));
    if (g_strcmp0(property_name, "Clipguard") == 0)
        return g_variant_new_boolean(d->clipguard);
    if (g_strcmp0(property_name, "LowCut") == 0)
        return g_variant_new_boolean(d->lowcut);
    if (g_strcmp0(property_name, "DirectMonitor") == 0)
        return g_variant_new_double(d->direct_monitor);
    if (g_strcmp0(property_name, "MuteColor") == 0)
        return g_variant_new_uint32(d->mute_rgb);
    if (g_strcmp0(property_name, "Brightness") == 0)
        return g_variant_new_uint32(d->brightness);
    if (g_strcmp0(property_name, "HeadphoneColor") == 0)
        return g_variant_new_uint32(0);
    if (g_strcmp0(property_name, "DialMode") == 0)
        return g_variant_new_uint32(d->dial_mode);
    if (g_strcmp0(property_name, "DialValue") == 0)
        return g_variant_new_uint32((guint)d->dial_value);
    if (g_strcmp0(property_name, "IndicatorColor") == 0)
        return g_variant_new_uint32(d->indicator_rgb);

    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
                "Unknown property %s", property_name);
    return NULL;
}

static const GDBusInterfaceVTable interface_vtable = {
    handle_method_call,
    handle_get_property,
    NULL,
    { 0 }
};

static const gchar *introspection_xml =
    "<node>"
    "  <interface name='org.wave3.Daemon'>"
    "    <method name='GetState'>"
    "      <arg type='(bbuuuubbdududduu)' name='state' direction='out'/>"
    "    </method>"
    "    <method name='SetMicMute'>"
    "      <arg type='b' name='muted' direction='in'/>"
    "      <arg type='b' name='ok' direction='out'/>"
    "    </method>"
    "    <method name='ToggleMicMute'>"
    "      <arg type='b' name='muted' direction='out'/>"
    "    </method>"
    "    <method name='SetHpMute'>"
    "      <arg type='b' name='muted' direction='in'/>"
    "      <arg type='b' name='ok' direction='out'/>"
    "    </method>"
    "    <method name='SetHpVolume'>"
    "      <arg type='u' name='percent' direction='in'/>"
    "      <arg type='b' name='ok' direction='out'/>"
    "    </method>"
    "    <method name='GetClipguard'>"
    "      <arg type='b' name='enabled' direction='out'/>"
    "    </method>"
    "    <method name='SetClipguard'>"
    "      <arg type='b' name='enabled' direction='in'/>"
    "      <arg type='b' name='ok' direction='out'/>"
    "    </method>"
    "    <method name='GetLowCut'>"
    "      <arg type='b' name='enabled' direction='out'/>"
    "    </method>"
    "    <method name='SetLowCut'>"
    "      <arg type='b' name='enabled' direction='in'/>"
    "      <arg type='b' name='ok' direction='out'/>"
    "    </method>"
    "    <method name='GetDirectMonitor'>"
    "      <arg type='d' name='mix' direction='out'/>"
    "    </method>"
    "    <method name='SetDirectMonitor'>"
    "      <arg type='d' name='mix' direction='in'/>"
    "      <arg type='b' name='ok' direction='out'/>"
    "    </method>"
    "    <method name='GetMuteColor'>"
    "      <arg type='u' name='rgb' direction='out'/>"
    "    </method>"
    "    <method name='SetMuteColor'>"
    "      <arg type='u' name='rgb' direction='in'/>"
    "      <arg type='b' name='ok' direction='out'/>"
    "    </method>"
    "    <method name='GetHeadphoneColor'>"
    "      <arg type='u' name='rgb' direction='out'/>"
    "    </method>"
    "    <method name='SetHeadphoneColor'>"
    "      <arg type='u' name='rgb' direction='in'/>"
    "      <arg type='b' name='ok' direction='out'/>"
    "    </method>"
    "    <method name='GetBrightness'>"
    "      <arg type='u' name='value' direction='out'/>"
    "    </method>"
    "    <method name='SetBrightness'>"
    "      <arg type='u' name='value' direction='in'/>"
    "      <arg type='b' name='ok' direction='out'/>"
    "    </method>"
    "    <method name='GetInputLevel'>"
    "      <arg type='d' name='db' direction='out'/>"
    "    </method>"
    "    <method name='GetPlaybackLevel'>"
    "      <arg type='d' name='db' direction='out'/>"
    "    </method>"
    "    <method name='GetDialMode'>"
    "      <arg type='u' name='mode' direction='out'/>"
    "    </method>"
    "    <method name='GetDialValue'>"
    "      <arg type='u' name='value' direction='out'/>"
    "    </method>"
    "    <method name='GetIndicatorColor'>"
    "      <arg type='u' name='rgb' direction='out'/>"
    "    </method>"
    "    <signal name='StateChanged'>"
    "      <arg type='(bbuuuubbdududduu)' name='state'/>"
    "    </signal>"
    "    <property name='MicMute' type='b' access='read'/>"
    "    <property name='HpMute' type='b' access='read'/>"
    "    <property name='HpVolumePercent' type='u' access='read'/>"
    "    <property name='MicGainPercent' type='u' access='read'/>"
    "    <property name='Clipguard' type='b' access='read'/>"
    "    <property name='LowCut' type='b' access='read'/>"
    "    <property name='DirectMonitor' type='d' access='read'/>"
    "    <property name='MuteColor' type='u' access='read'/>"
    "    <property name='Brightness' type='u' access='read'/>"
    "    <property name='HeadphoneColor' type='u' access='read'/>"
    "    <property name='DialMode' type='u' access='read'/>"
    "    <property name='DialValue' type='u' access='read'/>"
    "    <property name='IndicatorColor' type='u' access='read'/>"
    "  </interface>"
    "</node>";

static void on_bus_acquired(GDBusConnection *conn, G_GNUC_UNUSED const gchar *name, gpointer user_data)
{
    Wave3Daemon *d = user_data;
    GError *error = NULL;
    static GDBusNodeInfo *introspection = NULL;

    if (!introspection)
        introspection = g_dbus_node_info_new_for_xml(introspection_xml, NULL);

    d->bus = conn;
    g_dbus_connection_register_object(conn,
                                      "/org/wave3/Daemon",
                                      introspection->interfaces[0],
                                      &interface_vtable,
                                      d,
                                      NULL,
                                      &error);
    if (error) {
        g_printerr("register object failed: %s\n", error->message);
        g_error_free(error);
    }
}

static gboolean quit_loop(gpointer data)
{
    g_main_loop_quit(data);
    return G_SOURCE_REMOVE;
}

static gboolean poll_hardware(gpointer user_data)
{
    Wave3Daemon *d = user_data;
    if (wave3_refresh(d))
        wave3_dbus_emit_state(d);
    return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    Wave3Daemon *d = &g_daemon;
    int r;

    memset(d, 0, sizeof(*d));

    r = libusb_init(&d->ctx);
    if (r < 0) {
        g_printerr("libusb_init: %s\n", libusb_error_name(r));
        return 1;
    }

    /* Try to open the device; if it's not present yet, the poll loop will retry. */
    (void)wave3_open_device(d);

    g_print("wave3-daemon: D-Bus name org.wave3.Daemon ready\n");
    if (d->connected) {
        g_print("  HP volume range: %.1f … %.1f dB\n",
                d->hp_vol_min / 256.0, d->hp_vol_max / 256.0);
        g_print("  Mic gain range:  %.1f … %.1f dB (read-only dial)\n",
                d->mic_gain_min / 256.0, d->mic_gain_max / 256.0);
    } else {
        g_print("  Wave:3 not present at startup; will reconnect automatically\n");
    }

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);

    g_unix_signal_add(SIGINT, quit_loop, loop);
    g_unix_signal_add(SIGTERM, quit_loop, loop);

    d->owner_id = g_bus_own_name(G_BUS_TYPE_SESSION,
                                 "org.wave3.Daemon",
                                 G_BUS_NAME_OWNER_FLAGS_NONE,
                                 on_bus_acquired,
                                 NULL, NULL,
                                 d, NULL);

    g_timeout_add(POLL_MS, poll_hardware, d);

    g_main_loop_run(loop);

    g_bus_unown_name(d->owner_id);
    g_main_loop_unref(loop);

    wave3_close_device(d);
    libusb_exit(d->ctx);
    return 0;
}
