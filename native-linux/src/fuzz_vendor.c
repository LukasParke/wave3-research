#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <libusb-1.0/libusb.h>

#define VID 0x0fd9
#define PID 0x0070

int main(int argc, char **argv) {
    libusb_context *ctx = NULL;
    libusb_init(&ctx);
    libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_WARNING);

    libusb_device_handle *h = libusb_open_device_with_vid_pid(ctx, VID, PID);
    if (!h) { fprintf(stderr, "open failed\n"); libusb_exit(ctx); return 1; }

    int r = libusb_claim_interface(h, 3);
    if (r < 0) { fprintf(stderr, "claim failed: %s\n", libusb_error_name(r)); libusb_close(h); libusb_exit(ctx); return 1; }

    unsigned char buf[256];
    for (int req = 0; req <= 0x20; req++) {
        memset(buf, 0, sizeof(buf));
        r = libusb_control_transfer(h, 0xC1, req, 0x0000, 0x0003, buf, 64, 500);
        if (r >= 0) {
            printf("IN  req=0x%02x idx=0x0003 len=%d data=", req, r);
            for (int i = 0; i < r && i < 16; i++) printf("%02x", buf[i]);
            printf("\n");
        } else if (r != LIBUSB_ERROR_PIPE && r != LIBUSB_ERROR_IO) {
            printf("IN  req=0x%02x idx=0x0003 error=%s\n", req, libusb_error_name(r));
        }
    }
    for (int req = 0; req <= 0x20; req++) {
        memset(buf, 0, sizeof(buf));
        r = libusb_control_transfer(h, 0xC0, req, 0x0000, 0x0000, buf, 64, 500);
        if (r >= 0) {
            printf("IN  req=0x%02x idx=0x0000 len=%d data=", req, r);
            for (int i = 0; i < r && i < 16; i++) printf("%02x", buf[i]);
            printf("\n");
        } else if (r != LIBUSB_ERROR_PIPE && r != LIBUSB_ERROR_IO) {
            printf("IN  req=0x%02x idx=0x0000 error=%s\n", req, libusb_error_name(r));
        }
    }

    libusb_release_interface(h, 3);
    libusb_close(h);
    libusb_exit(ctx);
    return 0;
}
