/*
 * test_host.c - 在电脑上用"假总线"测试 bmp280.c，不需要任何硬件
 *
 *   cc -Wall -Wextra -o test_host test_host.c bmp280.c && ./test_host
 *
 * 假设备用数据手册 §3.12 的示例校准值和 ADC 值，期望结果：
 *   T = 25.08 °C，P = 100653.27 Pa
 * 这也是"驱动先在主机上跑通、再上板"的常见做法（Linux 内核里对应 KUnit）。
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "bmp280.h"

struct fake {
    uint8_t regs[256];
    int measure_polls;           /* 模拟转换需要几次轮询才完成 */
};

static int fake_read(void *ctx, uint8_t reg, uint8_t *buf, size_t len)
{
    struct fake *f = ctx;
    if (reg == BMP280_REG_STATUS && f->measure_polls > 0) {
        f->measure_polls--;
        buf[0] = 0x08;           /* 还在测量 */
        return 0;
    }
    memcpy(buf, f->regs + reg, len);
    return 0;
}

static int fake_write(void *ctx, uint8_t reg, uint8_t val)
{
    struct fake *f = ctx;
    f->regs[reg] = val;
    if (reg == BMP280_REG_CTRL_MEAS && (val & 3) == 1)
        f->measure_polls = 2;
    return 0;
}

static void fake_delay(void *ctx, uint32_t ms) { (void)ctx; (void)ms; }

static void put16(uint8_t *p, int v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }

int main(void)
{
    struct fake f = {0};
    const int cal[12] = {27504, 26435, -1000, 36477, -10685, 3024,
                         2855, 140, -7, 15500, -14600, 6000};
    for (int i = 0; i < 12; i++) put16(f.regs + BMP280_REG_CALIB + 2 * i, cal[i]);
    f.regs[BMP280_REG_ID] = BMP280_CHIP_ID;

    const int32_t adc_p = 415148, adc_t = 519888;
    uint8_t *d = f.regs + BMP280_REG_DATA;
    d[0] = adc_p >> 12; d[1] = (adc_p >> 4) & 0xff; d[2] = (adc_p & 0xf) << 4;
    d[3] = adc_t >> 12; d[4] = (adc_t >> 4) & 0xff; d[5] = (adc_t & 0xf) << 4;

    struct bmp280 dev = { .bus = { fake_read, fake_write, fake_delay, &f } };
    assert(bmp280_init(&dev) == BMP280_OK);

    int32_t t; uint32_t p;
    assert(bmp280_measure(&dev, &t, &p) == BMP280_OK);
    printf("T = %d.%02d C, P = %u.%02u Pa\n", t / 100, t % 100, p / 256, (p % 256) * 100 / 256);
    assert(t == 2508);
    assert(p / 256 == 100653);

    f.regs[BMP280_REG_ID] = 0x00;            /* 接错芯片要能识别出来 */
    assert(bmp280_init(&dev) == BMP280_E_ID);

    puts("all tests passed");
    return 0;
}
