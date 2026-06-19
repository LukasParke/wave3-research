/*
 * device_info — read the Wave:3 51-byte device info block.
 */

#include <stdio.h>
#include <stdlib.h>
#include <libusb-1.0/libusb.h>

#define VID 0x0fd9
#define PID 0x0070
#define IFACE 3
#define WINDEX 0x3303

int main(void) {
    libusb_context *ctx = NULL;
    libusb_init(&ctx);
    libusb_device_handle *h = libusb_open_device_with_vid_pid(ctx, VID, PID);
    if (!h) { fprintf(stderr, "Wave:3 not found\n"); return 1; }
    if (libusb_claim_interface(h, IFACE) < 0) {
        fprintf(stderr, "claim interface %d failed (daemon running?)\n", IFACE);
        return 1;
    }

    unsigned char info[51];
    int r = libusb_control_transfer(h, 0xA1, 0x85, 0x000A, WINDEX, info, 51, 1000);
    printf("device info read r=%d\n", r);
    if (r == 51) {
        printf("API version: %d.%d\n", info[1], info[0]);
        printf("Firmware:    %d.%d.%d\n", info[6], info[7], info[8]);
        printf("Full hex dump:\n");
        for (int i = 0; i < 51; i++) {
            printf("%02x ", info[i]);
            if ((i + 1) % 16 == 0) printf("\n");
        }
        printf("\nASCII:\n");
        for (int i = 0; i < 51; i++) {
            unsigned char c = info[i];
            putchar((c >= 0x20 && c < 0x7f) ? c : '.');
            if ((i + 1) % 16 == 0) putchar('\n');
        }
        printf("\n");
    }

    libusb_release_interface(h, IFACE);
    libusb_close(h);
    libusb_exit(ctx);
    return 0;
}
