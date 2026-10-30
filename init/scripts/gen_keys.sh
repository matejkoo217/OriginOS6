#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
PROJ=$(cd "$HERE/.." && pwd)
AVBTOOL=${AVBTOOL:-$HERE/avbtool.py}
KEY=${KEY:-$PROJ/avb/oemvgc.pem}
PUB=${OEMKEY:-$PROJ/avb/oemvgc.avbpubkey}
[ -f "$AVBTOOL" ] || { echo "no avbtool at $AVBTOOL" >&2; exit 1; }
if [ -f "$KEY" ] && [ -f "$PUB" ]; then
    echo "[=] reusing signing key $KEY"
else
    mkdir -p "$(dirname "$KEY")" "$(dirname "$PUB")"
    command -v openssl >/dev/null 2>&1 || { echo "openssl is required to generate a key" >&2; exit 1; }
    openssl genrsa -out "$KEY" 2048 2>/dev/null || { echo "openssl genrsa failed" >&2; exit 1; }
    chmod 600 "$KEY"
    python3 "$AVBTOOL" extract_public_key --key "$KEY" --output "$PUB"
    echo "[+] generated signing key $KEY"
fi
if [ "${SYNC_PUBKEYS:-1}" = "1" ]; then
    cp -f "$PUB" "$PROJ/avb/oem.avbpubkey"
    cp -f "$PUB" "$PROJ/avb/vgc.avbpubkey"
fi
echo "[i] signer sha1: $(sha1sum "$PUB" | cut -c1-40)"
