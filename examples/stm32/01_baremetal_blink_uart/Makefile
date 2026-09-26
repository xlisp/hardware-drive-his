# 需要：arm-none-eabi-gcc（brew install --cask gcc-arm-embedded / apt install gcc-arm-none-eabi）
#       烧录二选一：st-flash（stlink 工具） 或 openocd
CROSS  ?= arm-none-eabi-
CC      = $(CROSS)gcc
OBJCOPY = $(CROSS)objcopy
SIZE    = $(CROSS)size

CFLAGS  = -mcpu=cortex-m3 -mthumb -Os -g -Wall -Wextra -ffreestanding -ffunction-sections
LDFLAGS = -T linker.ld -nostdlib -Wl,--gc-sections -Wl,-Map=blink.map

blink.bin: blink.elf
	$(OBJCOPY) -O binary $< $@
	$(SIZE) $<

blink.elf: startup.c main.c linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ startup.c main.c

flash: blink.bin
	st-flash write $< 0x08000000

flash-openocd: blink.elf
	openocd -f interface/stlink.cfg -f target/stm32f1x.cfg -c "program $< verify reset exit"

# 调试：一个终端 make gdbserver，另一个 arm-none-eabi-gdb blink.elf -ex "target extended-remote :3333"
gdbserver:
	openocd -f interface/stlink.cfg -f target/stm32f1x.cfg

# 反汇编看看编译器把寄存器操作变成了什么
disasm: blink.elf
	$(CROSS)objdump -d $< | less

clean:
	rm -f blink.elf blink.bin blink.map
.PHONY: flash flash-openocd gdbserver disasm clean
