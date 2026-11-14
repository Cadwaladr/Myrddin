#!/usr/bin/env python3
"""fg-summ.py NAME...: one line per test in logs/test/NAME (tools/test-chain.sh), from the events of the last
60 % of the run and the layer's log: real frames a second, frame times, how long frames waited for
their present call (hand50/95), the limiter's holds, mismatches found (off)."""
import glob, os, sys, numpy as np
root = os.path.join(os.path.dirname(os.path.dirname(os.path.realpath(__file__))), 'logs', 'test') + os.sep
print('%-14s %6s %6s %7s %7s %7s | %6s %6s %6s %6s %6s | %5s %5s %4s %s' % ('test', 'real', 'shown', 'per50', 'per95', 'per99',
      'hand50', 'hand95', 'sleep', 'own', 'ready', 'gated', 'guard', 'off', 'mult/shift'))
for name in sys.argv[1:]:
    files = sorted(glob.glob(root + name + '/events-*.txt'))
    log = open(glob.glob(root + name + '/fg-*.log')[0]).read()
    rows = {}
    for f in files:
        for line in open(f):
            p = line.split('|')
            if len(p) < 5 or line.startswith('#'):
                continue
            rows[float(p[0])] = ([float(x) for x in p[3].split()], p[4].split())
    ts = np.array(sorted(rows))
    if len(ts) < 20:
        print('%-14s too few events' % name); continue
    keep = ts > ts[0] + 0.4 * (ts[-1] - ts[0])
    t = ts[keep]
    a = np.array([rows[x][0] for x in t])
    tail = [rows[x][1] for x in t]
    per = np.diff(t) * 1000
    mult = sorted(set(x[0] for x in tail)); shift = sorted(set(x[1] for x in tail))
    flags = ''.join(x[2] if len(x) > 2 else '' for x in tail)
    gen = np.array([('P' not in (x[2] if len(x) > 2 else '')) and ('D' not in (x[2] if len(x) > 2 else '')) for x in tail])
    m = np.median([int(x[0][1:]) for x in tail])
    real = len(t) / (t[-1] - t[0])
    stat = [l for l in log.splitlines() if 'STAT' in l]
    shown = stat[-1].split('shown=')[1].split('/')[0] if stat else '?'
    off = log.count('FRAME CHAIN OFF')
    guard = (a[:, 9] > 0.05).sum() if a.shape[1] > 9 else 0
    print('%-14s %6.1f %6s %7.1f %7.1f %7.1f | %6.2f %6.2f %6.1f %6.1f %6.1f | %5d %5d %4d %s shift %s' % (
        name, real, shown, np.percentile(per, 50), np.percentile(per, 95), np.percentile(per, 99),
        np.percentile(a[:, 0], 50), np.percentile(a[:, 0], 95), np.median(a[:, 1]),
        np.median(a[:, 8]) if a.shape[1] > 8 else 0, np.median(a[:, 6]), flags.count('G'), guard, off, ','.join(mult), ','.join(shift)))
