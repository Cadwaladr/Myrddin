#!/usr/bin/env python3
"""hudmotion.py against a known motion: a captured HUD layer turned by a small view rotation (at a
wide field of view the motion grows towards the screen's edges), so that the right answer is known.
    known-motion.py PID DUMP_NUMBER CENTRE_PIXELS [CENTRE_PIXELS ...] [-b X0,Y0,X1,Y1] [-x X0] [-d] [-p]
CENTRE_PIXELS: the motion at the screen centre per frame. -b: only the content in that box moves;
-x: only content right of X0; -d: list the tiles whose motion is off by more than 1.5 pixels;
-p: pictures (the true half-way layer, ours, the layer as it is) into HUD_MOTION_OUT."""
import os, sys, time
import numpy as np
from PIL import Image
HERE = os.path.dirname(os.path.realpath(__file__))
sys.path.insert(0, HERE)
import hudmotion as hm
D = os.path.join(os.path.dirname(os.path.dirname(HERE)), "logs", "fg") + os.sep   # where sc-turbo fg-dump writes
OUT = os.environ.get("HUD_MOTION_OUT", ".")                                       # where pictures go
args = [a for a in sys.argv[1:] if not a.startswith("-") and "," not in a]
x_from = int(sys.argv[sys.argv.index("-x") + 1]) if "-x" in sys.argv else 0
if "-x" in sys.argv:
    args.remove(str(x_from))
pid, n = args[0], int(args[1])
base = np.clip(hm.load(D, pid, n)[:, :, 3].astype(np.float32), 0, 1)
H, W = base.shape
F = (W / 2) / np.tan(np.radians(60.0))  # 120 degrees across


def sample(img, x, y):
    x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
    fx, fy = x - x0, y - y0
    def at(yy, xx):
        ok = (yy >= 0) & (yy < H) & (xx >= 0) & (xx < W)
        return np.where(ok, img[np.clip(yy, 0, H - 1), np.clip(xx, 0, W - 1)], 0.0)
    return (at(y0, x0) * (1 - fx) * (1 - fy) + at(y0, x0 + 1) * fx * (1 - fy) + at(y0 + 1, x0) * (1 - fx) * fy + at(y0 + 1, x0 + 1) * fx * fy).astype(np.float32)


def turned(img, yaw, pitch):
    """The picture after the view turned by (yaw, pitch): -> picture, and for each of its pixels
    where that content was (sx, sy)."""
    ys, xs = np.mgrid[0:H, 0:W].astype(np.float64)
    x, y = xs + 0.5 - W / 2, ys + 0.5 - H / 2
    # direction of the pixel in the new view, turned back into the old one
    d = np.stack([x, y, np.full_like(x, F)], axis=-1)
    cy, sy_ = np.cos(yaw), np.sin(yaw)
    cp, sp = np.cos(pitch), np.sin(pitch)
    ry = np.array([[cy, 0, sy_], [0, 1, 0], [-sy_, 0, cy]])
    rp = np.array([[1, 0, 0], [0, cp, -sp], [0, sp, cp]])
    o = d @ (ry @ rp).T
    sx = o[..., 0] / o[..., 2] * F + W / 2 - 0.5
    sy = o[..., 1] / o[..., 2] * F + H / 2 - 0.5
    return sample(img, sx, sy), sx, sy


moving = np.zeros_like(base)
if "-b" in sys.argv:
    bx0, by0, bx1, by1 = (int(v) for v in sys.argv[sys.argv.index("-b") + 1].split(","))
    moving[by0:by1, bx0:bx1] = base[by0:by1, bx0:bx1]
else:
    moving[:, x_from:] = base[:, x_from:]
still = base - moving
for centre in [float(v) for v in args[2:]]:
    yaw = centre / F
    pitch = -0.15 * yaw
    before = np.maximum(still, moving)
    now_m, sx, sy = turned(moving, yaw, pitch)
    now = np.maximum(still, now_m)
    half_m, _, _ = turned(moving, yaw / 2, pitch / 2)
    half = np.maximum(still, half_m)
    start = time.time()
    logs = {}
    motion, reached = hm.track(now, before, logs)
    mid, kind = hm.layer_at(now, before, motion, reached, 0.5)
    took = time.time() - start
    # the true motion at the centres of tiles: content at x in "now" was at (sx, sy) in "before"
    ys, xs = np.mgrid[0:H, 0:W]
    tx_true, ty_true = sx - xs, sy - ys
    errs, miss = [], 0
    tiles = logs["tiles"]
    for ty in range(motion.shape[0]):
        for tx in range(motion.shape[1]):
            if tiles[ty, tx, 2] in (hm.FITS, hm.NOT):
                cx, cy = min(tx * 64 + 32, W - 1), min(ty * 64 + 32, H - 1)
                if cx < x_from:
                    continue
                if motion[ty, tx, 2] > 0:
                    e = max(abs(motion[ty, tx, 0] - tx_true[cy, cx]), abs(motion[ty, tx, 1] - ty_true[cy, cx]))
                    errs.append(e)
                    if e > 1.5 and "-d" in sys.argv:
                        t = tiles[ty, tx]
                        print(f"   tile ({tx},{ty}): field {motion[ty, tx, 0]:.1f},{motion[ty, tx, 1]:.1f} true {tx_true[cy, cx]:.1f},{ty_true[cy, cx]:.1f}; its own {t[0]},{t[1]} state {t[2]} cost {t[3] / max(t[4], 1):.2f} zero {t[5] / max(t[4], 1):.2f} content {t[4] // 1023}; searched {tuple(logs['mine'][ty, tx])}")
                else:
                    miss += 1
    errs = np.array(errs) if errs else np.zeros(1)
    def box4(v):
        c = np.cumsum(np.cumsum(np.pad(v.astype(np.float64), ((4, 0), (4, 0))), axis=0), axis=1)
        return (c[4:, 4:] - c[:-4, 4:] - c[4:, :-4] + c[:-4, :-4]) / 16.0
    content = float(half.sum())
    stepping = float(np.abs(box4(now) - box4(half)).sum()) / content
    ours = float(np.abs(box4(mid) - box4(half)).sum()) / content
    counts = logs["counts"]
    shares = sorted(((logs["total"][l], 100 * logs["fits"].get(l, 0) // max(logs["total"][l], 1), counts[l]) for l in logs["total"]), reverse=True)[:4]
    print(f"centre {centre:5.1f} px (edge {abs(tx_true[H // 2, W - 40]):.0f}): tiles with content {len(errs) + miss}: {miss} not moved; field error mean {errs.mean():.2f} max {errs.max():.2f} px, >1.5 px: {int((errs > 1.5).sum())}; "
          f"layer at 1/2 differs from the true one by {ours:.3f} of the content (as it is now, stepping: {stepping:.3f}); pieces (content, % fit, moves): {shares} ({took:.0f} s)")
    if "-p" in sys.argv:
        x0, y0, x1, y1 = 1280, 380, 2560, 790
        rows = [half, mid, now]
        sheet = Image.new("L", (x1 - x0, (y1 - y0 + 6) * 3), 90)
        for k, v in enumerate(rows):
            sheet.paste(Image.fromarray((np.sqrt(np.clip(v[y0:y1, x0:x1], 0, 1)) * 255).astype(np.uint8)), (0, k * (y1 - y0 + 6)))
        sheet.save(f"{OUT}/known-{n}-{int(centre)}.png")
