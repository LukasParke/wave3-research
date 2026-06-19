/*
 * auto_probe — automated Wave:3 config block enumeration
 *
 * Stops wave3-daemon, probes every byte with multiple values, reads back,
 * and writes a machine/human-readable report. Uses only safe class
 * control transfers on interface 3.
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

static libusb_device_handle *open_dev(libusb_context **ctx) {
    libusb_init(ctx);
    libusb_device_handle *h = libusb_open_device_with_vid_pid(*ctx, VID, PID);
    if (!h) { fprintf(stderr, "Wave:3 not found\n"); exit(1); }
    int r = libusb_claim_interface(h, IFACE);
    if (r < 0) { fprintf(stderr, "claim failed: %s\n", libusb_error_name(r)); exit(1); }
    return h;
}

static void close_dev(libusb_device_handle *h, libusb_context *ctx) {
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

static void print_cfg(FILE *f, const unsigned char *cfg) {
    for (int i = 0; i < 16; i++) fprintf(f, "%02x ", cfg[i]);
}

static void diff_cfg(FILE *f, const unsigned char *a, const unsigned char *b) {
    int any = 0;
    for (int i = 0; i < 16; i++) {
        if (a[i] != b[i]) {
            fprintf(f, "  [%d] %02x -> %02x\n", i, a[i], b[i]);
            any = 1;
        }
    }
    if (!any) fprintf(f, "  (no change)\n");
}

static int readback_write(libusb_device_handle *h, int off, int val,
                          unsigned char *before_out, unsigned char *after_out) {
    unsigned char tmp[16];
    int r = cfg_read(h, tmp);
    if (r != 16) return -1;
    memcpy(before_out, tmp, 16);
    tmp[off] = (unsigned char)val;
    r = cfg_write(h, tmp);
    if (r != 16) return -1;
    usleep(20000);
    r = cfg_read(h, after_out);
    return r;
}

static void restore(libusb_device_handle *h, const unsigned char *baseline) {
    cfg_write(h, baseline);
    usleep(20000);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    FILE *out = fopen("/home/USER/wave3-auto-probe-report.txt", "w");
    if (!out) { perror("fopen"); return 1; }

    fprintf(out, "# Wave:3 Automated Config Block Probe Report\n");
    fprintf(out, "# Generated: 2026-06-19\n\n");

    /* stop daemon */
    system("systemctl --user stop wave3-daemon 2>/dev/null");
    sleep(1);

    libusb_context *ctx = NULL;
    libusb_device_handle *h = open_dev(&ctx);

    unsigned char baseline[16];
    if (cfg_read(h, baseline) != 16) {
        fprintf(out, "ERROR: baseline read failed\n");
        close_dev(h, ctx);
        fclose(out);
        return 1;
    }
    fprintf(out, "Baseline config: ");
    print_cfg(out, baseline);
    fprintf(out, "\n\n");

    /* Per-byte sweep */
    int testvals[] = {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0xFF};
    int nvals = sizeof(testvals)/sizeof(testvals[0]);

    fprintf(out, "## Per-byte write/readback sweep\n");
    fprintf(out, "For each offset, the value was written and the device was read back.\n");
    fprintf(out, "'sticky' means the device kept the value; 'norm' means it changed it.\n\n");

    for (int off = 0; off < 16; off++) {
        fprintf(out, "### offset %d\n", off);
        for (int vi = 0; vi < nvals; vi++) {
            int val = testvals[vi];
            unsigned char before[16], after[16];
            int r = readback_write(h, off, val, before, after);
            if (r != 16) {
                fprintf(out, "  value 0x%02x: read/write error r=%d\n", val, r);
                continue;
            }
            int sticky = (after[off] == (unsigned char)val);
            fprintf(out, "  write 0x%02x -> readback 0x%02x [%s]", val, after[off],
                    sticky ? "sticky" : "norm");
            if (!sticky) {
                fprintf(out, " | full diff:");
                for (int j = 0; j < 16; j++) {
                    if (after[j] != baseline[j]) fprintf(out, " [%d]%02x", j, after[j]);
                }
            }
            fprintf(out, "\n");
            restore(h, baseline);
            usleep(20000);
        }
        fprintf(out, "\n");
    }

    /* Multi-byte pattern tests */
    fprintf(out, "## Multi-byte pattern tests\n\n");

    struct { const char *name; unsigned char cfg[16]; } patterns[] = {
        {"all zeros",           {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}},
        {"all ones",            {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1}},
        {"clipguard on",        {0,1,0,0,0,1,0,0,0xf7,0,0,0,3,0,1,1}},
        {"lowcut 0x0001",       {0,1,0,0,0,0,0,0x01,0,0,0,0,3,0,1,1}},
        {"lowcut 0x0100",       {0,1,0,0,0,0,0,0x00,0x01,0,0,0,3,0,1,1}},
        {"direct mon 0%",       {0,1,0,0,0,0,0,0,0xf7,0,0,0,3,0,0,1}},
        {"direct mon 50%",      {0,1,0,0,0,0,0,0,0xf7,0,0,0,3,0,0x80,1}},
        {"direct mon 100%",     {0,1,0,0,0,0,0,0,0xf7,0,0,0,3,0,0xff,1}},
        {"mute red",            {0,1,0,0,0,0,0,0,0xf7,0,0xff,0,3,0,1,1}},
        {"mute green",          {0,1,0,0,0,0,0,0,0xf7,0,0,0xff,3,0,1,1}},
        {"mute blue",           {0,1,0,0,0,0,0,0,0xf7,0,0,0,3,0xff,1,1}},
        {"mute white",          {0,1,0,0,0,0,0,0,0xf7,0,0xff,0xff,3,0xff,1,1}},
        {"indicator off",       {0,1,0,0,0,0,0,0,0xf7,0,0,0,3,0,1,0}},
        {"indicator max",       {0,1,0,0,0,0,0,0,0xf7,0,0,0,3,0,1,0xff}},
    };

    for (size_t pi = 0; pi < sizeof(patterns)/sizeof(patterns[0]); pi++) {
        fprintf(out, "### %s\n", patterns[pi].name);
        unsigned char before[16], after[16];
        cfg_read(h, before);
        cfg_write(h, patterns[pi].cfg);
        usleep(50000);
        cfg_read(h, after);
        fprintf(out, "  requested: "); print_cfg(out, patterns[pi].cfg); fprintf(out, "\n");
        fprintf(out, "  readback:  "); print_cfg(out, after); fprintf(out, "\n");
        fprintf(out, "  diff from baseline:\n");
        diff_cfg(out, baseline, after);
        restore(h, baseline);
        usleep(20000);
    }

    /* Final restore and cleanup */
    restore(h, baseline);
    unsigned char final[16];
    cfg_read(h, final);
    fprintf(out, "\n## Final verification\n");
    fprintf(out, "Baseline: "); print_cfg(out, baseline); fprintf(out, "\n");
    fprintf(out, "Final:    "); print_cfg(out, final); fprintf(out, "\n");
    fprintf(out, "Match: %s\n", memcmp(baseline, final, 16) == 0 ? "YES" : "NO");

    close_dev(h, ctx);
    fclose(out);

    /* restart daemon */
    system("systemctl --user start wave3-daemon 2>/dev/null");

    printf("Report written to /home/USER/wave3-auto-probe-report.txt\n");
    return 0;
}
