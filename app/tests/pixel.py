#!/usr/bin/env python3
"""Is the pixel at X,Y of a PNG picture the colour R G B, give or take 14? (negative X or Y: from
the right or the bottom). Exit 0 when it is. Reads 8-bit RGB and RGBA pictures; no library needed.

    tests/pixel.py PICTURE X Y R G B
"""
import struct
import sys
import zlib


def rows(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        sys.exit(f"{path}: not a PNG picture")
    pos, parts, head = 8, [], None
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        if kind == b"IHDR":
            head = struct.unpack(">IIBBBBB", data[pos + 8:pos + 21])
        if kind == b"IDAT":
            parts.append(data[pos + 8:pos + 8 + n])
        pos += 12 + n
    width, height, depth, colour = head[:4]
    if depth != 8 or colour not in (2, 6):
        sys.exit(f"{path}: only 8-bit RGB and RGBA pictures are read")
    bpp = 3 if colour == 2 else 4
    raw = zlib.decompress(b"".join(parts))
    stride = width * bpp + 1
    out, prev = [], bytearray(stride - 1)
    for y in range(height):
        kind = raw[y * stride]
        line = bytearray(raw[y * stride + 1:(y + 1) * stride])
        for i in range(len(line)):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if kind == 1:
                line[i] = (line[i] + a) & 255
            elif kind == 2:
                line[i] = (line[i] + b) & 255
            elif kind == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out.append(line)
        prev = line
    return out, width, height, bpp


def main():
    if len(sys.argv) != 7:
        sys.exit(__doc__)
    lines, width, height, bpp = rows(sys.argv[1])
    x, y, r, g, b = (int(v) for v in sys.argv[2:7])
    x, y = x % width, y % height
    got = tuple(lines[y][x * bpp:x * bpp + 3])
    if all(abs(a - e) <= 14 for a, e in zip(got, (r, g, b))):
        return 0
    print(f"pixel {x},{y} is {got}, not {(r, g, b)}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
