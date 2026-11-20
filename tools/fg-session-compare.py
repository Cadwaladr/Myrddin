#!/usr/bin/env python3
"""Two sessions of the frame-generation layer side by side, from their logs: for a new game version, a
new runner or a new driver. Says what the layer found at the start of each (the game's render size,
its HUD layer, DLSS's path, the swapchain, the extensions), how each session went (frames, inputs,
HUD captures, the frame chain, frames lost) and which kinds of log lines only one of them has.
Reads the two logs and nothing else.

    tools/fg-session-compare.py [--whole] [NEW_LOG [OLD_LOG]]

NEW_LOG defaults to the newest logs/fg/fg-*.log, OLD_LOG to the newest before it that has frame-rate
lines. Only the same stretch of each is compared, from the start to the end of the shorter one (a
session's loading and its play differ a great deal); --whole takes all of both. What a session went
through is not the same from one to the next (a menu, a city, space), so the counts are to be read,
not only the last line. Ends with "same" or with the list of what differs; exit 1 when something that
matters differs.
"""
import glob
import os
import re
import statistics
import sys

HERE = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))


def logs():
    return sorted(glob.glob(os.path.join(HERE, "logs", "fg", "fg-*.log")), key=os.path.getmtime, reverse=True)


def read(path):
    with open(path, errors="replace") as f:
        return [line.rstrip("\n") for line in f]


def body(line):
    """A line without its time stamp."""
    return re.sub(r"^\s*[\d.]+\s+", "", line)


def kind(line):
    """A line with what changes from run to run taken out."""
    text = re.sub(r"0x[0-9a-fA-F]+", "ADDR", body(line))
    text = re.sub(r"\bthread \S+", "thread T", text)
    return re.sub(r"-?\d+(\.\d+)?", "N", text)


def first(lines, pattern):
    for line in lines:
        m = re.search(pattern, line)
        if m:
            return m.group(1) if m.groups() else m.group(0)
    return None


def facts(lines):
    out = {}
    out["render size"] = first(lines, r"render size (\d+x\d+)")
    out["depth format"] = first(lines, r"render size \d+x\d+, depth format (\d+)")
    out["HUD layer"] = first(lines, r"HUD layer (\d+x\d+)")
    out["HUD pass ready"] = "yes" if first(lines, r"HUD pass ready") else "no"
    out["picture"] = first(lines, r"ready for (\d+x\d+ format \d+, \d+ swapchain images, \d+ generated per frame)")
    out["memory offered MB"] = first(lines, r"offered to it (\d+) MB")
    out["swapchain"] = first(lines, r"swapchain: (\d+x\d+ format \d+ present mode \d+)")
    engines = sorted({m.group(1) for line in lines for m in [re.search(r'instance created: application "[^"]*", engine "([^"]*)"', line)] if m})
    out["engines"] = ", ".join(engines)
    out["devices with DLSS's path"] = str(sum(1 for line in lines if "DLSS path available" in line))
    out["inputs imported"] = "yes" if first(lines, r"copies of device \S+ imported") else "no"
    out["HUD layer imported"] = "yes" if first(lines, r"HUD layer copies imported") else "no"
    return out


def extensions(lines):
    names = set()
    for line in lines:
        m = re.search(r"device created: device \S+ has (.*)", line)
        if m:
            names.update(m.group(1).split())
    return names


STAT = re.compile(r"STAT x(\d) real=([\d.]+)/s generated=([\d.]+)/s shown=([\d.]+)/s")


def number(line, name):
    m = re.search(r"\b" + re.escape(name) + r"=(-?[\d.]+)", line)
    return float(m.group(1)) if m else 0.0


def health(lines):
    stats = [line for line in lines if " STAT x" in line]
    out = {"frame-rate lines": len(stats)}
    if not stats:
        return out
    real = [float(STAT.search(line).group(2)) for line in stats if STAT.search(line)]
    shown = [float(STAT.search(line).group(4)) for line in stats if STAT.search(line)]
    out["real/s median"] = round(statistics.median(real), 1)
    out["shown/s median"] = round(statistics.median(shown), 1)
    # "inputs game=N blank=N": frames with DLSS's inputs, and without
    inputs = sum(float(m.group(1)) for line in stats for m in [re.search(r"inputs game=(\d+)", line)] if m)
    blank = sum(float(m.group(1)) for line in stats for m in [re.search(r"inputs game=\d+ blank=(\d+)", line)] if m)
    layer = sum(float(m.group(1)) for line in stats for m in [re.search(r"hud=\d+ layer=(\d+)", line)] if m)
    chain = sum(float(m.group(1)) for line in stats for m in [re.search(r"\| chain=(\d+)", line)] if m)
    out["frames with DLSS's inputs"] = int(inputs)
    out["frames without them"] = int(blank)
    out["frames with a HUD layer, % of the chain"] = round(100.0 * layer / chain, 1) if chain else 0.0
    for name in ("unlinked", "shift", "nohud", "untouched"):
        out[name + " (sum)"] = int(sum(abs(number(line, name)) for line in stats))
    out["lost (sum)"] = int(sum(float(m.group(1)) for line in stats for m in [re.search(r"late copies=\d+ lost=(\d+)", line)] if m))
    out["CHAIN OFF lines"] = sum(1 for line in lines if "CHAIN OFF" in line)
    return out


def stamp(line):
    m = re.match(r"\s*([\d.]+)\s", line)
    return float(m.group(1)) if m else None


def head(lines, count):
    """The log up to its COUNT-th frame-rate line, and that line's time in seconds."""
    seen = 0
    for i, line in enumerate(lines):
        if " STAT x" in line:
            seen += 1
            if seen == count:
                return lines[:i + 1], stamp(line) or 0.0
    return lines, (stamp(lines[-1]) or 0.0) if lines else 0.0


def main():
    args = sys.argv[1:]
    if args and args[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    whole = "--whole" in args
    args = [a for a in args if a != "--whole"]
    found = logs()
    new = args[0] if args else (found[0] if found else None)
    if len(args) > 1:
        old = args[1]
    else:
        old = next((p for p in found if os.path.realpath(p) != os.path.realpath(new or "") and
                    any(" STAT x" in line for line in read(p))), None)
    if not new or not old:
        print("two logs are needed (logs/fg/fg-*.log)")
        return 2
    a, b = read(old), read(new)
    print(f"before: {os.path.relpath(old, HERE)}\nnow:    {os.path.relpath(new, HERE)}")
    if not whole:
        # A session's start (loading, the menu) and its play differ: the same stretch of each.
        count = min(sum(" STAT x" in line for line in a), sum(" STAT x" in line for line in b))
        if count:
            (a, ta), (b, tb) = head(a, count), head(b, count)
            print(f"the first {count} frame-rate lines of each ({ta:.0f} s and {tb:.0f} s from the start; --whole: all of both)")
    print()
    differs = []
    fa, fb = facts(a), facts(b)
    print("What the layer found at the start:")
    for name in fa:
        mark = "" if fa[name] == fb[name] else "   <-- differs"
        print(f"  {name:28} {str(fa[name]):60} | {fb[name]}{mark}")
        if mark and name != "memory offered MB":
            differs.append(name)
    ea, eb = extensions(a), extensions(b)
    if ea != eb:
        print(f"  device extensions only before: {' '.join(sorted(ea - eb)) or '-'}\n  only now: {' '.join(sorted(eb - ea)) or '-'}")
        if any(x.startswith("NVX_") for x in ea ^ eb):
            differs.append("NVX extensions")
    else:
        print(f"  device extensions: the same {len(ea)}")
    ha, hb = health(a), health(b)
    print("\nHow each session went:")
    for name in ha:
        print(f"  {name:42} {str(ha[name]):>10} | {hb.get(name)}")
    for name in ("frames without them", "unlinked (sum)", "shift (sum)", "lost (sum)", "CHAIN OFF lines"):
        if hb.get(name, 0) > ha.get(name, 0):
            differs.append(name)
    if hb.get("frames with a HUD layer, % of the chain", 0) < ha.get("frames with a HUD layer, % of the chain", 0) - 5:
        differs.append("HUD layer captures")
    if not hb.get("frames with DLSS's inputs") and ha.get("frames with DLSS's inputs"):
        differs.append("DLSS inputs")
    ka = {kind(line) for line in a if " STAT x" not in line}
    kb = {kind(line) for line in b if " STAT x" not in line}
    print(f"\nKinds of lines only before ({len(ka - kb)}):")
    for line in sorted(ka - kb)[:25]:
        print("  " + line[:170])
    print(f"Kinds of lines only now ({len(kb - ka)}):")
    for line in sorted(kb - ka)[:25]:
        print("  " + line[:170])
    print()
    if differs:
        print("DIFFERS: " + ", ".join(dict.fromkeys(differs)))
        return 1
    print("same in everything that matters to the layer")
    return 0


if __name__ == "__main__":
    sys.exit(main())
