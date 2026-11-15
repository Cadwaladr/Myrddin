#!/usr/bin/env python3
"""How well the moving-content rules explain captures of the game's HUD layer: the layer at time 0,
worked out from this capture and the motion found, should be the capture before. For each pair of
captures in a row and each rule set of hudmotion.layer_at: the pixels where it is not.

    proto/hud-motion/explain.py PREFIX FIRST COUNT      (PREFIX-NNN-hud-WxH.bin, as "sc-turbo fg-dump N" writes them)

What is wrong is given as alpha summed over the picture (in pixels' worth): a thin line half a
pixel off its place is nearly right, and counts so. "in place" is what showing this capture as it
is gets wrong at time 0 (the layer without these rules); "in tiles" the part of it in tiles that a
motion was found for or reaches, which is all the rules can put right. Also at time 1, where the
answer is this capture itself.
"""
import glob, re, sys, time
import numpy as np

import hudmotion as hm


def alpha_of(path):
    w, h = (int(v) for v in re.search(r"-hud-(\d+)x(\d+)\.bin$", path).groups())
    return np.clip(np.fromfile(path, dtype=np.float16).reshape(h, w, 4)[:, :, 3].astype(np.float32), 0.0, 1.0)


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    prefix, first, count = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    captures = [alpha_of(glob.glob(f"{prefix}-{first + k:03d}-hud-*.bin")[0]) for k in range(count)]
    sets = {"neither": dict(fill=False, alike=False), "fill": dict(fill=True, alike=False), "fill and alike": dict()}
    print(f"{'pair':>7s} {'in place':>8s} {'in tiles':>8s} {'moving':>6s}  " + "  ".join(f"{k:>22s}" for k in sets) + "   (wrong at time 0 / at time 1)")
    for k in range(1, count):
        a_p, a_n = captures[k - 1], captures[k]
        start = time.time()
        motion, reached = hm.track(a_n, a_p)
        involved = np.repeat(np.repeat((motion[:, :, 2] > 0) | np.array([[len(r) > 0 for r in row] for row in reached]), hm.TILE, axis=0), hm.TILE, axis=1)[: a_n.shape[0], : a_n.shape[1]]
        changed = np.abs(a_n - a_p)
        row = []
        for rules in sets.values():
            zero, _ = hm.layer_at(a_n, a_p, motion, reached, 0.0, **rules)
            one, _ = hm.layer_at(a_n, a_p, motion, reached, 1.0, **rules)
            row.append(f"{int(np.abs(zero - a_p).sum()):12d} / {int(np.abs(one - a_n).sum()):7d}")
        print(f"{k - 1:3d}>{k:<3d} {int(changed.sum()):8d} {int((changed * involved).sum()):8d} {int((motion[:, :, 2] > 0).sum()):6d}  " + "  ".join(row)
              + f"   {time.time() - start:.0f} s", flush=True)


if __name__ == "__main__":
    sys.exit(main())
