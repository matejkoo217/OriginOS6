#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
PROJ=$(cd "$HERE/.." && pwd)
AVBTOOL=${AVBTOOL:-$HERE/avbtool.py}
KEY=${KEY:-$PROJ/avb/oemvgc.pem}
OUT=${OUT:-$PROJ/out}
[ -f "$AVBTOOL" ] || { echo "no avbtool at $AVBTOOL" >&2; exit 1; }
[ -f "$KEY" ]     || { echo "no signing key at $KEY" >&2; exit 1; }
mkdir -p "$OUT"
if [ $# -eq 0 ]; then
    set -- oem.img vgc.img
fi
for arg in "$@"; do
    img=$arg
    case $img in
        *.img) ;;
        *) img=$img.img ;;
    esac
    [ -f "$img" ] || img="$PROJ/$img"
    [ -f "$img" ] || { echo "no such image: $arg" >&2; exit 1; }
    name=$(basename "$img" .img)
    out="$OUT/new_$(basename "$img")"
    case $name in
        oem|vgc) ;;
        *) echo "unsupported partition '$name' (expected oem or vgc)" >&2; exit 1 ;;
    esac
    if tail -c 64 "$img" 2>/dev/null | head -c 4 | grep -q AVBf; then
        echo "$img already carries an AVB footer - refusing to sign" >&2
        exit 1
    fi
    echo "[*] $img -> $out  (partition=$name)"
    cp "$img" "$out"
    python3 "$AVBTOOL" add_hashtree_footer \
        --image "$out" \
        --partition_name "$name" \
        --algorithm SHA256_RSA2048 \
        --key "$KEY" \
        --hash_algorithm sha256 \
        --do_not_generate_fec
    tmp=$(mktemp -d)
    cp "$out" "$tmp/$name.img"
    python3 "$AVBTOOL" verify_image --image "$tmp/$name.img" --key "$KEY"
    rm -rf "$tmp"
    printf '[+] %s: %s bytes -> %s bytes, footer=%s\n' \
        "$name" "$(stat -c%s "$img")" "$(stat -c%s "$out")" \
        "$(tail -c 64 "$out" | head -c 4)"
done
