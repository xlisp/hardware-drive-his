# 在树莓派上：sudo apt install linux-headers-rpi-v8 device-tree-compiler
#   make            编译 pifocuser.ko 和 pifocuser.dtbo
#   make install    安装模块（按设备树 compatible 开机自动加载）和 overlay
obj-m += pifocuser.o
KDIR ?= /lib/modules/$(shell uname -r)/build
OVERLAY_DIR ?= /boot/firmware/overlays

all: pifocuser.dtbo
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

pifocuser.dtbo: pifocuser-overlay.dts
	dtc -@ -I dts -O dtb -o $@ $<

install: all
	sudo $(MAKE) -C $(KDIR) M=$(CURDIR) modules_install
	sudo depmod -a
	sudo cp pifocuser.dtbo $(OVERLAY_DIR)/
	@echo "now add 'dtoverlay=pifocuser' (and 'dtoverlay=w1-gpio,gpiopin=4') to /boot/firmware/config.txt and reboot"

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f pifocuser.dtbo
.PHONY: all install clean
