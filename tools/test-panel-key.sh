#!/usr/bin/env bash
# The performance panel's key (fg/panel_key.h), end to end and headless: real key presses, sent by a
# compositor to the test program's window, have to step the layer's panel on.
#
#   tools/test-panel-key.sh        (about 30 s; PASS/FAIL lines, exit 1 on a failure)
#
# What runs: a KWin of its own with no screen (kwin_wayland --virtual) and its own Xwayland, on a
# message bus of its own (dbus-run-session), with a configuration folder of its own, so that
# nothing of the desktop is read, changed or shown. In it:
#   Wayland  build/test_chain/test_chain under the layer, launched with the panel. Keys are pressed
#            through KWin's test interface (probe/fake_key.c). Shift+7 steps the panel on; 7 alone
#            and Ctrl+Shift+7 do not; the control file PANEL changes the key while the program runs;
#            the layer writes the step it is at back into PANEL; a key it does not know is refused.
#   X11      probe/panel_key_test.cpp reads the keyboard the way the layer does for a game that runs
#            through X11 (Xwayland here, with an X11 window that has the keyboard).
#   names    how key names are read (no compositor needed).
# Results in logs/test/panel-key. TEST_LIB=path/libsc_fg.so: that build of the layer.
set -u
here="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
out="$here/logs/test/panel-key"
bin="$here/build/test_chain"
socket=scfg-key
waits() { sleep "$1"; }
press() { WAYLAND_DISPLAY=$socket "$bin/fake_key" "$@"; }

if [ "${1:-}" = --inside ]; then
    # --- in the headless KWin: WAYLAND_DISPLAY, DISPLAY and XAUTHORITY are its own ---
    echo "inside: WAYLAND_DISPLAY=$WAYLAND_DISPLAY DISPLAY=${DISPLAY:-}" > "$out/inside.txt"
    echo "1 tl" > "$out/PANEL"   # (what a launch with the panel writes)
    (
        waits 4;   press 42:1 8:1 8:0 42:0                 # Shift+7: step 2
        waits 1;   press 42:1 8:1 8:0 42:0                 # Shift+7: step 3
        waits 1;   press 8:1 8:0                           # 7 alone: nothing
        waits 1;   press 29:1 42:1 8:1 8:0 42:0 29:0       # Ctrl+Shift+7: nothing
        waits 1;   cp "$out/PANEL" "$out/PANEL.at8"        # (what the layer wrote)
                   echo "3 tr Ctrl+F9" > "$out/PANEL.tmp"; mv "$out/PANEL.tmp" "$out/PANEL"   # another corner, another key
        waits 1.5; press 42:1 8:1 8:0 42:0                 # the old key: nothing
        waits 1;   press 29:1 67:1 67:0 29:0               # Ctrl+F9: off
        waits 1;   press 29:1 67:1 67:0 29:0               # Ctrl+F9: step 1
        waits 1;   cp "$out/PANEL" "$out/PANEL.at13"
                   echo "1 tr Shift+Enter" > "$out/PANEL.tmp"; mv "$out/PANEL.tmp" "$out/PANEL"   # not a key
    ) >> "$out/inside.txt" 2>&1 &
    env DISPLAY= SC_FG=1 SC_FG_MATCH=test_chain SC_FG_TEST_INPUTS=1 SC_FG_TARGET=60 SC_FG_MULT=2 SC_FG_MAX_BASE=0 \
        SC_FG_PANEL=1 SC_FG_DIR="$out" VK_ADD_IMPLICIT_LAYER_PATH="$out/layer" DISABLE_LSFGVK=1 \
        timeout 40 "$bin/test_chain" 17 > "$out/out.txt" 2>&1
    echo "exit $?" >> "$out/out.txt"
    wait
    # X11: the keyboard as the X server reports it. Xwayland is sent keys while an X11 window has
    # the keyboard, so one is opened (in the headless KWin), and the keys are pressed as above.
    if [ -n "${DISPLAY:-}" ]; then
        python3 -I -c '
import ctypes, sys, time
x = ctypes.CDLL("libX11.so.6")
x.XOpenDisplay.restype = ctypes.c_void_p
display = x.XOpenDisplay(None)
if not display: sys.exit("no X display")
x.XDefaultRootWindow.argtypes = [ctypes.c_void_p]; x.XDefaultRootWindow.restype = ctypes.c_ulong
x.XCreateSimpleWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong] + [ctypes.c_int] * 2 + [ctypes.c_uint] * 3 + [ctypes.c_ulong] * 2
x.XCreateSimpleWindow.restype = ctypes.c_ulong
window = x.XCreateSimpleWindow(display, x.XDefaultRootWindow(display), 0, 0, 400, 300, 0, 0, 0)
for call in (x.XSelectInput, x.XMapWindow): call.argtypes = [ctypes.c_void_p, ctypes.c_ulong] + ([ctypes.c_long] if call is x.XSelectInput else [])
x.XSelectInput(display, window, 3)   # key presses and releases
x.XMapWindow(display, window)
x.XFlush.argtypes = [ctypes.c_void_p]; x.XFlush(display)
time.sleep(float(sys.argv[1]))
' 9 >> "$out/inside.txt" 2>&1 &
        waits 2
        "$bin/panel_key_test" x11 Shift+7 6 > "$out/x11.txt" 2>&1 &
        waits 1;   press 42:1 8:1 8:0 42:0                 # a press
        waits 0.3; press 8:1 8:0                           # 7 alone
        waits 0.3; press 29:1 42:1 8:1 8:0 42:0 29:0       # Ctrl+Shift+7
        waits 0.3; press 42:1 8:1; waits 0.6; press 8:0 42:0   # held: one press
        wait
    else
        echo "no DISPLAY in the headless KWin" > "$out/x11.txt"
    fi
    exit 0
fi

# --- outside ---
if pgrep -x StarCitizen.exe > /dev/null; then echo "the game is running: not now (the GPU is shared)"; exit 1; fi
for tool in kwin_wayland dbus-run-session python3; do
    command -v $tool > /dev/null || { echo "$tool is needed for this test"; exit 1; }
done
lib="$here/build/fg/libsc_fg.new.so"; [ -f "$lib" ] || lib="$here/build/fg/libsc_fg.so"
lib=${TEST_LIB:-$lib}
[ -x "$bin/test_chain" ] || { echo "build/test_chain/test_chain is missing (./build.sh)"; exit 1; }
cc -O1 -Wall -Wno-unused-parameter -o "$bin/fake_key" "$here/probe/fake_key.c" -lwayland-client || exit 1
c++ -std=c++20 -O1 -Wall -o "$bin/panel_key_test" "$here/probe/panel_key_test.cpp" -ldl || exit 1
rm -rf "$out"; mkdir -p "$out/layer" "$out/config" "$out/cache" "$out/data"
cp "$lib" "$out/layer/libsc_fg.so"; cp "$here/build/fg/sc_fg.json" "$out/layer/"

fail=0
result() { printf '%-4s %s\n' "$1" "$2"; [ "$1" = FAIL ] && fail=1; return 0; }
has() { grep -q -- "$1" "$out"/fg-*.log 2>/dev/null; }
count() { cat "$out"/fg-*.log 2>/dev/null | grep -c -- "$1"; }

"$bin/panel_key_test" names > "$out/names.txt" 2>&1 && result PASS "names: $(grep -c '^PASS' "$out/names.txt") forms read as they should" \
    || result FAIL "names: $(grep '^FAIL' "$out/names.txt" | head -3 | tr '\n' ' ')"

# (KWin ends when the part inside has ended, and the bus with it)
env -u DISPLAY -u WAYLAND_DISPLAY -u XAUTHORITY -u QT_QPA_PLATFORM KWIN_WAYLAND_NO_PERMISSION_CHECKS=1 \
    XDG_CONFIG_HOME="$out/config" XDG_CACHE_HOME="$out/cache" XDG_DATA_HOME="$out/data" \
    timeout 90 dbus-run-session -- kwin_wayland --virtual --xwayland --width 1280 --height 720 --socket "$socket" \
    --no-lockscreen --no-global-shortcuts --no-kactivities --exit-with-session "$0 --inside" > "$out/kwin.txt" 2>&1
[ -f "$out/out.txt" ] || { result FAIL "the headless KWin did not start the test (logs/test/panel-key/kwin.txt)"; exit 1; }

[ "$(sed -n 's/^exit //p' "$out/out.txt")" = 0 ] && result PASS "the test program ran to its end ($(grep -o '[0-9]* presents' "$out/out.txt" | tail -1))" \
    || result FAIL "the test program: $(tail -2 "$out/out.txt" | tr '\n' ' ')"
has "Shift+7 steps it on, read from the game's own keyboard (Wayland)" && result PASS "the keyboard is read on the program's own Wayland connection" \
    || result FAIL "no keyboard was found on the program's Wayland connection"
has "performance panel: the shown rate and the game's, top left (Shift+7)" && has "performance panel: both rates and the pacing graph, top left (Shift+7)" \
    && result PASS "Shift+7 pressed twice: step 1 -> 2 -> 3" || result FAIL "Shift+7 did not step the panel on"
[ "$(count '(Shift+7)$')" = 2 ] && result PASS "7 alone, Ctrl+Shift+7 and Shift+7 after the key was changed: nothing" \
    || result FAIL "$(count '(Shift+7)$') steps by Shift+7, 2 expected"
[ "$(cat "$out/PANEL.at8" 2>/dev/null)" = "3 tl Shift+7" ] && result PASS "the layer wrote its step into the control file (3 tl Shift+7)" \
    || result FAIL "the control file after two presses: \"$(cat "$out/PANEL.at8" 2>/dev/null)\""
has "performance panel: both rates and the pacing graph, top right$" && has "performance panel: the key is Ctrl+F9 now" \
    && result PASS "the control file moved the panel and changed the key while the program ran" || result FAIL "the control file's corner or key was not taken"
has "performance panel: off (Ctrl+F9)" && has "performance panel: the shown rate, top right (Ctrl+F9)" \
    && result PASS "Ctrl+F9 pressed twice: off, then step 1" || result FAIL "Ctrl+F9 did not step the panel"
[ "$(cat "$out/PANEL.at13" 2>/dev/null)" = "1 tr Ctrl+F9" ] && result PASS "the control file keeps the new key (1 tr Ctrl+F9)" \
    || result FAIL "the control file after the key change: \"$(cat "$out/PANEL.at13" 2>/dev/null)\""
has '"Shift+Enter" is no key this layer knows; it stays Ctrl+F9' && result PASS "a key that is not known is refused, and said so" \
    || result FAIL "the unknown key was not refused"
[ "$(tail -1 "$out/x11.txt")" = "presses 2 source X11" ] && result PASS "X11: Shift+7 twice (one of them held) counted as two presses; 7 alone and Ctrl+Shift+7 not" \
    || result FAIL "X11: $(tail -1 "$out/x11.txt")"
left="$(pgrep -a kwin_wayland | grep -c -- "--socket $socket")"
[ "$left" = 0 ] && result PASS "the headless KWin has ended" || result FAIL "a headless KWin on $socket is still running"
exit $fail
