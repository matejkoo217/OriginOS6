#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import argparse
import hashlib
import os
import re
import struct
import sys
AVB_MAGIC = b"AVB0"
AVB_FOOTER_MAGIC = b"AVBf"
HDR_SIZE = 256
TAG_CHAIN_PARTITION = 4
DO_NOT_USE_AB = 0x1  # AVB_HASH_DESCRIPTOR_FLAGS_DO_NOT_USE_AB
AVB_VBMETA_IMAGE_FLAGS_VERIFICATION_DISABLED = 0x2  # avbtool --flags 2 / "verification disabled"
ALGO_NAMES = {
    0: "NONE",
    1: "SHA256_RSA2048",
    2: "SHA256_RSA4096",
    3: "SHA256_RSA8192",
    4: "SHA512_RSA2048",
    5: "SHA512_RSA4096",
    6: "SHA512_RSA8192",
}
DESCRIPTOR_TAGS = {
    0: "PROPERTY",
    1: "HASHTREE",
    2: "HASH",
    3: "KERNEL_CMDLINE",
    4: "CHAIN_PARTITION",
}
def be32(b, o):
    return struct.unpack_from(">I", b, o)[0]
def be64(b, o):
    return struct.unpack_from(">Q", b, o)[0]
def parse_header(buf):
    if buf[:4] != AVB_MAGIC:
        return None
    h = {
        "ver_major": be32(buf, 0x04),
        "ver_minor": be32(buf, 0x08),
        "auth_size": be64(buf, 0x0C),
        "aux_size": be64(buf, 0x14),
        "algorithm_type": be32(buf, 0x1C),
        "hash_offset": be64(buf, 0x20),
        "hash_size": be64(buf, 0x28),
        "signature_offset": be64(buf, 0x30),
        "signature_size": be64(buf, 0x38),
        "public_key_offset": be64(buf, 0x40),
        "public_key_size": be64(buf, 0x48),
        "public_key_metadata_offset": be64(buf, 0x50),
        "public_key_metadata_size": be64(buf, 0x58),
        "descriptors_offset": be64(buf, 0x60),
        "descriptors_size": be64(buf, 0x68),
        "rollback_index": be64(buf, 0x70),
        "flags": be32(buf, 0x78),
        "rollback_index_location": be32(buf, 0x7C),
        "release_string": buf[0x80:0x80 + 48].split(b"\x00")[0].decode("utf-8", "replace"),
    }
    h["total_size"] = HDR_SIZE + h["auth_size"] + h["aux_size"]
    return h
def find_footer(buf):
    if len(buf) < 64 or buf[-64:-60] != AVB_FOOTER_MAGIC:
        return None
    return be64(buf, len(buf) - 64 + 20), be64(buf, len(buf) - 64 + 28)
def chain_partitions(buf, hdr):
    out = []
    base = HDR_SIZE + hdr["auth_size"]
    desc = buf[base + hdr["descriptors_offset"]:base + hdr["descriptors_offset"] + hdr["descriptors_size"]]
    off = 0
    while off + 16 <= len(desc):
        tag = be64(desc, off)
        num_bytes = be64(desc, off + 8)
        if num_bytes == 0 or off + 16 + num_bytes > len(desc):
            break
        if tag == TAG_CHAIN_PARTITION:
            rollback_loc, name_len, key_len, flags = struct.unpack_from(">IIII", desc, off + 16)
            for fields_end_minus_16, layout in ((92 - 16, "v2(+60 reserved)"), (32 - 16, "legacy")):
                if ((92 - 16) == fields_end_minus_16 and 92 + name_len + key_len <= len(desc)):
                    name_at = off + 92
                elif fields_end_minus_16 == 32 - 16:
                    name_at = off + 32
                else:
                    continue
                expected = fields_end_minus_16 + name_len + key_len
                expected = (expected + 7) & ~7
                if expected != num_bytes:
                    continue
                raw = desc[name_at:name_at + name_len]
                if len(raw) != name_len:
                    continue
                try:
                    name = raw.decode("utf-8")
                except UnicodeDecodeError:
                    continue
                out.append((name, bool(flags & DO_NOT_USE_AB), rollback_loc, key_len, layout))
                break
        off = off + 16 + num_bytes
    return out
def load_partition(d, name, suffix):
    for cand in (f"{name}{suffix}.img", f"{name}.img", name, f"{name}{suffix}", f"{name}.bin"):
        p = os.path.join(d, cand)
        if os.path.exists(p):
            with open(p, "rb") as fh:
                return p, fh.read()
    return None, None
def gather(d, suffix, top="vbmeta", mode="auto"):
    order = []
    seen = set()
    main_flags = {"v": None}
    def read_vbmeta(name, use_ab, is_main):
        nm = name + (suffix if use_ab else "")
        if nm in seen:
            return
        path, buf = load_partition(d, nm, suffix if use_ab else "")
        if buf is None:
            path, buf = load_partition(d, name, "")
        if buf is None:
            print(f"!! partition image for '{name}' not found in {d}", file=sys.stderr)
            return
        seen.add(nm)
        hdr = parse_header(buf)
        off = 0
        if hdr is None:
            foot = find_footer(buf)
            if foot is None:
                print(f"!! {path}: no AVB0 header at 0 and no AVBf footer — skipped", file=sys.stderr)
                return
            off, vsize = foot
            hdr = parse_header(buf[off:off + HDR_SIZE])
            if hdr is None:
                print(f"!! {path}: footer points to garbage", file=sys.stderr)
                return
        blob = buf[off:off + hdr["total_size"]]
        order.append((nm, path, off, len(blob), hdr))
        if is_main:
            main_flags["v"] = hdr["flags"]
        chains_disabled = bool((main_flags["v"] or 0) & AVB_VBMETA_IMAGE_FLAGS_VERIFICATION_DISABLED)
        if mode == "main-only" or (mode == "auto" and is_main and chains_disabled) or chains_disabled:
            if is_main and chains_disabled:
                print("-- main vbmeta has AVB_VBMETA_IMAGE_FLAGS_VERIFICATION_DISABLED (flags & 2):"
                      " libavb loads ONLY this struct, chains are not followed", file=sys.stderr)
            return
        for cname, no_ab, rollback_loc, klen, layout in chain_partitions(buf[off:], hdr):
            read_vbmeta(cname, not no_ab, False)

    read_vbmeta(top, True, True)
    return order
def load_vbmeta_at(path):
    with open(path, "rb") as fh:
        buf = fh.read()
    hdr = parse_header(buf)
    off = 0
    if hdr is None:
        foot = find_footer(buf)
        if foot is None:
            return buf, None, None
        off, _ = foot
        hdr = parse_header(buf[off:off + HDR_SIZE])
        if hdr is None:
            return buf, None, None
    return buf, off, hdr
def pubkey_blob(buf, off, hdr):
    base = off + HDR_SIZE + hdr["auth_size"]
    s, n = hdr["public_key_offset"], hdr["public_key_size"]
    if n == 0:
        return b""
    return buf[base + s:base + s + n]
def describe_key(blob):
    if not blob:
        return {"present": False}
    bits = int.from_bytes(blob[:4], "big") if len(blob) >= 4 else 0
    n = (bits + 7) // 8 if bits else 0
    kind = "unknown"
    if bits and len(blob) == 8 + 2 * n:
        kind = "RSA (bits|n0inv|n|rr)"
    elif bits and len(blob) == 8 + n:
        kind = "RSA (bits|n0inv|n)"
    elif bits and len(blob) == 4 + 2 * n:
        kind = "EC (bits|x||y)"
    return {"present": True, "key_num_bits": bits, "bytes": len(blob), "kind": kind,
            "sha1": hashlib.sha1(blob).hexdigest()}
def describe_descriptors(buf, off, hdr):
    base = off + HDR_SIZE + hdr["auth_size"]  # auxiliary data block
    desc = buf[base + hdr["descriptors_offset"]:base + hdr["descriptors_offset"] + hdr["descriptors_size"]]
    out = []
    o = 0
    while o + 16 <= len(desc):
        tag = be64(desc, o)
        nbf = be64(desc, o + 8)
        if nbf == 0 or o + 16 + nbf > len(desc):
            break
        name = DESCRIPTOR_TAGS.get(tag, f"tag{tag}")
        extra = ""
        try:
            if tag == 1:  # HASHTREE, avbtool SIZE = 180 (120 + 60 reserved)
                alg = desc[o + 72:o + 104].split(b"\x00")[0].decode("ascii", "replace")
                pn_len, salt_len, dg_len = struct.unpack_from(">III", desc, o + 104)
                p = o + 180
                pn = desc[p:p + pn_len].decode("utf-8", "replace")
                dg = desc[p + pn_len + salt_len:p + pn_len + salt_len + dg_len]
                extra = f" alg={alg} partition={pn!r} root_digest={dg.hex()} salt={salt_len}B"
            elif tag == 2:  # HASH, avbtool SIZE = 132 (72 + 60 reserved)
                alg = desc[o + 24:o + 56].split(b"\x00")[0].decode("ascii", "replace")
                pn_len, salt_len, dg_len = struct.unpack_from(">III", desc, o + 56)
                p = o + 132
                pn = desc[p:p + pn_len].decode("utf-8", "replace")
                dg = desc[p + pn_len + salt_len:p + pn_len + salt_len + dg_len]
                extra = f" alg={alg} partition={pn!r} digest={dg.hex()}"
            elif tag == 0:  # PROPERTY, SIZE = 32 (key/value NUL-padded)
                klen, vlen = struct.unpack_from(">QQ", desc, o + 16)
                k = desc[o + 32:o + 32 + klen].strip(b"\x00").decode("utf-8", "replace")
                v = desc[o + 32 + klen:o + 32 + klen + vlen].strip(b"\x00").decode("utf-8", "replace")
                extra = f" {k}={v!r}"
            elif tag == 3:  # KERNEL_CMDLINE
                fl = be32(desc, o + 16)
                clen = be32(desc, o + 20)
                extra = f" flags={fl} cmdline={desc[o + 32:o + 32 + clen]!r}"
            elif tag == 4:  # CHAIN_PARTITION: 4 u32 + 60 reserved, then name + key
                rl, pn_len, key_len, fl = struct.unpack_from(">IIII", desc, o + 16)
                pn = desc[o + 92:o + 92 + pn_len].decode("utf-8", "replace") if pn_len else "?"
                extra = f" rollback_loc={rl} partition={pn!r} key_len={key_len} flags={fl}"
        except Exception as e:
            extra = f" (parse note: {e})"
        out.append(f"{name}{extra}")
        o = o + 16 + nbf
    return out
def verify_key_mode(args):
    ok_all = True
    for img in args.verify_key:
        print("=" * 78)
        print(f"image: {img}")
        buf, off, hdr = load_vbmeta_at(img)
        if hdr is None:
            print("  !! no AVB0 header at offset 0 and no AVBf footer -> this image can never verify")
            ok_all = False
            continue
        algo = hdr["algorithm_type"]
        print(f"  vbmeta @ {off:#x}  ver {hdr['ver_major']}.{hdr['ver_minor']}"
              f"  struct_size={hdr['total_size']}  release={hdr['release_string']!r}")
        print(f"  algorithm_type={algo} ({ALGO_NAMES.get(algo, 'unknown/newer')})"
              f"  flags={hdr['flags']:#x}"
              f"{'  [VERIFICATION_DISABLED]' if hdr['flags'] & AVB_VBMETA_IMAGE_FLAGS_VERIFICATION_DISABLED else ''}"
              f"  rollback_index={hdr['rollback_index']} loc={hdr['rollback_index_location']}")
        blob = pubkey_blob(buf, off, hdr)
        img_key = describe_key(blob)
        if not img_key["present"]:
            print("  !! no public key in the auth block (unsigned / --flags 2 vbmeta)")
            ok_all = False
        else:
            print(f"  signer key: {img_key['bytes']} B, key_num_bits={img_key['key_num_bits']}, "
                  f"{img_key['kind']}")
            print(f"  signer sha1: {img_key['sha1']}")
            print(f"  hash={hdr['hash_size']} B signature={hdr['signature_size']} B")
        ds = describe_descriptors(buf, off, hdr)
        print(f"  descriptors ({len(ds)}): " + ("; ".join(ds) if ds else "none"))
        if args.dump_pubkey:
            with open(args.dump_pubkey, "wb") as fh:
                fh.write(blob)
            print(f"  -> wrote signer key blob to {args.dump_pubkey} (use as the .avbpubkey)")
        for cand in args.pubkey:
            try:
                with open(cand, "rb") as fh:
                    cblob = fh.read()
            except OSError as e:
                print(f"  candidate {cand}: UNREADABLE ({e})")
                ok_all = False
                continue
            ck = describe_key(cblob)
            same = img_key.get("sha1") == ck.get("sha1")
            print(f"  candidate {cand}: {ck['bytes']} B, bits={ck['key_num_bits']}, sha1={ck['sha1']}"
                  f"  -> {'MATCH' if same else 'MISMATCH'}")
            if not same:
                ok_all = False
                if ck["bytes"] != img_key.get("bytes"):
                    print(f"      size differs: image {img_key.get('bytes')} B vs file {ck['bytes']} B"
                          f" (PEM/QP or the other testkey, or rr omitted)")
    return 0 if ok_all else 1
def parse_triple(s):
    alg, size, digest = None, None, None
    for tok in re.split(r"[\s,]+", s.strip()):
        if "=" in tok:
            k, v = tok.split("=", 1)
            k = k.lower()
            if k in ("hash_alg", "alg", "hashalg"):
                alg = v.lower()
            elif k == "size":
                size = int(v, 0)
            elif k == "digest":
                digest = v.lower()
        elif tok.lower() in ("sha256", "sha512"):
            alg = tok.lower()
        elif re.fullmatch(r"[0-9a-fA-F]{8,128}", tok):
            digest = tok.lower()
        elif tok.isdigit():
            size = int(tok)
        else:
            m = re.fullmatch(r"(\w+):(\d+):([0-9a-fA-F]+)", tok)
            if m:
                alg, size, digest = m.group(1).lower(), int(m.group(2)), m.group(3).lower()
    return alg, size, digest
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", help="directory holding the partition images (for digest modes)")
    ap.add_argument("--verify-key", nargs="*", default=[], metavar="IMG",
                    help="FAIL-FAST key check: report the signer key of each partition image and compare "
                         "against --pubkey candidates (exit 1 if none matches)")
    ap.add_argument("--pubkey", action="append", default=[], metavar="FILE",
                    help="candidate .avbpubkey / key blob to compare (repeatable)")
    ap.add_argument("--dump-pubkey", metavar="FILE",
                    help="write the image's signer key blob to FILE (what avb_keys= must point at)")
    ap.add_argument("--expect-triple", metavar='"hash_alg=.. size=.. digest=.."',
                    help="compare the computed triple with the values currently in the cmdline/props")
    ap.add_argument("--suffix", default="_a", help="slot suffix appended to partition names (default _a; use '' for none)")
    ap.add_argument("--alg", default="sha256", choices=["sha256", "sha512"])
    ap.add_argument("--mode", default="auto", choices=["auto", "main-only", "all-chains"],
                    help="auto = mirror libavb (chains only when the main vbmeta is not "
                         "VERIFICATION_DISABLED); main-only = just the top-level struct; "
                         "all-chains = force walking chain descriptors")
    ap.add_argument("--expect-digest", default="")
    ap.add_argument("--expect-size", type=int, default=0)
    ap.add_argument("--emit-cmdline", action="store_true")
    a = ap.parse_args()
    if a.verify_key:
        sys.exit(verify_key_mode(a))
    if not a.dir:
        ap.error("--dir is required for digest modes (or use --verify-key IMG)")
    order = gather(a.dir, a.suffix, mode=a.mode)
    if not order:
        sys.exit("no vbmeta images found")
    print(f"{'partition':<22} {'offset':>8} {'size':>8}  source")
    h = hashlib.new(a.alg)
    total = 0
    for nm, path, off, size, hdr in order:
        print(f"{nm:<22} {off:>8} {size:>8}  {path}  (avb {hdr['ver_major']}.{hdr['ver_minor']}, flags=0x{hdr['flags']:x})")
        with open(path, "rb") as fh:
            fh.seek(off)
            blob = fh.read(size)
        h.update(blob)
        total += size
    digest = h.hexdigest()
    print(f"\ncount  = {len(order)} vbmeta image(s) hashed")
    print(f"sha_alg= {a.alg}")
    print(f"size   = {total}   (= androidboot.vbmeta.size)")
    print(f"digest = {digest}")
    if a.emit_cmdline:
        print(f"\nandroidboot.vbmeta.hash_alg={a.alg} androidboot.vbmeta.size={total} "
              f"androidboot.vbmeta.digest={digest}")
    ok = True
    if a.expect_size:
        print(f"size check  : {'OK' if total == a.expect_size else 'MISMATCH'} "
              f"(expected {a.expect_size}, got {total})")
        ok &= total == a.expect_size
    if a.expect_digest:
        match = digest.lower() == a.expect_digest.lower().strip()
        print(f"digest check: {'OK' if match else 'MISMATCH'} (expected {a.expect_digest})")
        ok &= match
    if a.expect_triple:
        e_alg, e_size, e_digest = parse_triple(a.expect_triple)
        print(f"\n--- consistency with the triple already in the cmdline/props ---")
        print(f"  in cmdline: alg={e_alg} size={e_size} digest={e_digest}")
        print(f"  computed  : alg={a.alg} size={total} digest={digest}")
        if e_alg and e_alg != a.alg:
            print(f"  ALG MISMATCH: cmdline says {e_alg}, computed with {a.alg}")
            ok = False
        if e_size is not None and e_size != total:
            print(f"  SIZE MISMATCH: cmdline {e_size} vs computed {total}"
                  f"  -> {'flashed vbmeta set' if total < e_size else 'cmdline'}"
                  f" is stale; reflash or update androidboot.vbmeta.size")
            ok = False
        if e_digest and e_digest != digest:
            print(f"  DIGEST MISMATCH: {e_digest} vs {digest}")
            print("  -> the images on the device are NOT the ones the cmdline was built from:"
                  " restore the matching images or update androidboot.vbmeta.digest/size")
            ok = False
        if ok:
            print("  consistent ✔")
    sys.exit(0 if ok else 1)
if __name__ == "__main__":
    main()
