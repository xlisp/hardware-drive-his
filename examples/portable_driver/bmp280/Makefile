# 主机单元测试：make test
test: test_host
	./test_host

test_host: test_host.c bmp280.c bmp280.h
	$(CC) -Wall -Wextra -std=c99 -o $@ test_host.c bmp280.c

clean:
	rm -f test_host
.PHONY: test clean
