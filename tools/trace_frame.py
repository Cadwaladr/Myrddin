#!/usr/bin/env python3
"""Rebuilds one frame of a trace-<pid>.log in execution order (the order of the command buffers
in the frame's big submit). Usage: trace_frame.py trace.log [frame index]"""
import re, sys
lines = [l.rstrip("\n") for l in open(sys.argv[1])]
want = int(sys.argv[2]) if len(sys.argv) > 2 else 1
events = []   # (line no, cb, text)
submits = []  # (line no, [cbs])
for i, l in enumerate(lines):
    body = l[21:].strip() if len(l) > 21 else l
    m = re.match(r"\s*[\d.]+\s+t\d+\s+(.*)", l)
    body = m.group(1) if m else l
    if body.startswith("SUBMIT"):
        submits.append((i, re.findall(r"cb\d+", body), body.split(":")[0]))
    elif body.startswith("PRESENT"):
        submits.append((i, [], body))
    else:
        m = re.match(r"(cb\d+) (.*)", body)
        if m:
            events.append((i, m.group(1), m.group(2)))
big = [s for s in submits if len(s[1]) > 20]
start = big[want - 1][0] if want > 0 else 0
end = big[want][0]
used = set()
for i, cbs, head in submits:
    if not (start < i <= big[want][0] + 12):
        continue
    print(f"== {head} ({len(cbs)} command buffers)")
    for cb in cbs:
        mine = [e for e in events if e[1] == cb and e[0] < i and e[0] not in used]
        # only this recording: after the last BEGIN
        begins = [k for k, e in enumerate(mine) if e[2].startswith("BEGIN")]
        mine = mine[begins[-1]:] if begins else mine
        shown = [e for e in mine if not e[2].startswith("BEGIN")]
        for e in mine:
            used.add(e[0])
        for e in shown:
            print(f"   {cb:6s} {e[2]}")
