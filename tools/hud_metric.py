#!/usr/bin/env python3
"""HUD error of frame-generation dumps: over the pixels that are HUD-like (unchanged between the
previous and the current real frame, bright, standing out from their surroundings), the mean
absolute difference between the generated frame and the real one (0-255). 0 = HUD untouched.
Usage: hud_metric.py logs/fg/dump-PID-NNN-real.ppm ..."""
import sys
import numpy as np
from PIL import Image


def uniform_filter(a, k):
    """Box mean of size k (odd), edges padded."""
    r = k // 2
    p = np.pad(a, r, mode="edge").astype(np.float64)
    c = np.cumsum(np.cumsum(p, axis=0), axis=1)
    c = np.pad(c, ((1, 0), (1, 0)))
    h, w = a.shape
    return ((c[k:k + h, k:k + w] - c[:h, k:k + w] - c[k:k + h, :w] + c[:h, :w]) / (k * k)).astype(np.float32)

if __name__ == "__main__":
    for real_path in sys.argv[1:]:
        real = np.asarray(Image.open(real_path), np.float32)
        prev = np.asarray(Image.open(real_path.replace("-real", "-previous")), np.float32)
        gen = np.asarray(Image.open(real_path.replace("-real", "-generated")), np.float32)
        luma = real.max(axis=2)
        same = np.abs(real - prev).max(axis=2) < 8
        moving = uniform_filter((~same).astype(np.float32), 31) > 0.3   # the scene around it changes
        hud = same & (luma > 60) & (np.abs(luma - uniform_filter(luma, 9)) > 25) & moving
        err = np.abs(gen - real).max(axis=2)
        scene = ~same
        print(f"{real_path.split('/')[-1]}: HUD pixels {hud.sum():7d}  HUD error {err[hud].mean() if hud.any() else 0:6.2f}"
              f"  (p95 {np.percentile(err[hud], 95) if hud.any() else 0:5.1f})  scene change {np.abs(real - prev).max(axis=2)[scene].mean() if scene.any() else 0:5.1f}")
