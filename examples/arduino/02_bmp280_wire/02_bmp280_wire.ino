/*
 * 02_bmp280_wire.ino - Arduino 端口：把 bmp280_bus 接到 Wire（TWI 外设）
 *
 * 准备：把 examples/portable_driver/bmp280/bmp280.{c,h} 复制（或软链接）到本目录下的
 *       src/ 子目录（Arduino 构建系统会编译 src/ 里的文件）：
 *   mkdir -p src && cp ../../portable_driver/bmp280/bmp280.[ch] src/
 *
 * 接线（Uno）：A4=SDA  A5=SCL  3.3V  GND；SDO 接地 → 地址 0x76
 * 注意：BMP280 模块是 3.3V 器件，5V Uno 请用带电平转换的模块。
 */
#include <Wire.h>
#include "src/bmp280.h"

static const uint8_t ADDR = BMP280_ADDR_SDO_LOW;

static int wire_read(void *ctx, uint8_t reg, uint8_t *buf, size_t len)
{
    (void)ctx;
    Wire.beginTransmission(ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0)       // false = 发 repeated START，不释放总线
        return -1;
    if (Wire.requestFrom(ADDR, (uint8_t)len) != len)
        return -1;
    for (size_t i = 0; i < len; i++) buf[i] = Wire.read();
    return 0;
}

static int wire_write(void *ctx, uint8_t reg, uint8_t val)
{
    (void)ctx;
    Wire.beginTransmission(ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0 ? 0 : -1;
}

static void wire_delay(void *ctx, uint32_t ms) { (void)ctx; delay(ms); }

static struct bmp280 dev;

void setup()
{
    Serial.begin(115200);
    Wire.begin();
    Wire.setClock(400000);                      // Fast-mode 400 kHz
    dev.bus = { wire_read, wire_write, wire_delay, nullptr };

    int r = bmp280_init(&dev);
    if (r != BMP280_OK) {
        Serial.print(F("bmp280_init failed: "));
        Serial.println(r);
        while (true) delay(1000);
    }
    Serial.print(F("chip id 0x"));
    Serial.println(dev.chip_id, HEX);
}

void loop()
{
    int32_t t;
    uint32_t p;
    if (bmp280_measure(&dev, &t, &p) == BMP280_OK) {
        Serial.print(t / 100.0);
        Serial.print(F(" C  "));
        Serial.print(p / 25600.0);              // Q24.8 Pa → hPa
        Serial.println(F(" hPa"));
    }
    delay(1000);
}
