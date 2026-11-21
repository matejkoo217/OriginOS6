#!/bin/sh
set -e

NDK=${NDK:-/home/jkoo/Android/ndk/android-ndk-r25c}
CC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang
STRIP=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip
READELF=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf
NM=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-nm
OUT=${OUT:-./build/libvivodispcolor-shim.so}

cd "$(dirname "$0")"

$CC -O2 -fPIC -shared -Wall -Wextra \
    -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables \
    -mbranch-protection=none \
    -Wl,-soname,libvivodispcolor-shim.so -Wl,--build-id=sha1 \
    -o "$OUT" ./src/vivodispcolor_shim.c -llog -lbinder_ndk

if $READELF -n "$OUT" | grep -q 'aarch64 feature'; then
	echo "ERROR: $OUT declares a GNU property (BTI/PAC)" >&2
	exit 1
fi
$NM -D --defined-only "$OUT" | grep -q 'AServiceManager_addService$' || {
	echo "ERROR: AServiceManager_addService not exported" >&2; exit 1; }

$STRIP --strip-unneeded "$OUT"

echo "== $OUT =="
ls -l "$OUT"
md5sum "$OUT"
$READELF -d "$OUT" | grep -E "NEEDED|SONAME"
echo "== exports =="
$NM -D --defined-only "$OUT" | head
