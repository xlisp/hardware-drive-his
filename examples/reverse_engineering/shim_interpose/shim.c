/*
 * shim.c - 同名替换库：新的 libvendor.so，原库改名为 libvendor_orig.so
 *
 * 和 hezi-hack/shim/asi_qhy_shim.cpp 同一套技术，缩小到 100 行：
 *   1. 导出符号与原库完全一致（nm -D --defined-only 两边 diff 为空）
 *   2. 原库用 dlopen(RTLD_LOCAL | RTLD_DEEPBIND) 加载：它内部调用自己的函数时不会绕回 shim
 *   3. 在原厂设备列表后面追加一台"外来设备"，自己实现它的全部行为
 *   4. dev_get_vid 只对"应用本体"撒谎（dladdr1 判断调用者），库之间的调用看到的仍是真实 VID
 *   5. 原型未知的 vendor_secret 用汇编跳板原样转发（trampolines.S），不用知道参数
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAKE_VID  0x1234
#define REAL_VID  0xABCD      /* 外来设备的真实 VID */

static int (*o_count)(void);
static int (*o_vid)(int);
static const char *(*o_name)(int);
static int (*o_capture)(int, unsigned char *, int);
__attribute__((visibility("hidden"))) void *fwd_vendor_secret;   /* 汇编跳板读这个指针 */

__attribute__((constructor)) static void shim_init(void)
{
    const char *path = getenv("SHIM_ORIG") ? getenv("SHIM_ORIG") : "libvendor_orig.so";
    int flags = RTLD_NOW | RTLD_LOCAL | (getenv("SHIM_NO_DEEPBIND") ? 0 : RTLD_DEEPBIND);
    void *h = dlopen(path, flags);
    if (!h) {
        fprintf(stderr, "[shim] %s: %s -> foreign device only\n", path, dlerror());
        return;
    }
    o_count = (int (*)(void))dlsym(h, "dev_count");
    o_vid = (int (*)(int))dlsym(h, "dev_get_vid");
    o_name = (const char *(*)(int))dlsym(h, "dev_name");
    o_capture = (int (*)(int, unsigned char *, int))dlsym(h, "dev_capture");
    fwd_vendor_secret = dlsym(h, "vendor_secret");
    fprintf(stderr, "[shim] loaded %s%s\n", path, flags & RTLD_DEEPBIND ? " (DEEPBIND)" : "");
}

static int n_orig(void) { return o_count ? o_count() : 0; }
static int is_foreign(int i) { return i == n_orig(); }

/* 返回地址所在的模块是不是主程序？比较 link_map：主程序的 link_map 就是 dlopen(NULL) 的句柄 */
static int called_from_main_exe(void *ret_addr)
{
    Dl_info info;
    struct link_map *caller = NULL, *main_map = dlopen(NULL, RTLD_NOW);
    int yes = dladdr1(ret_addr, &info, (void **)&caller, RTLD_DL_LINKMAP) && caller == main_map;
    dlclose(main_map);
    return yes;
}

int dev_count(void) { return n_orig() + 1; }

int dev_get_vid(int i)
{
    if (!is_foreign(i)) return o_vid(i);
    if (called_from_main_exe(__builtin_return_address(0))) {
        fprintf(stderr, "[shim] reporting foreign device #%d as VID %04x to the app\n", i, FAKE_VID);
        return FAKE_VID;
    }
    return REAL_VID;
}

const char *dev_name(int i) { return is_foreign(i) ? "OtherBrand X1" : o_name(i); }

int dev_capture(int i, unsigned char *buf, int len)
{
    if (!is_foreign(i)) return o_capture(i, buf, len);
    memset(buf, 0xEE, len);                /* 这里才是真正的"外来设备驱动"：调用另一家的 SDK */
    return len;
}
