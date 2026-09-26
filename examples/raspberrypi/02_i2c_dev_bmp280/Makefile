# 在树莓派上直接 make；交叉编译：make CC=aarch64-linux-gnu-gcc
DRV    := ../../portable_driver/bmp280
CFLAGS ?= -O2 -Wall -Wextra
CFLAGS += -I$(DRV)

bmp280_linux: bmp280_linux.c $(DRV)/bmp280.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f bmp280_linux
.PHONY: clean
