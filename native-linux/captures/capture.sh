#!/usr/bin/env bash
# Capture Wave:3 USB traffic while Wave Link runs in the Windows VM.
# Run on the Linux host as root (usbmon requires root).

set -euo pipefail

OUT="${1:-wave3-wavelink.pcapng}"

BUS=$(lsusb -d 0fd9:0070 | awk '{print $2}' | head -1)
if [[ -z "$BUS" ]]; then
    echo "ERROR: Elgato Wave:3 (0fd9:0070) not found on USB" >&2
    exit 1
fi
BUS_NUM=${BUS#0}

echo "Wave:3 found on bus $BUS -> capturing usbmon$BUS_NUM"
echo "Writing capture to: $OUT"
echo "NOTE: capturing the whole bus (no capture filter)."
echo "      Filter to the Wave:3 later with:"
echo "        usb.idVendor == 0x0fd9 && usb.idProduct == 0x0070"
echo "Press Ctrl+C when you are done using Wave Link."
echo

modprobe usbmon

tshark -i "usbmon$BUS_NUM" -w "$OUT" -F pcapng
