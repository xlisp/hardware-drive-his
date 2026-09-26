/*
 * bmp280_stm32_hal.c - STM32 HAL 端口：把 bmp280_bus 接到 HAL_I2C_Mem_Read/Write
 *
 * 用法（STM32CubeIDE / CubeMX 生成的工程）：
 *   1. CubeMX 里打开 I2C1（Standard/Fast mode），USART2 做 printf 输出
 *   2. 把本文件和 portable_driver/bmp280/bmp280.{c,h} 加到工程 Core/Src、Core/Inc
 *   3. 在 main.c 的 USER CODE BEGIN 2 里调用 bmp280_demo_run(&hi2c1)
 *
 * 和 Arduino / Linux 端口对比：驱动核心 bmp280.c 一行不改，差别只在这 3 个回调。
 * HAL 的 "Mem" 接口已经帮你做了"写寄存器地址 + repeated START + 读"。
 */
#include <stdio.h>
#include "main.h"          /* CubeMX 生成，里面 include 了 stm32xxxx_hal.h */
#include "bmp280.h"

struct stm32_i2c {
    I2C_HandleTypeDef *hi2c;
    uint16_t addr8;        /* HAL 要的是左移一位后的 8 位地址 */
};

static int hal_read(void *ctx, uint8_t reg, uint8_t *buf, size_t len)
{
    struct stm32_i2c *c = ctx;
    return HAL_I2C_Mem_Read(c->hi2c, c->addr8, reg, I2C_MEMADD_SIZE_8BIT,
                            buf, (uint16_t)len, 100) == HAL_OK ? 0 : -1;
}

static int hal_write(void *ctx, uint8_t reg, uint8_t val)
{
    struct stm32_i2c *c = ctx;
    return HAL_I2C_Mem_Write(c->hi2c, c->addr8, reg, I2C_MEMADD_SIZE_8BIT,
                             &val, 1, 100) == HAL_OK ? 0 : -1;
}

static void hal_delay(void *ctx, uint32_t ms) { (void)ctx; HAL_Delay(ms); }

void bmp280_demo_run(I2C_HandleTypeDef *hi2c)
{
    static struct stm32_i2c c;
    static struct bmp280 dev;

    c.hi2c = hi2c;
    c.addr8 = BMP280_ADDR_SDO_LOW << 1;
    dev.bus = (struct bmp280_bus){ hal_read, hal_write, hal_delay, &c };

    /* 先用 HAL 自带的探测确认 ACK，排查接线比看驱动错误码直观 */
    if (HAL_I2C_IsDeviceReady(hi2c, c.addr8, 3, 100) != HAL_OK) {
        printf("no ACK at 0x%02x, check wiring / pull-ups\r\n", BMP280_ADDR_SDO_LOW);
        return;
    }
    int r = bmp280_init(&dev);
    if (r != BMP280_OK) {
        printf("bmp280_init failed: %d\r\n", r);
        return;
    }
    for (;;) {
        int32_t t; uint32_t p;
        if (bmp280_measure(&dev, &t, &p) == BMP280_OK) {
            /* newlib-nano 默认不支持 %f，用整数打印；负温度单独处理符号 */
            long a = t < 0 ? -(long)t : t;
            printf("%s%ld.%02ld C  %lu Pa\r\n", t < 0 ? "-" : "", a / 100, a % 100,
                   (unsigned long)(p / 256));
        }
        HAL_Delay(1000);
    }
}

/*
 * printf 重定向到串口（GCC/newlib）：CubeIDE 工程里加上这个函数，printf 就会走 USART2。
 *
 * extern UART_HandleTypeDef huart2;
 * int _write(int fd, char *ptr, int len)
 * {
 *     (void)fd;
 *     HAL_UART_Transmit(&huart2, (uint8_t *)ptr, len, HAL_MAX_DELAY);
 *     return len;
 * }
 */
