/*
 * vendor.c - 扮演"闭源厂商库" libvendor.so（真实场景：libASICamera2.so、libEFWFilter.so）
 *
 * 我们假装拿不到源码，只有 .so 和一个不完整的头文件。
 */
#include <stdio.h>
#include <string.h>

static const char *names[] = { "VendorCam A", "VendorCam B" };

int dev_count(void) { return 2; }
int dev_get_vid(int i) { (void)i; return 0x1234; }
const char *dev_name(int i) { return i >= 0 && i < 2 ? names[i] : NULL; }

int dev_capture(int i, unsigned char *buf, int len)
{
    /* 库内部又调用了自己导出的 dev_count()/dev_name()。ELF 默认允许符号插入：如果原库不是用
     * RTLD_DEEPBIND 加载的，这两个调用会先找到 shim 里的同名函数 —— 而 shim 的下标编号和
     * 原库不同，于是原库拿到的是别的设备的信息。 */
    if (i < 0 || i >= dev_count()) {
        fprintf(stderr, "[vendor] dev_capture: bad index %d\n", i);
        return -1;
    }
    fprintf(stderr, "[vendor] capturing on %s\n", dev_name(i));
    memset(buf, 0x10 + i, len);
    return len;
}

/* 公开头文件里没有的内部函数：逆向时只知道符号名，不知道参数个数和类型 */
int vendor_secret(int a, int b, int c, int d, int e, int f, int g, int h)
{
    return a + b + c + d + e + f + g + h;   /* 8 个参数：x86-64 上有 2 个走栈，ARM 上有 4 个走栈 */
}
