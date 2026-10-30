#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"
NDK="${NDK:-$HOME/Android/ndk/android-ndk-r29}"
API="${API:-33}"
CXX="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android${API}-clang++"
if [ ! -x "$CXX" ]; then
    echo "clang++ not found: $CXX" >&2
    echo "Set NDK=/path/to/ndk and retry." >&2
    exit 1
fi
mkdir -p out
"$CXX" -shared -fPIC -O2 -std=c++17 -Wl,-z,nodelete -o out/libvivotorch.so src/vivotorch.cpp -llog -pthread
echo
readelf -d out/libvivotorch.so | grep -E 'NODELETE|SONAME' || true
llvm-objdump -T out/libvivotorch.so 2>/dev/null | grep torchLevelChange || true
ls -l out/libvivotorch.so
echo OK
