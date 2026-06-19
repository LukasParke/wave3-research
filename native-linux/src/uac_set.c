#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <libusb-1.0/libusb.h>

#define VID 0x0fd9
#define PID 0x0070
#define IFACE 3

/* UAC SET_CUR with wIndex interface trick */
static int uac_set(libusb_device_handle *h, uint8_t entity, uint8_t selector, uint8_t channel,
                   const unsigned char *data, uint16_t len) {
    uint16_t wValue = (selector << 8) | channel;
    uint16_t wIndex = (entity << 8) | IFACE;
    return libusb_control_transfer(h, 0x21, 0x01, wValue, wIndex, (unsigned char *)data, len, 1000);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s hpvol <percent 0-100> | hpmute <0|1> | micmute <0|1>\n", argv[0]);
        return 1;
    }
    libusb_context *ctx = NULL;
    libusb_init(&ctx);
    libusb_device_handle *h = libusb_open_device_with_vid_pid(ctx, VID, PID);
    if (!h) { fprintf(stderr, "Wave:3 not found\n"); return 1; }
    int r = libusb_claim_interface(h, IFACE);
    if (r < 0) { fprintf(stderr, "claim failed: %s\n", libusb_error_name(r)); return 1; }

    if (strcmp(argv[1], "hpvol") == 0 && argc == 3) {
        double pct = atof(argv[2]);
        double db = -60.0 * (1.0 - pct / 100.0);
        int16_t raw = (int16_t)(db * 256.0);
        unsigned char buf[2] = { raw & 0xff, (raw >> 8) & 0xff };
        r = uac_set(h, 5, 2, 0, buf, 2);
        printf("set hp vol %.1f%% (%.1f dB raw %d) r=%d\n", pct, db, raw, r);
    } else if (strcmp(argv[1], "hpmute") == 0 && argc == 3) {
        unsigned char buf[1] = { atoi(argv[2]) ? 1 : 0 };
        r = uac_set(h, 5, 1, 0, buf, 1);
        printf("set hp mute %d r=%d\n", buf[0], r);
    } else if (strcmp(argv[1], "micmute") == 0 && argc == 3) {
        unsigned char buf[1] = { atoi(argv[2]) ? 1 : 0 };
        r = uac_set(h, 6, 1, 0, buf, 1);
        printf("set mic mute %d r=%d\n", buf[0], r);
    } else {
        fprintf(stderr, "unknown command\n");
    }

    libusb_release_interface(h, IFACE);
    libusb_close(h);
    libusb_exit(ctx);
    return 0;
}
