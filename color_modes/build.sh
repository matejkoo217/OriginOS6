#!/bin/sh
set -e

NDK=${NDK:-/home/jkoo/Android/ndk/android-ndk-r25c}
CC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang
STRIP=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip
READELF=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf
OBJDUMP=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-objdump
NM=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-nm
OUT=${OUT:-./build/libstc-uiq.so}

cd "$(dirname "$0")"

$CC -O2 -fPIC -shared -Wall -Wextra \
    -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables \
    -mbranch-protection=none \
    -Wl,-soname,libstc-uiq.so -Wl,--build-id=sha1 \
    -o "$OUT" ./src/libstc_uiq.c -llog

if $OBJDUMP -d "$OUT" | grep -q '<unknown>'; then
	echo "ERROR: undecodable instruction(s) in $OUT" >&2
	$OBJDUMP -d "$OUT" | grep -n -B2 '<unknown>' >&2
	exit 1
fi
if $READELF -n "$OUT" | grep -q 'aarch64 feature'; then
	echo "ERROR: $OUT declares a GNU property (BTI/PAC); pages would be guarded" >&2
	exit 1
fi
$NM -D --defined-only "$OUT" | grep -q ' T GetScPostBlendInterface$' || {
	echo "ERROR: GetScPostBlendInterface not exported" >&2; exit 1; }

$STRIP --strip-unneeded "$OUT"

echo "== $OUT =="
ls -l "$OUT"
md5sum "$OUT"
$READELF -d "$OUT" | grep -E "NEEDED|SONAME"
echo "== exports =="
$NM -D --defined-only "$OUT"
