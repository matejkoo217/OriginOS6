#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build libabouthook.so + abouthookd for com.android.settings (arm64).
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
SDK=${SDK:-/home/jkoo/Android/Sdk} # dont forget to change this before building....
BT=${BT:-$SDK/build-tools/36.0.0}
NDK=${NDK:-$SDK/ndk/30.0.15729638}
AJAR=${AJAR:-$SDK/platforms/android-36/android.jar}

[ -x "$BT/d8" ] || { echo "d8 not found at $BT/d8 (set BT=)"; exit 1; }
[ -f "$NDK/build/cmake/android.toolchain.cmake" ] || { echo "NDK not found at $NDK (set NDK=)"; exit 1; }
[ -f "$AJAR" ] || { echo "android.jar not found at $AJAR (set AJAR=)"; exit 1; }

echo "[1/4] javac Hook.java"
rm -rf "$HERE/hook/out" "$HERE/hook/dexout"
mkdir -p "$HERE/hook/out" "$HERE/hook/dexout"
javac --release 8 -classpath "$AJAR" -d "$HERE/hook/out" "$HERE/hook/Hook.java"

echo "[2/4] d8 -> classes.dex"
"$BT/d8" --release --min-api 30 --lib "$AJAR" --output "$HERE/hook/dexout" \
    "$HERE/hook/out/abouthook/Hook.class"

echo "[3/4] embed dex -> hook/dexout/hooker_dex.h"
cp "$HERE/hook/dexout/classes.dex" "$HERE/hook/dexout.dex"
xxd -i "$HERE/hook/dexout.dex" | sed -E 's/[A-Za-z0-9_]*dexout_dex/hooker_dex/g' \
    > "$HERE/hook/dexout/hooker_dex.h"
cp "$HERE/hook/dexout/hooker_dex.h" "$HERE/hook/hooker_dex.h"
head -1 "$HERE/hook/hooker_dex.h"

echo "[4/4] cmake build"
cmake -S "$HERE" -B "$HERE/build" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 \
    -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$HERE/build" -j"$(nproc)"

echo
echo "Artifacts:"
ls -la "$HERE/build/libabouthook.so" "$HERE/build/abouthookd"
