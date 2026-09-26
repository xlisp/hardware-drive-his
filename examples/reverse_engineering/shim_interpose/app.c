/*
 * app.c - 扮演"闭源应用"（真实场景：zwoair_imager）。它只认 VID 0x1234 的设备。
 */
#include <stdio.h>

int dev_count(void);
int dev_get_vid(int i);
const char *dev_name(int i);
int dev_capture(int i, unsigned char *buf, int len);
int vendor_secret(int, int, int, int, int, int, int, int);

int main(void)
{
    unsigned char frame[4];
    int n = dev_count();
    printf("[app] %d device(s)\n", n);
    for (int i = 0; i < n; i++) {
        int vid = dev_get_vid(i);
        if (vid != 0x1234) {                       /* 厂商锁：和 imager 的 VID==0x03c3 检查一样 */
            printf("[app] skip #%d %s: VID %04x is not ours\n", i, dev_name(i), vid);
            continue;
        }
        int r = dev_capture(i, frame, sizeof frame);
        printf("[app] #%d %-14s capture=%d first byte 0x%02x\n", i, dev_name(i), r, r > 0 ? frame[0] : 0);
    }
    printf("[app] vendor_secret(1..8) = %d\n", vendor_secret(1, 2, 3, 4, 5, 6, 7, 8));
    return 0;
}
