#!/usr/bin/env python3
"""The tables of the page "Frame rates and latency", from the samples tools/fg-sample.py took:

    tools/fps-table.py [--since YYYY-MM-DD]

Per place and multiplier, the middle of the samples: the game's own frame rate and the shown one,
and the delay from DLSS's inputs to the display (with the part to the display only where the
layer's timing reports were on). Places with the most samples first.
"""
import argparse
import collections
import json
import os
import statistics

HERE = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
SAMPLES = os.environ.get("FPS_SAMPLES") or os.path.join(HERE, "logs", "research", "fps-samples.jsonl")
MULTIPLIERS = (2, 3, 4)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--since", default="")
    args = parser.parse_args()
    if not os.path.exists(SAMPLES):
        raise SystemExit("no samples yet (tools/fg-sample.py takes them)")
    groups = collections.defaultdict(list)
    for line in open(SAMPLES):
        s = json.loads(line)
        if s["taken"][:10] >= args.since and s.get("place") and not s.get("mixed"):
            groups[(s["place"], s["x"])].append(s)
    places = sorted({p for p, _ in groups}, key=lambda p: -sum(len(groups[(p, x)]) for x in MULTIPLIERS))
    mid = lambda part, key: statistics.median([s[key] for s in part if s.get(key) is not None]) if any(s.get(key) is not None for s in part) else None

    def cell(place, x, fmt):
        part = groups.get((place, x), [])
        return fmt(part) if part else "-"

    head = "| Place | " + " | ".join(f"{x}×" for x in MULTIPLIERS) + " |\n|---|" + "---|" * len(MULTIPLIERS)
    print("The game's own frames per second, and the shown ones:\n")
    print(head)
    for p in places:
        print(f"| {p} | " + " | ".join(cell(p, x, lambda part: f"{mid(part, 'real'):.0f} → {mid(part, 'shown'):.0f}") for x in MULTIPLIERS) + " |")
    print("\nThe delay from DLSS's inputs to the display, ms (* without the part to the display):\n")
    print(head)
    for p in places:
        def delay(part):
            # (a delay with the part to the display and one without are not mixed: the first if there are any)
            full = [s for s in part if s.get("to_display") is not None]
            total = mid(full or part, "total")
            return "-" if total is None else f"{total:.0f}{'' if full else '*'}"
        print(f"| {p} | " + " | ".join(cell(p, x, delay) for x in MULTIPLIERS) + " |")
    print("\nSamples per place and multiplier:", ", ".join(f"{p} " + "/".join(str(len(groups.get((p, x), []))) for x in MULTIPLIERS) for p in places))


if __name__ == "__main__":
    main()
