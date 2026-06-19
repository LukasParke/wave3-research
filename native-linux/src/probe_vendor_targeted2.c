/*
 * Expanded targeted Wave:3 vendor property probe.
 *
 * Tests low-byte property IDs from Wave Link descriptors across multiple
 * wIndex entities and payload lengths, using read-only bRequest=0x85.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <libusb-1.0/libusb.h>

#define VID 0x0fd9
#define PID 0x0070
#define IFACE 3
#define BREQ 0x85
#define TIMEOUT 500

struct probe { const char *name; uint16_t id; uint16_t windex; uint16_t len; };

static const struct probe probes[] = {
    // category 0x04 indicator/RGB IDs via entity 0x34 and 0x33
    {"indicator_main",   0x00, 0x3303, 4},
    {"indicator_main34", 0x00, 0x3403, 4},
    {"mute_bg",          0x03, 0x3303, 4},
    {"mute_bg34",        0x03, 0x3403, 4},
    {"gain_reduction",   0x07, 0x3303, 4},
    {"gain_reduction34", 0x07, 0x3403, 4},
    {"gain_val",         0x0b, 0x3303, 4},
    {"gain_val34",       0x0b, 0x3403, 4},
    {"gain_meter_bg",    0x0f, 0x3303, 4},
    {"gain_meter_bg34",  0x0f, 0x3403, 4},
    {"hp_val",           0x13, 0x3303, 4},
    {"hp_val34",         0x13, 0x3403, 4},
    {"hp_meter_bg",      0x17, 0x3303, 4},
    {"hp_meter_bg34",    0x17, 0x3403, 4},
    {"mix_val",          0x1b, 0x3303, 4},
    {"mix_val34",        0x1b, 0x3403, 4},
    {"mix_bg",           0x1f, 0x3303, 4},
    {"mix_bg34",         0x1f, 0x3403, 4},
    // monitor mix (category 0x01, ids 0x55/0x56)
    {"monitor_0",        0x55, 0x3303, 4},
    {"monitor_1",        0x56, 0x3303, 4},
    {"monitor_0_34",     0x55, 0x3403, 4},
    {"monitor_1_34",     0x56, 0x3403, 4},
};

int main(void) {
    libusb_context *ctx = NULL;
    libusb_init(&ctx);
    libusb_device_handle *h = libusb_open_device_with_vid_pid(ctx, VID, PID);
    if (!h) { fprintf(stderr, "Wave:3 not found\n"); return 1; }
    int r = libusb_claim_interface(h, IFACE);
    if (r < 0) { fprintf(stderr, "claim failed: %s\n", libusb_error_name(r)); return 1; }

    unsigned char buf[64];
    int found = 0;
    for (size_t i = 0; i < sizeof(probes)/sizeof(probes[0]); i++) {
        memset(buf, 0, sizeof(buf));
        r = libusb_control_transfer(h, 0xC1, BREQ, probes[i].id, probes[i].windex,
                                    buf, probes[i].len, TIMEOUT);
        if (r == probes[i].len) {
            printf("%s id=0x%02x wIndex=0x%04x len=%u OK: ",
                   probes[i].name, probes[i].id, probes[i].windex, probes[i].len);
            for (int j = 0; j < r; j++) printf("%02x ", buf[j]);
            printf("\n");
            found++;
        }
    }
    if (!found) printf("No responses from expanded targeted IDs.\n");

    libusb_release_interface(h, IFACE);
    libusb_close(h);
    libusb_exit(ctx);
    return 0;
}
