/*
 * pipewire-sync.c — bidirectional PipeWire <-> Wave:3 hardware sync helpers
 *
 * This file is only used when the daemon is started with --sync-pipewire.
 * It uses pactl(1) to read/write PipeWire node volume and mute because
 * PipeWire's PulseAudio compatibility layer accepts stable node names.
 */

#include "pipewire-sync.h"

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POLL_MS          100      /* must match daemon poll interval */
#define SYNC_SETTLE_US   800000   /* 0.8 s settle window after we set PW */
#define SYNC_INTERVAL_MS 1000     /* poll PW for user changes every 1 s */
#define VOLUME_THRESHOLD 2        /* ignore PW volume drift within ±2% */

struct _PipeWireSync {
    gboolean enabled;

    /* targets: the values we believe PipeWire should currently reflect */
    gint     source_volume_pct;
    gboolean source_mute;
    gint     sink_volume_pct;
    gboolean sink_mute;

    /* last time we pushed a value to PipeWire (monotonic us) */
    gint64 last_pw_push_us;

    /* cycle counter: only poll PipeWire every SYNC_INTERVAL_MS / POLL_MS ticks */
    guint poll_counter;
};

static gint64 monotonic_us(void)
{
    return g_get_monotonic_time();
}

static gboolean spawn_pactl(const gchar *args, gchar **stdout_str, gint *exit_status)
{
    GError *err = NULL;
    gchar *cmd = g_strdup_printf("pactl %s", args);
    gchar *out = NULL;
    gchar *errstr = NULL;
    gint status = 0;
    gboolean ok;

    ok = g_spawn_command_line_sync(cmd, &out, &errstr, &status, &err);
    g_free(cmd);
    g_free(errstr);

    if (!ok) {
        g_free(out);
        if (err) {
            g_warning("Failed to spawn pactl: %s", err->message);
            g_error_free(err);
        }
        return FALSE;
    }

    if (exit_status) *exit_status = status;
    if (stdout_str) {
        *stdout_str = out;
    } else {
        g_free(out);
    }
    return TRUE;
}

/* Parse a pactl "list" blob and find the named object's volume percent and mute.
 * Volume lines look like:
 *   Volume: mono: 65536 / 100% / 0.00 dB          (source)
 *   Volume: front-left: 26224 /  40% / -23.87 dB, front-right: ... (sink)
 * Mute lines look like:
 *   Mute: no
 */
static gboolean parse_pactl_list(const gchar *text, const gchar *name,
                                 gint *volume_pct, gboolean *mute)
{
    if (!text || !name) return FALSE;

    const gchar *section = strstr(text, name);
    if (!section) return FALSE;

    /* Limit search to the next blank line (end of this object's section) */
    const gchar *section_end = strstr(section, "\n\n");
    if (!section_end) section_end = section + strlen(section);

    gboolean found_vol = FALSE;
    gboolean found_mute = FALSE;

    for (const gchar *p = section; p < section_end; ) {
        const gchar *eol = strchr(p, '\n');
        if (!eol) eol = section_end;
        gsize len = (gsize)(eol - p);
        gchar *line = g_strndup(p, len);

        if (g_str_has_prefix(line, "\tVolume:") || g_str_has_prefix(line, "        Volume:")) {
            /* grab the first percent we see, e.g. "100%" or " 40%" */
            const gchar *perc = strchr(line, '/');
            if (perc) {
                int pct = atoi(perc + 1);
                if (pct >= 0 && pct <= 200) {
                    *volume_pct = CLAMP(pct, 0, 100);
                    found_vol = TRUE;
                }
            }
        } else if (g_str_has_prefix(line, "\tMute:") || g_str_has_prefix(line, "        Mute:")) {
            if (strstr(line, "yes"))
                *mute = TRUE;
            else
                *mute = FALSE;
            found_mute = TRUE;
        }

        g_free(line);
        p = eol + 1;
        if (found_vol && found_mute) break;
    }

    return found_vol && found_mute;
}

PipeWireSync *pipewire_sync_new(gboolean enabled)
{
    PipeWireSync *s = g_new0(PipeWireSync, 1);
    s->enabled = enabled;
    s->source_volume_pct = -1;
    s->sink_volume_pct = -1;
    return s;
}

void pipewire_sync_free(PipeWireSync *s)
{
    g_free(s);
}

gboolean pipewire_sync_enabled(const PipeWireSync *s)
{
    return s && s->enabled;
}

/* Push hardware state to PipeWire. Returns TRUE if any command was issued. */
void pipewire_sync_push_source(PipeWireSync *s, gint volume_pct, gboolean mute)
{
    g_return_if_fail(s != NULL);
    if (!s->enabled) return;

    s->source_volume_pct = volume_pct;
    s->source_mute = mute;

    gchar *cmd_vol = g_strdup_printf("set-source-volume wave3-source %d%%", volume_pct);
    gchar *cmd_mute = g_strdup_printf("set-source-mute wave3-source %d", mute ? 1 : 0);

    spawn_pactl(cmd_vol, NULL, NULL);
    spawn_pactl(cmd_mute, NULL, NULL);

    g_free(cmd_vol);
    g_free(cmd_mute);

    s->last_pw_push_us = monotonic_us();
    g_message("PipeWire sync: source -> %d%%, mute=%s", volume_pct, mute ? "yes" : "no");
}

void pipewire_sync_push_sink(PipeWireSync *s, gint volume_pct, gboolean mute)
{
    g_return_if_fail(s != NULL);
    if (!s->enabled) return;

    s->sink_volume_pct = volume_pct;
    s->sink_mute = mute;

    gchar *cmd_vol = g_strdup_printf("set-sink-volume wave3-sink %d%%", volume_pct);
    gchar *cmd_mute = g_strdup_printf("set-sink-mute wave3-sink %d", mute ? 1 : 0);

    spawn_pactl(cmd_vol, NULL, NULL);
    spawn_pactl(cmd_mute, NULL, NULL);

    g_free(cmd_vol);
    g_free(cmd_mute);

    s->last_pw_push_us = monotonic_us();
    g_message("PipeWire sync: sink -> %d%%, mute=%s", volume_pct, mute ? "yes" : "no");
}

/* Pull user-made changes from PipeWire. Returns TRUE if any value changed. */
gboolean pipewire_sync_poll(PipeWireSync *s,
                            gint *out_source_volume_pct, gboolean *out_source_mute,
                            gint *out_sink_volume_pct, gboolean *out_sink_mute)
{
    g_return_val_if_fail(s != NULL, FALSE);
    g_return_val_if_fail(out_source_volume_pct != NULL, FALSE);
    g_return_val_if_fail(out_source_mute != NULL, FALSE);
    g_return_val_if_fail(out_sink_volume_pct != NULL, FALSE);
    g_return_val_if_fail(out_sink_mute != NULL, FALSE);

    *out_source_volume_pct = s->source_volume_pct;
    *out_source_mute = s->source_mute;
    *out_sink_volume_pct = s->sink_volume_pct;
    *out_sink_mute = s->sink_mute;

    if (!s->enabled) return FALSE;

    /* Don't poll right after we just pushed to PipeWire. */
    if (s->last_pw_push_us > 0 &&
        (monotonic_us() - s->last_pw_push_us) < SYNC_SETTLE_US) {
        return FALSE;
    }

    s->poll_counter += POLL_MS;
    if (s->poll_counter < SYNC_INTERVAL_MS) return FALSE;
    s->poll_counter = 0;

    gchar *sources_text = NULL;
    gchar *sinks_text = NULL;
    gboolean changed = FALSE;

    if (!spawn_pactl("list sources", &sources_text, NULL)) goto out;
    if (!spawn_pactl("list sinks", &sinks_text, NULL)) goto out;

    gint pw_src_vol = -1;
    gboolean pw_src_mute = FALSE;
    if (parse_pactl_list(sources_text, "Name: wave3-source", &pw_src_vol, &pw_src_mute)) {
        if (abs(pw_src_vol - s->source_volume_pct) > VOLUME_THRESHOLD ||
            pw_src_mute != s->source_mute) {
            s->source_volume_pct = pw_src_vol;
            s->source_mute = pw_src_mute;
            *out_source_volume_pct = pw_src_vol;
            *out_source_mute = pw_src_mute;
            changed = TRUE;
            g_message("PipeWire sync: source changed by user -> %d%%, mute=%s",
                      pw_src_vol, pw_src_mute ? "yes" : "no");
        }
    }

    gint pw_sink_vol = -1;
    gboolean pw_sink_mute = FALSE;
    if (parse_pactl_list(sinks_text, "Name: wave3-sink", &pw_sink_vol, &pw_sink_mute)) {
        if (abs(pw_sink_vol - s->sink_volume_pct) > VOLUME_THRESHOLD ||
            pw_sink_mute != s->sink_mute) {
            s->sink_volume_pct = pw_sink_vol;
            s->sink_mute = pw_sink_mute;
            *out_sink_volume_pct = pw_sink_vol;
            *out_sink_mute = pw_sink_mute;
            changed = TRUE;
            g_message("PipeWire sync: sink changed by user -> %d%%, mute=%s",
                      pw_sink_vol, pw_sink_mute ? "yes" : "no");
        }
    }

out:
    g_free(sources_text);
    g_free(sinks_text);
    return changed;
}
