#!/usr/bin/env python3
"""dump_view.py PID INDEX OUT.png: real | generated on top, HUD mask (zeroed vectors) | centre crop of generated below."""
import sys, glob, re, numpy as np
from PIL import Image
P, i, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
a = Image.open(f'logs/fg/dump-{P}-{i:03d}-real.ppm'); b = Image.open(f'logs/fg/dump-{P}-{i:03d}-generated.ppm')
f = glob.glob(f'logs/fg/dump-{P}-{i:03d}-inputs-*.bin')[0]; w, h = map(int, re.search(r'(\d+)x(\d+)\.bin', f).groups())
mv = np.frombuffer(open(f, 'rb').read()[:w * h * 4], np.float16).reshape(h, w, 2).astype(np.float32)
z = (mv[..., 0] == 0) & (mv[..., 1] == 0)
print(i, 'median mv', np.median(mv[..., 0]), np.median(mv[..., 1]), 'masked %', round(z.mean() * 100, 2))
c = Image.new('RGB', (2560, 1440)); c.paste(a.resize((1280, 720)), (0, 0)); c.paste(b.resize((1280, 720)), (1280, 0))
c.paste(Image.fromarray((z * 255).astype(np.uint8)).resize((1280, 720)).convert('RGB'), (0, 720)); c.paste(b.crop((640, 200, 1920, 920)), (1280, 720))
c.save(out)
