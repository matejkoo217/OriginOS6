#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu
ALG=${1:-both}
HERE=$(cd "$(dirname "$0")" && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
adb exec-out "dd if=/dev/block/by-name/vbmeta_a bs=4096 count=8 2>/dev/null" > "$TMP/vbmeta_a.bin" || { echo "cannot read vbmeta_a (device reachable? root?)" >&2; exit 1; }
python3 - "$TMP" "$ALG" "$HERE/avbtool.py" <<'EOF'
import sys, os, struct, hashlib, subprocess
tmp, alg, AVBTOOL = sys.argv[1], sys.argv[2], sys.argv[3]
def get(path, blocks, skip=0):
    return subprocess.run(["adb", "exec-out",
        f"dd if={path} bs=4096 skip={skip} count={blocks} 2>/dev/null"],
        capture_output=True).stdout
def devpath(part):
    out = subprocess.run(["adb", "shell",
        f"if [ -e /dev/block/by-name/{part} ]; then echo /dev/block/by-name/{part}; "
        f"elif [ -e /dev/block/mapper/{part} ]; then echo /dev/block/mapper/{part}; fi"],
        capture_output=True, text=True).stdout.strip()
    if not out:
        raise SystemExit(f"partition {part} not found")
    return out
def part_size(dev):
    out = subprocess.run(["adb", "shell", f"blockdev --getsize64 {dev}"],
                         capture_output=True, text=True).stdout
    return int(out.strip().split()[-1])
def struct_size(b, off=0):
    assert b[off:off+4] == b"AVB0", f"not a vbmeta at {off}"
    authen = struct.unpack_from(">Q", b, off+12)[0]
    aux = struct.unpack_from(">Q", b, off+20)[0]
    return 256 + authen + aux
def chain_order(vbmeta_path):
    out = subprocess.run(["python3", AVBTOOL, "info_image", "--image", vbmeta_path],
                         capture_output=True, text=True).stdout
    return [l.split(":", 1)[1].strip() for l in out.splitlines()
            if l.strip().startswith("Partition Name:")]
main = open(f"{tmp}/vbmeta_a.bin", "rb").read()
n_main = struct_size(main)
main_struct = main[:n_main]
order = chain_order(f"{tmp}/vbmeta_a.bin")
flags = struct.unpack_from(">I", main, 0x78)[0]
chains_disabled = bool(flags & 2)   # AVB_VBMETA_IMAGE_FLAGS_VERIFICATION_DISABLED
print(f"main vbmeta struct : {n_main} bytes  (flags={flags:#x})")
print(f"chain partitions   : {', '.join(order) if order else '(none found!)'}")
if chains_disabled:
    print("  (VERIFICATION_DISABLED: libavb hashes only the main struct)")
    order = []
sizes = []
hashers = {}
if alg in ("sha256", "both"): hashers["sha256"] = hashlib.sha256(main_struct)
if alg in ("sha1", "both"):   hashers["sha1"]   = hashlib.sha1(main_struct)
for p in order:
    dev = devpath(f"{p}_a")
    sz = part_size(dev)
    tail = get(dev, 1, (sz // 4096) - 1)
    f = tail[-64:]
    if f[:4] == b"AVBf":
        off, siz = struct.unpack_from(">Q", f, 20)[0], struct.unpack_from(">Q", f, 28)[0]
    else:
        off, siz = 0, 0
    blocks = max(1, (siz + 4095) // 4096)
    blob = get(dev, blocks, off // 4096)
    cn = struct_size(blob)
    blob = blob[:cn]
    sizes.append(cn)
    print(f"  {p:14s} blob {cn:5d} B  (partition {sz} B)")
    for h in hashers.values(): h.update(blob)
print()
total = n_main + sum(sizes)
alg = "sha256" if "sha256" in hashers else "sha1"
print(f"# total vbmeta size (sum of structs) = {total}")
print(f"androidboot.vbmeta.hash_alg={alg}")
print(f"androidboot.vbmeta.size={total}")
for name, h in hashers.items():
    print(f"androidboot.vbmeta.digest={h.hexdigest()}   # {name}")
EOF
