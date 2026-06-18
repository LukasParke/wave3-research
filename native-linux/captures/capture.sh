#!/usr/bin/env bash
# Helper to capture Wave:3 USB traffic while Wave Link runs in a VM.
# Run on the Linux host as root.

set -euo pipefail

DEV=$(lsusb -d 0fd9:0070 | awk '{print $2 "/" $4}' | tr -d ':')
if [[ -z "$DEV" ]]; then
    echo "Wave:3 not found" >&2
    exit 1
fi

BUS=$(lsusb -d 0fd9:0070 | awk '{print $2}')
BUS_NUM=${BUS#0}

echo "Wave:3 found on bus $BUS (device $DEV)"
echo "Loading usbmon..."
modprobe usbmon

OUT="wave3-wavelink-$(date +%Y%m%d-%H%M%S).pcapng"
echo "Writing capture to: $OUT"
echo "Press Ctrl+C when done."

tshark -i "usbmon$BUS_NUM" \
    -f "usb.idVendor == 0x0fd9 and usb.idProduct == 0x0070" \
    -w "$OUT" -F pcapng
