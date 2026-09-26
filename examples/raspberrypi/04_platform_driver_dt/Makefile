# 在树莓派上：sudo apt install linux-headers-rpi-v8（或 raspberrypi-kernel-headers），然后 make
# 交叉编译：make KDIR=~/linux ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
obj-m += demo_gpio_led.o
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
