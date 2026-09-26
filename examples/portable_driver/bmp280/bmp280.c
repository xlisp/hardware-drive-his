/*
 * bmp280.c - 平台无关的 BMP280 驱动实现（补偿公式来自 Bosch BST-BMP280-DS001 §3.11.3）
 */
#include "bmp280.h"

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

int bmp280_init(struct bmp280 *dev)
{
    uint8_t c[24];

    if (dev->bus.read(dev->bus.ctx, BMP280_REG_ID, &dev->chip_id, 1))
        return BMP280_E_BUS;
    if (dev->chip_id != BMP280_CHIP_ID && dev->chip_id != BME280_CHIP_ID)
        return BMP280_E_ID;

    /* 软复位，然后等 NVM 校准数据拷贝完成（status.im_update 清零） */
    if (dev->bus.write(dev->bus.ctx, BMP280_REG_RESET, BMP280_RESET_CMD))
        return BMP280_E_BUS;
    for (int i = 0;; i++) {
        uint8_t st;
        dev->bus.delay_ms(dev->bus.ctx, 2);
        if (dev->bus.read(dev->bus.ctx, BMP280_REG_STATUS, &st, 1))
            return BMP280_E_BUS;
        if (!(st & 0x01)) break;
        if (i > 50) return BMP280_E_TIMEOUT;
    }

    if (dev->bus.read(dev->bus.ctx, BMP280_REG_CALIB, c, sizeof c))
        return BMP280_E_BUS;
    dev->cal.T1 = le16(c + 0);
    dev->cal.T2 = (int16_t)le16(c + 2);
    dev->cal.T3 = (int16_t)le16(c + 4);
    dev->cal.P1 = le16(c + 6);
    dev->cal.P2 = (int16_t)le16(c + 8);
    dev->cal.P3 = (int16_t)le16(c + 10);
    dev->cal.P4 = (int16_t)le16(c + 12);
    dev->cal.P5 = (int16_t)le16(c + 14);
    dev->cal.P6 = (int16_t)le16(c + 16);
    dev->cal.P7 = (int16_t)le16(c + 18);
    dev->cal.P8 = (int16_t)le16(c + 20);
    dev->cal.P9 = (int16_t)le16(c + 22);

    /* config: 滤波 x4，其余默认 */
    return dev->bus.write(dev->bus.ctx, BMP280_REG_CONFIG, 2 << 2) ? BMP280_E_BUS : BMP280_OK;
}

int32_t bmp280_compensate_t(struct bmp280 *dev, int32_t adc_t)
{
    const struct bmp280_calib *c = &dev->cal;
    int32_t var1 = ((((adc_t >> 3) - ((int32_t)c->T1 << 1))) * (int32_t)c->T2) >> 11;
    int32_t var2 = (((((adc_t >> 4) - (int32_t)c->T1) * ((adc_t >> 4) - (int32_t)c->T1)) >> 12) *
                    (int32_t)c->T3) >> 14;
    dev->t_fine = var1 + var2;
    return (dev->t_fine * 5 + 128) >> 8;
}

uint32_t bmp280_compensate_p(const struct bmp280 *dev, int32_t adc_p)
{
    const struct bmp280_calib *c = &dev->cal;
    int64_t var1 = (int64_t)dev->t_fine - 128000;
    int64_t var2 = var1 * var1 * c->P6;
    var2 += (var1 * c->P5) * ((int64_t)1 << 17);
    var2 += (int64_t)c->P4 * ((int64_t)1 << 35);
    var1 = ((var1 * var1 * c->P3) >> 8) + (var1 * c->P2) * ((int64_t)1 << 12);
    var1 = ((((int64_t)1 << 47) + var1) * c->P1) >> 33;
    if (var1 == 0) return 0;                 /* 避免除零 */
    int64_t p = 1048576 - adc_p;
    p = ((p * ((int64_t)1 << 31)) - var2) * 3125 / var1;
    var1 = ((int64_t)c->P9 * (p >> 13) * (p >> 13)) >> 25;
    var2 = ((int64_t)c->P8 * p) >> 19;
    p = ((p + var1 + var2) >> 8) + ((int64_t)c->P7 << 4);
    return (uint32_t)p;
}

int bmp280_measure(struct bmp280 *dev, int32_t *temp_centi, uint32_t *press_q8)
{
    uint8_t d[6];

    /* ctrl_meas: osrs_t=x2, osrs_p=x16, mode=forced(01) —— 测一次后自动回 sleep */
    if (dev->bus.write(dev->bus.ctx, BMP280_REG_CTRL_MEAS, (2 << 5) | (5 << 2) | 1))
        return BMP280_E_BUS;
    for (int i = 0;; i++) {
        uint8_t st;
        dev->bus.delay_ms(dev->bus.ctx, 10);
        if (dev->bus.read(dev->bus.ctx, BMP280_REG_STATUS, &st, 1))
            return BMP280_E_BUS;
        if (!(st & 0x08)) break;             /* measuring 位清零 = 完成 */
        if (i > 20) return BMP280_E_TIMEOUT;
    }

    /* 一次突发读 6 字节，保证温度和气压来自同一次转换 */
    if (dev->bus.read(dev->bus.ctx, BMP280_REG_DATA, d, sizeof d))
        return BMP280_E_BUS;
    int32_t adc_p = (int32_t)(((uint32_t)d[0] << 12) | ((uint32_t)d[1] << 4) | (d[2] >> 4));
    int32_t adc_t = (int32_t)(((uint32_t)d[3] << 12) | ((uint32_t)d[4] << 4) | (d[5] >> 4));

    *temp_centi = bmp280_compensate_t(dev, adc_t);   /* 必须先算温度（得到 t_fine） */
    *press_q8 = bmp280_compensate_p(dev, adc_p);
    return BMP280_OK;
}
