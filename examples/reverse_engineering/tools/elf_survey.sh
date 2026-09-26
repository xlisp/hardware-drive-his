#!/bin/bash
# elf_survey.sh - 拿到一个闭源 .so / 可执行文件后的第一轮静态侦察
#
#   elf_survey.sh libASICamera2.so
#   elf_survey.sh zwoair_imager ASI          # 第二个参数：只看含该前缀的导入/导出
#
# 回答写 shim / 兼容层之前必须知道的问题（hezi-hack 第 1、3、5 节就是这么开始的）：
#   * 什么架构、什么 ABI（armhf 还是 armel？32 还是 64 位？）
#   * 依赖哪些库（NEEDED）、自己叫什么（SONAME）→ 能不能被同名库替换
#   * 需要多高版本的 glibc/libstdc++（VERNEED）→ 能不能在目标系统上加载
#   * 导出哪些函数（shim 必须一个不少）、导入哪些函数（程序实际用到的 API 子集）
#   * 有没有 strip（有符号名 = 逆向难度低一个数量级）
#   * 字符串里的线索：USB VID/PID、设备路径、日志格式、固件文件名
# 需要 binutils（readelf/objdump/nm）；交叉架构的文件用 binutils-multiarch 或 llvm-readelf。
set -u
F=${1:?usage: $0 FILE [SYMBOL_PREFIX]}
P=${2:-}
hr() { printf '\n== %s ==\n' "$1"; }

hr "file"
file -L "$F"
readelf -h "$F" | grep -E 'Class|Machine|Flags'
readelf -A "$F" 2>/dev/null | grep -E 'Tag_ABI_VFP_args|Tag_CPU_name|Tag_CPU_arch:' || true

hr "SONAME / NEEDED / RPATH"
readelf -d "$F" | grep -E 'SONAME|NEEDED|RPATH|RUNPATH'

hr "required symbol versions (max per library)"
objdump -T "$F" | grep -oE '(GLIBC|GLIBCXX|CXXABI|GCC)_[0-9.]+' | sort -Vu |
    awk -F_ '{max[$1]=$0} END {for (k in max) print "  " max[k]}'
echo "  (target system has: ldd --version; strings \$(gcc -print-file-name=libstdc++.so.6) | grep GLIBCXX | sort -V | tail -1)"

hr "stripped?"
if readelf -S "$F" | grep -q '\.symtab'; then echo "  NOT stripped (.symtab present) - static function names available"
else echo "  stripped - only dynamic symbols"; fi

hr "exported functions${P:+ matching $P}"
nm -D --defined-only "$F" 2>/dev/null | awk '$2 ~ /[TW]/ {print $3}' | grep -E "^_?Z?.*${P}" | c++filt | sort | head -80
echo "  total: $(nm -D --defined-only "$F" 2>/dev/null | awk '$2 ~ /[TW]/' | wc -l)"

hr "imported functions${P:+ matching $P}"
nm -D --undefined-only "$F" 2>/dev/null | awk '{print $2}' | grep -E "${P}" | c++filt | sort | head -80

hr "interesting strings"
strings -n 5 "$F" | grep -iE '0x[0-9a-f]{4}|vid|pid|/dev/|/sys/|\.img|\.hex|firmware|usb|serial|version|error' | sort -u | head -60
