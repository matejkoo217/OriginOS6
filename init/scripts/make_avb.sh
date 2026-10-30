#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu
AVBTOOL=${AVBTOOL:-$(dirname "$0")/avbtool.py}
IMG=$1
NAME=$2
SIZE=$3
OUT=${4:-${IMG%.img}_avb.img}
[ -f "$IMG" ] || { echo "no such image: $IMG" >&2; exit 1; }
if tail -c 64 "$IMG" 2>/dev/null | head -c 4 | grep -q AVBf; then
    echo "$IMG already carries an AVB footer - nothing to do" >&2
    exit 1
fi
cp "$IMG" "$OUT"
python3 "$AVBTOOL" add_hashtree_footer \
    --image "$OUT" \
    --partition_name "$NAME" \
    --partition_size "$SIZE" \
    --algorithm NONE \
    --hash_algorithm sha256 \
    --do_not_generate_fec
printf '\n[+] %s: %s -> %s bytes, footer=%s\n' "$NAME" "$(stat -c%s "$IMG")" "$(stat -c%s "$OUT")" \
       "$(tail -c 64 "$OUT" | head -c 4)"
python3 "$AVBTOOL" info_image --image "$OUT" | sed -n '1,8p'
