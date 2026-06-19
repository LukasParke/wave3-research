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
#define CFG_MIC_MUTE        4
#define CFG_HP_VOLUME       8   /* signed 8-bit dB attenuation */
#define CFG_HP_MUTE         9
#define CFG_HP_CONNECTED    12  /* read-only? */
#define CFG_VOLUME_SELECT   14  /* 0/1/2 dial mode */
#define CFG_UNKNOWN_15      15

typedef struct {
    libusb_context *ctx;
    libusb_device_handle *dev;

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
    guint32  hp_rgb;
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

/* ── UAC helpers ───────────────────────────────────────────────────────── */

static int wave3_uac_get(Wave3Daemon *d, int entity, int selector,
                         int channel, unsigned char *out, int len)
{
    uint16_t wValue  = (uint16_t)((selector << 8) | channel);
    uint16_t wIndex3 = (uint16_t)((entity << 8) | WAVE3_IFACE);
    return libusb_control_transfer(d->dev, UAC_BM_IN, UAC_GET_CUR,
                                   wValue, wIndex3, out, len, 1000);
}

static int wave3_uac_set(Wave3Daemon *d, int entity, int selector,
                         int channel, const unsigned char *in, int len)
{
    uint16_t wValue  = (uint16_t)((selector << 8) | channel);
    uint16_t wIndex3 = (uint16_t)((entity << 8) | WAVE3_IFACE);
    return libusb_control_transfer(d->dev, UAC_BM_OUT, UAC_SET_CUR,
                                   wValue, wIndex3, (unsigned char *)in, len, 1000);
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
    return g_variant_new("(bbuuuubbddudd)",
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
                         d->hp_rgb,
                         d->input_level_db,
                         d->playback_level_db);
}

static GVariant *wave3_build_state_outer(Wave3Daemon *d)
{
    return g_variant_new("((bbuuuubbddudd))",
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
                         d->hp_rgb,
                         d->input_level_db,
                         d->playback_level_db);
}

static gboolean wave3_refresh(Wave3Daemon *d)
{
    unsigned char buf[8];
    unsigned char cfg[16];
    gboolean changed = FALSE;
    int r;

    /* mic gain is still read via standard UAC (physical dial) */
    r = wave3_uac_get(d, MIC_FU, UAC_VOLUME, 0, buf, 2);
    if (r == 2) {
        gint16 v = (gint16)((buf[1] << 8) | buf[0]);
        if (v != d->mic_gain) { d->mic_gain = v; changed = TRUE; }
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
        }
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
    return libusb_control_transfer(d->dev, 0xA1, 0x85, WAVE3_CFG_WVALUE,
                                   (0x33 << 8) | WAVE3_IFACE,
                                   cfg, 16, 1000);
}

static int wave3_cfg_write(Wave3Daemon *d, const unsigned char *cfg)
{
    return libusb_control_transfer(d->dev, 0x21, 0x05, WAVE3_CFG_WVALUE,
                                   (0x33 << 8) | WAVE3_IFACE,
                                   (unsigned char *)cfg, 16, 1000);
}

static int wave3_meter_read(Wave3Daemon *d, unsigned char *meter)
{
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

    if (g_strcmp0(method_name, "GetState") == 0) {
        g_dbus_method_invocation_return_value(inv, wave3_build_state_outer(d));
        return;
    }

    if (g_strcmp0(method_name, "SetMicMute") == 0) {
        gboolean muted;
        g_variant_get(parameters, "(b)", &muted);
        unsigned char cfg[16];
        int r = wave3_cfg_read(d, cfg);
        if (r != 16) goto usb_err;
        cfg[CFG_MIC_MUTE] = muted ? 1 : 0;
        r = wave3_cfg_write(d, cfg);
        if (r != 16) goto usb_err;
        d->mic_mute = muted;
        wave3_dbus_emit_state(d);
        g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", TRUE));
        return;
    }

    if (g_strcmp0(method_name, "ToggleMicMute") == 0) {
        unsigned char cfg[16];
        int r = wave3_cfg_read(d, cfg);
        if (r != 16) goto usb_err;
        cfg[CFG_MIC_MUTE] = d->mic_mute ? 0 : 1;
        r = wave3_cfg_write(d, cfg);
        if (r != 16) goto usb_err;
        d->mic_mute = !d->mic_mute;
        wave3_dbus_emit_state(d);
        g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", d->mic_mute));
        return;
    }

    if (g_strcmp0(method_name, "SetHpMute") == 0) {
        gboolean muted;
        g_variant_get(parameters, "(b)", &muted);
        unsigned char cfg[16];
        int r = wave3_cfg_read(d, cfg);
        if (r != 16) goto usb_err;
        cfg[CFG_HP_MUTE] = muted ? 1 : 0;
        r = wave3_cfg_write(d, cfg);
        if (r != 16) goto usb_err;
        d->hp_mute = muted;
        wave3_dbus_emit_state(d);
        g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", TRUE));
        return;
    }

    if (g_strcmp0(method_name, "SetHpVolume") == 0) {
        guint pct;
        g_variant_get(parameters, "(u)", &pct);
        gint16 raw = pct_to_raw((int)pct, d->hp_vol_min, d->hp_vol_max);
        unsigned char cfg[16];
        int r = wave3_cfg_read(d, cfg);
        if (r != 16) goto usb_err;
        cfg[CFG_HP_VOLUME] = (unsigned char)(raw / 256);   /* signed dB */
        r = wave3_cfg_write(d, cfg);
        if (r != 16) goto usb_err;
        d->hp_volume = raw;
        wave3_dbus_emit_state(d);
        g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", TRUE));
        return;
    }

    if (g_strcmp0(method_name, "GetClipguard") == 0) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", d->clipguard));
        return;
    }

    if (g_strcmp0(method_name, "SetClipguard") == 0) {
        g_dbus_method_invocation_return_error(inv, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                              "Clipguard offset not yet identified");
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
        g_dbus_method_invocation_return_error(inv, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                              "Direct monitor offset not yet identified");
        return;
    }

    if (g_strcmp0(method_name, "GetMuteColor") == 0) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", d->mute_rgb));
        return;
    }

    if (g_strcmp0(method_name, "SetMuteColor") == 0) {
        g_dbus_method_invocation_return_error(inv, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                              "RGB LED protocol not yet decoded");
        return;
    }

    if (g_strcmp0(method_name, "GetHeadphoneColor") == 0) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", d->hp_rgb));
        return;
    }

    if (g_strcmp0(method_name, "SetHeadphoneColor") == 0) {
        g_dbus_method_invocation_return_error(inv, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                              "RGB LED protocol not yet decoded");
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

    g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
                                          G_DBUS_ERROR_UNKNOWN_METHOD,
                                          "Unknown method %s", method_name);
    return;

usb_err:
    g_dbus_method_invocation_return_error(inv, G_IO_ERROR,
                                          G_IO_ERROR_FAILED,
                                          "USB error");
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
    if (g_strcmp0(property_name, "HeadphoneColor") == 0)
        return g_variant_new_uint32(d->hp_rgb);

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
    "      <arg type='(bbuuuubbddudd)' name='state' direction='out'/>"
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
    "    <method name='GetInputLevel'>"
    "      <arg type='d' name='db' direction='out'/>"
    "    </method>"
    "    <method name='GetPlaybackLevel'>"
    "      <arg type='d' name='db' direction='out'/>"
    "    </method>"
    "    <signal name='StateChanged'>"
    "      <arg type='(bbuuuubbddudd)' name='state'/>"
    "    </signal>"
    "    <property name='MicMute' type='b' access='read'/>"
    "    <property name='HpMute' type='b' access='read'/>"
    "    <property name='HpVolumePercent' type='u' access='read'/>"
    "    <property name='MicGainPercent' type='u' access='read'/>"
    "    <property name='Clipguard' type='b' access='read'/>"
    "    <property name='LowCut' type='b' access='read'/>"
    "    <property name='DirectMonitor' type='d' access='read'/>"
    "    <property name='MuteColor' type='u' access='read'/>"
    "    <property name='HeadphoneColor' type='u' access='read'/>"
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

    d->dev = libusb_open_device_with_vid_pid(d->ctx, WAVE3_VID, WAVE3_PID);
    if (!d->dev) {
        g_printerr("Wave:3 (%04x:%04x) not found\n", WAVE3_VID, WAVE3_PID);
        libusb_exit(d->ctx);
        return 1;
    }

    r = libusb_claim_interface(d->dev, WAVE3_IFACE);
    if (r < 0) {
        g_printerr("claim interface %d: %s\n", WAVE3_IFACE,
                   libusb_error_name(r));
        libusb_close(d->dev);
        libusb_exit(d->ctx);
        return 1;
    }

    if (wave3_get_range(d, HP_FU, &d->hp_vol_min, &d->hp_vol_max, &(gint16){0}) < 0) {
        d->hp_vol_min = -73 * 256;
        d->hp_vol_max = 0;
    }
    if (wave3_get_range(d, MIC_FU, &d->mic_gain_min, &d->mic_gain_max, &(gint16){0}) < 0) {
        d->mic_gain_min = 0;
        d->mic_gain_max = 24 * 256;
    }

    wave3_refresh(d);

    g_print("wave3-daemon: Wave:3 ready on D-Bus name org.wave3.Daemon\n");
    g_print("  HP volume range: %.1f … %.1f dB\n",
            d->hp_vol_min / 256.0, d->hp_vol_max / 256.0);
    g_print("  Mic gain range:  %.1f … %.1f dB (read-only dial)\n",
            d->mic_gain_min / 256.0, d->mic_gain_max / 256.0);

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

    libusb_release_interface(d->dev, WAVE3_IFACE);
    libusb_close(d->dev);
    libusb_exit(d->ctx);
    return 0;
}
