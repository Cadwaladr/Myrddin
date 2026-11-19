#!/usr/bin/env bash
# Transparent huge pages for every program (the kernel's own setting), switched on or back, for the
# base-rate test: the game keeps about 28 GB in 4 KiB pages and spends 13-17 % of its P-core cycles
# walking page tables (tools/base-probe). Needs root, so it is run through sudo:
#
#   sudo tools/base-thp-root.sh on        huge pages for all memory; the kernel's collector at a moderate pace
#                                         (128 MiB looked at a second). Given before the game is launched, the
#                                         game's memory is in huge pages from the start.
#   sudo tools/base-thp-root.sh pack [MINUTES=10]   for a game that is already running: "on" with the collector
#                                         at full pace until the amount of huge pages stops growing (or MINUTES
#                                         have passed), then the moderate pace. Short hitches while it packs.
#   sudo tools/base-thp-root.sh settle    the collector back to the kernel's slow default pace; huge pages stay on
#   sudo tools/base-thp-root.sh off       everything as it was before "on" (as saved, else Fedora's defaults)
#   tools/base-thp-root.sh status         what is set now (no root needed)
#
# It writes only the files under /sys/kernel/mm/transparent_hugepage listed below. Nothing is attached to
# the game and nothing in it is touched: the kernel itself re-packs memory that lies in whole 2 MiB blocks.
# The settings last until the next boot or until "off".
set -u
here="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
t=/sys/kernel/mm/transparent_hugepage
saved="$here/logs/base/thp-before.txt"
files="enabled defrag khugepaged/pages_to_scan khugepaged/scan_sleep_millisecs khugepaged/alloc_sleep_millisecs khugepaged/max_ptes_none"
now() { v="$(cat "$t/$1")"; case "$v" in *\[*) v="${v#*[}"; v="${v%%]*}" ;; esac; echo "$v"; }
status() { for f in $files; do printf '  %-36s %s\n' "$f" "$(now "$f")"; done; grep -E '^(AnonHugePages|MemFree|MemAvailable):' /proc/meminfo | sed 's/^/  /'; }
put() { echo "$2" > "$t/$1" || echo "could not write $1"; }
case "${1:-}" in
status) status; exit 0 ;;
on|settle|off|pack) ;;
*) sed -n 2,21p "$0"; exit 1 ;;
esac
[ "$(id -u)" = 0 ] || { echo "run this through sudo"; exit 1; }
huge_mb() { awk '/^AnonHugePages:/{print int($2 / 1024)}' /proc/meminfo; }
settle() {
    put khugepaged/pages_to_scan 4096
    put khugepaged/scan_sleep_millisecs 10000
    put khugepaged/alloc_sleep_millisecs 60000
}
moderate() {   # 128 MiB looked at a second
    put khugepaged/pages_to_scan 32768
    put khugepaged/scan_sleep_millisecs 1000
    put khugepaged/alloc_sleep_millisecs 5000
}
case "$1" in
on|pack)
    if [ ! -e "$saved" ]; then
        mkdir -p "$(dirname "$saved")"
        for f in $files; do echo "$f $(now "$f")"; done > "$saved"
        chown "${SUDO_UID:-0}:${SUDO_GID:-0}" "$saved"
    fi
    put enabled always
    # a fault takes a huge page when one is free and otherwise leaves the packing to the background
    put defrag defer+madvise
    # the collector packs blocks with up to 64 of 512 pages never touched (the default, 511, would pack
    # nearly empty blocks and waste RAM)
    put khugepaged/max_ptes_none 64
    moderate
    if [ "$1" = pack ]; then
        # full pace: 256 MiB looked at per round, ten rounds a second
        put khugepaged/pages_to_scan 65536
        put khugepaged/scan_sleep_millisecs 100
        put khugepaged/alloc_sleep_millisecs 1000
        mins="${2:-10}"; case "$mins" in ''|*[!0-9]*) mins=10 ;; esac
        echo "huge pages on; packing (at most $mins minutes). Memory in huge pages, whole machine:"
        end=$(( $(date +%s) + mins * 60 )); last=$(huge_mb); quiet=0
        while [ "$(date +%s)" -lt "$end" ]; do
            sleep 10
            h=$(huge_mb); echo "  $(date +%T)  $h MiB"
            if [ $((h - last)) -lt 64 ]; then quiet=$((quiet + 1)); else quiet=0; fi
            last=$h
            # three quiet rounds in a row, once something has been packed at all
            [ "$quiet" -ge 3 ] && [ "$h" -gt 1024 ] && break
        done
        moderate
        echo "collector back to the moderate pace"
    fi
    ;;
settle)
    settle
    ;;
off)
    if [ -e "$saved" ]; then
        while read -r f v; do put "$f" "$v"; done < "$saved"
        rm -f "$saved"
    else
        put enabled madvise; put defrag madvise
        put khugepaged/pages_to_scan 4096; put khugepaged/scan_sleep_millisecs 10000
        put khugepaged/alloc_sleep_millisecs 60000; put khugepaged/max_ptes_none 511
    fi
    ;;
esac
echo "now:"; status
