#!/usr/bin/env python3
"""Filter ALL Xiaomi light-family sensors out of sensors.ssc.so.

Extends the original nolight filter (`cf3b9775`, which dropped type 5 and the
raw ALS 33171111) with the two remaining Xiaomi light sensors:

  33171007 (0x1FA263F)  xiaomi.sensor.ambientlight.factory  (handle 0x0100042f)
  33171039 (0x1FA265F)  xiaomi.sensor.oem13                 (0x0100056f/570)
"""
import hashlib
import os

HERE = os.path.dirname(os.path.abspath(__file__))
GYRO = os.path.normpath(os.path.join(HERE, "..", ".."))

BASE = os.path.join(GYRO, "sensors.ssc.so")
OUT = os.path.join(GYRO, "sensors.ssc.so-nolight")
BASE_MD5 = "7222ebb9ae7a71ac6e99a43393152203"

FILTER_HOOK = 0x56134
FILTER_CAVE = 0x41500
CAVE_SIZE = 0x50
NEXT_TARGET = FILTER_HOOK - 0x0C
KEEP_TARGET = FILTER_HOOK + 0x08
DROP_TYPES = [5, 0x1FA263F, 0x1FA265F, 0x1FA26A7]
FIRST_BIG = 0x1FA0000
NOP = 0xD503201F


def b(pc, t):
    return 0x14000000 | (((t - pc) >> 2) & 0x3FFFFFF)

def bcond(pc, t, c):
    return 0x54000000 | ((((t - pc) >> 2) & 0x7FFFF) << 5) | c

def cbz64(pc, t, rt):
    return 0xB4000000 | ((((t - pc) >> 2) & 0x7FFFF) << 5) | rt

def ldr_w(rt, rn, imm):
    return 0xB9400000 | ((imm >> 2) << 10) | (rn << 5) | rt

def ldr_x(rt, rn, imm):
    return 0xF9400000 | ((imm >> 3) << 10) | (rn << 5) | rt

def cmp_w(i, rn):
    return 0x7100001F | ((i & 0xFFF) << 10) | (rn << 5)

def cmp_wr(rn, rm):
    return 0x6B00001F | (rm << 16) | (rn << 5)

def movz_w(rd, i):
    return 0x52800000 | ((i & 0xFFFF) << 5) | rd

def movk_w(rd, i, sh):
    return 0x72800000 | (((sh // 16) & 3) << 21) | ((i & 0xFFFF) << 5) | rd

def W(*ins):
    return b"".join(i.to_bytes(4, "little") for i in ins)

def filter_cave():
    c = FILTER_CAVE
    exit_ = c + 0x48
    keep = c + 0x44
    ins = [
        ldr_x(8, 23, 0),                       # 0x00 ldr  x8,[x23]
        cbz64(c + 0x04, exit_, 8),             # 0x04 cbz  x8,exit
        ldr_w(11, 8, 0xA0),                    # 0x08 ldr  w11,[x8,#0xa0]
    ]
    for t in DROP_TYPES:
        if t < FIRST_BIG:
            ins.append(cmp_w(t, 11))           # cmp  w11,#imm
            ins.append(bcond(c + len(ins) * 4, exit_, 0))  # b.eq exit
        else:
            ins.append(movz_w(12, t & 0xFFFF))                 # movz w12,#lo
            ins.append(movk_w(12, (t >> 16) & 0xFFFF, 16))     # movk w12,#hi
            ins.append(cmp_wr(11, 12))                         # cmp  w11,w12
            ins.append(bcond(c + len(ins) * 4, exit_, 0))      # b.eq exit
    ins.append(b(keep, KEEP_TARGET))           # keep
    ins.append(b(exit_, NEXT_TARGET))          # next/exit
    return W(*ins)


def main():
    base = open(BASE, "rb").read()
    got = hashlib.md5(base).hexdigest()
    assert got == BASE_MD5, f"unexpected base md5 {got}"
    hook = int.from_bytes(base[FILTER_HOOK:FILTER_HOOK + 4], "little")
    assert hook == 0xF94002E8, f"hook site is {hook:#x}, expected ldr x8,[x23]"
    d = bytearray(base)
    d[FILTER_HOOK:FILTER_HOOK + 4] = b(FILTER_HOOK, FILTER_CAVE).to_bytes(4, "little")
    cave = filter_cave()
    assert len(cave) <= CAVE_SIZE, len(cave)
    d[FILTER_CAVE:FILTER_CAVE + len(cave)] = cave
    open(OUT, "wb").write(d)
    print("base :", got)
    print("out  :", hashlib.md5(d).hexdigest(), OUT)
    print("hook :", d[FILTER_HOOK:FILTER_HOOK + 4].hex())
    print("cave :", d[FILTER_CAVE:FILTER_CAVE + CAVE_SIZE].hex())

if __name__ == "__main__":
    main()
