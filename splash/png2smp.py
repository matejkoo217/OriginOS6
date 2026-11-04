#!/usr/bin/env python3
import struct
import sys
from PIL import Image

if len(sys.argv) != 3:
    print(f"Usage: {sys.argv[0]} input.png output.rgb")
    sys.exit(1)

img = Image.open(sys.argv[1]).convert("RGB")
width, height = img.size

with open(sys.argv[2], "wb") as f:
    f.write(b"SMP1")
    f.write(struct.pack("<II", width, height))
    f.write(img.tobytes())

print(f"Created {sys.argv[2]}: {width}x{height}, RGB888")
