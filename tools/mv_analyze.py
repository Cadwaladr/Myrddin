#!/usr/bin/env python3
"""Analyse sc-probe captures: which motion-vector convention reprojects one frame onto the next?

For each pair of consecutive captured frames (previous P, current C, with C's vectors mv):
  reprojection R(x) = P(x + s * mv(x))   for the candidate conventions s
and the error |tonemap(R) - tonemap(C)| is compared with doing nothing (R = P).
A convention is right when its error is far below the no-warp error on moving frames.

usage: mv_analyze.py <capture dir> [series]
"""
import glob, re, sys
import numpy as np

def load(path):
    h = open(path, 'rb').read(256).decode()
    g = lambda k, t=int: t(re.search(k + r'=([\d.]+)', h)[1])
    w, hh = g('w'), g('h')
    px = w * hh
    d = np.memmap(path, mode='r', offset=256, dtype=np.uint8)
    c = np.frombuffer(d[:px * 8], dtype=np.float16).reshape(hh, w, 4)[..., :3].astype(np.float32)
    m = np.frombuffer(d[px * 8:px * 12], dtype=np.float16).reshape(hh, w, 2).astype(np.float32)
    z = np.frombuffer(d[px * 12:px * 16], dtype=np.float32).reshape(hh, w)
    return dict(w=w, h=hh, color=c, mv=m, depth=z, eval=g('eval'), present=g('present'), time=g('time', float))

def luma(c):
    c = np.nan_to_num(np.clip(c, 0, 1e4))
    y = c @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    return y / (1 + y)            # Reinhard: keeps HDR highlights from dominating

def sample(img, x, y):
    h, w = img.shape
    x = np.clip(x, 0, w - 1.001); y = np.clip(y, 0, h - 1.001)
    x0 = x.astype(np.int32); y0 = y.astype(np.int32)
    fx = x - x0; fy = y - y0
    return (img[y0, x0] * (1 - fx) * (1 - fy) + img[y0, x0 + 1] * fx * (1 - fy) +
            img[y0 + 1, x0] * (1 - fx) * fy + img[y0 + 1, x0 + 1] * fx * fy)

def main():
    d = sys.argv[1]
    files = sorted(glob.glob(f'{d}/cap-*-s{sys.argv[2] if len(sys.argv) > 2 else "*"}-b*-f*.bin'))
    bursts = {}
    for f in files:
        bursts.setdefault(re.sub(r'-f\d\.bin$', '', f), []).append(f)
    conv = {'+px': (1, 1), '-px': (-1, -1), '+x-y': (1, -1), '-x+y': (-1, 1)}
    print(f"{'burst':>8} {'evals':>14} {'|mv| med':>9} {'p99':>7} {'no-warp':>8} " + ' '.join(f'{k:>8}' for k in conv) + '   best  gain')
    tot = {k: [] for k in conv}; base = []
    for b, fs in sorted(bursts.items()):
        fr = [load(f) for f in sorted(fs)]
        for p, c in zip(fr, fr[1:]):
            tag = f"{b[-3:]} {p['eval']}>{c['eval']}"
            if c['eval'] != p['eval'] + 1 or p['w'] != c['w']:
                print(f"{tag:>23}  not consecutive, skipped"); continue
            lp, lc = luma(p['color']), luma(c['color'])
            h, w = lc.shape
            yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
            mag = np.hypot(c['mv'][..., 0], c['mv'][..., 1])
            e0 = np.abs(lp - lc).mean()
            errs = {}
            for k, (sx, sy) in conv.items():
                r = sample(lp, xx + sx * c['mv'][..., 0], yy + sy * c['mv'][..., 1])
                errs[k] = np.abs(r - lc).mean()
            best = min(errs, key=errs.get)
            print(f"{tag:>23} {np.median(mag):9.2f} {np.percentile(mag, 99):7.1f} {e0:8.5f} " +
                  ' '.join(f'{errs[k]:8.5f}' for k in conv) + f"   {best:>4}  {e0 / max(errs[best], 1e-9):4.1f}x")
            if np.median(mag) > 0.5:
                base.append(e0)
                for k in conv: tot[k].append(errs[k])
    if base:
        print(f"\nmoving frames (median |mv| > 0.5 px): {len(base)}   no-warp error {np.mean(base):.5f}")
        for k in conv: print(f"  {k:>5}: {np.mean(tot[k]):.5f}  ({np.mean(base) / np.mean(tot[k]):.1f}x better than no warp)")

main()
