/*
 * usbspy.c - LD_PRELOAD 钩子：记录闭源程序/SDK 通过 libusb 发出的每一个 USB 传输
 *
 * 场景：厂商只给了 libqhyccd.so / libASICamera2.so 这类二进制 SDK，想知道它对相机
 * 发了哪些厂商请求（曝光、增益、读温度……），以便自己写驱动或做兼容层。
 *
 * 和 usbmon/Wireshark 抓包相比：
 *   + 能看到"是哪个函数、哪个线程"发的，能和 SDK API 调用对上号
 *   + 不需要 root、不需要 debugfs
 *   - 只能抓到经过 libusb 动态符号的调用（静态链接 libusb、或用 RTLD_DEEPBIND 加载的库抓不到，
 *     那时就回到 usbmon：tools/usbmon_parse.py）
 *
 *   make
 *   USBSPY_LOG=/tmp/usb.log LD_PRELOAD=$PWD/usbspy.so ./some_vendor_tool
 *   USBSPY_MAX=64 ...        每个包最多打印多少字节（默认 32）
 *
 * 原理：LD_PRELOAD 的库排在全局符号搜索顺序的最前面，程序里对 libusb_control_transfer
 * 的调用先解析到这里；我们用 dlsym(RTLD_NEXT, ...) 找到真正的 libusb 函数再转发。
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <libusb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static FILE *out;
static int max_dump = 32;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static int (*real_ctrl)(libusb_device_handle *, uint8_t, uint8_t, uint16_t, uint16_t,
                        unsigned char *, uint16_t, unsigned int);
static int (*real_bulk)(libusb_device_handle *, unsigned char, unsigned char *, int, int *,
                        unsigned int);

__attribute__((constructor)) static void spy_init(void)
{
    const char *path = getenv("USBSPY_LOG");
    out = path ? fopen(path, "a") : NULL;
    if (!out) out = stderr;
    if (getenv("USBSPY_MAX")) max_dump = atoi(getenv("USBSPY_MAX"));
    real_ctrl = dlsym(RTLD_NEXT, "libusb_control_transfer");
    real_bulk = dlsym(RTLD_NEXT, "libusb_bulk_transfer");
    fprintf(out, "# usbspy loaded in pid %d\n", getpid());
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void dump(const unsigned char *p, int n)
{
    for (int i = 0; i < n && i < max_dump; i++) fprintf(out, " %02x", p[i]);
    if (n > max_dump) fprintf(out, " ...(+%d)", n - max_dump);
}

int libusb_control_transfer(libusb_device_handle *h, uint8_t type, uint8_t req, uint16_t val,
                            uint16_t idx, unsigned char *data, uint16_t len, unsigned int timeout)
{
    if (!real_ctrl) return LIBUSB_ERROR_NOT_SUPPORTED;
    /* 调用者地址 → 哪个 .so 发的请求（SDK 里还是 libusb 自己） */
    Dl_info di = {0};
    dladdr(__builtin_return_address(0), &di);

    int r = real_ctrl(h, type, req, val, idx, data, len, timeout);

    pthread_mutex_lock(&lock);
    fprintf(out, "%.6f tid %ld CTRL %s type=%02x req=%02x val=%04x idx=%04x len=%-4u -> %d  [%s]",
            now(), (long)syscall(SYS_gettid), type & 0x80 ? "IN " : "OUT", type, req, val, idx, len,
            r, di.dli_sname ? di.dli_sname : (di.dli_fname ? di.dli_fname : "?"));
    if (r > 0 && data) {                     /* OUT 看发出去的数据，IN 看收回来的数据 */
        fputs("\n    data:", out);
        dump(data, r);
    }
    fputc('\n', out);
    fflush(out);
    pthread_mutex_unlock(&lock);
    return r;
}

int libusb_bulk_transfer(libusb_device_handle *h, unsigned char ep, unsigned char *data, int len,
                         int *transferred, unsigned int timeout)
{
    if (!real_bulk) return LIBUSB_ERROR_NOT_SUPPORTED;
    double t0 = now();
    int r = real_bulk(h, ep, data, len, transferred, timeout);
    int n = transferred ? *transferred : 0;

    pthread_mutex_lock(&lock);
    /* 图像数据动辄几十 MB，只打印头部；时间和速率能看出 SDK 的读出分块策略 */
    fprintf(out, "%.6f tid %ld BULK ep=%02x len=%d got=%d -> %d  %.1f ms", t0,
            (long)syscall(SYS_gettid), ep, len, n, r, (now() - t0) * 1e3);
    if (n > 0 && !(ep & 0x80)) {
        fputs("\n    data:", out);
        dump(data, n);
    }
    fputc('\n', out);
    fflush(out);
    pthread_mutex_unlock(&lock);
    return r;
}
