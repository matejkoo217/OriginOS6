#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -e
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"
CC="${CC:-aarch64-linux-gnu-gcc-12}"
SYSLIB="$DIR/sysroot/system/lib64"
OUT="$DIR/out"
mkdir -p "$OUT"
echo "[1/3] assembling _start.o"
"$CC" -c -O2 -o "$OUT/_start.o" native/_start.S
echo "[2/3] linking out/fod_bridge"
"$CC" -nostdlib -fPIE -pie -O2 -Wall \
    -Werror=implicit-function-declaration \
    -U_FORTIFY_SOURCE \
    -Wl,-dynamic-linker,/system/bin/linker64 \
    -Wl,-z,max-page-size=4096 -Wl,-z,common-page-size=4096 \
    -Wl,-rpath-link,"$SYSLIB" \
    -o "$OUT/fod_bridge" \
    "$OUT/_start.o" \
    src/main.c \
    native/hidl_native_client.c \
    native/wm_client.c \
    -I"$DIR/native" \
    -L"$SYSLIB" -ldl -lc -lm
rm -f "$OUT/_start.o"
echo "[3/3] done"
md5sum "$OUT/fod_bridge"
ls -lh "$OUT/fod_bridge"
