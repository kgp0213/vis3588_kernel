#!/bin/bash
# 编译 vismpwr（aarch64）
#   ./build-vismpwr.sh            交叉编译（默认 SDK 工具链）
#   ./build-vismpwr.sh -native    在板端编译（需要板上有 gcc）
#
# 体积说明：-Os -s 后约 14.6KB；不加 -s 会带符号表（约 25KB）。
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$HERE/vismpwr}

if [ "$1" = "-native" ]; then
    CC=${CC:-gcc}
else
    CC=${CC:-/home/vis/vis3588/prebuilts/gcc/linux-x86/aarch64/\
gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-gcc}
    CC=$(echo "$CC" | tr -d ' \n')
fi

"$CC" -Os -s -Wall -o "$OUT" "$HERE/vismpwr.c"
echo "built: $OUT ($(stat -c%s "$OUT") bytes, CC=$CC)"
