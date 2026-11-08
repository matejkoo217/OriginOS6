#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build libsmartscreenon.so + smartscreenond for com.bbk.facewake (arm64).
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
SDK=${SDK:-/home/jkoo/Android/Sdk} # dont forget to change this before building....
BT=${BT:-$SDK/build-tools/36.0.0}
NDK=${NDK:-$SDK/ndk/30.0.15729638}
AJAR=${AJAR:-$SDK/platforms/android-36/android.jar}

[ -x "$BT/d8" ] || { echo "d8 not found at $BT/d8 (set BT=)"; exit 1; }
[ -f "$NDK/build/cmake/android.toolchain.cmake" ] || { echo "NDK not found at $NDK (set NDK=)"; exit 1; }
[ -f "$AJAR" ] || { echo "android.jar not found at $AJAR (set AJAR=)"; exit 1; }

echo "[1/5] javac Hook.java"
rm -rf "$HERE/hook/out" "$HERE/hook/dexout"
mkdir -p "$HERE/hook/out" "$HERE/hook/dexout"
javac --release 8 -classpath "$AJAR" -d "$HERE/hook/out" "$HERE/hook/Hook.java"

echo "[2/5] d8 -> classes.dex"
"$BT/d8" --release --min-api 30 --lib "$AJAR" --output "$HERE/hook/dexout" \
    "$HERE/hook/out/smartscreenon/Hook.class"

echo "[3/5] embed dex -> hook/hooker_dex.h"
cp "$HERE/hook/dexout/classes.dex" "$HERE/hook/dexout.dex"
xxd -i "$HERE/hook/dexout.dex" | sed -E 's/[A-Za-z0-9_]*dexout_dex/hooker_dex/g' \
    > "$HERE/hook/hooker_dex.h"

echo "[4/5] cmake build"
cmake -S "$HERE" -B "$HERE/build" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 \
    -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$HERE/build" -j"$(nproc)"

echo "[5/5] stage into rom/vendor"
install -D -m 0755 "$HERE/build/smartscreenond"            "$HERE/rom/vendor/bin/smartscreenond"
install -D -m 0644 "$HERE/build/libsmartscreenon.so"       "$HERE/rom/vendor/framework/smartscreenon/libsmartscreenon.so"
install -D -m 0644 "$HERE/libs/arm64-v8a/liblsplant.so"    "$HERE/rom/vendor/framework/smartscreenon/liblsplant.so"
install -D -m 0644 "$HERE/libs/arm64-v8a/libdobby.so"      "$HERE/rom/vendor/framework/smartscreenon/libdobby.so"
install -D -m 0644 "$HERE/libs/arm64-v8a/libc++_shared.so" "$HERE/rom/vendor/framework/smartscreenon/libc++_shared.so"

echo
echo "Artifacts:"
ls -la "$HERE/rom/vendor/bin/smartscreenond" "$HERE/rom/vendor/framework/smartscreenon/"
