#!/usr/bin/env bash
# probe-wave3-config.sh — interactive Wave:3 config-byte mapper
# Builds cfg_probe, then walks you through safe byte tests.
# All commands and your answers are logged to probe-wave3-config.log

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC_DIR="$REPO_ROOT/src"
PROBE="$SRC_DIR/cfg_probe"
LOG="$PWD/probe-wave3-config.log"

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

# ─── build probe tool ─────────────────────────────────────────────────────
log "=== Wave:3 Config Byte Mapper ==="
log "Building cfg_probe..."
cd "$SRC_DIR"
if ! gcc -o cfg_probe cfg_probe.c $(pkg-config --cflags --libs libusb-1.0) -O2; then
    log "ERROR: failed to build cfg_probe"
    exit 1
fi
log "OK."

# ─── baseline ─────────────────────────────────────────────────────────────
log ""
log "--- Baseline config ---"
run_probe read
BASELINE=$("$PROBE" read 2>/dev/null | grep '^config:' | sed 's/config: //')
log "Baseline: $BASELINE"

ask "What color is the ring LED right now? (e.g. blue, red, white, off)"
ask "Is the mic currently muted? (yes/no)"
ask "What does the dial control when you turn it? (mic gain / headphone volume / monitor mix / nothing)"
ask "Do you have Wave Link available to confirm current settings? (yes/no)"

# ─── byte tests ───────────────────────────────────────────────────────────
bytes=(0 1 2 3 5 6 7 10 11 13 14 15)
watch_for=(
    "Any LED change, any audio change, or device mode change"
    "Any LED change or feature enable/disable behavior"
    "Any LED change or dial behavior change"
    "Any LED change or dial behavior change"
    "Clipguard behavior — tap mute hard or make a loud noise; does the LED/clip indicator behave differently?"
    "Direct monitor mix — listen to headphones while speaking and playing PC audio"
    "Direct monitor mix or RGB/LED change"
    "Mute-ring RGB red channel — mute the mic and look at the ring color"
    "Mute-ring RGB green channel — mute the mic and look at the ring color"
    "Mute-ring RGB blue or brightness — mute the mic and look at the ring color"
    "Dial mode — turn the dial; what does it control now?"
    "LED/indicator brightness change"
)
test_values=(
    "0x01 0x02"
    "0x00 0xFF"
    "0x01 0xFF"
    "0x01 0xFF"
    "0x01 0x00"
    "0x00 0x40 0x80 0xFF"
    "0x01 0xFF"
    "0xFF"
    "0xFF"
    "0xFF"
    "0x00 0x01 0x02"
    "0x00 0x80 0xFF"
)

for i in "${!bytes[@]}"; do
    off="${bytes[$i]}"
    log ""
    log "========================================"
    log "Testing config byte offset $off"
    log "Watch for: ${watch_for[$i]}"
    log "========================================"

    vals="${test_values[$i]}"
    for val in $vals; do
        log ""
        log "--- Setting offset $off to $val ---"
        run_probe write "$off" "$val"
        ask "Offset $off = $val. What did you observe? (none / describe)"
        run_probe restore
        ask "After restore, did the device return to baseline? (yes/no / describe)"
    done
done

# ─── RGB combination test ─────────────────────────────────────────────────
log ""
log "========================================"
log "RGB combination test (if single bytes did nothing)"
log "========================================"
log "Setting offsets 10=0xFF, 11=0x00, 13=0x00 (force red)"
run_probe write 10 0xFF
run_probe write 11 0x00
run_probe write 13 0x00
ask "With the mic muted, what color is the ring LED now?"
run_probe restore

log ""
log "Setting offsets 10=0x00, 11=0xFF, 13=0x00 (force green)"
run_probe write 10 0x00
run_probe write 11 0xFF
run_probe write 13 0x00
ask "With the mic muted, what color is the ring LED now?"
run_probe restore

log ""
log "Setting offsets 10=0x00, 11=0x00, 13=0xFF (force blue)"
run_probe write 10 0x00
run_probe write 11 0x00
run_probe write 13 0xFF
ask "With the mic muted, what color is the ring LED now?"
run_probe restore

log ""
log "========================================"
log "Probe session complete. Log saved to: $LOG"
log "Please upload $LOG so I can map the bytes."
