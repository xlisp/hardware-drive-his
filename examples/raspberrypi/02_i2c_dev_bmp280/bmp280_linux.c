/*
 * bmp280_linux.c - Linux 用户态端口：通过 /dev/i2c-N（i2c-dev）驱动 BMP280
 *
 * 用户态驱动：内核只提供通用的 I2C 适配器驱动（i2c-bcm2835 / i2c-designware），
 * 芯片协议全部在用户态完成。优点是开发快、崩了不会拖垮内核；
 * 缺点是没有中断、别的程序看不到这个设备（不像 hwmon/IIO 那样有统一接口）。
 *
 * 启用 I2C：sudo raspi-config → Interface Options → I2C（或 config.txt 里 dtparam=i2c_arm=on）
 * 编译：    make
 * 运行：    ./bmp280_linux [/dev/i2c-1] [0x76]
 * 先确认设备在线：i2cdetect -y 1
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include "bmp280.h"

struct linux_i2c {
    int fd;
    uint16_t addr;
};

/* I2C_RDWR：一次 ioctl 里发"写寄存器地址 + repeated START + 读"两条消息，
 * 和 Arduino 的 endTransmission(false) + requestFrom() 在总线上是同一个波形。 */
static int li_read(void *ctx, uint8_t reg, uint8_t *buf, size_t len)
{
    struct linux_i2c *c = ctx;
    struct i2c_msg msgs[2] = {
        { .addr = c->addr, .flags = 0,        .len = 1,             .buf = &reg },
        { .addr = c->addr, .flags = I2C_M_RD, .len = (uint16_t)len, .buf = buf  },
    };
    struct i2c_rdwr_ioctl_data x = { .msgs = msgs, .nmsgs = 2 };
    return ioctl(c->fd, I2C_RDWR, &x) == 2 ? 0 : -errno;
}

static int li_write(void *ctx, uint8_t reg, uint8_t val)
{
    struct linux_i2c *c = ctx;
    uint8_t b[2] = { reg, val };
    struct i2c_msg m = { .addr = c->addr, .flags = 0, .len = 2, .buf = b };
    struct i2c_rdwr_ioctl_data x = { .msgs = &m, .nmsgs = 1 };
    return ioctl(c->fd, I2C_RDWR, &x) == 1 ? 0 : -errno;
}

static void li_delay(void *ctx, uint32_t ms)
{
    (void)ctx;
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/dev/i2c-1";
    struct linux_i2c c = { .addr = argc > 2 ? (uint16_t)strtol(argv[2], NULL, 0) : BMP280_ADDR_SDO_LOW };

    c.fd = open(path, O_RDWR);
    if (c.fd < 0) { perror(path); return 1; }

    struct bmp280 dev = { .bus = { li_read, li_write, li_delay, &c } };
    int r = bmp280_init(&dev);
    if (r) {
        fprintf(stderr, "bmp280_init: %d (%s)\n", r,
                r == BMP280_E_ID ? "wrong chip id" : "bus error - check i2cdetect");
        return 1;
    }
    printf("chip id 0x%02x at %s 0x%02x\n", dev.chip_id, path, c.addr);

    for (;;) {
        int32_t t; uint32_t p;
        if (bmp280_measure(&dev, &t, &p) == 0)
            printf("%.2f C  %.2f hPa\n", t / 100.0, p / 25600.0);
        sleep(1);
    }
}
