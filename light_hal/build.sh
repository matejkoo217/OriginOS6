#!/usr/bin/env bash
# lazy to change paths , use yours NDK.
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
NDK=${NDK:-/home/jkoo/Android/Sdk/ndk/29.0.14206865}
CXX="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android33-clang++"
DEVLIB=$ROOT/../../fod/device_libs
OUT=$ROOT/build
mkdir -p "$OUT"

if [ ! -x "$CXX" ]; then
    echo "clang++ not found at $CXX" >&2
    exit 1
fi

LIBCXX=${LIBCXX:-$HOME/.cache/light_hal/libcxx/include}
if [ ! -f "$LIBCXX/atomic" ]; then
    echo "Fetching Android 13 libc++ headers into $LIBCXX ..."
    mkdir -p "$(dirname "$LIBCXX")"
    tmp=$(mktemp -d)
    curl -sL "https://android.googlesource.com/platform/external/libcxx/+archive/refs/heads/android13-release/include.tar.gz" \
        -o "$tmp/libcxx.tar.gz"
    mkdir -p "$LIBCXX"
    tar xzf "$tmp/libcxx.tar.gz" -C "$LIBCXX"
    rm -rf "$tmp"
fi
CXXFLAGS=(
    -std=c++17 -O2 -fPIC -fvisibility=hidden -fno-exceptions -fno-rtti
    -nostdinc++ -I"$LIBCXX"
    -Wall -Wextra -Wno-unused-parameter -Wno-return-type-c-linkage -Wno-unused-function -Wno-unused-const-variable -Wno-unused-but-set-variable -Wno-nontrivial-memcall
    -I"$ROOT/include"
)
LDFLAGS=(
    -nostdlib++
    -Wl,-z,max-page-size=4096 -Wl,-z,common-page-size=4096
    -L"$DEVLIB"
)
echo "[1/2] sensors.light.so"
"$CXX" -shared "${CXXFLAGS[@]}" "${LDFLAGS[@]}" \
    -o "$OUT/sensors.light.so" \
    "$ROOT/src/engine.cpp" "$ROOT/src/subhal.cpp" \
    -lc++ -llog -lutils -ldl -lm
echo "[2/2] light_probe (standalone SSC probe)"
"$CXX" "${CXXFLAGS[@]}" "${LDFLAGS[@]}" \
    -o "$OUT/light_probe" \
    "$ROOT/src/engine.cpp" "$ROOT/src/test_main.cpp" \
    -lc++ -llog -ldl -lm
ls -l "$OUT/sensors.light.so" "$OUT/light_probe"
readelf -d "$OUT/sensors.light.so" | grep NEEDED || true
