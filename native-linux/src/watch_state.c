/*
 * watch_state — continuously read Wave:3 config block and report changes.
 *
 * Useful for discovering which config byte reflects physical state changes
 * (e.g. headphone plug/unplug, dial mode changes).
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <libusb-1.0/libusb.h>

#define VID 0x0fd9
#define PID 0x0070
#define IFACE 3
#define WINDEX 0x3303

static void print_cfg(const unsigned char *cfg) {
    for (int i = 0; i < 16; i++) printf("%02x ", cfg[i]);
}

int main(void) {
    libusb_context *ctx = NULL;
    libusb_init(&ctx);
    libusb_device_handle *h = libusb_open_device_with_vid_pid(ctx, VID, PID);
    if (!h) { fprintf(stderr, "Wave:3 not found\n"); return 1; }
    if (libusb_claim_interface(h, IFACE) < 0) {
        fprintf(stderr, "claim interface %d failed (daemon running?)\n", IFACE);
        return 1;
    }

    unsigned char prev[16] = {0};
    int first = 1;
    printf("Watching Wave:3 config block. Press Ctrl-C to stop.\n");
    printf("Try: unplug/replug headphones, rotate the dial, press the mute capacitive pad.\n\n");

    while (1) {
        unsigned char cfg[16];
        int r = libusb_control_transfer(h, 0xA1, 0x85, 0x0000, WINDEX, cfg, 16, 1000);
        if (r != 16) {
            printf("read error r=%d\n", r);
            usleep(100000);
            continue;
        }

        if (first || memcmp(cfg, prev, 16) != 0) {
            printf("[%ld] ", time(NULL));
            print_cfg(cfg);
            if (!first) {
                printf(" | changed:");
                for (int i = 0; i < 16; i++) {
                    if (cfg[i] != prev[i]) printf(" [%d]%02x->%02x", i, prev[i], cfg[i]);
                }
            }
            printf("\n");
            memcpy(prev, cfg, 16);
            first = 0;
        }
        usleep(100000); /* 10 Hz */
    }

    libusb_release_interface(h, IFACE);
    libusb_close(h);
    libusb_exit(ctx);
    return 0;
}
