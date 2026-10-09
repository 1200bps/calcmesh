#!/usr/bin/env python3
"""Exit 0 if two PNGs have the same pixels (8-bit grey or RGB, unfiltered or
filter 0 only, as host/png.c and tools/screen.py write them)."""
import struct
import sys
import zlib


def pixels(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", path
    i, idat, hdr = 8, b"", None
    while i < len(data):
        n, kind = struct.unpack(">I4s", data[i : i + 8])
        body = data[i + 8 : i + 8 + n]
        if kind == b"IHDR":
            hdr = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body
        i += 12 + n
    w, h, depth, ctype = hdr[:4]
    assert depth == 8 and ctype in (0, 2), (path, hdr)
    ch = 3 if ctype == 2 else 1
    raw = zlib.decompress(idat)
    out = []
    for y in range(h):
        row = raw[y * (w * ch + 1) : (y + 1) * (w * ch + 1)]
        assert row[0] == 0, "filtered row"
        px = row[1:]
        out.append(px if ch == 3 else bytes(v for v in px for _ in range(3)))
    return w, h, out


a, b = pixels(sys.argv[1]), pixels(sys.argv[2])
if a != b:
    print(f"{sys.argv[1]} and {sys.argv[2]} differ", file=sys.stderr)
    sys.exit(1)
