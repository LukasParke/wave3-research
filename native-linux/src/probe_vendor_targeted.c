/*
 * Targeted Wave:3 vendor property probe.
 *
 * Tests a curated list of 16-bit property IDs derived from Wave Link's
 * SessionAPI descriptor tables. Uses the known-good encoding:
 *   bmRequestType=0xC1, bRequest=0x85, wIndex=0x3303.
 *
 * Read-only. Exits on first successful response so it can be inspected
 * safely.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <libusb-1.0/libusb.h>

#define VID 0x0fd9
#define PID 0x0070
#define IFACE 3
#define WINDEX 0x3303
#define BREQ 0x85
#define TIMEOUT 500

static const uint16_t ids[] = {
    0x0000, 0x0001, 0x0002, 0x0003,
    0x0007, 0x000b, 0x000f, 0x0013,
    0x0017, 0x001b, 0x001f,
    0x0055, 0x0056,
};

int main(void) {
    libusb_context *ctx = NULL;
    libusb_init(&ctx);
    libusb_device_handle *h = libusb_open_device_with_vid_pid(ctx, VID, PID);
    if (!h) { fprintf(stderr, "Wave:3 not found\n"); return 1; }
    int r = libusb_claim_interface(h, IFACE);
    if (r < 0) { fprintf(stderr, "claim failed: %s\n", libusb_error_name(r)); return 1; }

    unsigned char buf[64];
    for (size_t i = 0; i < sizeof(ids)/sizeof(ids[0]); i++) {
        uint16_t id = ids[i];
        for (uint16_t len = 1; len <= 8; len *= 2) {
            memset(buf, 0, sizeof(buf));
            r = libusb_control_transfer(h, 0xC1, BREQ, id, WINDEX, buf, len, TIMEOUT);
            if (r == len) {
                printf("ID 0x%04X len=%u OK: ", id, len);
                for (int j = 0; j < len; j++) printf("%02x ", buf[j]);
                printf("\n");
                goto done;
            }
        }
    }
    printf("No responses from targeted IDs.\n");
done:
    libusb_release_interface(h, IFACE);
    libusb_close(h);
    libusb_exit(ctx);
    return 0;
}
