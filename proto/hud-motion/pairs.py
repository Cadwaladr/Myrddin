#!/usr/bin/env python3
"""hudmotion.py on consecutive captured HUD layers (frames saved in a row by "sc-turbo fg-dump N"):
    pairs.py PID [-v] [-p] N [N ...]     pairs N-1 -> N of logs/fg/dump-PID-NNN-hud-*.bin
-v: every tile's decision; -p: the layer half way between the two, as a picture (red: the capture
before, blue: this one, green: half way), into HUD_MOTION_OUT (default: the current folder)."""
import os, sys, time, collections
import numpy as np
from PIL import Image
HERE = os.path.dirname(os.path.realpath(__file__))
sys.path.insert(0, HERE)
import hudmotion as hm
D = os.path.join(os.path.dirname(os.path.dirname(HERE)), "logs", "fg") + os.sep   # where sc-turbo fg-dump writes
OUT = os.environ.get("HUD_MOTION_OUT", ".")                                       # where pictures go
pid = sys.argv[1]
def alpha(n):
    a = hm.load(D, pid, n)[:, :, 3].astype(np.float32)
    return np.clip(a, 0, 1)
verbose, pictures = "-v" in sys.argv, "-p" in sys.argv
for n in [int(v) for v in sys.argv[2:] if not v.startswith("-")]:
    a, p = alpha(n), alpha(n - 1)
    logs = {}
    start = time.time()
    motion, reached = hm.track(a, p, logs)
    took = time.time() - start
    tiles, label, counts, fits, total = logs["tiles"], logs["label"], logs["counts"], logs["fits"], logs["total"]
    states = collections.Counter(int(s) for s in tiles[:, :, 2].ravel())
    print(f"{n - 1} -> {n}: {logs['active']} search; tiles: {states[1]} fit, {states[2]} do not, {states[3]} too little; {int((motion[:, :, 2] > 0).sum())} move ({took:.1f} s)")
    for l in sorted(total):
        members = np.argwhere(label == l)
        x0, y0, x1, y1 = members[:, 1].min(), members[:, 0].min(), members[:, 1].max(), members[:, 0].max()
        ms = [tuple(int(v) for v in tiles[ty, tx, :2]) for ty, tx in members if tiles[ty, tx, 2] == hm.FITS]
        span = f"motions {min(m[0] for m in ms)}..{max(m[0] for m in ms)}, {min(m[1] for m in ms)}..{max(m[1] for m in ms)}" if ms else "no motion"
        print(f"   piece at tiles ({x0},{y0})-({x1},{y1}), {len(members)} tiles: {100 * fits.get(l, 0) // max(total[l], 1)} % of {total[l] // 1023} fits -> {'MOVES' if counts[l] else 'stays'}; {span}")
    if verbose:
        rows = collections.defaultdict(list)
        for ty, tx in np.argwhere(tiles[:, :, 2] > 0):
            mx, my, state, cost, content, zero = tiles[ty, tx]
            mark = {1: "", 2: "!", 3: "~"}[int(state)]
            final = f"=>{motion[ty, tx, 0]:.1f},{motion[ty, tx, 1]:.1f}" if motion[ty, tx, 2] > 0 else ""
            rows[ty].append(f"{tx}:{mark}" + (f"{mx},{my}({cost / max(content, 1):.2f}/{zero / max(content, 1):.2f})" if state != 3 else "") + final)
        for ty in sorted(rows):
            print(f"   row {ty:2d}: " + " ".join(rows[ty]))
    if pictures:
        mid, kind = hm.layer_at(a, p, motion, reached, 0.5)
        np.save(f"{OUT}/mid-{n:03d}.npy", mid.astype(np.float16))
        rgb = np.zeros(a.shape + (3,), dtype=np.uint8)
        rgb[:, :, 0] = (np.sqrt(p) * 255).astype(np.uint8)      # before: red
        rgb[:, :, 2] = (np.sqrt(a) * 255).astype(np.uint8)      # this: blue
        rgb[:, :, 1] = (np.sqrt(mid) * 255).astype(np.uint8)    # half way: green
        Image.fromarray(rgb).save(f"{OUT}/mid-{n:03d}.png")
