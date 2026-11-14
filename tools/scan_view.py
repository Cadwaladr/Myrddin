#!/usr/bin/env python3
"""scan_view.py PID OUTDIR: summarise a HUDSCAN (before/after each display-size pass) and write previews."""
import sys, glob, re, os
import numpy as np
from PIL import Image
pid, out = sys.argv[1], sys.argv[2]
here = os.path.dirname(os.path.abspath(__file__))
def load(path, w, h):
    return np.fromfile(path, np.float16).reshape(h, w, 4).astype(np.float32)
def tone(a):
    c = np.nan_to_num(a[..., :3], nan=0, posinf=1e4, neginf=0).clip(0, None)
    c = c / (1 + c)
    return (c ** (1 / 2.2) * 255).clip(0, 255).astype(np.uint8)
for before in sorted(glob.glob(f"{here}/../logs/fg/scan-{pid}-*-before-*.bin")):
    m = re.search(r"scan-\d+-(\d+)-before-(\d+)x(\d+)", before)
    i, w, h = m.group(1), int(m.group(2)), int(m.group(3))
    after = before.replace("before", "after")
    if not os.path.exists(after):
        continue
    b, a = load(before, w, h), load(after, w, h)
    d = np.abs(np.nan_to_num(a - b, nan=0, posinf=0, neginf=0))
    changed = d[..., :3].max(-1) > 1e-3
    alpha = d[..., 3] > 1e-3
    ys, xs = np.nonzero(changed)
    box = (xs.min(), ys.min(), xs.max(), ys.max()) if len(xs) else None
    print(f"{i}: {w}x{h} changed={changed.mean()*100:6.2f}% alpha_changed={alpha.mean()*100:6.2f}% "
          f"before_mean={np.nan_to_num(b[..., :3]).clip(0, 100).mean():.4f} after_mean={np.nan_to_num(a[..., :3]).clip(0, 100).mean():.4f} "
          f"alpha before={b[..., 3].mean():.3f} after={a[..., 3].mean():.3f} box={box}")
    s = 2 if w <= 2560 else 3
    row = np.concatenate([tone(b)[::s, ::s], tone(a)[::s, ::s], (changed[::s, ::s, None] * np.uint8(255)).repeat(3, -1)], 1)
    Image.fromarray(row).save(f"{out}/scan-{i}.png")
