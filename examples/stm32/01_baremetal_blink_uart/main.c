/*
 * main.c - STM32F103C8 裸机：不用 HAL、不用 CMSIS 头文件，只靠参考手册 RM0008 的寄存器地址
 *
 *   PC13  板载 LED（低电平亮），SysTick 1 ms 中断驱动闪烁
 *   PA9   USART1_TX，PA10 USART1_RX，115200 8N1
 *         接收用中断 + 环形缓冲区，收到的字符原样回显 —— 这是一个最小的"中断驱动的串口驱动"
 *
 * 时钟：复位后默认 HSI 8 MHz，这里不配置 PLL，保持最简单。
 * 编译烧录见 Makefile（arm-none-eabi-gcc + st-flash / OpenOCD）。
 */
#include <stdint.h>

#define REG(addr) (*(volatile uint32_t *)(addr))

/* ---- RCC  (RM0008 §7.3) ---- */
#define RCC_BASE       0x40021000u
#define RCC_APB2ENR    REG(RCC_BASE + 0x18)
#define RCC_IOPAEN     (1u << 2)
#define RCC_IOPCEN     (1u << 4)
#define RCC_USART1EN   (1u << 14)

/* ---- GPIO (RM0008 §9.2) ---- F1 的 GPIO 用 CRL/CRH 每脚 4 bit（MODE[1:0] CNF[1:0]），F4 以后改成了 MODER */
#define GPIOA_BASE     0x40010800u
#define GPIOC_BASE     0x40011000u
#define GPIO_CRH(b)    REG((b) + 0x04)
#define GPIO_BSRR(b)   REG((b) + 0x10)   /* 原子置位/复位：低 16 位置 1，高 16 位清 0，不需要读-改-写 */
#define GPIO_ODR(b)    REG((b) + 0x0C)

/* ---- USART1 (RM0008 §27.6) ---- */
#define USART1_BASE    0x40013800u
#define USART1_SR      REG(USART1_BASE + 0x00)
#define USART1_DR      REG(USART1_BASE + 0x04)
#define USART1_BRR     REG(USART1_BASE + 0x08)
#define USART1_CR1     REG(USART1_BASE + 0x0C)
#define USART_SR_RXNE  (1u << 5)
#define USART_SR_TXE   (1u << 7)
#define USART_CR1_UE     (1u << 13)
#define USART_CR1_RXNEIE (1u << 5)
#define USART_CR1_TE     (1u << 3)
#define USART_CR1_RE     (1u << 2)

/* ---- Cortex-M3 内核外设 (ARMv7-M ARM) ---- */
#define SYST_CSR       REG(0xE000E010)
#define SYST_RVR       REG(0xE000E014)
#define SYST_CVR       REG(0xE000E018)
#define NVIC_ISER1     REG(0xE000E104)   /* IRQ32..63 使能 */

static volatile uint32_t ticks;

/* 单生产者（中断）单消费者（main）环形缓冲区：只要 head/tail 各自只被一方写，就不需要关中断 */
#define RX_SIZE 64
static volatile uint8_t rx_buf[RX_SIZE];
static volatile uint32_t rx_head, rx_tail;

void SysTick_Handler(void) { ticks++; }

void USART1_IRQHandler(void)
{
    if (USART1_SR & USART_SR_RXNE) {
        uint8_t c = (uint8_t)USART1_DR;          /* 读 DR 同时清除 RXNE 标志 */
        uint32_t next = (rx_head + 1) % RX_SIZE;
        if (next != rx_tail) {                   /* 满了就丢弃 */
            rx_buf[rx_head] = c;
            rx_head = next;
        }
    }
}

static int uart_getc(void)
{
    if (rx_tail == rx_head) return -1;
    uint8_t c = rx_buf[rx_tail];
    rx_tail = (rx_tail + 1) % RX_SIZE;
    return c;
}

static void uart_putc(char c)
{
    while (!(USART1_SR & USART_SR_TXE)) {}       /* 发送用轮询，足够简单 */
    USART1_DR = (uint8_t)c;
}

static void uart_puts(const char *s) { while (*s) uart_putc(*s++); }

int main(void)
{
    RCC_APB2ENR |= RCC_IOPAEN | RCC_IOPCEN | RCC_USART1EN;   /* 外设先开时钟，否则写寄存器无效 */

    /* PC13：通用推挽输出 2 MHz → MODE=10 CNF=00 → 0x2，位于 CRH[23:20] */
    GPIO_CRH(GPIOC_BASE) = (GPIO_CRH(GPIOC_BASE) & ~(0xFu << 20)) | (0x2u << 20);
    /* PA9：复用推挽 50 MHz → 0xB；PA10：浮空输入 → 0x4 */
    GPIO_CRH(GPIOA_BASE) = (GPIO_CRH(GPIOA_BASE) & ~(0xFFu << 4)) | (0xBu << 4) | (0x4u << 8);

    /* 波特率：8 MHz / (16 * 115200) = 4.34 → 整数 4，小数 0.34*16≈5 → BRR = 0x45 */
    USART1_BRR = 0x45;
    USART1_CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE;
    NVIC_ISER1 = 1u << (37 - 32);

    SYST_RVR = 8000 - 1;                         /* 8 MHz / 8000 = 1 kHz */
    SYST_CVR = 0;
    SYST_CSR = 7;                                /* ENABLE | TICKINT | CLKSOURCE=CPU */

    uart_puts("\r\nbare-metal STM32F103 up, type something:\r\n");

    uint32_t last = 0;
    for (;;) {
        if (ticks - last >= 500) {               /* 无符号相减，溢出也正确 */
            last = ticks;
            if (GPIO_ODR(GPIOC_BASE) & (1u << 13))
                GPIO_BSRR(GPIOC_BASE) = 1u << (13 + 16);
            else
                GPIO_BSRR(GPIOC_BASE) = 1u << 13;
        }
        int c;
        while ((c = uart_getc()) >= 0) {
            uart_putc((char)c);
            if (c == '\r') uart_putc('\n');
        }
    }
}
