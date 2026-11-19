#!/usr/bin/env python3
"""How much of the game's own memory is in huge pages, and why the rest is not.

    tools/base-thp-map.py [--layout] [PID]          (the game's process if no PID is given)

--layout: sort all anonymous memory into blocks whether or not the kernel may back it with huge pages
right now (with huge pages on "madvise" nothing is eligible that the program did not ask for): what
the layout would allow.

Read from /proc/PID/smaps (every area: size, resident, in huge pages, eligible or not) and from
/proc/PID/pagemap (which 4 KiB pages are present; no contents, no addresses of physical memory).
Nothing is attached and nothing is changed. Reading takes the process's memory-map lock for short
moments: not during a measurement.

The game's anonymous memory is sorted into:
  - areas the kernel will not back with huge pages at all (and the reason it gives);
  - memory outside whole 2 MiB blocks: areas under 2 MiB, and the unaligned ends of larger areas;
  - memory in whole 2 MiB blocks, by how many of the block's 512 pages are present. A full block is
    a huge page or can become one; the kernel's collector packs a block only if at most
    khugepaged/max_ptes_none pages are missing, and packing fills the missing pages (more RAM used).
"""
import array
import collections
import os
import sys

HUGE = 2 << 20
PAGE = 4096
COMM = os.environ.get("SC_COMM", "StarCitizen.exe")


def game_pid():
    best = (0, None)
    for p in os.listdir("/proc"):
        if p.isdigit():
            try:
                if open(f"/proc/{p}/comm").read().strip() == COMM:
                    best = max(best, (len(os.listdir(f"/proc/{p}/task")), int(p)))
            except OSError:
                pass
    return best[1]


def areas(pid):
    """Every area of the process: dict(start, end, perms, path, and the smaps fields in kB)."""
    out, cur = [], None
    for line in open(f"/proc/{pid}/smaps"):
        first = line.split(None, 1)[0]
        if "-" in first and not first.endswith(":"):
            f = line.split(None, 5)
            a, b = f[0].split("-")
            cur = {"start": int(a, 16), "end": int(b, 16), "perms": f[1], "path": f[5].strip() if len(f) > 5 else ""}
            out.append(cur)
        elif cur is not None:
            key, _, val = line.partition(":")
            val = val.split()
            if key == "VmFlags":
                cur["flags"] = val
            elif val and val[0].isdigit():
                cur[key] = int(val[0])
    return out


def main():
    args = [a for a in sys.argv[1:] if a != "--layout"]
    layout = "--layout" in sys.argv[1:]
    pid = int(args[0]) if args else game_pid()
    if not pid:
        sys.exit(f"{COMM} is not running")
    tdir = "/sys/kernel/mm/transparent_hugepage"
    setting = open(f"{tdir}/enabled").read().strip()
    limit = int(open(f"{tdir}/khugepaged/max_ptes_none").read())
    vmas = areas(pid)
    anon = [v for v in vmas if not v["path"] or v["path"].startswith("[anon") or v["path"] == "[heap]"]
    total = sum(v.get("Rss", 0) for v in anon) * 1024
    huge = sum(v.get("AnonHugePages", 0) for v in anon) * 1024
    print(f"pid {pid}; huge pages: {setting}; the collector packs a block with up to {limit} of 512 pages missing")
    print(f"anonymous memory resident: {total / 2**30:.2f} GiB in {sum(1 for v in anon if v.get('Rss'))} areas; in huge pages: {huge / 2**30:.2f} GiB ({100 * huge / max(total, 1):.0f} %)")
    other = [v for v in vmas if v not in anon]
    print(f"everything else resident (mapped files, shared memory, devices): {sum(v.get('Rss', 0) for v in other) / 2**20:.2f} GiB")

    not_eligible = collections.Counter()
    outside = 0                     # resident bytes outside whole 2 MiB blocks
    small_by = collections.Counter()
    blocks = collections.Counter()  # class -> blocks
    present = collections.Counter() # class -> present bytes
    missing = collections.Counter() # class -> bytes that packing would add
    classes = [(512, 512, "full (512 of 512)"), (512 - limit, 511, f"{512 - limit} to 511 (the collector may pack these)"),
               (256, 511 - limit, f"256 to {511 - limit}"), (64, 255, "64 to 255"), (1, 63, "1 to 63")]
    full_in_huge_vmas = 0
    with open(f"/proc/{pid}/pagemap", "rb", buffering=0) as pm:
        for v in anon:
            rss = v.get("Rss", 0) * 1024
            if not rss:
                continue
            if not v.get("THPeligible", 0) and not (layout and "nh" not in v.get("flags", [])):
                why = "marked no-huge-page by the program" if "nh" in v.get("flags", []) else ("area under 2 MiB" if v["end"] - v["start"] < HUGE else "other (not eligible)")
                if why != "area under 2 MiB":
                    not_eligible[why] += rss
                    continue
            lo = (v["start"] + HUGE - 1) // HUGE * HUGE
            hi = v["end"] // HUGE * HUGE
            in_blocks = 0
            if hi > lo:
                n = (hi - lo) // PAGE
                try:
                    raw = os.pread(pm.fileno(), n * 8, lo // PAGE * 8)
                except OSError:
                    raw = b""
                a = array.array("q")
                a.frombytes(raw[:len(raw) // 8 * 8])
                for i in range(0, len(a) - 511, 512):
                    c = sum(1 for e in a[i:i + 512] if e < 0)   # bit 63: the page is present
                    if not c:
                        continue
                    in_blocks += c * PAGE
                    for low, high, label in classes:
                        if low <= c <= high:
                            blocks[label] += 1
                            present[label] += c * PAGE
                            missing[label] += (512 - c) * PAGE
                            break
            rest = max(rss - in_blocks, 0)
            outside += rest
            size = v["end"] - v["start"]
            small_by["areas under 2 MiB" if size < HUGE else "unaligned ends of larger areas (or blocks that span two areas)"] += rest
    sizes = collections.Counter()
    for v in anon:
        if v.get("Rss"):
            size = v["end"] - v["start"]
            sizes["under 64 KiB" if size < 65536 else "64 KiB to 1 MiB" if size < 1 << 20 else "1 to 2 MiB" if size < HUGE
                  else "2 to 16 MiB" if size < 16 << 20 else "16 to 256 MiB" if size < 256 << 20 else "256 MiB and more"] += v["Rss"] * 1024
    print("resident anonymous memory by the size of the area the kernel sees (neighbours with the same rights count as one):")
    for label in ("under 64 KiB", "64 KiB to 1 MiB", "1 to 2 MiB", "2 to 16 MiB", "16 to 256 MiB", "256 MiB and more"):
        if sizes[label]:
            print(f"  {sizes[label] / 2**30:6.2f} GiB  {label}")
    print()
    print("where the resident anonymous memory lies" + (" (layout only: eligibility ignored)" if layout else "") + ":")
    for why, b in not_eligible.most_common():
        print(f"  {b / 2**30:6.2f} GiB  {why}")
    for why, b in small_by.most_common():
        print(f"  {b / 2**30:6.2f} GiB  outside whole 2 MiB blocks: {why}")
    print("  in whole 2 MiB blocks, by pages present in the block:")
    for _, _, label in classes:
        if blocks[label]:
            print(f"  {present[label] / 2**30:6.2f} GiB    {label}: {blocks[label]} blocks; packing them all would add {missing[label] / 2**30:.2f} GiB")
    full = present[classes[0][2]]
    print()
    print(f"full blocks hold {full / 2**30:.2f} GiB and {huge / 2**30:.2f} GiB is in huge pages"
          + (": some full blocks are still made of small pages (not packed yet)" if full - huge > 64 << 20 else ""))
    reach = sum(present[label] for _, _, label in classes)
    print(f"whole blocks hold {100 * reach / max(total, 1):.0f} % of the resident anonymous memory; {100 * outside / max(total, 1):.0f} % lies outside them")
    return 0


if __name__ == "__main__":
    sys.exit(main())
