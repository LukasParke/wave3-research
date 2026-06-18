/* Restore safe defaults after probing. */
#include <stdio.h>
#include <libusb-1.0/libusb.h>

#define VID 0x0fd9
#define PID 0x0070
#define IFACE 3

#define SET_CUR 0x01
#define BM_OUT  0x21
#define MUTE    0x01
#define VOLUME  0x02

#define HP_FU  5
#define MIC_FU 6

static int set_mute(libusb_device_handle *h, int entity, int val)
{
    uint16_t wValue  = (MUTE << 8);
    uint16_t wIndex3 = (entity << 8) | IFACE;
    unsigned char buf[1] = { val };
    return libusb_control_transfer(h, BM_OUT, SET_CUR, wValue, wIndex3, buf, 1, 1000);
}

static int set_hp_vol(libusb_device_handle *h, int db_x256)
{
    uint16_t wValue  = (VOLUME << 8);
    uint16_t wIndex3 = (HP_FU << 8) | IFACE;
    unsigned char buf[2] = { db_x256 & 0xff, (db_x256 >> 8) & 0xff };
    return libusb_control_transfer(h, BM_OUT, SET_CUR, wValue, wIndex3, buf, 2, 1000);
}

int main(void)
{
    libusb_context *ctx = NULL;
    libusb_device_handle *h = NULL;
    libusb_init(&ctx);
    h = libusb_open_device_with_vid_pid(ctx, VID, PID);
    if (!h) { fprintf(stderr, "device not found\n"); return 1; }
    libusb_claim_interface(h, IFACE);

    /* Unmute mic and headphone */
    set_mute(h, MIC_FU, 0);
    set_mute(h, HP_FU, 0);
    /* Restore a comfortable headphone level (~ -9 dB) */
    set_hp_vol(h, -2304);

    printf("Restored: mic unmuted, hp unmuted, hp volume ~-9 dB\n");

    libusb_release_interface(h, IFACE);
    libusb_close(h);
    libusb_exit(ctx);
    return 0;
}
