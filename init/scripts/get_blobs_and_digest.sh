#!/bin/sh
# SPDX-License-Identifier: MIT
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
IMG=${1:-"$HERE/out"}
echo "== signing key blobs of the images in $IMG =="
python3 "$HERE/vbmeta_digest.py" --verify-key "$IMG/oem.img" --dump-pubkey ./oem.avbpubkey | tail -2
python3 "$HERE/vbmeta_digest.py" --verify-key "$IMG/vgc.img" --dump-pubkey ./vgc.avbpubkey | tail -2
echo
echo "sha1 (must equal 'Public key (sha1)' from avbtool info_image on the same images):"
sha1sum ./oem.avbpubkey ./vgc.avbpubkey
echo
echo "== cmdline for vendor_boot =="
python3 "$HERE/vbmeta_digest.py" --dir "$IMG" --suffix _a --mode auto --emit-cmdline | tail -4
