#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
Stock computes vendorCode = raw - 1000 and routes anything < 1000 through its
"Unknown acquiredmsg" path as (2,0). The Goodix HAL on this port emits codes at
base 1021, so stock reports 20/21/22/23 instead of -1/0/1/2 and the FOD
handshake never starts. 5 bytes fix it:

    stock : d7 a2 0f 71  0b 04 00 54    subs w23,w22,#1000 ; b.lt
    fixed : d7 f6 0f 71  1f 20 03 d5    subs w23,w22,#1021 ; nop

    ./patch_mfp_daemon.py <stock> <out>
    ./patch_mfp_daemon.py --check <binary>

Exit: 0 ok/already patched, 1 pattern not found, 2 error.
"""

import argparse
import hashlib
import sys
STOCK = bytes.fromhex("d7a20f71" "0b040054")   # subs w23,w22,#1000 ; b.lt
FIXED = bytes.fromhex("d7f60f71" "1f2003d5")   # subs w23,w22,#1021 ; nop
def sha(b: bytes) -> str:
    return hashlib.md5(b).hexdigest()
def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("output", nargs="?")
    ap.add_argument("--check", action="store_true",
                    help="only report the state, do not write")
    ap.add_argument("--logdir", metavar="DIR",
                    help="also move the HAL log files to DIR "
                         "(e.g. /dev/fpdump for a throwaway tmpfs log)")
    args = ap.parse_args()
    try:
        data = bytearray(open(args.input, "rb").read())
    except OSError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    n_fixed = data.count(FIXED)
    n_stock = data.count(STOCK)
    print(f"input    : {args.input} ({len(data)} bytes, md5 {sha(data)})")
    print(f"stock    : {n_stock} occurrence(s) of {STOCK.hex()}")
    print(f"already  : {n_fixed} occurrence(s) of {FIXED.hex()}")
    if n_fixed and not n_stock:
        print("state    : already patched")
        return 0
    if n_stock != 1:
        print(f"error    : expected exactly 1 stock pattern, found {n_stock}",
              file=sys.stderr)
        print("           binary is a different build - re-derive the pattern",
              file=sys.stderr)
        return 1
    off = data.find(STOCK)
    data[off:off + len(STOCK)] = FIXED
    print(f"patched  : 5 bytes at offset 0x{off:x}")

    if args.logdir:
        logdir = args.logdir.rstrip("/").encode()
        moved = 0
        for name in (b"fp_log.txt", b"fp_log2.txt", b"fp_init_log.txt"):
            old = b"/data/vendor/fpdump/" + name
            new = logdir + b"/" + name
            if len(new) > len(old):
                print(f"error    : {new.decode()} longer than the slot",
                      file=sys.stderr)
                return 1
            pos = data.find(old)
            if pos < 0:
                continue
            data[pos:pos + len(old)] = new + b"\0" * (len(old) - len(new))
            moved += 1
        print(f"logdir   : {moved} path(s) -> {args.logdir}")
    if args.check:
        print("state    : would patch (--check, nothing written)")
        return 0
    if not args.output:
        print("error    : no output path given", file=sys.stderr)
        return 2
    with open(args.output, "wb") as f:
        f.write(data)
    print(f"output   : {args.output} (md5 {sha(bytes(data))})")
    if sha(bytes(data)) == "04afcb7bccc2f3c826685339d5a5440c":
        print("verify   : matches the known-good port build")
    return 0
if __name__ == "__main__":
    sys.exit(main())
