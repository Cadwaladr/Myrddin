#!/usr/bin/env bash
# A picture of the launcher window (gui/myrddin-gui), taken in a headless Weston that draws in
# software: never on the desktop, and without the GPU, so it may run while a game does. The window
# is a dry run: nothing is launched and no command is sent to a running game.
#
#   tools/gui-shot.sh [NAME] [myrddin-gui options]      -> logs/test/gui-NAME/wayland-screenshot-*.png
#
#   GUI_SIZE=900x640   the headless screen          GUI_DARK=1   the dark colour scheme
here="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
cd "$here" || exit 1
name="${1:-shot}"; [ $# -gt 0 ] && shift
out="$here/logs/test/gui-$name"
rm -rf "$out"; mkdir -p "$out/config"
size="${GUI_SIZE:-900x640}"
socket="myrddin-gui-$$"
weston --backend=headless --renderer=pixman --debug --socket="$socket" --width="${size%x*}" --height="${size#*x}" > "$out/weston.txt" 2>&1 &
weston=$!
sleep 2
[ "${GUI_DARK:-0}" = 1 ] && export ADW_DEBUG_COLOR_SCHEME=prefer-dark
WAYLAND_DISPLAY="$socket" GDK_BACKEND=wayland GSK_RENDERER=cairo XDG_CONFIG_HOME="$out/config" \
    "$here/gui/myrddin-gui" --dry-run "$@" > "$out/gui.txt" 2>&1 &
gui=$!
sleep 4
( cd "$out" && WAYLAND_DISPLAY="$socket" weston-screenshooter > /dev/null 2>&1 )
kill "$gui" 2> /dev/null; sleep 0.3; kill "$weston" 2> /dev/null; wait 2> /dev/null
ls "$out"/*.png 2> /dev/null || { echo "no picture; see $out/gui.txt"; cat "$out/gui.txt"; exit 1; }
[ -s "$out/gui.txt" ] && { echo "-- the window said:"; cat "$out/gui.txt"; }
exit 0
