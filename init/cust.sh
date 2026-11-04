#!/bin/sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
SIZE=${SIZE:-2147483648} # you might want to change this ;)
RAW="$HERE/out/cust.build.raw.img"
OUT_SPARSE="$HERE/out/cust.img"
OUT_RAW="$HERE/out/cust.raw.img"

mkdir -p "$HERE/out"
rm -f "$RAW"
truncate -s "$SIZE" "$RAW"

mke2fs -q -t ext4 -b 4096 -m 1 \
  -O 64bit,metadata_csum,extent,flex_bg,sparse_super,dir_nlink,extra_isize,\
huge_file,large_file,filetype,dir_index,resize_inode,has_journal,ext_attr \
  -F "$RAW"

e2fsck -fy "$RAW" >/dev/null 2>&1 || true

# raw -> sparse with RAW + DONT_CARE
# Without this flashing cust would hang on fuxi..
python3 - "$RAW" "$OUT_SPARSE" <<'PY'
import struct, sys
src, dst = sys.argv[1], sys.argv[2]
B = 4096
d = open(src, "rb").read()
d += b"\0" * (-len(d) % B)
n = len(d) // B
z = [d[i*B:(i+1)*B] == b"\0" * B for i in range(n)]
chunks, i = [], 0
while i < n:
    j = i
    while j < n and z[j] == z[i]:
        j += 1
    chunks.append((0xCAC3 if z[i] else 0xCAC1, i, j - i))
    i = j
out = bytearray(struct.pack("<IHHHHIIII", 0xED26FF3A, 1, 0, 28, 12, B, n, len(chunks), 0))
for t, f, c in chunks:
    if t == 0xCAC3:
        out += struct.pack("<HHII", t, 0, c, 12)
    else:
        p = d[f*B:(f+c)*B]
        out += struct.pack("<HHII", t, 0, c, 12 + len(p)) + p
open(dst, "wb").write(out)
print("[+] %s  %d B  %d blocks (%.0f MiB)  RAW=%d DONT_CARE=%d FILL=0" % (
    dst, len(out), n, n * B / 2**20,
    sum(1 for t, _, _ in chunks if t == 0xCAC1),
    sum(1 for t, _, _ in chunks if t == 0xCAC3)))
PY
rm -f "$RAW"
#🐥
