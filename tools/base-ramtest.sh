#!/usr/bin/env bash
# The RAM's real speed against what the BIOS says it is set to (see tools/base-ramtest.c). About 25 s on six
# P-cores; do not run it while something is being measured. No root, nothing is changed.
set -eu
here="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
bin="${TMPDIR:-/tmp}/base-ramtest.$$"; trap 'rm -f "$bin"' EXIT
gcc -O2 -mavx2 -o "$bin" "$here/tools/base-ramtest.c" -lpthread
echo "what the BIOS reports:"
udevadm info -e | grep -E 'MEMORY_DEVICE_[0-9]+_(CONFIGURED_SPEED_MTS|PART_NUMBER|SIZE|CONFIGURED_VOLTAGE)=' | sed 's/^E: /  /' | paste - - - - | sed 's/\t/  /g'
# one logical CPU of each P-core (the first of each pair of siblings)
cpus="$(python3 - <<'PY'
import os
def cpus(path):
    out = set()
    try:
        for part in open(path).read().strip().split(","):
            a, _, b = part.partition("-")
            out.update(range(int(a), int(b or a) + 1))
    except (OSError, ValueError):
        pass
    return out
p = cpus("/sys/devices/cpu_core/cpus") or cpus("/sys/devices/system/cpu/online")
first = sorted(c for c in p if min(cpus(f"/sys/devices/system/cpu/cpu{c}/topology/thread_siblings_list") or {c}) == c)
print(" ".join(map(str, first[:8])))
PY
)"
[ -n "$cpus" ] || cpus="$(seq 0 $(( $(nproc) - 1 )) | tr '\n' ' ')"
echo "measured (P-cores $cpus):"
# shellcheck disable=SC2086
"$bin" $cpus | sed 's/^/  /'
