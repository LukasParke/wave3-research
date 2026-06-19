#!/usr/bin/env bash
# probe-wave3-config-pass2.sh — focused second pass
# Maps monitor mix scale and tests RGB/brightness more carefully.
# Stops the daemon, uses cfg_probe, restarts daemon at the end.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC_DIR="$REPO_ROOT/src"
PROBE="$SRC_DIR/cfg_probe"
LOG="$PWD/probe-wave3-config-pass2.log"

log() {
    echo "$@" | tee -a "$LOG"
}

ask() {
    local prompt="$1"
    local ans
    echo -e "\n${prompt}" | tee -a "$LOG"
    echo -n "> " | tee -a "$LOG"
    read -r ans
    echo "ANSWER: $ans" >> "$LOG"
    echo "$ans"
}

run_probe() {
    local cmd="$1"
    shift
    log "RUN: $PROBE $cmd $*"
    "$PROBE" "$cmd" "$@" 2>&1 | tee -a "$LOG"
}

# ─── setup ────────────────────────────────────────────────────────────────
log "=== Wave:3 Config Byte Mapper — Pass 2 ==="
log "Stopping wave3-daemon..."
systemctl --user stop wave3-daemon || true

cd "$SRC_DIR"
if ! gcc -o cfg_probe cfg_probe.c $(pkg-config --cflags --libs libusb-1.0) -O2; then
    log "ERROR: failed to build cfg_probe"
    exit 1
fi
log "OK."

# ─── clean baseline ───────────────────────────────────────────────────────
log ""
log "--- Clean baseline (mic unmuted, dial on monitor mix if possible) ---"
run_probe read
BASELINE=$("$PROBE" read 2>/dev/null | grep '^config:' | sed 's/config: //')
log "Baseline: $BASELINE"

ask "What color is the ring LED right now?"
ask "Is the mic currently muted? (yes/no)"
ask "What does the dial currently control? (mic gain / headphone volume / monitor mix / nothing)"

# ─── monitor mix sweep (offset 14) ─────────────────────────────────────────
log ""
log "========================================"
log "Monitor mix sweep — offset 14"
log "DO NOT touch the dial during this section."
log "Observe the monitor-mix LED indicator and what you hear in headphones."
log "========================================"

for val in 0x00 0x20 0x40 0x60 0x80 0xA0 0xC0 0xE0 0xFF; do
    log ""
    log "--- Setting offset 14 to $val ---"
    run_probe write 14 "$val"
    ask "Offset 14 = $val. Where is the monitor mix indicator? (far left / center / far right / gone)"
    ask "What do you hear in headphones? (only mic / only PC / blend / nothing)"
    run_probe restore
    ask "After restore, did it return to baseline? (yes/no)"
done

# ─── monitor mix indicator enable (offset 11) ──────────────────────────────
log ""
log "========================================"
log "Monitor mix indicator — offset 11"
log "========================================"

for val in 0x00 0x01 0x80 0xFF; do
    log ""
    log "--- Setting offset 11 to $val ---"
    run_probe write 11 "$val"
    ask "Offset 11 = $val. Is the monitor mix level indicator visible? (yes/no / changed)"
    ask "Any other LED change? (none / describe)"
    run_probe restore
    ask "After restore, did it return to baseline? (yes/no)"
done

# ─── RGB index test (if raw RGB doesn't work) ─────────────────────────────
log ""
log "========================================"
log "RGB / mute color index test"
log "The first-gen Wave:3 may use a color index instead of raw RGB."
log "Mute the mic now (tap the top) and keep it muted for this section."
log "========================================"
ask "Is the mic muted now? (yes/no)"

for off in 1 2 3 5 6 7 10 13; do
    for val in 0x01 0x02 0x03 0xFF; do
        log ""
        log "--- Setting offset $off to $val (mic muted) ---"
        run_probe write "$off" "$val"
        ask "Offset $off = $val with mic muted. What color is the ring LED? (red / white / other / no change)"
        run_probe restore
    done
done

# ─── brightness test (offset 15) ──────────────────────────────────────────
log ""
log "========================================"
log "Brightness — offset 15"
log "========================================"
log "Setting offset 15 to 0x00 then 0xFF. Compare LED brightness directly."

run_probe write 15 0x00
ask "Offset 15 = 0x00. Is the LED visibly dimmer than baseline? (yes/no / unsure)"
run_probe restore
run_probe write 15 0xFF
ask "Offset 15 = 0xFF. Is the LED visibly brighter than baseline? (yes/no / unsure)"
run_probe restore

# ─── clipguard candidate bits ─────────────────────────────────────────────
log ""
log "========================================"
log "Clipguard bit tests"
log "Clipguard might be a bit in byte 5, 6, or 7 rather than the whole byte."
log "========================================"

for off in 5 6 7; do
    for val in 0x01 0x02 0x04 0x08 0x10 0x20 0x40 0x80; do
        log ""
        log "--- Setting offset $off to $val ---"
        run_probe write "$off" "$val"
        ask "Offset $off = $val. Make a loud noise near the mic. Any clipguard indicator or LED behavior change? (yes/no / unsure)"
        run_probe restore
    done
done

# ─── cleanup ──────────────────────────────────────────────────────────────
log ""
log "========================================"
log "Probe session complete. Log saved to: $LOG"
log "Restarting wave3-daemon..."
systemctl --user start wave3-daemon
log "Done. Please upload $LOG."
