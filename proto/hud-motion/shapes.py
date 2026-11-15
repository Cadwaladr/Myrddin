#!/usr/bin/env python3
"""Scenes with a known answer for HUD content that moves over the screen: a shape drawn at two
places (the capture before, this capture) over things that stay where they are, and the layer as
it should be in between. For each scene and motion: how many pixels of hudmotion.layer_at's
answer differ from that, by rule set.

    proto/hud-motion/shapes.py [T ...]          (default: 0.5)

The scenes: "marker" is the test program's (probe/test_chain.c --marker: a frame, a block, four
bars); "panel" a filled box with a border and strokes in it, as a tooltip is. Each opaque, half
transparent, and over a half-transparent field that stands still.
"""
import sys
import numpy as np

import hudmotion as hm

W, H = 1280, 720
MARKER = [(0, 0, 72, 3), (0, 45, 72, 3), (0, 3, 3, 42), (69, 3, 3, 42), (12, 12, 20, 24), (40, 14, 3, 20), (46, 14, 3, 20), (52, 14, 3, 20), (58, 14, 3, 20)]
STILL = [(40, 40, 200, 3), (40, 40, 3, 100), (300, 200, 120, 60)]


def over(top, under):
    return top + (1.0 - top) * under


def marker(alpha):
    a = np.zeros((48, 72), dtype=np.float32)
    for x, y, w, h in MARKER:
        a[y : y + h, x : x + w] = alpha
    return a


def panel(fill):
    """220 x 90: a fill, an opaque border of 2, and opaque strokes (rows of dashes, as text is)."""
    a = np.full((90, 220), fill, dtype=np.float32)
    a[:2] = a[-2:] = 1.0
    a[:, :2] = a[:, -2:] = 1.0
    for row in range(3):
        for k in range(14):
            x = 14 + k * 14
            a[16 + row * 24 : 16 + row * 24 + 12, x : x + 2 + (k * 7 + row * 3) % 9] = 1.0
    return a


def still(field):
    a = np.zeros((H, W), dtype=np.float32)
    if field:
        a[150:560, 250:900] = field
    for x, y, w, h in STILL:
        a[y : y + h, x : x + w] = over(0.5, a[y : y + h, x : x + w])
    return a


def scene(shape, at, under):
    a = np.zeros((H, W), dtype=np.float32)
    h, w = shape.shape
    a[at[1] : at[1] + h, at[0] : at[0] + w] = shape
    return over(a, under)


def cases():
    for name, shape in (("marker", marker(1.0)), ("marker, half", marker(0.5)), ("panel", panel(1.0)), ("panel, 0.4", panel(0.4))):
        for field in (0.0, 0.5):
            for v in ((14, 0), (0, -9), (11, 7), (-90, -40), (3, 0), (4, 2), (-6, 8), (24, -2)):
                yield f"{name}{', over a field' if field else ''}", shape, field, v


def main():
    times = [float(v) for v in sys.argv[1:]] or [0.5]
    sets = {"neither": dict(fill=False, alike=False), "fill": dict(fill=True, alike=False), "fill and alike": dict()}
    totals = {k: 0 for k in sets}
    print(f"{'scene':28s} {'motion':>10s} {'t':>5s} " + " ".join(f"{k:>20s}" for k in sets) + "   (pixels that differ from the answer; in brackets: of them, where nothing moves)")
    for name, shape, field, v in cases():
        under = still(field)
        start = (400, 300)
        a_p = scene(shape, start, under)
        a_n = scene(shape, (start[0] + v[0], start[1] + v[1]), under)
        motion, reached = hm.track(a_n, a_p)
        found = sorted({(round(float(m[0])), round(float(m[1]))) for m in motion.reshape(-1, 3) if m[2] > 0})
        for t in times:
            # the answer: the shape on its way (at the nearest whole pixel)
            at = (start[0] + int(np.floor(v[0] * t + 0.5)), start[1] + int(np.floor(v[1] * t + 0.5)))
            answer = scene(shape, at, under)
            row = []
            for key, rules in sets.items():
                out, _ = hm.layer_at(a_n, a_p, motion, reached, t, **rules)
                wrong = np.abs(out - answer) > 0.04
                # the answer's place is rounded: a pixel is right when it has the answer of a place one pixel around
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        wrong &= np.abs(out - scene(shape, (at[0] + dx, at[1] + dy), under)) > 0.04
                outside = wrong & (np.abs(a_n - a_p) <= 0.04) & (np.abs(answer - a_n) <= 0.04)
                row.append(f"{int(wrong.sum()):13d} [{int(outside.sum()):4d}]")
                totals[key] += int(wrong.sum())
            print(f"{name:28s} {str(v):>10s} {t:5.2f} " + " ".join(row) + ("" if found == [(-v[0], -v[1])] else f"   field: {found}"))
    print(f"{'in all':45s} " + " ".join(f"{totals[k]:20d}" for k in sets))


if __name__ == "__main__":
    main()
