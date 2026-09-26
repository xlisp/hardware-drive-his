/*
 * usb_probe.c - libusb 用户态 USB 驱动骨架：枚举、看描述符、发厂商控制请求
 *
 * USB 设备在 Linux/macOS/Windows 上都可以不写内核驱动，直接用 libusb 在用户态驱动：
 * QHY/ZWO 相机 SDK、fx3load（hezi-hack）、OpenOCD 的 ST-Link 驱动、rtl-sdr 都是这么做的。
 *
 *   usb_probe                         列出所有设备（迷你 lsusb）
 *   usb_probe 1618:c183               打印该设备的配置/接口/端点描述符
 *   usb_probe 1618:c183 ctl-in 0xA0 0x0000 0x0000 16
 *                                     发一个厂商 IN 控制请求（bRequest wValue wIndex wLength）并 hexdump
 *
 * 编译：cc -O2 -Wall -o usb_probe usb_probe.c $(pkg-config --cflags --libs libusb-1.0)
 * 权限：Linux 上默认只有 root 能打开设备，写 udev 规则：
 *   SUBSYSTEM=="usb", ATTRS{idVendor}=="1618", MODE="0666"
 *
 * 逆向时的用法：先用 Wireshark/usbmon 抓到厂商软件发的控制请求，再用这个工具逐条重放，
 * 观察设备反应 —— "抓包 → 重放 → 改参数 → 观察" 是 USB 协议逆向的基本循环。
 * 注意：随意发厂商请求可能改写设备固件/EEPROM，先只重放抓到的 IN（读）请求。
 */
#include <libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *ep_type[] = { "control", "isoc", "bulk", "interrupt" };

static void hexdump(const unsigned char *p, int n)
{
    for (int i = 0; i < n; i += 16) {
        printf("  %04x ", i);
        for (int j = 0; j < 16; j++)
            j + i < n ? printf(" %02x", p[i + j]) : printf("   ");
        printf("  ");
        for (int j = 0; j < 16 && i + j < n; j++)
            putchar(p[i + j] >= 32 && p[i + j] < 127 ? p[i + j] : '.');
        putchar('\n');
    }
}

static void list_devices(libusb_context *ctx)
{
    libusb_device **list;
    ssize_t n = libusb_get_device_list(ctx, &list);
    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor d;
        if (libusb_get_device_descriptor(list[i], &d)) continue;
        printf("Bus %03d Device %03d: ID %04x:%04x class %02x speed %d\n",
               libusb_get_bus_number(list[i]), libusb_get_device_address(list[i]),
               d.idVendor, d.idProduct, d.bDeviceClass, libusb_get_device_speed(list[i]));
    }
    libusb_free_device_list(list, 1);
}

static void describe(libusb_device_handle *h)
{
    libusb_device *dev = libusb_get_device(h);
    struct libusb_device_descriptor d;
    unsigned char s[256];

    libusb_get_device_descriptor(dev, &d);
    printf("bcdUSB %x.%02x  bMaxPacketSize0 %d  configs %d\n",
           d.bcdUSB >> 8, d.bcdUSB & 0xff, d.bMaxPacketSize0, d.bNumConfigurations);
    if (d.iManufacturer && libusb_get_string_descriptor_ascii(h, d.iManufacturer, s, sizeof s) > 0)
        printf("Manufacturer: %s\n", s);
    if (d.iProduct && libusb_get_string_descriptor_ascii(h, d.iProduct, s, sizeof s) > 0)
        printf("Product:      %s\n", s);
    if (d.iSerialNumber && libusb_get_string_descriptor_ascii(h, d.iSerialNumber, s, sizeof s) > 0)
        printf("Serial:       %s\n", s);

    struct libusb_config_descriptor *cfg;
    if (libusb_get_active_config_descriptor(dev, &cfg)) return;
    for (int i = 0; i < cfg->bNumInterfaces; i++) {
        for (int a = 0; a < cfg->interface[i].num_altsetting; a++) {
            const struct libusb_interface_descriptor *id = &cfg->interface[i].altsetting[a];
            printf("  Interface %d alt %d class %02x/%02x/%02x\n", id->bInterfaceNumber,
                   id->bAlternateSetting, id->bInterfaceClass, id->bInterfaceSubClass,
                   id->bInterfaceProtocol);
            for (int e = 0; e < id->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ep = &id->endpoint[e];
                printf("    EP 0x%02x %-3s %-9s maxpkt %d\n", ep->bEndpointAddress,
                       ep->bEndpointAddress & 0x80 ? "IN" : "OUT",
                       ep_type[ep->bmAttributes & 3], ep->wMaxPacketSize);
            }
        }
    }
    libusb_free_config_descriptor(cfg);
}

int main(int argc, char **argv)
{
    libusb_context *ctx;
    int rc = 1;
    if (libusb_init(&ctx)) return 1;

    if (argc < 2) {
        list_devices(ctx);
        libusb_exit(ctx);
        return 0;
    }

    unsigned vid, pid;
    if (sscanf(argv[1], "%x:%x", &vid, &pid) != 2) {
        fprintf(stderr, "usage: %s [VID:PID [ctl-in bRequest wValue wIndex wLength]]\n", argv[0]);
        goto out;
    }
    libusb_device_handle *h = libusb_open_device_with_vid_pid(ctx, (uint16_t)vid, (uint16_t)pid);
    if (!h) { fprintf(stderr, "cannot open %04x:%04x (not present or no permission)\n", vid, pid); goto out; }

    describe(h);

    if (argc == 7 && !strcmp(argv[2], "ctl-in")) {
        uint8_t req = (uint8_t)strtol(argv[3], NULL, 0);
        uint16_t val = (uint16_t)strtol(argv[4], NULL, 0);
        uint16_t idx = (uint16_t)strtol(argv[5], NULL, 0);
        uint16_t len = (uint16_t)strtol(argv[6], NULL, 0);
        unsigned char *buf = calloc(1, len ? len : 1);
        /* bmRequestType = 0xC0：设备到主机 | 厂商请求 | 接收者是设备 */
        int r = libusb_control_transfer(h, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR |
                                               LIBUSB_RECIPIENT_DEVICE,
                                        req, val, idx, buf, len, 1000);
        if (r < 0)
            printf("ctl-in 0x%02x: %s\n", req, libusb_error_name(r));  /* STALL = 设备不认识这个请求 */
        else {
            printf("ctl-in 0x%02x -> %d bytes\n", req, r);
            hexdump(buf, r);
        }
        free(buf);
    }
    libusb_close(h);
    rc = 0;
out:
    libusb_exit(ctx);
    return rc;
}
