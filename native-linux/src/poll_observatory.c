/*
 * poll_observatory — high-rate Wave:3 state logger for physical experiments.
 *
 * Stops wave3-daemon, claims interface 3, and continuously polls:
 *   - the 16-byte class config block (wValue=0x0000)
 *   - the 8-byte meter block (wValue=0x0001)
 *   - UAC mic gain (entity 6, selector 2)
 *   - UAC headphone volume (entity 5, selector 2)
 *
 * Changed bytes are highlighted. Press 'm' + Enter to mark an event in the
 * log (describe what you just did), 'q' + Enter to quit.
 *
 * Usage:
 *   ./poll_observatory
 *
 * Then perform physical actions on the Wave:3 in this order:
 *   1. Press the mute capacitive pad (mute / unmute)
 *   2. Rotate the main dial through its modes (gain, headphone volume,
 *      monitor mix) and adjust values up/down
 *   3. Press the dial to switch modes
 *   4. Unplug / replug headphones
 *   5. Any other physical interaction
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <poll.h>
#include <termios.h>
#include <libusb-1.0/libusb.h>

#define VID 0x0fd9
#define PID 0x0070
#define IFACE 3
#define WINDEX 0x3303

#define POLL_US 50000  /* 20 Hz */

static struct termios old_tio;

static void set_terminal_raw(void) {
    struct termios new_tio;
    tcgetattr(STDIN_FILENO, &old_tio);
    new_tio = old_tio;
    new_tio.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
}

static void restore_terminal(void) {
    tcsetattr(STDIN_FILENO, TCSANOW, &old_tio);
}

static int cfg_read(libusb_device_handle *h, unsigned char *cfg) {
    return libusb_control_transfer(h, 0xA1, 0x85, 0x0000, WINDEX, cfg, 16, 1000);
}

static int cfg_write(libusb_device_handle *h, const unsigned char *cfg) {
    return libusb_control_transfer(h, 0x21, 0x05, 0x0000, WINDEX, (unsigned char *)cfg, 16, 1000);
}

static int meter_read(libusb_device_handle *h, unsigned char *meter) {
    return libusb_control_transfer(h, 0xA1, 0x85, 0x0001, WINDEX, meter, 8, 1000);
}

static int uac_volume_read(libusb_device_handle *h, int entity, int16_t *out) {
    unsigned char buf[2];
    uint16_t wIndex3 = (uint16_t)((entity << 8) | IFACE);
    int r = libusb_control_transfer(h, 0xA1, 0x81, (0x02 << 8), wIndex3, buf, 2, 1000);
    if (r == 2) *out = (int16_t)((buf[1] << 8) | buf[0]);
    return r;
}

static void print_timestamp(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    printf("[%5ld.%03ld] ", (long)ts.tv_sec, ts.tv_nsec / 1000000);
}

static void print_cfg_line(const unsigned char *cfg, const unsigned char *prev) {
    for (int i = 0; i < 16; i++) {
        if (prev && cfg[i] != prev[i]) {
            printf("\033[7m%02x\033[0m ", cfg[i]);
        } else {
            printf("%02x ", cfg[i]);
        }
    }
}

static void print_legend(void) {
    printf("\nLegend:\n");
    printf("  [0,1] checksum/validation   [2,3,6,7] unused/reserved\n");
    printf("  [4] mic mute                [5] clipguard\n");
    printf("  [8] headphone volume (s8)   [9] headphone mute\n");
    printf("  [10] mute R  [11] mute G    [13] mute B\n");
    printf("  [12] device state (RO)      [14] direct monitor mix\n");
    printf("  [15] LED brightness\n");
    printf("\nControls during logging:\n");
    printf("  m + Enter = mark event (type description)\n");
    printf("  q + Enter = quit and restore daemon\n\n");
}

int main(int argc, char **argv) {
    printf("Stopping wave3-daemon...\n");
    system("systemctl --user stop wave3-daemon 2>/dev/null");
    sleep(1);

    libusb_context *ctx = NULL;
    libusb_init(&ctx);
    libusb_device_handle *h = libusb_open_device_with_vid_pid(ctx, VID, PID);
    if (!h) {
        fprintf(stderr, "Wave:3 not found\n");
        libusb_exit(ctx);
        system("systemctl --user start wave3-daemon 2>/dev/null");
        return 1;
    }
    if (libusb_claim_interface(h, IFACE) < 0) {
        fprintf(stderr, "claim interface %d failed (daemon still running?)\n", IFACE);
        libusb_close(h);
        libusb_exit(ctx);
        system("systemctl --user start wave3-daemon 2>/dev/null");
        return 1;
    }

    set_terminal_raw();

    unsigned char prev_cfg[16] = {0};
    unsigned char prev_meter[8] = {0};
    int16_t prev_mic_gain = 0x7fff;
    int16_t prev_hp_vol = 0x7fff;
    int first = 1;

    const char *log_path = (argc > 1) ? argv[1] : "wave3-poll-observatory.log";
    FILE *log = fopen(log_path, "a");
    if (!log) perror("fopen log");

    print_legend();

    struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };

    while (1) {
        unsigned char cfg[16], meter[8];
        int16_t mic_gain, hp_vol;

        int r_cfg = cfg_read(h, cfg);
        int r_meter = meter_read(h, meter);
        int r_mic = uac_volume_read(h, 6, &mic_gain);
        int r_hp = uac_volume_read(h, 5, &hp_vol);
        if (r_mic != 2) mic_gain = prev_cfg[0] ? (int16_t)prev_mic_gain : 0;
        if (r_hp != 2) hp_vol = prev_cfg[0] ? (int16_t)prev_hp_vol : 0;

        int cfg_changed = (r_cfg == 16) && (first || memcmp(cfg, prev_cfg, 16) != 0);
        int meter_changed = (r_meter == 8) && (first || memcmp(meter, prev_meter, 8) != 0);
        int mic_changed = (r_mic == 2) && (first || mic_gain != prev_mic_gain);
        int hp_changed = (r_hp == 2) && (first || hp_vol != prev_hp_vol);

        if (cfg_changed || meter_changed || mic_changed || hp_changed || first) {
            print_timestamp();

            if (r_cfg == 16) {
                printf("CFG: ");
                print_cfg_line(cfg, first ? NULL : prev_cfg);
                printf(" |");
                for (int i = 0; i < 16; i++) {
                    if (!first && cfg[i] != prev_cfg[i]) printf(" [%d]%02x->%02x", i, prev_cfg[i], cfg[i]);
                }
                memcpy(prev_cfg, cfg, 16);
            } else {
                printf("CFG: read error r=%d |", r_cfg);
            }

            if (r_mic == 2) {
                printf(" MIC=%.1fdB", mic_gain / 256.0);
                prev_mic_gain = mic_gain;
            }
            if (r_hp == 2) {
                printf(" HP=%.1fdB", hp_vol / 256.0);
                prev_hp_vol = hp_vol;
            }

            if (r_meter == 8) {
                uint32_t in = (uint32_t)(meter[0] | (meter[1] << 8) | (meter[2] << 16) | (meter[3] << 24));
                uint32_t pb = (uint32_t)(meter[4] | (meter[5] << 8) | (meter[6] << 16) | (meter[7] << 24));
                printf(" IN=%08x PB=%08x", in, pb);
                memcpy(prev_meter, meter, 8);
            }

            printf("\n");
            fflush(stdout);

            if (log) {
                struct timespec ts;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                fprintf(log, "%5ld.%03ld CFG=", (long)ts.tv_sec, ts.tv_nsec / 1000000);
                for (int i = 0; i < 16; i++) fprintf(log, "%02x ", cfg[i]);
                fprintf(log, " MIC=%.1f HP=%.1f IN=%08x PB=%08x\n",
                        mic_gain / 256.0, hp_vol / 256.0,
                        (uint32_t)(meter[0] | (meter[1] << 8) | (meter[2] << 16) | (meter[3] << 24)),
                        (uint32_t)(meter[4] | (meter[5] << 8) | (meter[6] << 16) | (meter[7] << 24)));
                fflush(log);
            }

            first = 0;
        }

        int ready = poll(&pfd, 1, 50);
        if (ready > 0 && (pfd.revents & POLLIN)) {
            char c;
            if (read(STDIN_FILENO, &c, 1) == 1) {
                if (c == 'q' || c == 'Q') {
                    printf("\nQuitting...\n");
                    break;
                }
                if (c == 'm' || c == 'M') {
                    printf("\n\033[1mMARK EVENT:\033[0m type description and press Enter: ");
                    fflush(stdout);
                    /* switch back to canonical briefly */
                    restore_terminal();
                    char line[256];
                    if (fgets(line, sizeof(line), stdin)) {
                        line[strcspn(line, "\n")] = '\0';
                        print_timestamp();
                        printf("\033[1m>>> EVENT: %s <<<\033[0m\n", line);
                        if (log) {
                            struct timespec ts;
                            clock_gettime(CLOCK_MONOTONIC, &ts);
                            fprintf(log, "%5ld.%03ld EVENT: %s\n", (long)ts.tv_sec, ts.tv_nsec / 1000000, line);
                            fflush(log);
                        }
                    }
                    set_terminal_raw();
                }
            }
        }
    }

    restore_terminal();
    if (log) fclose(log);

    /* Restore safe defaults */
    unsigned char cfg[16];
    if (cfg_read(h, cfg) == 16) {
        cfg[4] = 0;
        cfg[5] = 0;
        cfg[8] = (unsigned char)(-9);
        cfg[9] = 0;
        cfg[10] = cfg[11] = cfg[13] = 0;
        cfg[14] = 0;
        cfg[15] = 1;
        cfg_write(h, cfg);
    }

    libusb_release_interface(h, IFACE);
    libusb_close(h);
    libusb_exit(ctx);

    printf("Restarting wave3-daemon...\n");
    system("systemctl --user start wave3-daemon 2>/dev/null");

    printf("Log saved to %s\n", log_path);
    return 0;
}
