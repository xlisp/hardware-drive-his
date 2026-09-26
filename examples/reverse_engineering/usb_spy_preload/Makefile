# Linux：apt install libusb-1.0-0-dev
usbspy.so: usbspy.c
	$(CC) -O2 -Wall -fPIC -shared -o $@ $< $(shell pkg-config --cflags libusb-1.0) -ldl -lpthread
clean:
	rm -f usbspy.so
.PHONY: clean
