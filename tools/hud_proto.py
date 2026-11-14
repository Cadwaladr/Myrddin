#!/usr/bin/env python3
"""Prototype of the HUD mask: a pixel is HUD when "it did not move" explains the two real frames
better than "it moved as the game's vectors say". Usage: hud_proto.py dump-...-real.ppm out.png"""
import sys, glob, re
import numpy as np
from PIL import Image
sys.path.insert(0, __file__.rsplit("/", 1)[0])
from hud_metric import uniform_filter

real_path, out = sys.argv[1], sys.argv[2]
cur = np.asarray(Image.open(real_path), np.float32) / 255
prev = np.asarray(Image.open(real_path.replace("-real", "-previous")), np.float32) / 255
inp = glob.glob(real_path.replace("-real.ppm", "-inputs-*.bin"))[0]
rw, rh = map(int, re.search(r"(\d+)x(\d+)\.bin", inp).groups())
raw = open(inp, "rb").read()
mv = np.frombuffer(raw[: rw * rh * 4], np.float16).reshape(rh, rw, 2).astype(np.float32)
H, W = cur.shape[:2]
# vectors at display resolution, in display pixels
ys, xs = np.mgrid[0:H, 0:W]
sx, sy = W / rw, H / rh
mvd = mv[np.clip((ys / sy).astype(int), 0, rh - 1), np.clip((xs / sx).astype(int), 0, rw - 1)] * [sx, sy]
px = np.clip(xs + mvd[..., 0], 0, W - 1.001); py = np.clip(ys + mvd[..., 1], 0, H - 1.001)
x0 = px.astype(int); y0 = py.astype(int); fx = (px - x0)[..., None]; fy = (py - y0)[..., None]
warped = (prev[y0, x0] * (1 - fx) + prev[y0, x0 + 1] * fx) * (1 - fy) + (prev[y0 + 1, x0] * (1 - fx) + prev[y0 + 1, x0 + 1] * fx) * fy
K = int(sys.argv[3]) if len(sys.argv) > 3 else 7
e_static = uniform_filter(np.abs(cur - prev).max(axis=2), K)
e_moved = uniform_filter(np.abs(cur - warped).max(axis=2), K)
moving = np.hypot(mvd[..., 0], mvd[..., 1]) > 1.0
mask = moving & (e_static + 0.004 < 0.6 * e_moved)
print(f"mask {mask.mean()*100:.1f} % of the frame; e_static mean {e_static.mean():.4f}, e_moved mean {e_moved.mean():.4f}; median |mv| {np.median(np.hypot(mvd[...,0],mvd[...,1])):.1f} px")
m = (mask * 255).astype(np.uint8)
c = Image.new("RGB", (W, H // 2))
c.paste(Image.fromarray((cur * 255).astype(np.uint8)).resize((W // 2, H // 2)), (0, 0))
c.paste(Image.fromarray(m).resize((W // 2, H // 2)).convert("RGB"), (W // 2, 0))
c.save(out)
