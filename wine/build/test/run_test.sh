#!/usr/bin/env bash
# usage: run_test.sh <runner-dir> [VAR=value ...]   -- runs test_cpusets.exe in the isolated test prefix
# and prints Wine's view plus the Linux kernel's view (Cpus_allowed_list) of every thread.
set -u
here=$(cd "$(dirname "$0")" && pwd)
. "$here/env.sh"
runner=$1; shift
out=$(mktemp)
env "$@" nice -n 19 "$runner/bin/wine" "$here/test_cpusets.exe" 5 > "$out" 2>&1 &
for i in $(seq 100); do grep -q READY "$out" && break; sleep 0.1; done
pid=$(pgrep -f 'test_cpusets.exe' | while read p; do [ "$(cat /proc/$p/comm 2>/dev/null)" = test_cpusets.ex ] && echo $p; done | head -1)
echo "== host view (pid $pid)"
for t in /proc/$pid/task/*; do
    c=$(cat $t/comm); case "$c" in t-*|test_cpusets.ex) printf '   %-16s %s\n' "$c" "$(awk '/Cpus_allowed_list/{print $2}' $t/status)";; esac
done | sort
wait
echo "== program output"; grep -vE 'MESA|pci id|dri2' "$out" | sed 's/^/   /'
rm -f "$out"
"$runner/bin/wineserver" -k 2>/dev/null; "$runner/bin/wineserver" -w 2>/dev/null
