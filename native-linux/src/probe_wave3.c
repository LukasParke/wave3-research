#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <libusb-1.0/libusb.h>

#define VID 0x0fd9
#define PID 0x0070

static void print_dev_desc(libusb_device *dev) {
    struct libusb_device_descriptor desc;
    int r = libusb_get_device_descriptor(dev, &desc);
    if (r < 0) { fprintf(stderr, "desc error %d\n", r); return; }
    printf("VID=%04x PID=%04x bcdUSB=%04x bcdDevice=%04x\n", desc.idVendor, desc.idProduct, desc.bcdUSB, desc.bcdDevice);
    printf("  bDeviceClass=%02x bDeviceSubClass=%02x bDeviceProtocol=%02x\n", desc.bDeviceClass, desc.bDeviceSubClass, desc.bDeviceProtocol);
    printf("  iManufacturer=%d iProduct=%d iSerialNumber=%d\n", desc.iManufacturer, desc.iProduct, desc.iSerialNumber);
    printf("  bNumConfigurations=%d\n", desc.bNumConfigurations);
}

static void dump_string(libusb_device_handle *h, uint8_t idx) {
    if (idx == 0) return;
    unsigned char buf[256];
    int r = libusb_get_string_descriptor_ascii(h, idx, buf, sizeof(buf));
    if (r > 0) printf("  string[%d]: %s\n", idx, buf);
}

static void print_config_desc(libusb_device *dev, libusb_device_handle *h) {
    struct libusb_config_descriptor *cfg = NULL;
    int r = libusb_get_active_config_descriptor(dev, &cfg);
    if (r < 0) { fprintf(stderr, "config desc error %d\n", r); return; }
    printf("Configuration wTotalLength=%d bNumInterfaces=%d bConfigurationValue=%d\n", cfg->wTotalLength, cfg->bNumInterfaces, cfg->bConfigurationValue);
    for (int i = 0; i < cfg->bNumInterfaces; i++) {
        const struct libusb_interface *itf = &cfg->interface[i];
        for (int alt = 0; alt < itf->num_altsetting; alt++) {
            const struct libusb_interface_descriptor *id = &itf->altsetting[alt];
            printf("  Interface %d alt %d: bInterfaceClass=%02x bInterfaceSubClass=%02x bInterfaceProtocol=%02x bNumEndpoints=%d\n",
                   id->bInterfaceNumber, id->bAlternateSetting, id->bInterfaceClass, id->bInterfaceSubClass, id->bInterfaceProtocol, id->bNumEndpoints);
            for (int ep = 0; ep < id->bNumEndpoints; ep++) {
                const struct libusb_endpoint_descriptor *ed = &id->endpoint[ep];
                printf("    Endpoint 0x%02x: bmAttributes=%02x wMaxPacketSize=%d bInterval=%d\n",
                       ed->bEndpointAddress, ed->bmAttributes, ed->wMaxPacketSize, ed->bInterval);
            }
            if (id->iInterface) dump_string(h, id->iInterface);
        }
    }
    libusb_free_config_descriptor(cfg);
}

static void try_vendor_control(libusb_device_handle *h) {
    unsigned char buf[64];
    int r;
    memset(buf, 0, sizeof(buf));
    r = libusb_control_transfer(h, 0xC0, 0x00, 0x0000, 0x0000, buf, 2, 1000);
    printf("Vendor IN bRequest=0x00 interface0: r=%d data=%02x%02x\n", r, buf[0], buf[1]);

    memset(buf, 0, sizeof(buf));
    r = libusb_control_transfer(h, 0xC1, 0x00, 0x0000, 0x0003, buf, 2, 1000);
    printf("Vendor IN bRequest=0x00 interface3: r=%d data=%02x%02x\n", r, buf[0], buf[1]);
}

int main(int argc, char **argv) {
    libusb_context *ctx = NULL;
    int r = libusb_init(&ctx);
    if (r < 0) { fprintf(stderr, "libusb_init failed: %d\n", r); return 1; }
    libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_INFO);

    libusb_device **devs;
    ssize_t cnt = libusb_get_device_list(ctx, &devs);
    if (cnt < 0) { fprintf(stderr, "device list error\n"); libusb_exit(ctx); return 1; }

    libusb_device *target = NULL;
    for (ssize_t i = 0; i < cnt; i++) {
        struct libusb_device_descriptor desc;
        libusb_get_device_descriptor(devs[i], &desc);
        if (desc.idVendor == VID && desc.idProduct == PID) {
            target = devs[i];
            libusb_ref_device(target);
            break;
        }
    }

    if (!target) {
        fprintf(stderr, "Wave:3 (0fd9:0070) not found\n");
        libusb_free_device_list(devs, 1);
        libusb_exit(ctx);
        return 1;
    }

    printf("Found Wave:3 at bus %d address %d\n", libusb_get_bus_number(target), libusb_get_device_address(target));
    print_dev_desc(target);

    libusb_device_handle *h = NULL;
    r = libusb_open(target, &h);
    if (r < 0) { fprintf(stderr, "libusb_open failed: %d\n", r); goto cleanup; }

    dump_string(h, 1);
    dump_string(h, 2);
    dump_string(h, 3);
    print_config_desc(target, h);

    r = libusb_claim_interface(h, 3);
    printf("claim interface 3: %d (%s)\n", r, r == 0 ? "ok" : libusb_error_name(r));
    if (r == 0) {
        try_vendor_control(h);
        libusb_release_interface(h, 3);
    }

cleanup:
    if (h) libusb_close(h);
    libusb_unref_device(target);
    libusb_free_device_list(devs, 1);
    libusb_exit(ctx);
    return 0;
}
