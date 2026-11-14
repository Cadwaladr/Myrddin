#!/usr/bin/env python3
"""Decodes a B10G11R11_UFLOAT image dump (WxH in the file name) to float RGB; writes a PNG
(tonemapped) and prints statistics. Usage: hud_layer.py file-WxH.bin [out.png]"""
import re, sys
import numpy as np
from PIL import Image

def ufloat(bits, mantissa_bits):
    m = bits & ((1 << mantissa_bits) - 1)
    e = bits >> mantissa_bits
    scale = float(1 << mantissa_bits)
    return np.where(e == 0, m / scale * 2.0 ** -14, (1 + m / scale) * np.exp2(e.astype(np.float32) - 15))

def load(path):
    w, h = map(int, re.search(r"(\d+)x(\d+)", path.split("/")[-1]).groups())
    v = np.frombuffer(open(path, "rb").read()[: w * h * 4], np.uint32).reshape(h, w)
    return np.stack([ufloat(v & 0x7FF, 6), ufloat((v >> 11) & 0x7FF, 6), ufloat(v >> 22, 5)], axis=2).astype(np.float32)

if __name__ == "__main__":
    rgb = load(sys.argv[1])
    peak = rgb.max(axis=2)
    print(f"{rgb.shape[1]}x{rgb.shape[0]} min {rgb.min():.4f} max {rgb.max():.3f} mean {rgb.mean():.4f}; "
          f"pixels > 0.02: {(peak > 0.02).mean() * 100:.1f} %, > 0.2: {(peak > 0.2).mean() * 100:.1f} %; first pixel {rgb[0, 0]}")
    if len(sys.argv) > 2:
        Image.fromarray((np.clip(rgb / (1 + rgb), 0, 1) ** (1 / 2.2) * 255).astype(np.uint8)).save(sys.argv[2])
