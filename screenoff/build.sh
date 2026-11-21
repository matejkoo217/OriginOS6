#!/bin/sh
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
NDK=${NDK:-/home/jkoo/Android/ndk/android-ndk-r29/toolchains/llvm/prebuilt/linux-x86_64/bin}
CC=$NDK/aarch64-linux-android30-clang
SRC=$HERE/src/vtsgesture.c
OUT=$HERE/out
[ -x "$CC" ] || { echo "compiler not found: $CC (set NDK=...)" >&2; exit 1; }
[ -f "$SRC" ] || { echo "source not found: $SRC" >&2; exit 1; }
mkdir -p "$OUT"
"$CC" -O2 -o "$OUT/vtsgesture" "$SRC" -lm -ldl
"$CC" -O2 -o "$OUT/vtsprox" "$HERE/src/vtsprox.c" -landroid -llog
echo "built: $OUT/vtsgesture $OUT/vtsprox"
ls -l "$OUT/vtsgesture" "$OUT/vtsprox"
