#!/usr/bin/env python3
"""pngcheck.py FILE [WIDTHxHEIGHT]: whether FILE is a PNG (8-bit, not interlaced, as Qt writes them) of
that size with more than one colour in it. Prints "WxH, N colours" and exits 0 when good, 1 when not.
Only the Python standard library (used by ../test-appimage.sh on the host)."""
import struct
import sys
import zlib


def decode(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, idat, w = 8, [], None
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            w, h, depth, ctype, _comp, _filt, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
    if w is None:
        raise ValueError("no IHDR")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(ctype)
    if depth != 8 or interlace or channels is None:
        return w, h, None   # size known, colours not counted
    raw = zlib.decompress(b"".join(idat))
    bpp, stride = channels, w * channels
    prev = bytearray(stride)
    colours = set()
    i = 0
    for _y in range(h):
        ftype = raw[i]
        line = bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        if ftype == 1:
            for x in range(bpp, stride):
                line[x] = (line[x] + line[x - bpp]) & 255
        elif ftype == 2:
            for x in range(stride):
                line[x] = (line[x] + prev[x]) & 255
        elif ftype == 3:
            for x in range(stride):
                left = line[x - bpp] if x >= bpp else 0
                line[x] = (line[x] + ((left + prev[x]) >> 1)) & 255
        elif ftype == 4:
            for x in range(stride):
                a = line[x - bpp] if x >= bpp else 0
                b = prev[x]
                c = prev[x - bpp] if x >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[x] = (line[x] + pred) & 255
        if len(colours) < 100000:
            mv = bytes(line)
            colours.update(mv[x:x + bpp] for x in range(0, stride, bpp))
        prev = line
    return w, h, len(colours)


def main():
    path = sys.argv[1]
    want = sys.argv[2] if len(sys.argv) > 2 and sys.argv[2] not in ("", "any") else None
    try:
        w, h, n = decode(path)
    except (OSError, ValueError, zlib.error, struct.error) as e:
        print(f"no picture ({e})")
        return 1
    text = f"{w}x{h}, " + ("colours not counted" if n is None else f"{n}{'+' if n >= 100000 else ''} colours")
    ok = (n is None or n > 1) and (want is None or want == f"{w}x{h}")
    if want and want != f"{w}x{h}":
        text += f" (expected {want})"
    print(text)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
