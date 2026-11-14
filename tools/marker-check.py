#!/usr/bin/env python3
"""A dump of a test with a moving marker (test_chain --marker): is the marker, in the generated
frame, where it is on its way from the frame before to this one, whole, and is what stays in
place untouched?

    tools/marker-check.py logs/test/NAME/dump-PID-NNN [MULTIPLIER]     (default 2: the frame at 1/2)

The dump's generated picture is the first of the generated frames: at 1/MULTIPLIER of the way.
Prints one line per dump and "ok" or "NOT ok".
"""
import glob, sys
import numpy as np


def ppm(path):
    data = open(path, "rb").read()
    end = 0
    for _ in range(3):  # "P6", "W H", "255": three lines
        end = data.index(b"\n", end) + 1
    w, h = (int(v) for v in data.split(b"\n", 2)[1].split())
    return np.frombuffer(data, dtype=np.uint8, offset=end).reshape(h, w, 3).astype(np.int32)


def magenta(p):  # the marker
    return (p[:, :, 0] > 200) & (p[:, :, 1] < 110) & (p[:, :, 2] > 170)


def cyan(p):  # what stays in place
    return (p[:, :, 0] < 90) & (p[:, :, 1] > 180) & (p[:, :, 2] > 190)


def place(mask):
    ys, xs = np.nonzero(mask)
    return (xs.min(), ys.min()) if len(xs) else None


def check(prefix, multiplier):
    previous, real, generated = (ppm(f"{prefix}-{n}.ppm") for n in ("previous", "real", "generated"))
    was, now, shown = magenta(previous), magenta(real), magenta(generated)
    a, b, c = place(was), place(now), place(shown)
    if a is None or b is None:
        return f"{prefix}: no marker in the frames", False
    t = 1.0 / multiplier
    moved = (b[0] - a[0], b[1] - a[1])
    want = (a[0] + int(np.floor(moved[0] * t + 0.5)), a[1] + int(np.floor(moved[1] * t + 0.5)))
    # the marker as it is in this frame, put where it should be shown: every pixel that differs
    # (the corner may round the other way: the best of the places one pixel around)
    wrong = None
    if c is not None:
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                shift = (want[0] + dx - b[0], want[1] + dy - b[1])
                ideal = np.roll(np.roll(now, shift[1], axis=0), shift[0], axis=1)
                n = int((ideal != shown).sum())
                if wrong is None or n < wrong[0]:
                    wrong = (n, (want[0] + dx, want[1] + dy))
    size = int(now.sum())
    still_now, still_shown = cyan(real), cyan(generated)
    # what stays in place: the same as in this frame, but for where the marker is over it in either
    lost = int((still_now & ~still_shown & ~shown & ~now).sum()) + int((still_shown & ~still_now & ~now & ~shown).sum())
    ok = c is not None and wrong[0] <= 0.03 * size and abs(c[0] - want[0]) <= 1 and abs(c[1] - want[1]) <= 1 and lost <= 0.01 * max(int(still_now.sum()), 1)
    line = (f"{prefix}: marker moved by {moved}; at 1/{multiplier} it should be at {want}, is at {c}; "
            f"{wrong[0] if wrong else size} of its {size} pixels wrong; {lost} pixels of what stays in place differ")
    if moved == (0, 0):
        line += " (it did not move between these frames: nothing shown by this one)"
    return line, ok


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    multiplier = int(sys.argv[2]) if len(sys.argv) > 2 else 2
    prefixes = sorted(p[: -len("-real.ppm")] for p in glob.glob(sys.argv[1] + "*-real.ppm"))
    if not prefixes:
        print(sys.argv[1] + ": no dump")
        return 1
    bad = 0
    for prefix in prefixes:
        line, ok = check(prefix, multiplier)
        print(line, "ok" if ok else "NOT ok")
        bad += not ok
    return 1 if bad else 0


sys.exit(main())
