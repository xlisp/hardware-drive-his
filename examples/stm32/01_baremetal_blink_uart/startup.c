/*
 * startup.c - Cortex-M3 启动代码（用 C 写，代替 CubeMX 生成的 startup_stm32f103xb.s）
 *
 * 上电后 Cortex-M 硬件做两件事：
 *   1. 从 0x0800_0000（映射到 0x0）取第 0 个字 → MSP（初始栈指针）
 *   2. 取第 1 个字 → PC，也就是 Reset_Handler
 * 所以"向量表"就是放在 Flash 最开头的一个函数指针数组，由链接脚本保证位置。
 */
#include <stdint.h>

extern uint32_t _estack, _sidata, _sdata, _edata, _sbss, _ebss;   /* 来自 linker.ld */
extern int main(void);

void Reset_Handler(void);
void Default_Handler(void);
void SysTick_Handler(void)   __attribute__((weak, alias("Default_Handler")));
void USART1_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void HardFault_Handler(void) __attribute__((weak, alias("Default_Handler")));

/* 16 个内核异常 + 60 个 STM32F103 外设中断（RM0008 表 63）。USART1 是 IRQ37 → 下标 16+37=53 */
__attribute__((section(".isr_vector"), used))
void (*const vector_table[16 + 60])(void) = {
    [0]  = (void (*)(void))&_estack,
    [1]  = Reset_Handler,
    [2]  = Default_Handler,          /* NMI */
    [3]  = HardFault_Handler,
    [15] = SysTick_Handler,
    [16 + 37] = USART1_IRQHandler,
};

void Reset_Handler(void)
{
    /* .data 的初值存在 Flash（LMA），运行时要搬到 RAM（VMA） */
    uint32_t *src = &_sidata, *dst = &_sdata;
    while (dst < &_edata) *dst++ = *src++;
    /* .bss 清零 —— C 标准保证未初始化的全局变量为 0，这件事就是在这里做的 */
    for (dst = &_sbss; dst < &_ebss;) *dst++ = 0;

    main();
    for (;;) {}
}

void Default_Handler(void)
{
    for (;;) {}                      /* 用调试器停在这里，看 xPSR 的 IPSR 字段就知道是哪个异常 */
}
