#!/usr/bin/env python3
import hashlib
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
GYRO = os.path.dirname(os.path.dirname(ROOT))
SRC = os.path.join(GYRO, "android.hardware.sensors-service.multihal-v2rev")
OUT = os.path.join(GYRO, "android.hardware.sensors-service.multihal-v3")

REV_TABLE = 0x5F67
FWD_TABLE = 0x5F8D
REV_DATA = 0x39
FWD_DATA = 0x2A
TYPES = [3, 5, 6, 8, 36]
NOP = bytes.fromhex("1f2003d5")

def main():
    data = bytearray(open(SRC, "rb").read())
    for t in TYPES:
        if data[FWD_TABLE + t] != FWD_DATA:
            sys.exit(f"forward table entry {t} is {data[FWD_TABLE+t]:#x}, expected {FWD_DATA:#x}")
    for t in TYPES:
        old = data[REV_TABLE + t]
        data[REV_TABLE + t] = REV_DATA
        print(f"reverse table type {t:2d}: {old:#04x} -> {REV_DATA:#04x}")
    assert data[0x144E4:0x144E8] == NOP, "v2rev guard NOP missing"
    open(OUT, "wb").write(data)
    print("wrote", OUT, hashlib.md5(data).hexdigest())
    print("forward :", " ".join(f"{b:02x}" for b in data[FWD_TABLE:FWD_TABLE + 0x26]))
    print("reverse :", " ".join(f"{b:02x}" for b in data[REV_TABLE:REV_TABLE + 0x26]))

if __name__ == "__main__":
    main()
