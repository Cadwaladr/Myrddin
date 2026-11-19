#!/usr/bin/env python3
"""What became of the frames the layer sent: logs/fg/presents-*.bin ("sc-turbo fg-params timing=1", then
"sc-turbo fg-events"). Finds the display's refresh from the times it reported, how long before a refresh a
frame has to be sent to make it, and what the frames it never showed have in common.

  tools/display-timing.py [--last SECONDS] [FILE]     (default: the newest in logs/fg; --last: only the end of it)
"""
import glob
import os
import statistics
import struct
import sys

FLAG_REAL, FLAG_REPLACED, FLAG_SILENT, FLAG_MISFIT = 1, 2, 4, 8


def load(path):
    data = open(path, "rb").read()
    if data[:8] != b"SCFGPRS1":
        sys.exit(f"{path}: not a file of frames sent")
    size, _, count, target = struct.unpack_from("<IIQd", data, 8)
    frames = []
    for i in range(count):
        sent, shown, call_ms, flags, position, multiplier, _ = struct.unpack_from("<ddfBBBB", data, 32 + i * size)
        frames.append(dict(sent=sent, shown=shown, call_ms=call_ms, flags=flags, position=position, multiplier=multiplier))
    return frames, target


def percentile(values, q):
    values = sorted(values)
    return values[min(int(q * len(values)), len(values) - 1)] if values else 0.0


def fit_refresh(times, nominal):
    """The refresh grid through the display's times: period and the time of refresh 0."""
    period, origin = nominal, times[0]
    for _ in range(6):
        pairs = [(round((t - origin) / period), t) for t in times]
        n = len(pairs)
        mean_k = sum(k for k, _ in pairs) / n
        mean_t = sum(t for _, t in pairs) / n
        spread = sum((k - mean_k) ** 2 for k, _ in pairs)
        if spread:
            period = sum((k - mean_k) * (t - mean_t) for k, t in pairs) / spread
        origin = mean_t - period * mean_k
    return period, origin


def bar(share, width=28):
    return "#" * round(share * width)


def main():
    args = sys.argv[1:]
    last = None
    if args[:1] == ["--last"]:
        last = float(args[1])
        args = args[2:]
    path = args[0] if args else max(glob.glob("logs/fg/presents-*.bin"), key=os.path.getmtime)
    frames, target = load(path)
    if last is not None:
        frames = [f for f in frames if f["sent"] >= frames[-1]["sent"] - last]
    shown = [f for f in frames if f["shown"] and not f["flags"] & FLAG_MISFIT]
    replaced = [f for f in frames if f["flags"] & FLAG_REPLACED]
    silent = [f for f in frames if f["flags"] & FLAG_SILENT]
    span = frames[-1]["sent"] - frames[0]["sent"]
    print(f"{os.path.basename(path)}: {len(frames)} frames sent over {span:.1f} s ({len(frames) / span:.1f}/s)")
    print(f"  on the display {len(shown)} ({100 * len(shown) / len(frames):.1f} %), replaced before reaching it {len(replaced)} "
          f"({100 * len(replaced) / len(frames):.1f} %), no report here {len(silent)} ({100 * len(silent) / len(frames):.1f} %)")
    if len(shown) < 50:
        return
    period, origin = fit_refresh([f["shown"] for f in shown], 1.0 / target)
    off = [abs((f["shown"] - origin) / period - round((f["shown"] - origin) / period)) * period * 1000 for f in shown]
    print(f"  refresh {period * 1000:.3f} ms ({1 / period:.2f} Hz); the display's times lie within {percentile(off, 0.99):.2f} ms of that grid (99 %)")
    used = {round((f["shown"] - origin) / period) for f in shown}
    total = max(used) - min(used) + 1
    print(f"  refreshes with a new frame: {100 * len(used) / total:.1f} % of {total}")

    delays = [(f["shown"] - f["sent"]) * 1000 for f in shown]
    print(f"\n  sent to display, ms: least {min(delays):.1f}, p5 {percentile(delays, 0.05):.1f}, p50 {percentile(delays, 0.5):.1f}, "
          f"p95 {percentile(delays, 0.95):.1f}, most {max(delays):.1f}")

    # Where in the refresh a frame was sent, and how long it then took: a frame sent just before the
    # compositor's deadline takes least, one sent just after it a whole refresh more.
    bins = 16
    by_phase = [[] for _ in range(bins)]
    lost_by_phase = [0] * bins
    phase = lambda t: ((t - origin) / period) % 1.0
    for f, d in zip(shown, delays):
        by_phase[int(phase(f["sent"]) * bins)].append(d)
    for f in replaced:
        lost_by_phase[int(phase(f["sent"]) * bins)] += 1
    medians = [statistics.median(b) if b else 0.0 for b in by_phase]
    print(f"\n  sent this long after a refresh | frames | middle delay to the display | replaced")
    for i in range(bins):
        n = len(by_phase[i]) + lost_by_phase[i]
        print(f"   {i * period * 1000 / bins:5.2f} ms  {n:6d}   {medians[i]:6.1f} ms   {100 * lost_by_phase[i] / n if n else 0:5.1f} %")
    filled = [m for m, b in zip(medians, by_phase) if len(b) >= 10]
    if filled:
        least = min(filled)
        print(f"  a frame sent at the best moment takes about {least:.1f} ms, on average {statistics.mean(delays) - least:.1f} ms less than now")

    # What the replaced frames have in common: the frame after them comes soon.
    print("\n  time until the next frame was sent | frames | replaced")
    edges = [0, 7, 8, 8.34, 9, 10, 12, 16, 1000]
    counts = [[0, 0] for _ in edges[:-1]]
    for a, b in zip(frames, frames[1:]):
        if a["flags"] & FLAG_SILENT:
            continue
        gap = (b["sent"] - a["sent"]) * 1000
        for i in range(len(edges) - 1):
            if edges[i] <= gap < edges[i + 1]:
                counts[i][0] += 1
                counts[i][1] += bool(a["flags"] & FLAG_REPLACED)
                break
    for i, (n, lost) in enumerate(counts):
        if n:
            print(f"   {edges[i]:5.2f} to {edges[i + 1]:<7.2f} ms {n:6d}   {100 * lost / n:5.1f} % {bar(lost / n)}")

    print("\n  place among the frames of one game frame (last = the game's own) | frames | replaced")
    places = {}
    for f in frames:
        if not f["flags"] & FLAG_SILENT:
            entry = places.setdefault((f["multiplier"], f["position"]), [0, 0])
            entry[0] += 1
            entry[1] += bool(f["flags"] & FLAG_REPLACED)
    for (multiplier, position), (n, lost) in sorted(places.items()):
        if n >= 20:
            print(f"   x{multiplier} place {position + 1}   {n:6d}   {100 * lost / n:5.1f} %")


if __name__ == "__main__":
    main()
