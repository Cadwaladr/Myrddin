#!/usr/bin/env python3
"""A/B of machine switches inside one game session, in time with the layer's frame-rate lines.

    tools/base-ab.py DIR SETTINGS [WINDOWS_PER_SETTING=1] [ROUNDS=3]

"sudo tools/base-ab-root.py serve DIR SETTINGS" must be waiting (started with the same DIR and the
same SETTINGS list, before the game; that helper's header says what a setting may be: prefetchers,
the kernel's preemption model). This tool is not root and touches nothing of the game: it follows the
newest logs/fg/fg-PID.log, and each time a frame-rate line ("STAT", one every 10 s) appears it books
that line to the setting that was in force and asks the helper for the next setting (the file
DIR/step). So every 10-second window lies wholly within one setting. After ROUNDS times round the list
it asks the helper to put everything back (DIR/restore) and prints, per setting, the frame rate and
frame times, and each setting against the windows of the first setting (the baseline) in the same
round. Name the baseline several times in the list, between the others ("base,pf=2f,base,preempt=full"):
its windows then surround every other setting in time, and its own spread shows.

    tools/base-ab.py DIR SETTINGS only=NAME [ROUNDS=12]

One setting of the list against the baseline, and nothing else: for a setting that looked good in the
run over all of them. The helper is the same one, waiting with the same list. Each round is one window
as found (DIR/restore) and one window with NAME (the helper is stepped through the list up to it, which
takes a few hundredths of a second; the settings passed on the way are in force that long). It ends
with one more window as found. Each window with NAME is set against the two baseline windows around it.

Switch frame generation off first (sc-turbo fg-off): with it on, the layer holds the game at a step of
the display's rate much of the time, and a gain does not show.
Hold still in one spot for the whole run: (number of settings x WINDOWS x ROUNDS + 1) x 10 s.
"""
import glob
import os
import re
import statistics
import sys
import time

HERE = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
STAT = re.compile(r"\s*([\d.]+) STAT x(\d) real=([\d.]+)/s.*?game frame ms mean=([\d.]+) p50=([\d.]+) p95=([\d.]+) p99=([\d.]+) max=([\d.]+)")


def newest_log():
    logs = glob.glob(os.path.join(HERE, "logs", "fg", "fg-*.log"))
    return max(logs, key=os.path.getmtime) if logs else None


def follow(path):
    """Yield the STAT lines appended to the log from now on."""
    with open(path, errors="replace") as f:
        f.seek(0, os.SEEK_END)
        buf = ""
        while True:
            chunk = f.read()
            if not chunk:
                time.sleep(0.01)
                continue
            buf += chunk
            while "\n" in buf:
                line, buf = buf.split("\n", 1)
                m = STAT.match(line)
                if m:
                    yield tuple(float(x) for x in m.groups())


def step(directory, name="step"):
    path = os.path.join(directory, name)
    open(path, "w").close()
    for _ in range(300):                    # the helper removes the file when it has acted
        if not os.path.exists(path):
            return True
        time.sleep(0.01)
    return False


def only(directory, settings, name, rounds):
    """NAME against the baseline, window by window in turn."""
    if name not in settings:
        sys.exit(f"{name} is not in the list the helper was started with")
    steps = settings.index(name) + 1        # after a restore the helper starts its list again
    log = newest_log()
    if not log or time.time() - os.path.getmtime(log) > 30:
        sys.exit("no layer log that is being written (is the game running?)")
    if not os.path.exists(os.path.join(directory, "ab-timeline.txt")):
        sys.exit(f"the root helper is not waiting in {directory} (no ab-timeline.txt)")
    print(f"log {os.path.basename(log)}; {name} against {settings[0]}, {rounds} rounds = {(2 * rounds + 1) * 10 + 10} s", flush=True)
    out = open(os.path.join(directory, f"ab-only-{time.strftime('%H%M%S')}.txt"), "w")

    def show(text):
        print(text, flush=True)
        out.write(text + "\n")

    base, mine = [], []                     # frame-rate lines: (time, multiplier, fps, mean, p50, p95, p99, max)
    lines = follow(log)
    next(lines)                             # the window under way ends here
    try:
        for rnd in range(rounds + 1):
            if not step(directory, "restore"):
                sys.exit("the root helper did not act on DIR/restore")
            row = next(lines)
            base.append(row)
            show(f"round {rnd + 1:2d} {settings[0]:>9}: {row[2]:5.1f} fps, frame ms mean {row[3]:5.1f} p50 {row[4]:5.1f} p95 {row[5]:5.1f} p99 {row[6]:5.1f} max {row[7]:6.1f}")
            if rnd == rounds:
                break
            for _ in range(steps):
                if not step(directory):
                    sys.exit("the root helper did not act on DIR/step")
            row = next(lines)
            mine.append(row)
            show(f"round {rnd + 1:2d} {name:>9}: {row[2]:5.1f} fps, frame ms mean {row[3]:5.1f} p50 {row[4]:5.1f} p95 {row[5]:5.1f} p99 {row[6]:5.1f} max {row[7]:6.1f}")
    finally:
        step(directory, "restore")
    if not mine or len(base) < len(mine) + 1:
        return 1
    show("")
    for label, rows in ((settings[0], base), (name, mine)):
        fps = [r[2] for r in rows]
        show(f"{label:>9}  {len(rows):3d} windows  fps mean {statistics.mean(fps):6.2f}  min {min(fps):5.1f}  max {max(fps):5.1f}  "
             f"p50 ms {statistics.mean(r[4] for r in rows):5.1f}  p95 ms {statistics.mean(r[5] for r in rows):5.1f}  p99 ms {statistics.mean(r[6] for r in rows):5.1f}")
    # each window with NAME against the two baseline windows around it
    diffs = [100 * (mine[i][2] / ((base[i][2] + base[i + 1][2]) / 2) - 1) for i in range(len(mine))]
    # the baseline against itself, the same way: every other baseline window against its two neighbours
    own = [100 * (base[i][2] / ((base[i - 1][2] + base[i + 1][2]) / 2) - 1) for i in range(1, len(base) - 1)]
    mean = statistics.mean(diffs)
    error = statistics.stdev(diffs) / len(diffs) ** 0.5 if len(diffs) > 1 else 0.0
    show(f"{name} against the baseline windows around each: " + " ".join(f"{d:+.1f}" for d in diffs) + " %")
    show(f"  mean {mean:+.2f} %, standard error {error:.2f} %, {sum(d > 0 for d in diffs)} of {len(diffs)} above the baseline")
    if own:
        show(f"a baseline window against the baseline windows around it (what no change looks like): mean {statistics.mean(own):+.2f} %, "
             f"from {min(own):+.1f} to {max(own):+.1f} %")
    out.close()
    return 0


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    directory, settings = sys.argv[1], [s.strip() for s in sys.argv[2].split(",")]
    if len(sys.argv) > 3 and sys.argv[3].startswith("only="):
        return only(directory, settings, sys.argv[3][5:], int(sys.argv[4]) if len(sys.argv) > 4 else 12)
    windows = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    rounds = int(sys.argv[4]) if len(sys.argv) > 4 else 3
    log = newest_log()
    if not log or time.time() - os.path.getmtime(log) > 30:
        sys.exit("no layer log that is being written (is the game running?)")
    if not os.path.exists(os.path.join(directory, "ab-timeline.txt")):
        sys.exit(f"the root helper is not waiting in {directory} (no ab-timeline.txt)")
    print(f"log {os.path.basename(log)}; {len(settings)} settings x {windows} window(s) x {rounds} rounds = {len(settings) * windows * rounds * 10 + 10} s", flush=True)
    rows = []                               # (setting index, round, fps, mean, p50, p95, p99, max)
    lines = follow(log)
    next(lines)                             # the window under way ends here: the first setting starts with a full one
    out = open(os.path.join(directory, f"ab-{time.strftime('%H%M%S')}.txt"), "w")
    try:
        for rnd in range(rounds):
            for i, name in enumerate(settings):
                if not step(directory):
                    sys.exit("the root helper did not act on DIR/step")
                for _ in range(windows):
                    t, mult, fps, mean, p50, p95, p99, worst = next(lines)
                    rows.append((i, rnd, fps, mean, p50, p95, p99, worst))
                    text = f"round {rnd + 1} setting {name:>9}: {fps:5.1f} fps, frame ms mean {mean:5.1f} p50 {p50:5.1f} p95 {p95:5.1f} p99 {p99:5.1f} max {worst:6.1f}"
                    print(text, flush=True)
                    out.write(text + "\n")
    finally:
        step(directory, "restore")
    print()
    names = list(dict.fromkeys(settings))           # each setting once, in order; the first is the baseline
    of = lambda name: [r for r in rows if settings[r[0]] == name]
    base = of(names[0])

    def show(text):
        print(text)
        out.write(text + "\n")

    show(f"{'setting':>9}  {'windows':>7}  {'fps mean':>8}  {'min':>5}  {'max':>5}  {'p50 ms':>6}  {'p99 ms':>6}   against the baseline windows of the same round")
    for name in names:
        mine = of(name)
        fps = [r[2] for r in mine]
        line = f"{name:>9}  {len(mine):7d}  {statistics.mean(fps):8.2f}  {min(fps):5.1f}  {max(fps):5.1f}  {statistics.mean(r[4] for r in mine):6.1f}  {statistics.mean(r[6] for r in mine):6.1f}"
        if name != names[0]:
            diffs = []
            for rnd in range(rounds):
                a = [r[2] for r in mine if r[1] == rnd]
                b = [r[2] for r in base if r[1] == rnd]
                if a and b:
                    diffs.append(100 * (statistics.mean(a) / statistics.mean(b) - 1))
            line += "   " + " ".join(f"{d:+5.1f} %" for d in diffs) + f"   mean {statistics.mean(diffs):+.1f} %"
        show(line)
    if len(base) > 1:
        fps = [r[2] for r in base]
        show(f"the baseline's own windows differ by up to {100 * (max(fps) / min(fps) - 1):.1f} %: a setting counts only if every round agrees and it is well beyond that")
    out.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
