#!/usr/bin/env python3
"""Patch Xiaomi 13 stock `gpt_both0.bin` so `super` is big enough for a given super.img.
    ./patch_gpt_for_super.py --base gpt_both0.bin --super super.img
    ./patch_gpt_for_super.py --super super.img --dry-run
    ./patch_gpt_for_super.py --super super.img --extra-mib 512   # headroom
    fastboot -w
    fastboot flash partition:0 out/gpt_both0.bin
    fastboot reboot bootloader
    fastboot getvar partition-size:super
    fastboot -w
    fastboot flash super super.img
"""
import argparse
import os
import struct
import sys
import zlib

SECTOR = 4096
HDRS = (0x1000, 0xA000)
SPARSE_MAGIC = 0xED26FF3A
MIB = 1 << 20

def super_image_size(path):
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        head = f.read(28)
    if len(head) < 28 or struct.unpack_from("<I", head, 0)[0] != SPARSE_MAGIC:
        return size, f"raw ({size} B)"
    major, minor, fhdr, chdr = struct.unpack_from("<HHHH", head, 4)
    blk_sz, total_blks, total_chunks, _csum = struct.unpack_from("<IIII", head, 12)
    expanded = total_blks * blk_sz
    if chdr <= 0 or fhdr <= 0:
        sys.exit(f"FATAL: malformed sparse header in {path}")
    return expanded, (f"sparse v{major}.{minor}: {total_chunks} chunks, "
                      f"block={blk_sz} B, expanded={expanded} B "
                      f"(file is {size} B)")

def arrays_for(buf, hoff):
    h = bytes(buf[hoff:hoff + 92])
    if h[:8] != b"EFI PART":
        sys.exit(f"FATAL: no GPT header at {hex(hoff)}")
    entry_lba, = struct.unpack_from("<Q", h, 72)
    count, esz, ecrc = struct.unpack_from("<III", h, 80)
    need = count * esz
    arrs = [o for o in range(0, len(buf) - need + 1, SECTOR)
            if (zlib.crc32(bytes(buf[o:o + need])) & 0xFFFFFFFF) == ecrc]
    if not arrs:
        sys.exit(f"FATAL: no entries array matches the CRC of header@{hex(hoff)}")
    return dict(hoff=hoff, count=count, esz=esz, ecrc=ecrc, arrays=arrs,
                entry_lba=entry_lba)

def spans(buf, tab):
    out = {}
    for i in range(tab["count"]):
        o = tab["arrays"][0] + i * tab["esz"]
        e = bytes(buf[o:o + tab["esz"]])
        if e[:16] == b"\0" * 16:
            continue
        name = e[0x38:0x38 + 72].decode("utf-16-le").rstrip("\0")
        first, last = struct.unpack_from("<QQ", e, 0x20)
        out[name] = dict(index=i, first=first, last=last)
    return out

def size_of(p):
    return (p["last"] - p["first"] + 1) * SECTOR if p["last"] >= p["first"] else 0

def report(buf, label):
    tab = arrays_for(buf, HDRS[0])
    sp = spans(buf, tab)
    sup, ud = sp["super"], sp["userdata"]
    print(f"-- {label}: entries={len(sp)} array_copies={[hex(a) for a in tab['arrays']]}")
    print(f"   super     {sup['first']:9d}..{sup['last']:9d}  {size_of(sup)/MIB:9.1f} MiB")
    print(f"   userdata  {ud['first']:9d}..{ud['last']:9d}  "
          f"{size_of(ud)/MIB:9.1f} MiB")
    return sp

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument("--base", default=os.path.join(here, "base", "gpt_both0.bin"), help="stock gpt_both0.bin to patch")
    ap.add_argument("--super", required=True, help="super.img you want to flash")
    ap.add_argument("--out-dir", default=os.path.join(here, "out"))
    ap.add_argument("--align-mib", type=int, default=1, help="round the new super size up to this many MiB (default 1)")
    ap.add_argument("--extra-mib", type=int, default=0, help="extra headroom on top of the image size")
    ap.add_argument("--allow-shrink", action="store_true", help="also allow a super.img smaller than the current super")
    ap.add_argument("--dry-run", action="store_true", help="patch + verify, write nothing")
    a = ap.parse_args()
    buf = bytearray(open(a.base, "rb").read())
    print("=== BASE ===")
    sp = report(buf, os.path.basename(a.base))
    sup = sp["super"]
    cur_mib = size_of(sup) / MIB
    img_bytes, how = super_image_size(a.super)
    if img_bytes == 0:
        sys.exit(f"FATAL: {a.super} is empty")
    want = img_bytes + a.extra_mib * MIB
    align = max(1, a.align_mib) * MIB
    target = ((want + align - 1) // align) * align
    print(f"\n=== IMAGE ===\n   {a.super}: {how}")
    print(f"   needed: {want} B -> rounded to {target} B ({target/MIB:.1f} MiB, "
          f"{align//MIB} MiB alignment)")
    if target < size_of(sup) and not a.allow_shrink:
        sys.exit(f"FATAL: image ({target/MIB:.1f} MiB) is SMALLER than the current super "
                 f"({cur_mib:.0f} MiB). Build the super image at the partition size "
                 f"(lpmake --device super:{size_of(sup)}) or pass --allow-shrink.")
    if target == size_of(sup):
        print("\nnothing to do: super already matches the requested size")
        return 0
    new_last = sup["first"] + target // SECTOR - 1
    ud_first = new_last + 1
    for name, p in sp.items():
        if name in ("super", "userdata"):
            continue
        if p["first"] > sup["last"]:
            sys.exit(f"FATAL: {name} lives behind super (lba {p['first']}) — this layout is "
                     f"not the fuxi 'super-then-userdata' shape; refusing to patch")
    if sp["userdata"]["first"] != sup["last"] + 1:
        sys.exit(f"FATAL: userdata does not start right after super "
                 f"(userdata={sp['userdata']['first']}, super.last={sup['last']}) — refusing")
    arr_offsets = list(arrays_for(buf, HDRS[0])["arrays"])
    need = arrays_for(buf, HDRS[0])["count"] * arrays_for(buf, HDRS[0])["esz"]
    for arr in arr_offsets:
        struct.pack_into("<QQ", buf, arr + sup["index"] * 128 + 0x20, sup["first"], new_last)
        struct.pack_into("<QQ", buf, arr + sp["userdata"]["index"] * 128 + 0x20,
                         ud_first, ud_first - 1)
    ecrc = zlib.crc32(bytes(buf[arr_offsets[0]:arr_offsets[0] + need])) & 0xFFFFFFFF
    for hoff in HDRS:
        h = bytearray(buf[hoff:hoff + 92])
        struct.pack_into("<I", h, 88, ecrc)
        struct.pack_into("<I", h, 16, 0)
        struct.pack_into("<I", h, 16, zlib.crc32(bytes(h[:92])) & 0xFFFFFFFF)
        buf[hoff:hoff + 92] = h
    print("\n=== PATCHED ===")
    report(buf, "output")
    for hoff in HDRS:
        h = bytearray(buf[hoff:hoff + 92])
        want_crc = struct.unpack_from("<I", h, 16)[0]
        struct.pack_into("<I", h, 16, 0)
        assert (zlib.crc32(bytes(h[:92])) & 0xFFFFFFFF) == want_crc, \
            f"header CRC stale @{hex(hoff)}"
        tab = arrays_for(buf, hoff)
        assert (zlib.crc32(bytes(buf[tab["arrays"][0]:tab["arrays"][0] + need])) & 0xFFFFFFFF) \
            == tab["ecrc"], "entries CRC stale"
        assert len(tab["arrays"]) == 2, "expected two entries copies"
        s2 = spans(buf, tab)
        assert s2["super"]["last"] == new_last, "super not patched"
        assert s2["userdata"]["first"] == ud_first and s2["userdata"]["last"] == ud_first - 1, \
            "userdata auto-size marker lost"
    assert bytes(buf[0x2000:0x2000 + need]) == bytes(buf[0x6000:0x6000 + need]), \
        "entries copies diverged"
    assert buf[0x1000:0x1008] == b"EFI PART" and buf[0xA000:0xA008] == b"EFI PART"
    new_size = (new_last - sup["first"] + 1) * SECTOR
    if not a.dry_run:
        os.makedirs(a.out_dir, exist_ok=True)
        both = os.path.join(a.out_dir, "gpt_both0.bin")
        open(both, "wb").write(buf)
        print(f"\nwrote {both}")
    print(f"\nsuper: {cur_mib:.0f} MiB -> {new_size/MIB:.0f} MiB   (partition size 0x{new_size:x})")
    print(f"userdata: starts at lba {ud_first}, auto-sized by ABL from there to the disk end.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
