/*
 * Focused fuzzer for the Wave:3 vendor control interface.
 * Tries vendor-class control transfers on interface 3 with various
 * bRequest/wValue/wIndex combinations, looking for any non-timeout response.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libusb-1.0/libusb.h>

#define VID 0x0fd9
#define PID 0x0070
#define IFACE 3

static void try_request(libusb_device_handle *h, uint8_t rt, uint8_t req,
                        uint16_t wV, uint16_t wI, uint16_t len)
{
    unsigned char *buf = calloc(1, len);
    int r;

    if (rt & 0x80) {
        r = libusb_control_transfer(h, rt, req, wV, wI, buf, len, 500);
    } else {
        /* OUT: send a simple payload */
        for (uint16_t i = 0; i < len; i++) buf[i] = (unsigned char)i;
        r = libusb_control_transfer(h, rt, req, wV, wI, buf, len, 500);
    }

    if (r != LIBUSB_ERROR_TIMEOUT && r != LIBUSB_ERROR_PIPE) {
        printf("HIT rt=0x%02x req=0x%02x wV=0x%04x wI=0x%04x len=%u -> %d (%s)\n",
               rt, req, wV, wI, len, r,
               r < 0 ? libusb_error_name(r) : "OK");
        if (r > 0) {
            printf("  data:");
            for (int i = 0; i < r && i < 32; i++) printf(" %02x", buf[i]);
            printf("\n");
        }
    }

    free(buf);
}

int main(void)
{
    libusb_context *ctx = NULL;
    libusb_device_handle *h = NULL;
    int r;

    r = libusb_init(&ctx);
    if (r < 0) { fprintf(stderr, "libusb_init: %s\n", libusb_error_name(r)); return 1; }

    h = libusb_open_device_with_vid_pid(ctx, VID, PID);
    if (!h) { fprintf(stderr, "device not found\n"); libusb_exit(ctx); return 1; }

    r = libusb_claim_interface(h, IFACE);
    if (r < 0) {
        fprintf(stderr, "claim: %s\n", libusb_error_name(r));
        libusb_close(h); libusb_exit(ctx); return 1;
    }

    printf("Fuzzing vendor requests on interface %d (safe: expect timeouts/stalls)\n", IFACE);

    /* Try common vendor request patterns */
    for (uint8_t req = 0; req < 16; req++) {
        for (uint16_t wV = 0; wV < 0x10; wV++) {
            /* Vendor interface IN */
            try_request(h, 0xc1, req, wV, (IFACE << 8) | IFACE, 64);
            try_request(h, 0xc1, req, wV, IFACE, 64);
            /* Vendor device IN */
            try_request(h, 0xc0, req, wV, 0, 64);
            try_request(h, 0xc0, req, wV, IFACE, 64);
            /* Vendor interface OUT */
            try_request(h, 0x41, req, wV, (IFACE << 8) | IFACE, 8);
            try_request(h, 0x41, req, wV, IFACE, 8);
        }
    }

    /* Try bRequest values that look like property IDs */
    for (uint8_t req = 0x10; req < 0x40; req++) {
        try_request(h, 0xc1, req, 0, (IFACE << 8) | IFACE, 64);
        try_request(h, 0x41, req, 0, (IFACE << 8) | IFACE, 8);
    }

    printf("Fuzz complete.\n");

    libusb_release_interface(h, IFACE);
    libusb_close(h);
    libusb_exit(ctx);
    return 0;
}
