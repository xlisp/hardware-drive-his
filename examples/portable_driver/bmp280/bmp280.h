/*
 * bmp280.h - 平台无关的 BMP280 温度/气压传感器驱动
 *
 * 驱动本身只认识"寄存器"，不认识 Arduino Wire / STM32 HAL / Linux i2c-dev。
 * 每个平台只需实现 struct bmp280_bus 里的三个回调（读、写、延时），
 * 同一份 bmp280.c 就能在三个平台上编译运行。这就是 Linux regmap、
 * Zephyr/RT-Thread 设备模型、Bosch 官方 BMP2-Sensor-API 共同的思路。
 */
#ifndef BMP280_H
#define BMP280_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BMP280_ADDR_SDO_LOW   0x76
#define BMP280_ADDR_SDO_HIGH  0x77

#define BMP280_REG_CALIB      0x88   /* 0x88..0x9F, 24 字节出厂校准 */
#define BMP280_REG_ID         0xD0
#define BMP280_REG_RESET      0xE0
#define BMP280_REG_STATUS     0xF3
#define BMP280_REG_CTRL_MEAS  0xF4
#define BMP280_REG_CONFIG     0xF5
#define BMP280_REG_DATA       0xF7   /* 0xF7..0xFC: press[3] temp[3] */

#define BMP280_CHIP_ID        0x58
#define BME280_CHIP_ID        0x60   /* BME280 兼容温压部分 */
#define BMP280_RESET_CMD      0xB6

/* 返回 0 表示成功，负数表示失败（平台自定义错误码） */
struct bmp280_bus {
    int  (*read)(void *ctx, uint8_t reg, uint8_t *buf, size_t len);
    int  (*write)(void *ctx, uint8_t reg, uint8_t val);
    void (*delay_ms)(void *ctx, uint32_t ms);
    void *ctx;                        /* 平台私有数据：I2C 句柄、fd、地址…… */
};

struct bmp280_calib {
    uint16_t T1; int16_t T2, T3;
    uint16_t P1; int16_t P2, P3, P4, P5, P6, P7, P8, P9;
};

struct bmp280 {
    struct bmp280_bus bus;
    struct bmp280_calib cal;
    uint8_t chip_id;
    int32_t t_fine;                  /* 温度补偿的中间量，气压补偿要用 */
};

enum bmp280_err {
    BMP280_OK = 0,
    BMP280_E_BUS = -1,
    BMP280_E_ID = -2,
    BMP280_E_TIMEOUT = -3,
};

int bmp280_init(struct bmp280 *dev);
/* 强制模式测一次：temp_centi = 0.01°C，press_q8 = Pa * 256 */
int bmp280_measure(struct bmp280 *dev, int32_t *temp_centi, uint32_t *press_q8);

/* 纯计算函数，单独导出便于在主机上做单元测试 */
int32_t  bmp280_compensate_t(struct bmp280 *dev, int32_t adc_t);
uint32_t bmp280_compensate_p(const struct bmp280 *dev, int32_t adc_p);

#ifdef __cplusplus
}
#endif
#endif
