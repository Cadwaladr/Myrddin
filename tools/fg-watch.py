#!/usr/bin/env python3
"""A session's frame-generation log at a glance: one short line per statistics line, and the
lines in between that matter (multiplier changes, waits given up, the chain switched off).

    tools/fg-watch.py [LOG] [--last N] [--since SECONDS]

LOG: a logs/fg/fg-*.log (default: the newest). Columns: time (s since the layer started),
multiplier, real and shown frames a second, the game's frame time (median / 95th / 99th
percentile, ms), the age of the picture shown (median / 99th), the wait for the generated
frames' GPU work (median) and when the first is ready, frames the limiter held (of how many)
and for how long on average, the GPU's time for the game's frame, frames the gate let on /
the guard kept back / that waited for the generated frames, and copies lost / late.
"""
import glob, os, re, sys

args = sys.argv[1:]
last = since = None
log = None
i = 0
while i < len(args):
    if args[i] == "--last": last = int(args[i + 1]); i += 2
    elif args[i] == "--since": since = float(args[i + 1]); i += 2
    else: log = args[i]; i += 1
if not log:
    here = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
    logs = sorted(glob.glob(os.path.join(here, "logs/fg/fg-*.log")), key=os.path.getmtime)
    if not logs: sys.exit("no logs/fg/fg-*.log")
    log = logs[-1]

def f(pattern, line, default="-"):
    m = re.search(pattern, line)
    return m.group(1) if m else default

keep = re.compile(r"multiplier x|did not|no guard|CHAIN OFF|stayed queued|not done|given up|go on without|by its timer alone|"
                  r"fg: frame chain |ready for|swapchain:|generation (off|on)|does not tell|vectors")
out = []
for line in open(log, errors="replace"):
    t = f(r"^\s*([0-9.]+) ", line, None)
    if t is None: continue
    if since is not None and float(t) < since: continue
    if " STAT " in line:
        out.append("%8s %-3s real %5s shown %6s | frame %5s %5s %5s | age %5s %5s | fg %4s ready %4s | held %3s/%-3s %5s ms | gpu %5s | g %3s gu %3s f %3s | lost %s late %s%s" % (
            t, f(r"STAT (x\d)", line), f(r"real=([0-9.]+)", line), f(r"shown=([0-9.]+)", line),
            f(r"game frame ms mean=[0-9.]+ p50=([0-9.]+)", line), f(r"game frame ms mean=[0-9.]+ p50=[0-9.]+ p95=([0-9.]+)", line),
            f(r"game frame ms mean=[0-9.]+ p50=[0-9.]+ p95=[0-9.]+ p99=([0-9.]+)", line),
            f(r"age ms p50=([0-9.]+)", line), f(r"age ms p50=[0-9.]+ p99=([0-9.]+)", line),
            f(r"fg gpu wait ms p50=([0-9.]+)", line), f(r"ready=([0-9.]+)", line),
            f(r"limit (\d+) of", line), f(r"limit \d+ of (\d+)", line), f(r"sleep ms=([0-9.]+)", line), f(r"game gpu ms=([0-9.]+)", line),
            f(r"gate=(\d+)", line), f(r"guard=(\d+)", line), f(r"first=(\d+)", line),
            f(r"lost=(\d+)", line), f(r"late copies=(\d+)", line),
            (" | kept for GPU " + f(r"kept back for the GPU (\d+)", line)) if f(r"kept back for the GPU (\d+)", line, "0") != "0" else ""))
    elif keep.search(line):
        out.append(line.rstrip()[:230])
if last: out = out[-last:]
print(log)
print("\n".join(out))
