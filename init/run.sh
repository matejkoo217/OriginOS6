#!/bin/sh
# SPDX-License-Identifier: MIT
# It uses the unsigned ./oem.img and ./vgc.img in this directory
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
exec "$HERE/scripts/make_vbmeta.sh" "$@"
