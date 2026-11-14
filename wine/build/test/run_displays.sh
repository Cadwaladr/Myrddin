#!/usr/bin/env bash
# usage: run_displays.sh [VAR=value ...]
# What the runner's Wayland driver tells a program about the displays of the compositor in
# WAYLAND_DISPLAY (test_displays.exe: a console program, it opens no window), in the isolated
# test prefix. Example: run_displays.sh WAYLANDDRV_PRIMARY_MONITOR=HDMI-A-1
set -u
here=$(cd "$(dirname "$0")" && pwd)
compositor=${WAYLAND_DISPLAY:-}
[ -n "$compositor" ] || { echo "WAYLAND_DISPLAY is not set" >&2; exit 1; }
. "$here/env.sh" # (no display, both drivers off: the prefix is made that way, without any window)
if [ ! -f "$here/test_displays.exe" ] || [ "$here/test_displays.c" -nt "$here/test_displays.exe" ]; then
    nice -n 19 podman run --rm --security-opt label=disable -v "$here:/t" -w /t localhost/sc-wine-builder \
        x86_64-w64-mingw32-gcc -O2 -Wall -o test_displays.exe test_displays.c -luser32 -lgdi32 || exit 1
fi
if [ ! -f "$WINEPREFIX/system.reg" ]; then
    nice -n 19 "$PATCHED/bin/wine" wineboot -i > /dev/null 2>&1
    "$PATCHED/bin/wineserver" -w 2> /dev/null
fi
export WAYLAND_DISPLAY="$compositor"
export WINEDLLOVERRIDES="winemenubuilder.exe=d;mscoree=d;mshtml=d;winex11.drv=d"
env "$@" nice -n 19 "$PATCHED/bin/wine" "$here/test_displays.exe" 2>&1 | grep -vE 'MESA|pci id|dri2'
"$PATCHED/bin/wineserver" -k 2> /dev/null
"$PATCHED/bin/wineserver" -w 2> /dev/null
