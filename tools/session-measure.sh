#!/usr/bin/env bash
# What the game and this layer do in a busy place, measured from outside while the player plays on (nothing in the
# game is changed or attached to): when the game's frame is done on the GPU against when it reaches the layer, this
# layer's GPU time by step, the game's threads and the CPUs, GPU memory against what the driver offers, and whether
# ntsync is in use. Every frame asks the display when it got there meanwhile (timing=1), switched off again however
# it ends.
#
#   tools/session-measure.sh [SECONDS=120]
cd "$(dirname "$0")/.." || exit 1
secs=${1:-120}
pid=$(pgrep -x StarCitizen.exe | head -n 1)
[ -n "$pid" ] || { echo "the game is not running"; exit 1; }
log="logs/fg/fg-$pid.log"
[ -f "$log" ] || { echo "no $log: was the game launched with frame generation?"; exit 1; }
finish() { pgrep -x StarCitizen.exe > /dev/null && ./sc-turbo fg-params timing=0 > /dev/null; }
trap finish EXIT
trap 'exit 1' INT TERM HUP
first=$(grep -c ' STAT ' "$log")
./sc-turbo fg-params timing=1 > /dev/null
echo "$(date +%H:%M:%S) measuring for $secs s"
python3 -B tools/thread-load.py "$pid" "$secs" > "logs/fg/threads-$pid.txt"
./sc-turbo fg-events > /dev/null
timeout 3 tail -f /dev/null
./sc-turbo fg-params timing=0 > /dev/null
echo "== ntsync: $(ls -l /proc/"$pid"/fd 2>/dev/null | grep -c '/dev/ntsync') handles open in the game"
echo "== the game's threads and the CPUs"; cat "logs/fg/threads-$pid.txt"
python3 -B - "$log" "$first" <<'PY'
import re, statistics, sys
lines = [l for l in open(sys.argv[1], errors="replace") if " STAT " in l][int(sys.argv[2]) + 1:]
def mid(pattern, group=1):
    values = [float(m[group]) for m in (re.search(pattern, l) for l in lines) if m]
    return statistics.median(values) if values else float("nan")
print(f"== the layer, {len(lines)} ten-second lines")
print(f"real frames {mid(r'real=([\d.]+)/s'):.1f}/s, multiplier x{mid(r'STAT x(\d)'):.0f}, GPU memory used {mid(r'gpu memory used=([\d.]+)'):.0f}"
      f" of {mid(r'gpu memory used=[\d.]+ of ([\d.]+)'):.0f} MB")
print(f"game frame done on the GPU after its hand-over: {mid(r'after its hand-over ms p50=([-\d.]+)'):.1f} ms;"
      f" done before the present reached the layer: {mid(r'before the present hook ms p50=([-\d.]+)'):.1f} ms"
      f" (p95 {mid(r'before the present hook ms p50=[-\d.]+ p95=([-\d.]+)'):.1f}; below 0: the present came first)")
steps = ["before", "prepare", "flow", "interpolation", "hud", "after"]
print("this layer's GPU ms by step: " + ", ".join(f"{s} {mid(r'by step:.*? ' + s + r'=([\d.]+)'):.2f}" for s in steps))
print(f"present hook to first generated frame out: ready {mid(r' ready=([\d.]+) tail'):.1f} ms, of which waiting for the GPU"
      f" {mid(r'fg gpu wait ms p50=([\d.]+)'):.1f}")
PY
echo "== the delay"; python3 -B tools/latency-report.py "$pid" | tail -n +2
