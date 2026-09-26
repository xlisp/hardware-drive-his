/*
 * 01_blink_register.ino - 同一件事的三层写法：Arduino API / AVR 寄存器 / 数据手册
 *
 * 目标板：Arduino Uno（ATmega328P，16 MHz）。D13 = PB5（板载 LED）。
 *
 * 串口会打印三种方式翻转 1000 次引脚所用的时间，直观感受"抽象层的代价"：
 *   digitalWrite()      每次要查表 pin→port/bit、检查 PWM 定时器、关中断……约 3~5 µs
 *   PORTB ^= _BV(PB5)   IN/EOR/OUT 三条指令（PORTB |= 常量 则只是一条 SBI）
 *   PINB = _BV(PB5)     ATmega328P 的特殊功能：往 PINx 写 1 会翻转对应输出位，一条 OUT
 * （测得的时间都包含函数指针调用开销，看相对差距即可）
 *
 * 寄存器含义（ATmega328P 数据手册 §14 I/O-Ports）：
 *   DDRB  方向寄存器，1 = 输出
 *   PORTB 输出寄存器
 *   PINB  输入寄存器（读），写 1 = 翻转
 */
#include <avr/io.h>

static void bench(const __FlashStringHelper *name, void (*fn)())
{
    uint32_t t0 = micros();
    for (int i = 0; i < 1000; i++) fn();
    uint32_t dt = micros() - t0;
    Serial.print(name);
    Serial.print(F(": "));
    Serial.print(dt);
    Serial.println(F(" us / 1000 toggles"));
}

static void toggle_arduino() { digitalWrite(13, !digitalRead(13)); }
static void toggle_port()    { PORTB ^= _BV(PB5); }
static void toggle_pin()     { PINB = _BV(PB5); }

void setup()
{
    Serial.begin(115200);
    DDRB |= _BV(DDB5);                // 等价于 pinMode(13, OUTPUT)

    bench(F("digitalWrite"), toggle_arduino);
    bench(F("PORTB ^=    "), toggle_port);
    bench(F("PINB =      "), toggle_pin);
}

void loop()
{
    PINB = _BV(PB5);                  // 翻转 LED
    delay(500);
}
