# USB Capture Directory

Place `usbmon` / Wireshark captures of Wave Link talking to the Wave:3 here.

## Required capture

A `.pcapng` file captured on the Linux host while a Windows VM with Wave
Link has the Wave:3 passed through.

## Capture commands

```bash
sudo modprobe usbmon
lsusb | grep -i elgato   # note Bus and Device numbers
sudo tshark -i usbmonN -f "usb.idVendor == 0x0fd9 and usb.idProduct == 0x0070" \
    -w wave3-wavelink.pcapng -F pcapng
```

Replace `usbmonN` with the bus number (e.g. `usbmon5`).

## Recommended toggle sequence (one feature at a time, ~3 s apart)

1. Mic mute on/off (already known via UAC — good sanity check)
2. Headphone mute on/off
3. Headphone volume 0%, 50%, 100%
4. Mic gain dial min/mid/max
5. Clipguard on/off
6. Low-cut filter on/off
7. Direct monitor / monitor mix 0%, 50%, 100%
8. Mute RGB color: red (`#ff0000`), green (`#00ff00`), blue (`#0000ff`), white, black
9. Headphone RGB color (if available in UI)
10. LED brightness / indicator brightness changes
11. Start/stop audio playback and recording

Annotate the timestamp or note the order so the capture can be correlated
with the UI actions.
