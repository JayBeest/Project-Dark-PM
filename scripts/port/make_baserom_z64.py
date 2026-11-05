#!/usr/bin/env python3
"""Write baserom.u.z64 from rom/007 - GoldenEye (USA).n64.

The extraction scripts and the Makefile both want `baserom.u.z64` (big-endian
z64 order) in the repository root. The ROM in rom/ is byte-swapped (v64 order:
37 80 40 12), which the *game* accepts because port/src/romdata.c sniffs the
header, but the asset extractor reads raw offsets and needs z64 order.

Both orders describe the same bytes; this is a reordering, not a dump.
"""
import hashlib
import pathlib
import sys

ROM = pathlib.Path("rom/007 - GoldenEye (USA).n64")
OUT = pathlib.Path("baserom.u.z64")

STOCK_Z64_SHA1 = "abe01e4aeb033b6c0836819f549c791b26cfde83"

data = ROM.read_bytes()
magic = data[:4].hex()

if magic == "80371240":
    out = data
elif magic == "37804012":
    b = bytearray(len(data))
    b[0::2] = data[1::2]
    b[1::2] = data[0::2]
    out = bytes(b)
elif magic == "40123780":
    b = bytearray(len(data))
    b[0::4], b[1::4], b[2::4], b[3::4] = data[3::4], data[2::4], data[1::4], data[0::4]
    out = bytes(b)
else:
    sys.exit(f"{ROM}: unrecognised header {magic}")

# Reordering does not change which bytes are present, so the length must match.
if len(out) != 12582912:
    sys.exit(f"unexpected size {len(out)}")

OUT.write_bytes(out)
sha = hashlib.sha1(out).hexdigest()
print(f"wrote {OUT} ({len(out)} bytes) sha1 {sha}")
print("stock (U) cart" if sha == STOCK_Z64_SHA1 else f"WARNING: not the stock image (want {STOCK_Z64_SHA1})")
