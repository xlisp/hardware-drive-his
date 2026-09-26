# macOS: brew install libusb pkg-config   Linux: apt install libusb-1.0-0-dev pkg-config
CFLAGS ?= -O2 -Wall -Wextra
usb_probe: usb_probe.c
	$(CC) $(CFLAGS) -o $@ $< $(shell pkg-config --cflags --libs libusb-1.0)
clean:
	rm -f usb_probe
.PHONY: clean
