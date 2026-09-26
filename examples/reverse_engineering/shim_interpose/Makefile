# 仅限 Linux（glibc）：RTLD_DEEPBIND、dladdr1、ELF 符号插入是 Linux 特性。
# macOS 上可以用：docker run --rm -v $PWD:/w -w /w gcc make demo
CC     ?= cc
CFLAGS ?= -O2 -Wall -Wextra

all: app libvendor_orig.so libvendor.so

# 1) "厂商"交付物：libvendor.so + 链接了它的 app
vendor/libvendor.so: vendor.c
	mkdir -p vendor
	# -fno-inline：保证库内调用走 PLT（真实厂商库一般也是这样），便于演示符号插入
	$(CC) $(CFLAGS) -fno-inline -fPIC -shared -Wl,-soname,libvendor.so -o $@ $<
app: app.c vendor/libvendor.so
	$(CC) $(CFLAGS) -o $@ app.c -Lvendor -lvendor

# 2) 安装 shim：原库改名，shim 顶替原名（在盒子上就是改 /home/pi/ASIAIR/lib 里的文件）
libvendor_orig.so: vendor/libvendor.so
	cp $< $@
libvendor.so: shim.c trampolines.S
	$(CC) $(CFLAGS) -fPIC -shared -Wl,-soname,libvendor.so -o $@ shim.c trampolines.S -ldl

demo: all
	@echo "===== 原厂：app + 原库 ====="
	LD_LIBRARY_PATH=vendor ./app
	@echo; echo "===== 装上 shim（DEEPBIND）====="
	LD_LIBRARY_PATH=. ./app
	@echo; echo "===== 不用 DEEPBIND：原库内部的 dev_count() 被 shim 截走 ====="
	-SHIM_NO_DEEPBIND=1 LD_LIBRARY_PATH=. ./app
	@echo; echo "===== 核对导出符号是否一致（应无输出）====="
	diff <(nm -D --defined-only vendor/libvendor.so | awk '{print $$3}' | sort) \
	     <(nm -D --defined-only libvendor.so | awk '{print $$3}' | sort)
	@echo; echo "===== 看每个符号绑定到哪个库（scripts/re/ld_bindings.sh 的原理）====="
	LD_LIBRARY_PATH=. LD_DEBUG=bindings ./app 2>&1 | grep -E "binding file .*(dev_count|dev_get_vid)'" | sed 's/^ *[0-9]*: *//' | sort -u

clean:
	rm -rf app vendor libvendor.so libvendor_orig.so
.PHONY: all demo clean
SHELL := /bin/bash
