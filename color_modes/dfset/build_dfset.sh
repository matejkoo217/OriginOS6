#!/usr/bin/env bash
set -e
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"
NDK=${NDK:-/home/jkoo/Android/ndk/android-ndk-r25c/toolchains/llvm/prebuilt/linux-x86_64/bin}
CXX="$NDK/aarch64-linux-android30-clang++"
"$CXX" -O2 -Wall -Wextra -static-libstdc++ -o dfset dfset.cpp -ldl
md5sum dfset
ls -lh dfset
file dfset
