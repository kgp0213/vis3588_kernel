#!/bin/bash
# 编译 vismpwr（aarch64）
#   ./build-vismpwr.sh            交叉编译（默认 SDK 工具链）
#   ./build-vismpwr.sh -native    在板端编译（需要板上有 gcc）
#
# 【最小体积参数】（2026-10-28 实测，不要随意删）
#   基线 -Os -s ................ 14616 B
#   加下面这些 .................. 14144 B（省 472 B / 3.2%）
#
#   -Os                              体积优先
#   -s                               去符号表（不加约 25KB）
#   -fno-asynchronous-unwind-tables -fno-unwind-tables
#                                    去掉 C 用不到的异常展开表
#   -fno-ident                       去掉 .comment 里的 GCC 版本串
#   -ffunction-sections -fdata-sections
#   -Wl,--gc-sections                未引用的函数/数据不链接
#   -Wl,--build-id=none              不生成 build-id
#   -Wl,-z,noseparate-code           不拆只读段（省一页对齐）
#   -Wl,--hash-style=sysv            小二进制 sysv hash 比 gnu hash 小
#   -Wl,--no-eh-frame-hdr            不生成 .eh_frame_hdr（省 80 B）
#   末尾 strip -R .comment -R .note.ABI-tag  再省约 240 B
#
# 【不要用的】
#   -static           静态链接 516 KB（大 36 倍）
#   -Oz               本工具链 GCC 10.3 不支持（编译失败）
#   -flto / -mno-outline-atomics / -fno-jump-tables   实测无收益
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$HERE/vismpwr}

if [ "$1" = "-native" ]; then
    CC=${CC:-gcc}
    STRIP=${STRIP:-strip}
else
    TC=/home/vis/vis3588/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin
    CC=${CC:-$TC/aarch64-none-linux-gnu-gcc}
    STRIP=${STRIP:-$TC/aarch64-none-linux-gnu-strip}
fi

CFLAGS="-Os -s -Wall \
 -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-ident \
 -ffunction-sections -fdata-sections \
 -Wl,--gc-sections -Wl,--build-id=none -Wl,-z,noseparate-code \
 -Wl,--hash-style=sysv -Wl,--no-eh-frame-hdr"

"$CC" $CFLAGS -o "$OUT" "$HERE/vismpwr.c"
# 去掉 .comment（GCC 版本串）和 .note.ABI-tag；板端 strip 不支持时忽略
"$STRIP" -R .comment -R .note.ABI-tag "$OUT" 2>/dev/null || true

echo "built: $OUT ($(stat -c%s "$OUT") bytes)"
