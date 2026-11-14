#!/usr/bin/env python3
"""Is the performance panel in a panel dump (logs/.../paneldump-*.ppm, made on the control file
PANELDUMP)? Looks for its label's own green and its figures' white inside its dark box.

    tools/panel-seen.py FILE.ppm ...     -> one line per file: "yes" or "no", and the counts
"""
import sys


def read(path):
    with open(path, "rb") as f:
        data = f.read()
    # "P6\n<width> <height>\n255\n", then the pixels (which may begin with bytes that look like spaces)
    end = 0
    for _ in range(3):
        end = data.index(b"\n", end) + 1
    kind, w, h, _ = data[:end].split()
    assert kind == b"P6", path
    return int(w), int(h), data[end : end + int(w) * int(h) * 3]


for path in sys.argv[1:]:
    w, h, px = read(path)
    green = white = dark = 0
    for i in range(0, len(px), 3):
        r, g, b = px[i], px[i + 1], px[i + 2]
        if abs(r - 97) < 8 and abs(g - 230) < 8 and abs(b - 168) < 8:
            green += 1
        elif r > 236 and g > 238 and b > 240:
            white += 1
        elif r < 70 and g < 75 and b < 85:
            dark += 1
    seen = green >= 20 and white >= 60 and dark >= w * h // 8
    print(f"{'yes' if seen else 'no'} ({green} green, {white} white, {100 * dark // (w * h)} % dark) {path}")
