#!/bin/sh
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
KDIR=${KDIR:-$HOME/common}
OUT=${OUT:-$HOME/common/out}
MIRROR=${MIRROR:-$HERE/vendor_dlkm/lib/modules}
LOCK=${LOCK:-$HERE/kbuild.lock}
PATH="/home/jkoo/Android/clang/clang-r584948/bin:$PATH"
export PATH

[ -d "$OUT" ] || { echo "error: build dir $OUT missing" >&2; exit 1; }
[ -d "$MIRROR" ] || { echo "error: staging dir $MIRROR missing" >&2; exit 1; }

build() {
	d=$1
	name=$(basename "$d")
	ko="$name.ko"
	echo "== $name"
	(
		cd "$d"
		make -C "$KDIR" O="$OUT" ARCH=arm64 LLVM=1 LLVM_IAS=1 \
			M="$PWD" KASAN_SANITIZE=n clean >/dev/null
		if command -v flock >/dev/null 2>&1; then
			flock "$LOCK" make -C "$KDIR" O="$OUT" ARCH=arm64 \
				LLVM=1 LLVM_IAS=1 M="$PWD" KASAN_SANITIZE=n modules
		else
			make -C "$KDIR" O="$OUT" ARCH=arm64 LLVM=1 LLVM_IAS=1 \
				M="$PWD" KASAN_SANITIZE=n modules
		fi
		if [ ! -f "$ko" ]; then
			echo "error: $name did not produce $ko" >&2
			exit 1
		fi
		cp -f "$ko" "$MIRROR/$ko"
		chmod 644 "$MIRROR/$ko"
		printf '   staged %-16s %s\n' "$ko" "$(md5sum "$ko" | cut -c1-10)"
	)
}
for d in "$HERE"/*/; do
	[ -f "$d/Makefile" ] || continue
	grep -q '^obj-m' "$d/Makefile" || continue
	build "${d%/}"
done
echo
echo "all modules built and staged into $MIRROR"
