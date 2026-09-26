/*
 * gpio_mmap.c - 直接 mmap GPIO 寄存器（WiringPi / pigpio / bcm2835 库的做法）
 *
 * 仅适用于 Pi 0～4（BCM2835/2836/2837/2711）。Pi 5 的 GPIO 在 RP1 上，寄存器布局完全不同，
 * 这就是 Pi 5 发布时大量老库失效的原因 —— 它们绕过了内核驱动，直接依赖硬件细节。
 *
 * 为什么要演示它：
 *   1. 这是最直观的"驱动 = 按数据手册读写寄存器"，和 STM32 裸机、AVR 寄存器是同一回事；
 *   2. 翻转速度能到几十 MHz 级别，比 libgpiod 的 ioctl 快两个数量级；
 *   3. 它说明了为什么"绕过内核"是双刃剑：没有占用检查、没有权限模型、换硬件就挂。
 *
 * /dev/gpiomem 是树莓派内核提供的"只映射 GPIO 那一页"的设备，普通用户（gpio 组）即可访问，
 * 不需要像 /dev/mem 那样 root 并暴露全部物理内存。
 *
 * 寄存器（BCM2835 ARM Peripherals §6.1）：
 *   GPFSELn  0x00 + 4n   每脚 3 bit 功能选择，000=输入 001=输出
 *   GPSET0   0x1C        写 1 置高
 *   GPCLR0   0x28        写 1 置低
 *   GPLEV0   0x34        读电平
 *
 * 编译：gcc -O2 -Wall -o gpio_mmap gpio_mmap.c     运行：./gpio_mmap [17]
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define GPFSEL(n)  ((n) / 10)            /* 以 32 位字为单位的偏移 */
#define GPSET0     (0x1C / 4)
#define GPCLR0     (0x28 / 4)
#define GPLEV0     (0x34 / 4)

int main(int argc, char **argv)
{
    int pin = argc > 1 ? atoi(argv[1]) : 17;
    if (pin < 0 || pin > 27) { fprintf(stderr, "pin 0..27\n"); return 1; }

    int fd = open("/dev/gpiomem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("/dev/gpiomem (Pi 5 has no BCM GPIO here)"); return 1; }
    volatile uint32_t *gpio = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (gpio == MAP_FAILED) { perror("mmap"); return 1; }

    /* 设为输出：读-改-写 GPFSEL 里对应的 3 bit */
    int shift = (pin % 10) * 3;
    gpio[GPFSEL(pin)] = (gpio[GPFSEL(pin)] & ~(7u << shift)) | (1u << shift);

    /* 测速：连续翻转 1,000,000 次 */
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < 1000000; i++) {
        gpio[GPSET0] = 1u << pin;
        gpio[GPCLR0] = 1u << pin;
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    double s = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
    printf("1e6 toggles in %.3f s -> %.1f MHz square wave\n", s, 1e6 / s / 1e6);

    /* 慢速闪烁，肉眼可见 */
    for (int i = 0; i < 10; i++) {
        gpio[i & 1 ? GPCLR0 : GPSET0] = 1u << pin;
        printf("level=%u\n", (gpio[GPLEV0] >> pin) & 1);
        usleep(300 * 1000);
    }
    gpio[GPCLR0] = 1u << pin;
    return 0;
}
