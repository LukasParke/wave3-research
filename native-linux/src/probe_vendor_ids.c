/*
 * Live Wave:3 vendor property-ID probe.
 *
 * Reads property IDs using the vendor request encoding recovered from
 * static reverse engineering of Wave Link's LegacyUAC1VendorUSBBackendStrategy:
 *
 *   bmRequestType = 0xC1   (vendor, IN, interface recipient)
 *   bRequest      = 0x85
 *   wValue        = property ID (16-bit)
 *   wIndex        = 0x3303   (entity 0x33, interface 3)
 *
 * This program is read-only and safe: it does not write to the device.
 * It claims interface 3 via libusb; stop wave3-daemon first to avoid -EBUSY.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <libusb-1.0/libusb.h>

#define VID 0x0fd9
#define PID 0x0070
#define VENDOR_INTERFACE 3
#define WINDEX 0x3303
#define BREQUEST_READ 0x85
#define TIMEOUT_MS 200

static int try_read(libusb_device_handle *h, uint16_t prop_id,
                    unsigned char *buf, uint16_t len) {
    memset(buf, 0, len);
    int r = libusb_control_transfer(h,
                                    0xC1,          /* bmRequestType */
                                    BREQUEST_READ, /* bRequest */
                                    prop_id,       /* wValue */
                                    WINDEX,        /* wIndex */
                                    buf, len, TIMEOUT_MS);
    return r;
}

int main(int argc, char **argv) {
    unsigned char buf[64];
    uint16_t start = 0x0000;
    uint16_t end   = 0x00FF;
    int detailed = 0;

    if (argc > 1) start = (uint16_t)strtoul(argv[1], NULL, 0);
    if (argc > 2) end   = (uint16_t)strtoul(argv[2], NULL, 0);
    if (argc > 3) detailed = atoi(argv[3]);

    libusb_context *ctx = NULL;
    int r = libusb_init(&ctx);
    if (r < 0) { fprintf(stderr, "libusb_init failed: %d\n", r); return 1; }
    libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_WARNING);

    libusb_device **devs;
    ssize_t cnt = libusb_get_device_list(ctx, &devs);
    if (cnt < 0) { fprintf(stderr, "device list error\n"); libusb_exit(ctx); return 1; }

    libusb_device_handle *h = NULL;
    for (ssize_t i = 0; i < cnt; i++) {
        struct libusb_device_descriptor desc;
        libusb_get_device_descriptor(devs[i], &desc);
        if (desc.idVendor == VID && desc.idProduct == PID) {
            r = libusb_open(devs[i], &h);
            if (r < 0) {
                fprintf(stderr, "libusb_open failed: %s\n", libusb_error_name(r));
                h = NULL;
            }
            break;
        }
    }
    libusb_free_device_list(devs, 1);

    if (!h) {
        fprintf(stderr, "Wave:3 (0fd9:0070) not found or could not be opened\n");
        libusb_exit(ctx);
        return 1;
    }

    r = libusb_claim_interface(h, VENDOR_INTERFACE);
    if (r < 0) {
        fprintf(stderr, "claim interface %d failed: %s (stop wave3-daemon first)\n",
                VENDOR_INTERFACE, libusb_error_name(r));
        libusb_close(h);
        libusb_exit(ctx);
        return 1;
    }

    printf("Probing property IDs 0x%04X .. 0x%04X (read-only)\n", start, end);
    printf("%-8s %-6s %-6s %-10s %s\n", "prop_id", "len1", "len2", "len4", "hex");

    for (uint32_t id = start; id <= end; id++) {
        uint16_t prop_id = (uint16_t)id;
        int r1 = try_read(h, prop_id, buf, 1);
        int r2 = try_read(h, prop_id, buf, 2);
        int r4 = try_read(h, prop_id, buf, 4);

        int ok1 = r1 == 1;
        int ok2 = r2 == 2;
        int ok4 = r4 == 4;

        if (ok1 || ok2 || ok4) {
            /* Re-read with the largest successful length for display */
            int display_len = ok4 ? 4 : (ok2 ? 2 : 1);
            try_read(h, prop_id, buf, display_len);

            char hex[16] = {0};
            for (int i = 0; i < display_len; i++)
                snprintf(hex + i*3, sizeof(hex) - i*3, "%02x ", buf[i]);

            printf("0x%04X   %-6s %-6s %-10s %s\n",
                   prop_id,
                   ok1 ? "OK" : "--",
                   ok2 ? "OK" : "--",
                   ok4 ? "OK" : "--",
                   hex);
        } else if (detailed) {
            printf("0x%04X   err1=%s err2=%s err4=%s\n",
                   prop_id,
                   libusb_error_name(r1),
                   libusb_error_name(r2),
                   libusb_error_name(r4));
        }
    }

    libusb_release_interface(h, VENDOR_INTERFACE);
    libusb_close(h);
    libusb_exit(ctx);
    return 0;
}
