#!/usr/bin/env python3
"""Where the time goes between a frame of the game and the display, from a session's log:

    tools/latency-report.py [PID]     (logs/fg/fg-PID.log; default: the newest session)

Per multiplier, and apart for frames the layer held back and for a game that set the pace itself
(with the GPU's boost on and off apart, where a session had both). All in ms, the middle value of
the ten-second lines:

    waiting      from the game handing DLSS its inputs to the present call reaching the layer
    generating   from there until the generated frames can go out
    spreading    from there until the real frame goes out: the generated frames go first, a
                 refresh apart. This part belongs to interpolation
    to display   from the real frame's present call to the moment the driver says it went out
    total        those four

What came before DLSS's inputs (the game's own work on the frame, from the input it read) is not
in here: nothing outside the game can see it. "as planned": of the real frames, those that reached
the display as many refreshes after the one before as the multiplier says.
"""
import collections
import csv
import glob
import os
import re
import statistics
import struct
import sys

HERE = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
FIELDS = {
    "real": r"real=([\d.]+)/s", "shown": r"shown=([\d.]+)/s", "hook": r"hook ms=([\d.]+)", "age": r"age ms p50=([\d.]+)",
    "gpu_wait": r"fg gpu wait ms p50=([\d.]+)", "ready": r" ready=([\d.]+) tail", "game_gpu": r"game gpu ms=([\d.]+)",
    "display": r"present to display ms p50=([\d.]+)", "display95": r"present to display ms p50=[\d.]+ p95=([\d.]+)",
    "generated_display": r"generated frames to display ms p50=([\d.]+)",
    "real_out": r"real frame goes out ms p50=([\d.]+)",
}


def number(pattern, line):
    m = re.search(pattern, line)
    return float(m[1]) if m else None


def layer_start(pid):
    try:
        with open(os.path.join(HERE, "logs", "fg", f"THREADS-{pid}"), "rb") as f:
            return struct.unpack_from("<8siId", f.read(32), 0)[3]
    except (OSError, struct.error):
        return None


def clocks(pid):
    """The GPU's clock per second, where the session was recorded ("sc-turbo fg N watch")."""
    try:
        with open(os.path.join(HERE, "logs", "io", f"io-{pid}.csv")) as f:
            rows = [(float(r["time"]), float(r["gpu_mhz"])) for r in csv.DictReader(f) if r.get("gpu_mhz")]
    except (OSError, KeyError, ValueError):
        return []
    return rows


def windows(pid):
    start, clock = layer_start(pid), clocks(pid)
    out = []
    for line in open(os.path.join(HERE, "logs", "fg", f"fg-{pid}.log"), errors="replace"):
        m = re.match(r"\s*([\d.]+) STAT x(\d)", line)
        if not m:
            continue
        w = {name: number(pattern, line) for name, pattern in FIELDS.items()}
        if not w["real"] or w["real"] < 5.0 or not w["hook"]:
            continue
        for key in ("display", "display95", "generated_display"):  # (0: no report came in)
            w[key] = w[key] or None
        limit = re.search(r"limit (\d+) of (\d+)", line)
        beat = re.search(r"as planned=(\d+) closer=(\d+) later=(\d+) much later=(\d+)", line)
        boost = re.search(r"boost=(\d)", line)
        never = re.search(r"never on the display real=(\d+) generated=(\d+)", line)
        silent = re.search(r"no report here real=(\d+) generated=(\d+)", line)
        others = re.search(r"reports read by others=(\d+)", line)
        after = re.search(r"after 0 refreshes=(\d+) 1=(\d+) 2=(\d+) more=(\d+)", line)
        w.update(time=float(m[1]), x=int(m[2]), held=bool(limit and int(limit[2]) and int(limit[1]) > 0.5 * int(limit[2])),
                 beat=[int(v) for v in beat.groups()] if beat else [0, 0, 0, 0], boost=boost and boost[1] == "1",
                 never=[int(v) for v in never.groups()] if never else [0, 0],
                 silent=sum(int(v) for v in silent.groups()) if silent else 0, others=int(others[1]) if others else 0,
                 after=[int(v) for v in after.groups()] if after else [0, 0, 0, 0])
        if start and clock:
            at = start + w["time"]
            part = [mhz for t, mhz in clock if at - 10.0 <= t <= at]
            w["mhz"] = statistics.mean(part) if part else None
        out.append(w)
    return out


def middle(part, key):
    values = [w[key] for w in part if w.get(key) is not None]
    return statistics.median(values) if values else None


def text(value, width=6, digits=1):
    return f"{value:{width}.{digits}f}" if value is not None else " " * (width - 1) + "-"


def main():
    if len(sys.argv) > 1:
        pid = sys.argv[1]
    else:
        logs = sorted(glob.glob(os.path.join(HERE, "logs", "fg", "fg-*.log")), key=os.path.getmtime)
        if not logs:
            sys.exit("no session in logs/fg")
        pid = re.search(r"fg-(\d+)\.log", logs[-1])[1]
    all_windows = windows(pid)
    if not all_windows:
        sys.exit(f"session {pid}: no frame generation in its log")
    groups = collections.defaultdict(list)
    for w in all_windows:
        groups[(-w["x"], not w["held"], w["boost"])].append(w)
    print(f"session {pid}: {len(all_windows) / 6:.0f} minutes of frame generation. ms, the middle of the ten-second lines\n")
    print(f"{'':<36}   real  shown | waiting generating spreading to display  (95 %) |  total | GPU MHz | real frames on the display: as planned / closer / later / much later")
    any_display = False
    for (x, free, boost), part in sorted(groups.items()):
        age, ready, hook, display = (middle(part, k) for k in ("age", "ready", "hook", "display"))
        spreading = hook - ready if hook is not None and ready is not None else None
        real_out = middle(part, "real_out")
        if real_out is not None:  # (what is measured since it exists)
            ready = min(ready or 0.0, real_out)
            spreading = real_out - ready
        any_display |= display is not None
        total = sum(v for v in (age, ready, spreading, display) if v is not None)
        beat = [sum(w["beat"][i] for w in part) for i in range(4)]
        share = " / ".join(f"{100 * b / sum(beat):.0f} %" for b in beat) if sum(beat) else "-"
        name = f"x{-x} {'game sets the pace' if free else 'held by the layer'}{', boost' if boost else ''}"
        print(f"{name:<28} {len(part) / 6:3.0f} min {text(middle(part, 'real'))} {text(middle(part, 'shown'), 6, 0)} | {text(age, 7)} {text(ready, 10)} "
              f"{text(spreading, 9)} {text(display, 10)} {text(middle(part, 'display95'), 6)} | {text(total, 6)} | {text(middle(part, 'mhz'), 7, 0)} | {share}")
        after = [sum(w["after"][i] for w in part) for i in range(4)]
        if sum(after):
            never = [sum(w["never"][i] for w in part) for i in range(2)]
            print(f"{'':<36}   every frame on the display, refreshes after the one before: 0: {100 * after[0] / sum(after):.1f} %, 1: {100 * after[1] / sum(after):.1f} %, "
                  f"2: {100 * after[2] / sum(after):.1f} %, more: {100 * after[3] / sum(after):.1f} %; replaced before reaching it: {never[0]} real, {never[1]} generated; "
                  f"no report here: {sum(w['silent'] for w in part)} (read by others: {sum(w['others'] for w in part)}); "
                  f"generated frames to display {text(middle(part, 'generated_display'), 0)} ms")
    if not any_display:
        print("\nno times from the display in this log: a build from before they were read, or a runner whose vkd3d-proton does not ask for them")


if __name__ == "__main__":
    main()
