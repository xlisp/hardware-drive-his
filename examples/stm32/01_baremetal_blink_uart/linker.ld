/* linker.ld - STM32F103C8（"Blue Pill"）：64 KB Flash，20 KB SRAM */
ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 64K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 20K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);        /* 栈从 RAM 顶部向下生长 */

SECTIONS
{
    .isr_vector : { KEEP(*(.isr_vector)) } > FLASH   /* 必须在 Flash 的最开头 */

    .text : {
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    _sidata = LOADADDR(.data);
    .data : {                                /* 运行在 RAM，初值存在 Flash */
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    .bss (NOLOAD) : {
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM
}
