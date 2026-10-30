#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
PROJ=$(cd "$HERE/.." && pwd)
SRC=${SRC:-$PROJ}
OUT=${OUT:-$PROJ/out}
AVBTOOL=${AVBTOOL:-$HERE/avbtool.py}
KEY=${KEY:-$PROJ/avb/oemvgc.pem}
OEMKEY=${OEMKEY:-$PROJ/avb/oemvgc.avbpubkey}
DIGEST_PY=${DIGEST_PY:-$HERE/vbmeta_digest.py}
OEM_SLOT=3
VGC_SLOT=5
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$OUT"
for f in "$AVBTOOL" "$DIGEST_PY"; do
    [ -f "$f" ] || { echo "missing: $f" >&2; exit 1; }
done
if [ ! -f "$KEY" ] || [ ! -f "$OEMKEY" ]; then
    KEY="$KEY" OEMKEY="$OEMKEY" "$HERE/gen_keys.sh"
fi
echo "[*] sign oem / vgc"
OUT="$OUT" "$HERE/sign.sh" "$SRC/oem.img" "$SRC/vgc.img"
echo "[*] new_vbmeta_noverify.img (flags 3, chains oem/vgc)"
python3 "$AVBTOOL" make_vbmeta_image \
    --output "$OUT/new_vbmeta_noverify.img" \
    --algorithm SHA256_RSA2048 \
    --key "$KEY" \
    --flags 3 \
    --chain_partition "oem:$OEM_SLOT:$OEMKEY" \
    --chain_partition "vgc:$VGC_SLOT:$OEMKEY"
python3 "$AVBTOOL" verify_image --image "$OUT/new_vbmeta_noverify.img" --key "$KEY" \
    --expected_chain_partition "oem:$OEM_SLOT:$OEMKEY" \
    --expected_chain_partition "vgc:$VGC_SLOT:$OEMKEY"
echo "[*] digest (flags 3 => main vbmeta struct only)"
mkdir -p "$TMP/digest"
ln -sf "$OUT/new_vbmeta_noverify.img" "$TMP/digest/vbmeta.img"
python3 "$DIGEST_PY" --dir "$TMP/digest" --mode auto --emit-cmdline \
    > "$OUT/new_digest_flags3.txt" 2>&1
cat "$OUT/new_digest_flags3.txt"
