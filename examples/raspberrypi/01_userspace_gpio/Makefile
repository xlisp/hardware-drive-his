CFLAGS ?= -O2 -Wall -Wextra
all: gpio_libgpiod gpio_mmap
gpio_libgpiod: gpio_libgpiod.c
	$(CC) $(CFLAGS) -o $@ $< -lgpiod
gpio_mmap: gpio_mmap.c
	$(CC) $(CFLAGS) -o $@ $<
clean:
	rm -f gpio_libgpiod gpio_mmap
.PHONY: all clean
