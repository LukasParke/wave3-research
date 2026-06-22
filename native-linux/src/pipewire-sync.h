/*
 * pipewire-sync.h — bidirectional PipeWire <-> Wave:3 hardware sync helpers
 */

#ifndef PIPEWIRE_SYNC_H
#define PIPEWIRE_SYNC_H

#include <glib.h>

typedef struct _PipeWireSync PipeWireSync;

/* Create/free sync state. enabled=FALSE makes all calls no-ops. */
PipeWireSync *pipewire_sync_new(gboolean enabled);
void          pipewire_sync_free(PipeWireSync *s);
gboolean      pipewire_sync_enabled(const PipeWireSync *s);

/* Push hardware state to PipeWire nodes.
 * source maps to wave3-source (mic gain / mute).
 * sink maps to wave3-sink (headphone volume / mute).
 */
void pipewire_sync_push_source(PipeWireSync *s, gint volume_pct, gboolean mute);
void pipewire_sync_push_sink(PipeWireSync *s, gint volume_pct, gboolean mute);

/* Poll PipeWire for user-made changes.
 * Call at the same rate as hardware polling (e.g. 100 ms); the helper
 * internally throttles the actual pactl query to ~1 Hz.
 * Returns TRUE if any value changed. Output arguments are only updated when
 * a change is detected; otherwise they keep the current targets.
 */
gboolean pipewire_sync_poll(PipeWireSync *s,
                            gint *out_source_volume_pct,
                            gboolean *out_source_mute,
                            gint *out_sink_volume_pct,
                            gboolean *out_sink_mute);

#endif /* PIPEWIRE_SYNC_H */
