/*
 * cfg_probe — safe Elgato Wave:3 config block probe
 *
 * Reads/writes the 16-byte config block (wValue=0x0000, wIndex=0x3303).
 * Always "read" first to snapshot, then "restore" returns to that snapshot.
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

static unsigned char snapshot[16];
static int snapshot_valid = 0;

static libusb_device_handle *open_device(libusb_context **ctx) {
    libusb_init(ctx);
    libusb_device_handle *h = libusb_open_device_with_vid_pid(*ctx, VID, PID);
    if (!h) {
        fprintf(stderr, "Wave:3 (0fd9:0070) not found\n");
        libusb_exit(*ctx);
        exit(1);
    }
    int r = libusb_claim_interface(h, IFACE);
    if (r < 0) {
        fprintf(stderr, "claim interface %d failed: %s\n", IFACE, libusb_error_name(r));
        libusb_close(h);
        libusb_exit(*ctx);
        exit(1);
    }
    return h;
}

static void close_device(libusb_device_handle *h, libusb_context *ctx) {
    libusb_release_interface(h, IFACE);
    libusb_close(h);
    libusb_exit(ctx);
}

static int cfg_read(libusb_device_handle *h, unsigned char *cfg) {
    return libusb_control_transfer(h, 0xA1, 0x85, 0x0000, WINDEX, cfg, 16, 1000);
}

static int cfg_write(libusb_device_handle *h, const unsigned char *cfg) {
    return libusb_control_transfer(h, 0x21, 0x05, 0x0000, WINDEX, (unsigned char *)cfg, 16, 1000);
}

static void print_cfg(const unsigned char *cfg) {
    for (int i = 0; i < 16; i++) printf("%02x ", cfg[i]);
    printf("\n");
}

int main(int argc, char **argv) {
    libusb_context *ctx = NULL;
    libusb_device_handle *h = open_device(&ctx);

    if (argc < 2) {
        printf("Usage: %s read | write <offset> <value> | restore\n", argv[0]);
        close_device(h, ctx);
        return 1;
    }

    if (strcmp(argv[1], "read") == 0) {
        unsigned char cfg[16];
        int r = cfg_read(h, cfg);
        if (r != 16) {
            fprintf(stderr, "read failed: %d\n", r);
            close_device(h, ctx);
            return 1;
        }
        memcpy(snapshot, cfg, 16);
        snapshot_valid = 1;
        printf("config: ");
        print_cfg(cfg);
    } else if (strcmp(argv[1], "write") == 0 && argc == 4) {
        int off = atoi(argv[2]);
        int val = strtol(argv[3], NULL, 0);
        if (off < 0 || off > 15 || val < 0 || val > 255) {
            fprintf(stderr, "invalid offset/value\n");
            close_device(h, ctx);
            return 1;
        }
        unsigned char cfg[16];
        int r = cfg_read(h, cfg);
        if (r != 16) {
            fprintf(stderr, "read failed: %d\n", r);
            close_device(h, ctx);
            return 1;
        }
        if (!snapshot_valid) {
            memcpy(snapshot, cfg, 16);
            snapshot_valid = 1;
        }
        printf("before: ");
        print_cfg(cfg);
        cfg[off] = (unsigned char)val;
        r = cfg_write(h, cfg);
        printf("write offset %d = 0x%02x -> r=%d\n", off, val, r);
        memset(cfg, 0, sizeof(cfg));
        r = cfg_read(h, cfg);
        printf("after:  ");
        print_cfg(cfg);
    } else if (strcmp(argv[1], "restore") == 0) {
        if (!snapshot_valid) {
            fprintf(stderr, "no snapshot; run 'read' first\n");
            close_device(h, ctx);
            return 1;
        }
        int r = cfg_write(h, snapshot);
        printf("restore snapshot -> r=%d\n", r);
        unsigned char cfg[16];
        r = cfg_read(h, cfg);
        printf("after:  ");
        print_cfg(cfg);
    } else {
        printf("Usage: %s read | write <offset> <value> | restore\n", argv[0]);
    }

    close_device(h, ctx);
    return 0;
}
