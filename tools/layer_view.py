#!/usr/bin/env python3
"""layer_view.py PID INDEX OUT.png: checks a layer-mode dump (real, scene without HUD, HUD layer, generated)."""
import sys, os
import numpy as np
from PIL import Image
pid, idx, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
base = f"{os.path.dirname(os.path.abspath(__file__))}/../logs/fg/dump-{pid}-{idx:03d}"
real = np.asarray(Image.open(base + "-real.ppm")).astype(np.float32) / 255
scene = np.asarray(Image.open(base + "-scene.ppm")).astype(np.float32) / 255
gen = np.asarray(Image.open(base + "-generated.ppm")).astype(np.float32) / 255
h, w = real.shape[:2]
layer = np.fromfile(f"{base}-hud-{w}x{h}.bin", np.float16).reshape(h, w, 4).astype(np.float32)
a = layer[..., 3].clip(0, 1)
diff = np.abs(real - scene).max(-1)
print(f"alpha>0: {(a > 0).mean()*100:.2f}%  alpha>0.5: {(a > 0.5).mean()*100:.2f}%  alpha max {layer[...,3].max():.2f}")
out_hud = a <= 0
print(f"outside HUD: |real-scene| mean {diff[out_hud].mean()*255:.3f}/255  p99 {np.percentile(diff[out_hud], 99)*255:.2f}  >2/255: {(diff[out_hud] > 2/255).mean()*100:.3f}%")
print(f"inside HUD:  |real-scene| mean {diff[~out_hud].mean()*255:.2f}/255")
# blend model: real = scene*(1-a) + c ; contribution c should be >= 0 and independent of scene
c = real - scene * (1 - a[..., None])
print(f"contribution negative share (inside): {(c[~out_hud].min(-1) < -2/255).mean()*100:.2f}%")
print(f"generated vs real: mean abs outside {np.abs(gen-real).max(-1)[out_hud].mean()*255:.2f}, inside {np.abs(gen-real).max(-1)[~out_hud].mean()*255:.2f}")
s = 2
row = np.concatenate([real[::s, ::s], scene[::s, ::s], np.repeat(a[::s, ::s, None], 3, -1), np.repeat((diff[::s, ::s, None] * 8).clip(0, 1), 3, -1)], 1)
Image.fromarray((row * 255).astype(np.uint8)).save(out)
Image.fromarray((np.concatenate([gen, real], 1)[:, :, :] * 255).astype(np.uint8)).resize((w, h // 2)).save(out.replace(".png", "-gen.png"))
