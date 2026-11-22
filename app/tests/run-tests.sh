#!/usr/bin/env bash
# What the window does, checked without a display and without the game: every test runs the built
# program against a stand-in runtime (tests/fake-runtime.sh) whose sc-turbo writes down how it was called.
#   tests/run-tests.sh MYRDDIN_BINARY [WORK_DIR]
# Nothing outside WORK_DIR is written; no window appears. Ends with the number of failed checks.
set -u
bin=$(readlink -f "${1:?the myrddin binary}")
here=$(cd "$(dirname "$0")" && pwd)
work=${2:-$(mktemp -d)}; mkdir -p "$work"; work=$(cd "$work" && pwd)
fails=0
ok()   { echo "  ok    $1"; }
bad()  { echo "  FAIL  $1"; fails=$((fails + 1)); }
has()  { grep -qxF -- "$2" "$1" 2>/dev/null && ok "$3" || { bad "$3 (no line \"$2\" in $(basename "$1"))"; }; }
hasnt(){ grep -qE -- "$2" "$1" 2>/dev/null && bad "$3 (found /$2/ in $(basename "$1"))" || ok "$3"; }
fresh() { rm -rf "$work/t"; eval "$("$here/fake-runtime.sh" "$work/t")"; rt="$work/t/runtime"; }
# Whether the game or an RSI Launcher of this user is open on this machine, found as the window finds
# them (Backend::launcherOpen): the program one of Wine's, the command line naming the launcher.
real_session() {
    local dir exe
    pgrep -x StarCitizen.exe > /dev/null && return 0
    for dir in /proc/[0-9]*; do
        [ -O "$dir" ] || continue
        exe="$(readlink "$dir/exe" 2>/dev/null)" || continue
        case "${exe##*/}" in wine*) ;; *) continue ;; esac
        tr '\0' ' ' < "$dir/cmdline" 2>/dev/null | grep -qi 'rsi launcher\.exe' && return 0
    done
    return 1
}
# the window, drawn nowhere, for SECONDS/1000, then gone
window() {   # STATE SETTLE_MS [args...]
    local state=$1 settle=$2; shift 2
    MYRDDIN_FAKE=$state QT_FORCE_STDERR_LOGGING=1 QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
        timeout 40 "$bin" --screenshot "$work/last.png" --settle "$settle" "$@" 2> "$work/last.err"
    local code=$?
    grep -v '^QProcess: Destroyed' "$work/last.err" | sed 's/^/        /' | cut -c1-240
    return $code
}

echo "1. a launch from the window"
fresh
window idle 2500 --page play --test-eval 'backend.setMode("3"); backend.setOverlay("panel"); backend.conf.scheduler = "lavd"; backend.launch()' || bad "the window ran"
sleep 0.5
has "$rt/calls.log" "fg 3 fixed panel" "sc-turbo was called with the chosen words (x3, held; Myrddin's panel)"
hasnt "$rt/sc-turbo.conf" 'SC_MANGOHUD' "the panel as the overlay: no MangoHud in sc-turbo.conf"
window idle 2500 --test-eval 'backend.setOverlay("mangohud")' || bad "the window ran"
grep -q '^panel=false' "$MYRDDIN_CONFIG_DIR/myrddin.conf" && ok "MangoHud as the overlay takes the panel's place" || bad "MangoHud as the overlay takes the panel's place"
has "$rt/sc-turbo.conf" "SC_PREFIX='$work/t/prefix'" "the game folder is in sc-turbo.conf"
has "$rt/sc-turbo.conf" "SC_MANGOHUD=1" "MangoHud is in sc-turbo.conf"
has "$rt/sc-turbo.conf" "SC_SCHED=lavd" "the scheduler is in sc-turbo.conf"
bash -n "$rt/sc-turbo.conf" && ok "sc-turbo.conf is valid shell" || bad "sc-turbo.conf is valid shell"
hasnt "$rt/calls.env" '^(APPDIR|APPIMAGE|LD_LIBRARY_PATH|QT_|QML|MYRDDIN_)' "nothing of the window's environment reaches sc-turbo"
grep -q 'Frame generation x3' "$rt/logs/launch.out" 2>/dev/null && ok "sc-turbo's words are kept for the window" || bad "sc-turbo's words are kept for the window"
grep -q '^mode=3' "$MYRDDIN_CONFIG_DIR/myrddin.conf" && ok "the multiplier is remembered" || bad "the multiplier is remembered"

echo "2. off: the game as it is, with the overlay that is chosen; \"Run with logs\"; the switches of Options at a launch"
fresh
window idle 2500 --test-eval 'backend.setMode("off"); backend.launch()' || bad "the window ran"
sleep 0.5
has "$rt/calls.log" "run" "\"run\" for Off"
fresh
window idle 2500 --test-eval 'backend.setMode("off"); backend.prefs.watch = true; backend.launch()' || bad "the window ran"
sleep 0.5
has "$rt/calls.log" "run watch" "Off with logs: \"run watch\""
fresh
window idle 2500 --test-eval 'backend.setMode("off"); backend.setOverlay("mangohud"); backend.launch()' || bad "the window ran"
sleep 0.5
has "$rt/calls.log" "run" "Off with MangoHud: \"run\" ..."
has "$rt/sc-turbo.conf" "SC_MANGOHUD=1" "... with SC_MANGOHUD=1 in sc-turbo.conf, which sc-turbo's \"run\" takes too"
fresh
window idle 2500 --page play --test-eval 'backend.setMode("off"); backend.setOverlay("panel"); backend.prefs.watch = true; backend.launch()' \
    --test-eval '@900 console.warn("overlay words: " + (function find(o) { if (!o) return ""; if (typeof o.few === "string" && typeof o.generating === "boolean") return o.few; for (var i = 0; i < o.children.length; i++) { var r = find(o.children[i]); if (r) return r } return "" })(window.contentItem))' || bad "the window ran"
sleep 0.5
has "$rt/calls.log" "fg 4 panel off watch" "Off with Myrddin's panel: the layer loaded with nothing generated, up to the limit when switched on (\"fg 4 panel off\")"
grep -q 'overlay words: The game.s rate and pacing; nothing is generated' "$work/last.err" && ok "and Play says what the panel shows then" || bad "and Play says what the panel shows then ($(grep -o 'overlay words: .*' "$work/last.err"))"
fresh
fresh
window idle 2500 --test-eval 'backend.panel(backend.conf.fill === false ? "tl" : "tr")' || bad "the window ran"
has "$rt/calls.log" "fg-panel tl" "\"Limit the game's own frame rate\" is off at first"
hasnt "$rt/sc-turbo.conf" 'SC_FG_FILL' "which is what a launch does anyway: no line for it"
fresh
window idle 2500 --test-eval 'backend.setMode("4"); backend.prefs.watch = true; backend.setOption("markers", false); backend.setOption("keepHud", false); backend.setOption("gapfill", false); backend.setOption("pictures", false); backend.setOption("boost", true); backend.setOption("fill", true); backend.setOption("rm -rf", true); backend.launch()' || bad "the window ran"
sleep 0.5
has "$rt/calls.log" "fg 4 fixed watch" "\"watch\" is passed"
hasnt "$rt/calls.log" '^fg-(params|hud)' "no game runs: nothing is sent to one"
has "$rt/sc-turbo.conf" "SC_FG_MARKERS=0" "markers off is in sc-turbo.conf"
has "$rt/sc-turbo.conf" "SC_FG_KEEP_HUD=0" "the HUD not kept apart is in sc-turbo.conf"
has "$rt/sc-turbo.conf" "export SC_FG_GAPFILL=0" "frames without the HUD not bridged: exported from sc-turbo.conf"
has "$rt/sc-turbo.conf" "export SC_FG_PICTURES=0" "the map moved: exported from sc-turbo.conf"
has "$rt/sc-turbo.conf" "export SC_FG_BOOST=1" "the GPU's speed held up: exported from sc-turbo.conf"
has "$rt/sc-turbo.conf" "SC_FG_FILL=1" "the game's frames limited to fill the display: in sc-turbo.conf"
hasnt "$rt/sc-turbo.conf" 'rm -rf' "a switch that is not one of them is not written"
( . "$rt/sc-turbo.conf"; [ "$(sh -c 'echo $SC_FG_GAPFILL$SC_FG_PICTURES$SC_FG_BOOST')" = 001 ] ) && ok "a program started after it sees the three" || bad "a program started after it sees the three"
window idle 2500 --test-eval 'backend.conf.targetRate = "100"' || bad "the window ran"
[ "$(grep -c 'SC_FG_GAPFILL\|SC_FG_BOOST\|SC_FG_KEEP_HUD\|SC_FG_PICTURES\|SC_FG_FILL' "$rt/sc-turbo.conf")" = 5 ] && ok "they are read back and written once" || bad "they are read back and written once"
window idle 2500 --test-eval 'backend.setOption("keepHud", true); backend.setOption("gapfill", true); backend.setOption("pictures", true); backend.setOption("boost", false); backend.setOption("fill", false)' || bad "the window ran"
hasnt "$rt/sc-turbo.conf" 'SC_FG_GAPFILL|SC_FG_BOOST|SC_FG_KEEP_HUD|SC_FG_PICTURES|SC_FG_FILL' "back at their defaults the lines are taken out"

echo "3. nothing is launched while something is in the way"
fresh; rm "$work/t/prefix/sc-launch.sh"
window idle 2500 --test-eval 'backend.launch()' || bad "the window ran"
hasnt "$rt/calls.log" '^(fg|run)' "no launch without a launch script"
fresh
window launcher 2500 --test-eval 'backend.launch()' || bad "the window ran"
hasnt "$rt/calls.log" '^(fg|run)' "no second launch while the launcher is open"

echo "4. in the running game"
fresh
window running 3000 --test-eval 'backend.setMode("3"); backend.setOption("markers", false); backend.setOption("keepHud", false); backend.setOption("boost", true); backend.setOption("fill", true); backend.setOption("bad;name", true); backend.setMode("$(id)"); backend.panel("tr"); backend.panel("--help")' || bad "the window ran"
has "$rt/calls.log" "fg-mult 3 fixed" "a multiplier of one's own is sent, held"
has "$rt/calls.log" "fg-params markers=0" "a switch of Options is sent"
has "$rt/calls.log" "fg-hud off" "the HUD switch is sent"
has "$rt/calls.log" "fg-params boost=1" "the GPU switch is sent"
has "$rt/calls.log" "fg-params fill=1" "\"Limit the game's own frame rate\" is sent as the layer's \"fill\""
has "$rt/calls.log" "fg-panel tr" "the panel's corner is sent"
has "$rt/sc-turbo.conf" "SC_FG_MARKERS=0" "and what was switched is kept for the next launch"
hasnt "$rt/calls.log" 'bad|id\)|^rm|help' "words outside the lists are not sent"
hasnt "$rt/calls.log" '^(fg [0-9]|run)' "nothing is launched"
window running 2500 --test-eval 'backend.setOption("fill", false)' || bad "the window ran"
has "$rt/calls.log" "fg-params fill=0" "and off again"
hasnt "$rt/sc-turbo.conf" 'SC_FG_FILL' "which is what a launch does anyway: no line for it"
window running 2500 --test-eval 'backend.setMode("off")' || bad "the window ran"
has "$rt/calls.log" "fg-off" "Off stops generation"
[ -e "$rt/logs/fg/OFF" ] && ok "the OFF file is there" || bad "the OFF file is there"

echo "5. a settings file written by hand survives"
fresh
cat > "$rt/sc-turbo.conf" <<CONF
# my own note
SC_PREFIX="\$HOME/somewhere else"
SC_LAUNCH="\$SC_PREFIX/start.sh"
SC_FG_PRIORITY=5
export SC_FG_FILL=1
export WINE_SOMETHING="a b"
CONF
window idle 2500 --test-eval 'backend.conf.targetRate = "144"' || bad "the window ran"
has "$rt/sc-turbo.conf" "SC_PREFIX='$HOME/somewhere else'" "a \$HOME path is kept (written out)"
has "$rt/sc-turbo.conf" "SC_LAUNCH='$HOME/somewhere else/start.sh'" "a \$SC_PREFIX path is kept (written out)"
has "$rt/sc-turbo.conf" "SC_FG_TARGET=144" "the new value is written"
has "$rt/sc-turbo.conf" "SC_FG_PRIORITY=5" "an unknown setting is kept"
has "$rt/sc-turbo.conf" "SC_FG_FILL=1" "a fill rule of 1 set by hand is read as \"Limit the game's own frame rate\" on, and kept"
has "$rt/sc-turbo.conf" 'export WINE_SOMETHING="a b"' "an unknown export is kept"
has "$rt/sc-turbo.conf" "# my own note" "a comment is kept"
window idle 2500 --test-eval 'backend.conf.targetRate = ""' || bad "the window ran"
[ "$(grep -c 'my own note' "$rt/sc-turbo.conf")" = 1 ] && [ "$(grep -c 'lines the window does not know' "$rt/sc-turbo.conf")" = 1 ] && ok "nothing doubles on the second save" || bad "nothing doubles on the second save"
hasnt "$rt/sc-turbo.conf" 'SC_FG_TARGET' "an emptied value is taken out"
echo 'export SC_FG_FILL=0' >> "$rt/sc-turbo.conf"
window idle 2500 --test-eval 'if (backend.conf.fill === false) backend.conf.targetRate = "165"' || bad "the window ran"
has "$rt/sc-turbo.conf" "SC_FG_TARGET=165" "a fill rule of 0 set by hand after it is read as \"Limit the game's own frame rate\" off"
hasnt "$rt/sc-turbo.conf" 'SC_FG_FILL' "which is what a launch does anyway: no line for it"

echo "6. menu entry and terminal command"
fresh
window idle 2000 --test-eval 'backend.setMenuEntry(true); backend.setCliLink(true)' || bad "the window ran"
entry="$XDG_DATA_HOME/applications/io.github.cadwaladr.Myrddin.desktop"
grep -q "^Exec=\"$bin\"$" "$entry" 2>/dev/null && ok "the menu entry starts this program" || bad "the menu entry starts this program"
[ -s "$XDG_DATA_HOME/icons/hicolor/scalable/apps/io.github.cadwaladr.Myrddin.svg" ] && ok "its icon is installed" || bad "its icon is installed"
[ "$(readlink "$MYRDDIN_BIN_DIR/myrddin")" = "$bin" ] && ok "the myrddin link points here" || bad "the myrddin link points here"
command -v desktop-file-validate > /dev/null && { desktop-file-validate "$entry" && ok "the entry is valid" || bad "the entry is valid"; }
echo "not a link" > "$work/t/keep"; rm "$MYRDDIN_BIN_DIR/myrddin"; cp "$work/t/keep" "$MYRDDIN_BIN_DIR/myrddin"
window idle 2000 --test-eval 'backend.setCliLink(true); backend.setMenuEntry(false)' || bad "the window ran"
[ "$(cat "$MYRDDIN_BIN_DIR/myrddin")" = "not a link" ] && ok "a file of that name that is not a link is left alone" || bad "a file of that name that is not a link is left alone"
[ ! -e "$entry" ] && ok "the menu entry is removed again" || bad "the menu entry is removed again"

echo "7. from a terminal: words go to sc-turbo, no display needed"
fresh
( unset QT_QPA_PLATFORM; "$bin" fg 4 hud > "$work/cli.out" 2>&1 ); code=$?
[ $code = 0 ] && ok "exit code 0" || bad "exit code 0 (got $code)"
has "$rt/calls.log" "fg 4 hud" "sc-turbo got the words"
grep -q 'Frame generation x4' "$work/cli.out" && ok "its output comes through" || bad "its output comes through"
( unset QT_QPA_PLATFORM; "$bin" > "$work/cli.out" 2>&1 < /dev/null ); [ $? = 1 ] && grep -q 'no display' "$work/cli.out" && ok "without a display the window says so and ends" || bad "without a display the window says so and ends"
( "$bin" nonsense-word > /dev/null 2>&1 ); [ $? = 0 ] && ok "sc-turbo's exit code comes back (0 from the stand-in)" || bad "exit code of an unknown word"

echo "8. the runtime an AppImage carries is put in place"
fresh
pay="$work/t/payload"; mkdir -p "$pay/tools" "$pay/build/fg"
cp "$rt/sc-turbo" "$pay/sc-turbo"; cp "$rt/tools/sc-sched" "$pay/tools/"; echo lib > "$pay/build/fg/libsc_fg.so"; echo '{}' > "$pay/build/fg/sc_fg.json"
echo "v2" > "$pay/VERSION"; echo "must not arrive" > "$pay/sc-turbo.conf"; chmod -R a-w "$pay"
inst="$XDG_DATA_HOME/myrddin/runtime"
( unset MYRDDIN_RUNTIME; MYRDDIN_PAYLOAD="$pay" "$bin" displays > "$work/cli.out" 2>&1 ) || bad "first start from a payload"
[ -x "$inst/sc-turbo" ] && [ "$(cat "$inst/VERSION")" = v2 ] && ok "copied to the lasting folder" || bad "copied to the lasting folder"
[ -w "$inst/sc-turbo" ] && ok "the copies can be replaced later" || bad "the copies can be replaced later"
[ ! -e "$inst/sc-turbo.conf" ] && ok "a settings file in the payload is not copied" || bad "a settings file in the payload is not copied"
grep -q '^HDMI-A-1' "$work/cli.out" && ok "sc-turbo ran from there" || bad "sc-turbo ran from there"
echo "SC_PREFIX='/keep/me'" > "$inst/sc-turbo.conf"; mkdir -p "$inst/logs/fg"; echo x > "$inst/logs/fg/fg-1.log"
chmod -R u+w "$pay"; echo "v3" > "$pay/VERSION"; rm "$pay/tools/sc-sched"; echo new > "$pay/tools/new-tool"; chmod -R a-w "$pay"
stamp=$(stat -c %Y "$inst/sc-turbo")
( unset MYRDDIN_RUNTIME; MYRDDIN_PAYLOAD="$pay" "$bin" displays > /dev/null 2> "$work/update.err" )
# (a newer version waits while the game or the RSI Launcher is open, and that counts on the machine the
# tests run on too: with either of them open here, the wait is what is checked)
if real_session; then
    [ "$(cat "$inst/VERSION")" = v2 ] && grep -q "once the game and the RSI Launcher are closed" "$work/update.err" \
        && ok "a newer version waits while the game or the RSI Launcher is open here, and says so" \
        || bad "a newer version waits while the game or the RSI Launcher is open here, and says so"
    echo "  skip  a newer version replaces it, and a file it has not is removed (the game or the RSI Launcher is open on this machine)"
else
    [ "$(cat "$inst/VERSION")" = v3 ] && [ -e "$inst/tools/new-tool" ] && ok "a newer version replaces it" || bad "a newer version replaces it"
    [ ! -e "$inst/tools/sc-sched" ] && ok "a file the new version has not is removed" || bad "a file the new version has not is removed"
fi
[ "$(cat "$inst/sc-turbo.conf")" = "SC_PREFIX='/keep/me'" ] && [ -e "$inst/logs/fg/fg-1.log" ] && ok "settings and logs are kept" || bad "settings and logs are kept"
touch -d '2001-01-01' "$inst/sc-turbo"
( unset MYRDDIN_RUNTIME; MYRDDIN_PAYLOAD="$pay" "$bin" displays > /dev/null 2>&1 )
[ "$(stat -c %Y "$inst/sc-turbo")" = "$(date -d 2001-01-01 +%s)" ] && ok "the same version is not copied again" || bad "the same version is not copied again"
chmod -R u+w "$pay"
fresh
mkdir -p "$pay/tools"; cp "$rt/sc-turbo" "$pay/sc-turbo"; echo "v9" > "$pay/VERSION"; echo new > "$pay/tools/new-tool"
before=$(cat "$rt/VERSION")
MYRDDIN_PAYLOAD="$pay" "$bin" displays > /dev/null 2>&1
[ "$(cat "$rt/VERSION")" = "$before" ] && [ ! -e "$rt/tools/new-tool" ] && ok "a runtime that was named is never written over" || bad "a runtime that was named is never written over"

echo "9. the log of a session that ran with logs"
fresh
conf="$MYRDDIN_CONFIG_DIR/myrddin.conf"; mkdir -p "$MYRDDIN_CONFIG_DIR" "$rt/logs/io"
pending() { printf '[General]\nwelcomed=true\nwatch=true\nlastLaunchLayer=true\nlogPending=true\nlogSince=%s\nlogCommand=myrddin fg 4 watch\n' "$1" > "$conf"; }
printf 'Display: HDMI-A-1\nlog: %s/x/logs/fg\n' "$HOME" > "$rt/logs/launch.out"
pending 0
window idle 2500 || bad "the window ran"
[ -z "$(ls "$rt/logs/sessions" 2>/dev/null)" ] && ok "no recording: no log is made" || bad "no recording: no log is made"
grep -q '^logPending=false' "$conf" && ok "and nothing is left pending" || bad "and nothing is left pending"
recording() { printf 'time,x\n1,2\n' > "$rt/logs/io/io-$1.csv"; : > "$rt/logs/io/io-$1.threads"; touch -d '-20 seconds' "$rt/logs/io/io-$1.csv"; }   # (finished a while ago)
recording 1234
pending 99999999999999
window idle 2500 || bad "the window ran"
[ -z "$(ls "$rt/logs/sessions" 2>/dev/null)" ] && ok "a recording from before the launch does not count" || bad "a recording from before the launch does not count"
pending 0
window running 2500 || bad "the window ran"
[ -z "$(ls "$rt/logs/sessions" 2>/dev/null)" ] && ok "nothing is made while the game runs" || bad "nothing is made while the game runs"
window idle 3000 || bad "the window ran"
log=$(ls "$rt/logs/sessions"/session-*.log 2>/dev/null | head -1)
[ -s "$log" ] && ok "the log is made when the game has closed" || bad "the log is made when the game has closed"
grep -q '^Launched with: myrddin fg 4 watch$' "$log" && grep -q '^Display: HDMI-A-1$' "$log" && grep -q '^GPU memory: 10.0 GB' "$log" && grep -q 'x4    30.1' "$log" \
    && ok "it holds the launch, the report and the frame rates" || bad "it holds the launch, the report and the frame rates"
has "$rt/calls.log" "iowatch report $rt/logs/io/io-1234.csv" "the report is asked for this session's recording"
hasnt "$log" "$HOME" "the home folder is not in it"
grep -q "^logOffer=$log$" "$conf" && ok "it is offered (and still is after a restart)" || bad "it is offered (and still is after a restart)"
window idle 2500 --test-eval 'backend.readSessionLog(); backend.copySessionLog(); backend.keepSessionLog()' || bad "the window ran (read, copy, keep)"
[ -s "$log" ] && ! grep -q '^logOffer=' "$conf" && ok "kept: the file stays, the offer is gone" || bad "kept: the file stays, the offer is gone"
[ -e "$rt/logs/io/io-1234.csv" ] && ok "keeping leaves the recording" || bad "keeping leaves the recording"
sed -i 's/^logPending=.*/logPending=true/' "$conf"   # (as if the launcher had still been open)
window idle 2500 || bad "the window ran"
[ "$(ls "$rt/logs/sessions" | wc -l)" = 1 ] && grep -q '^logPending=false' "$conf" && ok "a run that has its log gets no second one, and the launch is settled" || bad "a run that has its log gets no second one, and the launch is settled"
# the game closed, the RSI Launcher still open: the log is offered then, and a second run gets its own
pending 0; rm -f "$rt/logs/sessions"/*; sed -i '/^logMadeFor=/d' "$conf"
window launcher 3000 || bad "the window ran"
[ "$(ls "$rt/logs/sessions" | wc -l)" = 1 ] && ok "the log is made while the launcher is still open" || bad "the log is made while the launcher is still open"
grep -q '^logPending=true' "$conf" && ok "the launch stays open for another run of the game" || bad "the launch stays open for another run of the game"
sleep 1.1; recording 5678; touch -d '-5 seconds' "$rt/logs/io/io-5678.csv"
window launcher 3000 || bad "the window ran"
[ "$(ls "$rt/logs/sessions" | wc -l)" = 2 ] && grep -q "^logRecording=$rt/logs/io/io-5678.csv$" "$conf" && ok "a second run from the launcher gets its own log" || bad "a second run from the launcher gets its own log"
rm -f "$rt/logs/io/io-5678".*
pending 0; rm -f "$rt/logs/sessions"/*; sed -i '/^logMadeFor=/d' "$conf"
window idle 3000 || bad "the window ran"
window idle 2500 --test-eval 'backend.deleteSessionLog()' || bad "the window ran (delete)"
[ -z "$(ls "$rt/logs/sessions" 2>/dev/null)" ] && [ ! -e "$rt/logs/io/io-1234.csv" ] && [ ! -e "$rt/logs/io/io-1234.threads" ] && ok "deleted: the log and its recording are gone" || bad "deleted: the log and its recording are gone"
! grep -q '^logOffer=' "$conf" && ok "and the offer with them" || bad "and the offer with them"

echo "10. the desktop's colours"
fresh
printf '[Colors:Window]\nBackgroundNormal=250,240,230\nForegroundNormal=30,20,10\n[Colors:View]\nBackgroundNormal=255,255,255\n[Colors:Button]\nBackgroundNormal=240,230,220\n[Colors:Selection]\nBackgroundNormal=200,60,20\nForegroundNormal=255,255,255\n' > "$work/t/colors"
mkdir -p "$MYRDDIN_CONFIG_DIR"; printf '[General]\nwelcomed=true\n' > "$MYRDDIN_CONFIG_DIR/myrddin.conf"   # (no first-start pages over the window)
MYRDDIN_COLORS="$work/t/colors" window idle 2000 --test-eval 'backend.setMenuEntry(true)' || bad "the window ran"
icon="$XDG_DATA_HOME/icons/hicolor/scalable/apps/io.github.cadwaladr.Myrddin.svg"
grep -q 'fill="#faf0e6"' "$icon" 2>/dev/null && grep -q 'stroke="#c83c14"' "$icon" && grep -q 'fill="#1e140a"' "$icon" && ok "the menu icon is drawn in the scheme's colours" || bad "the menu icon is drawn in the scheme's colours"
python3 "$here/pixel.py" "$work/last.png" 232 60 250 240 230 && ok "the window is drawn in them (its background is the scheme's)" || bad "the window is drawn in them (its background is the scheme's)"
window idle 2000 || bad "the window ran"
grep -q 'fill="#faf0e6"' "$icon" && bad "another scheme at the next start redraws the menu icon" || ok "another scheme at the next start redraws the menu icon"

echo "11. the first start finds the game"
fresh
window idle 2000 || bad "the window ran"
has "$rt/sc-turbo.conf" "SC_PREFIX='$work/t/prefix'" "found through the LUG Helper's note"

echo "12. Off, a multiplier of one's own or Auto, and the highest multiplier"
fresh; conf="$MYRDDIN_CONFIG_DIR/myrddin.conf"
window idle 2500 --test-eval 'backend.launch()' || bad "the window ran"
sleep 0.5
has "$rt/calls.log" "fg 4" "a first launch: Auto, up to x4"
fresh
window idle 2500 --test-eval 'backend.setLimit(3); backend.launch()' || bad "the window ran"
sleep 0.5
has "$rt/calls.log" "fg 3" "Auto: the limit set in Options is the launch's number"
grep -q '^limit=3' "$conf" && ok "the limit is remembered" || bad "the limit is remembered"
fresh
window idle 2500 --test-eval 'backend.setMode("2"); backend.setLimit(3); backend.launch()' || bad "the window ran"
sleep 0.5
has "$rt/calls.log" "fg 2 fixed" "x2 is x2, held, whatever the limit"
fresh
window idle 2500 --test-eval 'backend.setMode("off"); backend.setLimit(2); backend.launch()' || bad "the window ran"
sleep 0.5
has "$rt/calls.log" "run" "Off stays off when the limit is changed"
grep -q '^limit=2' "$conf" && grep -q '^mode=off' "$conf" && ok "and the limit is kept for the next Auto" || bad "and the limit is kept for the next Auto"
fresh; mkdir -p "$MYRDDIN_CONFIG_DIR"; printf '[General]\nwelcomed=true\nmultiplier=3\n' > "$conf"   # (settings from before there was a mode)
window idle 2500 --test-eval 'backend.launch()' || bad "the window ran"
sleep 0.5
has "$rt/calls.log" "fg 3" "a multiplier chosen in an earlier version is Auto with that limit, as it was then"
fresh; mkdir -p "$MYRDDIN_CONFIG_DIR"; printf '[General]\nwelcomed=true\nmultiplier=1\n' > "$conf"
window idle 2500 --test-eval 'backend.launch()' || bad "the window ran"
sleep 0.5
has "$rt/calls.log" "run" "Off chosen in an earlier version stays off"
fresh; mkdir -p "$MYRDDIN_CONFIG_DIR"; printf '[General]\nwelcomed=true\npanel=true\n' > "$conf"; echo "SC_MANGOHUD=1" >> "$rt/sc-turbo.conf"
window idle 2500 --test-eval 'backend.launch()' || bad "the window ran"
sleep 0.5
has "$rt/calls.log" "fg 4" "settings with both overlays: MangoHud alone (no \"panel\" word)"
has "$rt/sc-turbo.conf" "SC_MANGOHUD=1" "and MangoHud stays in sc-turbo.conf"
fresh
window running 3000 --test-eval 'backend.setLimit(2)' || bad "the window ran"
has "$rt/calls.log" "fg-mult 2 auto" "Auto: a new limit reaches the running game"
window running 2500 --test-eval 'backend.setMode("3")' || bad "the window ran"
has "$rt/calls.log" "fg-mult 3 fixed" "x3 reaches the running game, held"
window running 2500 --test-eval 'backend.setLimit(4)' || bad "the window ran"
hasnt "$rt/calls.log" '^fg-mult 4' "a multiplier that is held is not moved by the limit"
window running 2500 --test-eval 'backend.setMode("off")' || bad "the window ran"
has "$rt/calls.log" "fg-off" "Off stops generation in the running game"
window running 2500 --test-eval 'backend.setMode("auto")' || bad "the window ran"
has "$rt/calls.log" "fg-on" "Auto brings it back"
has "$rt/calls.log" "fg-mult 4 auto" "and follows the game's rate, up to the limit"
fresh; mkdir -p "$MYRDDIN_CONFIG_DIR"; printf '[General]\nwelcomed=true\nlastLaunchLayer=true\n' > "$conf"
window launcher 2500 --test-eval 'backend.setOption("markers", false); backend.setMode("2")' || bad "the window ran"
has "$rt/calls.log" "fg-params markers=0" "with the RSI Launcher open a switch is kept for the game it starts"
has "$rt/calls.log" "fg-mult 2 fixed" "and so is the multiplier"

echo "13. a report over the page, and putting it away"
fresh
printf 'Display: HDMI-A-1\n' > "$rt/logs/launch.out"
window idle 2500 --test-eval 'backend.showLaunchLog(); if (backend.toolTitle === "The last launch" && backend.toolOutput.indexOf("HDMI-A-1") >= 0) backend.panel("bl"); backend.closeTool(); if (backend.toolTitle === "") backend.panel("br")' || bad "the window ran"
has "$rt/calls.log" "fg-panel bl" "the last launch's words are shown"
has "$rt/calls.log" "fg-panel br" "and put away again"
window idle 2500 --page session || bad "the window ran (a page that is gone)"
ok "a page that is gone opens Play"

echo "14. the system check says whether the prefix has NVAPI"
level='backend.checks.filter((c) => c.key === "nvapi").map((c) => c.level).join("")'
fresh
window idle 2500 --test-eval "if ($level === \"\") backend.panel(\"tl\")" || bad "the window ran"
has "$rt/calls.log" "fg-panel tl" "a folder that is no prefix yet: no row"
fresh; sys="$work/t/prefix/drive_c/windows/system32"; mkdir -p "$sys"; : > "$sys/nvngx.dll"; : > "$sys/_nvngx.dll"
window idle 2500 --test-eval "if ($level === \"warn\") backend.panel(\"tr\"); if (backend.ready) backend.panel(\"br\")" || bad "the window ran"
has "$rt/calls.log" "fg-panel tr" "a prefix without nvapi64.dll: a warning"
has "$rt/calls.log" "fg-panel br" "which does not stand in the way of a launch"
: > "$sys/nvapi64.dll"; rm "$sys/_nvngx.dll"; ln -s /nonexistent/_nvngx.dll "$sys/_nvngx.dll"
window idle 2500 --test-eval "if ($level === \"warn\") backend.panel(\"bl\")" || bad "the window ran"
has "$rt/calls.log" "fg-panel bl" "NVAPI there but a link to a driver file that is gone: a warning"
rm "$sys/_nvngx.dll"; : > "$sys/_nvngx.dll"; : > "$rt/calls.log"
window idle 2500 --test-eval "if ($level === \"ok\") backend.panel(\"tl\")" || bad "the window ran"
has "$rt/calls.log" "fg-panel tl" "NVAPI and NVIDIA's NGX libraries there: good"

echo "15. the system check says whether the game itself is set to DLSS"
level='backend.checks.filter((c) => c.key === "dlss").map((c) => c.level).join("")'
detail='backend.checks.filter((c) => c.key === "dlss").map((c) => c.detail).join("")'
fresh; games="$work/t/prefix/drive_c/Program Files/Roberts Space Industries/StarCitizen"; attr="$games/LIVE/user/client/0/Profiles/default/attributes.xml"
window idle 2500 --test-eval "if ($level === \"ok\" && $detail.indexOf(\"(LIVE)\") > 0) backend.panel(\"tl\")" || bad "the window ran"
has "$rt/calls.log" "fg-panel tl" "the game's settings name DLSS: good, and the channel is said"
sed -i 's/name="UpscalingTechnique" value="2"/name="UpscalingTechnique" value="0"/' "$attr"
window idle 2500 --test-eval "if ($level === \"warn\") backend.panel(\"tr\"); if (backend.ready) backend.panel(\"br\")" || bad "the window ran"
has "$rt/calls.log" "fg-panel tr" "another upscaler: a warning"
has "$rt/calls.log" "fg-panel br" "which does not stand in the way of a launch"
sed -i '/UpscalingTechnique/d' "$attr"; : > "$rt/calls.log"
window idle 2500 --test-eval "if ($level === \"info\" && $detail.indexOf(\"(LIVE)\") > 0) backend.panel(\"tr\")" || bad "the window ran"
has "$rt/calls.log" "fg-panel tr" "no upscaler named in the settings: said as not known, not as a fault"
mkdir -p "$games/PTU/user/client/0/Profiles/default"
printf '<Attributes Version="1">\n <Attr name="UpscalingTechnique" value="2"/>\n</Attributes>\n' > "$games/PTU/user/client/0/Profiles/default/attributes.xml"
touch -d '-1 hour' "$attr"; : > "$rt/calls.log"
window idle 2500 --test-eval "if ($level === \"ok\" && $detail.indexOf(\"(PTU)\") > 0) backend.panel(\"bl\")" || bad "the window ran"
has "$rt/calls.log" "fg-panel bl" "the channel played last is the one that counts"
rm -rf "$games"; : > "$rt/calls.log"
window idle 2500 --test-eval "if ($level === \"info\") backend.panel(\"tl\")" || bad "the window ran"
has "$rt/calls.log" "fg-panel tl" "no settings file yet: said as not known, not as a fault"

echo "16. Stop and Kill on the launch button (a pointer stands in for the mouse)"
# (the launch button, at the window's own size of 1080x720)
bx=930; by=215
fresh
window idle 2500 --page play --test-eval "@700 pointer.click($bx, $by)" || bad "the window ran"
sleep 0.5
grep -q '^fg ' "$rt/calls.log" && ok "idle: a click on the button launches" || bad "idle: a click on the button launches"
hasnt "$rt/calls.log" '^(stop|kill)$' "and stops nothing"
fresh
window launcher 2200 --page play --test-eval "@700 pointer.click($bx, $by)" --test-eval "@1500 pointer.click($bx, $by)" || bad "the window ran"
sleep 0.3
has "$rt/calls.log" "stop" "the RSI Launcher open: a click on the button asks the session to end (sc-turbo stop)"
[ "$(grep -cx stop "$rt/calls.log")" = 1 ] && ok "a second click while it is stopping asks nothing again" || bad "a second click while it is stopping asks nothing again"
hasnt "$rt/calls.log" '^kill$' "within two seconds there is no Kill"
fresh
window running 4200 --page play --test-eval "@700 pointer.click($bx, $by)" --test-eval "@3300 pointer.click($bx, $by)" || bad "the window ran"
sleep 0.3
has "$rt/calls.log" "stop" "the game running: the same"
has "$rt/calls.log" "kill" "still there two seconds later: the button offers Kill, and a click ends the session (sc-turbo kill)"
fresh
window idle 2500 --page play --test-eval 'backend.stop(); backend.kill()' || bad "the window ran"
hasnt "$rt/calls.log" '^(stop|kill)$' "nothing running: stop and kill do nothing"
fresh
window launcher 2500 --page play --test-eval 'backend.kill()' || bad "the window ran"
hasnt "$rt/calls.log" '^kill$' "no Kill before a Stop"

echo "17. the panel's key, and the window's own switches kept as they were left"
fresh
window idle 2500 --test-eval 'backend.setPanelKey("ctrl+f9")' || bad "the window ran"
has "$rt/sc-turbo.conf" "SC_FG_PANEL_KEY='Ctrl+F9'" "another key for the panel is in sc-turbo.conf, written tidily"
bash -n "$rt/sc-turbo.conf" && ok "sc-turbo.conf is valid shell" || bad "sc-turbo.conf is valid shell"
hasnt "$rt/calls.log" '^fg-panel key' "no game runs: nothing is sent to one"
window idle 2500 --test-eval 'if (backend.conf.panelKey === "Ctrl+F9") backend.panel("tr"); backend.setPanelKey("Super+P"); backend.setPanelKey("Shift+Enter")' || bad "the window ran"
has "$rt/calls.log" "fg-panel tr" "and is read back at the next start"
has "$rt/sc-turbo.conf" "SC_FG_PANEL_KEY='Ctrl+F9'" "a Super key and a key the panel does not know are refused"
window idle 2500 --test-eval 'backend.setPanelKey("Shift+7")' || bad "the window ran"
hasnt "$rt/sc-turbo.conf" 'SC_FG_PANEL_KEY' "Shift+7 is the layer's own key: nothing is written for it"
echo "SC_FG_PANEL_KEY='ctrl+shift+\`'" >> "$rt/sc-turbo.conf"; : > "$rt/calls.log"
window idle 2500 --test-eval 'if (backend.conf.panelKey === "Ctrl+Shift+`") backend.panel("bl")' || bad "the window ran"
has "$rt/calls.log" "fg-panel bl" "a key written by hand is read, signs and all"
fresh; mkdir -p "$MYRDDIN_CONFIG_DIR"; printf '[General]\nwelcomed=true\nmode=4\npanel=true\nlastLaunchLayer=true\n' > "$MYRDDIN_CONFIG_DIR/myrddin.conf"
window running 2500 --test-eval 'backend.setPanelKey("Alt+Home")' || bad "the window ran"
has "$rt/calls.log" "fg-panel key=Alt+Home" "a game running with the panel: the new key goes to it at once"
# what a key press is called: Qt hands over the key's own number (the input code plus 8) and what is held
fresh; : > "$rt/calls.log"
window idle 2500 --test-eval 'if (backend.keyName(16, Qt.ShiftModifier) === "Shift+7") backend.panel("tl"); if (backend.keyName(75, Qt.ControlModifier | Qt.AltModifier) === "Ctrl+Alt+F9") backend.panel("tr"); if (backend.keyName(16, Qt.MetaModifier) === "" && backend.keyName(36, 0) === "") backend.panel("bl")' || bad "the window ran"
has "$rt/calls.log" "fg-panel tl" "a key press is named by where the key is (Shift and the 7 of the number row: Shift+7)"
has "$rt/calls.log" "fg-panel tr" "with what is held (Ctrl+Alt+F9)"
has "$rt/calls.log" "fg-panel bl" "a Super key and Enter are not offered"
window idle 2500 --test-eval 'backend.runTool("Panel key", ["fg-panel-key", "install"])' || bad "the window ran"
hasnt "$rt/calls.log" 'fg-panel-key' "nothing is set up on the desktop any more"
fresh; mkdir -p "$MYRDDIN_CONFIG_DIR"; printf '[General]\nwelcomed=true\nmode=auto\npanel=false\nwatch=false\nlimit=3\n' > "$MYRDDIN_CONFIG_DIR/myrddin.conf"
window idle 2500 --test-eval 'backend.panel(backend.prefs.watch ? "tr" : "tl"); backend.panel(backend.prefs.panel ? "br" : "bl"); backend.panel(backend.prefs.limit === 3 ? "1" : "2")' || bad "the window ran"
has "$rt/calls.log" "fg-panel tl" "a switch that was left off is off after a restart (\"Run with logs\")"
has "$rt/calls.log" "fg-panel bl" "and so is the panel as the overlay"
has "$rt/calls.log" "fg-panel 1" "the highest multiplier comes back as a number"

echo "18. an RSI Launcher that was open before the window, under the name it gives itself"
# A stand-in for the launcher's main process: Wine's program (a copy of bash under a Wine name), the
# launcher's command line, a prefix in its environment, and the name Chromium gives its main thread.
# The window looks for processes for real here (no MYRDDIN_FAKE).
mkdir -p "$work/winebin"; cp "$(command -v bash)" "$work/winebin/wine64-preloader"
launcher_like() {   # PREFIX [PROGRAM]: one runs, its pid in $fl
    WINEPREFIX="$1" bash -c 'exec -a "$0" "$1" -c "echo CrBrowserMain > /proc/\$\$/comm; sleep 30; :"' \
        'C:\Program Files\Roberts Space Industries\RSI Launcher\RSI Launcher.exe' "${2:-$work/winebin/wine64-preloader}" > /dev/null 2>&1 &
    fl=$!
    for _ in $(seq 50); do [ "$(cat /proc/$fl/comm 2>/dev/null)" = CrBrowserMain ] && break; sleep 0.1; done
}
gone() { pkill -P "$fl" 2>/dev/null; kill "$fl" 2>/dev/null; wait "$fl" 2>/dev/null; }
fresh; : > "$rt/calls.log"
launcher_like "$work/t/prefix/"
window "" 2500 --page play --test-eval '@900 if (backend.state === "launcher") backend.panel("tl")' --test-eval '@1200 backend.stop()' || bad "the window ran"
gone
has "$rt/calls.log" "fg-panel tl" "the launcher of the game's prefix, renamed: the window sees it open"
has "$rt/calls.log" "stop" "and Stop reaches it (sc-turbo stop)"
fresh; : > "$rt/calls.log"
launcher_like "$work/other-prefix"
window "" 2500 --test-eval '@900 if (backend.state === "idle") backend.panel("bl")' || bad "the window ran"
gone
has "$rt/calls.log" "fg-panel bl" "one of another prefix is not this session (Stop would not reach it)"
fresh; : > "$rt/calls.log"
launcher_like "$work/t/prefix" "$(command -v bash)"
window "" 2500 --test-eval '@900 if (backend.state === "idle") backend.panel("br")' || bad "the window ran"
gone
has "$rt/calls.log" "fg-panel br" "a program that is not Wine's, with the launcher in its command line, is not the launcher"

echo "19. the advanced performance tweaks: switches that work whether the tweak is set up or not"
fresh; : > "$rt/calls.log"
TUNE_ALLOWED=0 window idle 2500 --test-eval 'backend.setOption("prefetchOff", true); backend.setOption("hugePages", true)' || bad "the window ran"
sleep 0.5
has "$rt/sc-turbo.conf" "SC_PREFETCH=off" "prefetchers off can be switched on where the root helper is not set up, and is kept in sc-turbo.conf"
has "$rt/sc-turbo.conf" "SC_THP=on" "huge pages too"
window idle 2500 --test-eval '@1500 if (backend.conf.prefetchOff && backend.conf.hugePages && backend.tuneAllowed) backend.panel("tl")' || bad "the window ran"
has "$rt/calls.log" "fg-panel tl" "both read back, and the helper is seen as set up"
TUNE_ALLOWED=0 window idle 2500 --test-eval '@1500 if (!backend.tuneAllowed && backend.checks.some((c) => c.key === "tune" && c.level === "warn")) backend.panel("bl")' || bad "the window ran"
has "$rt/calls.log" "fg-panel bl" "without the helper: a warning in the system check that says so"
window idle 2500 --test-eval 'backend.setOption("prefetchOff", false); backend.setOption("hugePages", false)' || bad "the window ran"
sleep 0.5
hasnt "$rt/sc-turbo.conf" 'SC_PREFETCH|SC_THP' "switched off: gone from sc-turbo.conf"
# no sched_ext scheduler installed (a window that finds no program at all): lavd and bpfland are offered all the same
mkdir -p "$work/nothing"; printf '#!/bin/sh\nPATH=%s exec "%s" "$@"\n' "$work/nothing" "$bin" > "$work/window-bare"; chmod +x "$work/window-bare"
bin="$work/window-bare" window idle 1800 --test-eval '@600 console.warn("offered: " + backend.schedulers.join(","))' --test-eval '@700 backend.conf.scheduler = "lavd"' || bad "the window ran"
grep -qE 'offered: lavd,bpfland$' "$work/last.err" && ok "no scheduler installed: scx_lavd and scx_bpfland are offered" || bad "no scheduler installed: scx_lavd and scx_bpfland are offered ($(grep -o 'offered: .*' "$work/last.err"))"
sleep 0.5
has "$rt/sc-turbo.conf" "SC_SCHED=lavd" "and one can be chosen, kept for the launch"
window idle 1800 --test-eval '@600 console.warn("offered: " + backend.schedulers.join(","))' || bad "the window ran"
grep -o 'offered: .*' "$work/last.err" | grep -qE '^offered: lavd,bpfland(,[a-z0-9]+)*$' && ! grep -o 'offered: .*' "$work/last.err" | tr ',' '\n' | sort | uniq -d | grep -q . \
    && ok "with schedulers installed: the two first, the others after them, none twice" || bad "with schedulers installed ($(grep -o 'offered: .*' "$work/last.err"))"
# the card's link: the page on GitHub, in the desktop's browser, and nothing but an https address
mkdir -p "$work/opener"; printf '#!/bin/sh\necho "xdg-open $*" >> "%s/calls.log"\n' "$rt" > "$work/opener/xdg-open"; chmod +x "$work/opener/xdg-open"
: > "$rt/calls.log"
PATH="$work/opener:$PATH" window idle 1800 --page options \
    --test-eval '@600 (function find(o) { if (!o) return null; if (o.text === "Advanced performance tweaking on GitHub" && typeof o.clicked === "function") return o; for (var i = 0; i < o.children.length; i++) { var r = find(o.children[i]); if (r) return r } return null })(window.contentItem).clicked()' \
    --test-eval '@700 backend.openLink("file:///etc/passwd"); backend.openLink("http://example.com/"); backend.openLink("javascript:alert(1)")' || bad "the window ran"
sleep 0.5
has "$rt/calls.log" "xdg-open https://github.com/Cadwaladr/Myrddin/blob/main/docs/advanced-performance-tweaking.md" "the card's link opens Advanced performance tweaking on GitHub"
hasnt "$rt/calls.log" 'xdg-open (file|http:|javascript)' "nothing but an https address is opened"

echo "20. the Console: the game's process and its logs"
fresh; mkdir -p "$rt/logs/fg"
printf '     1.0 fg: ready for 2560x1440\n    10.0 STAT x4 real=24.0/s generated=72.0/s shown=96.0/s untouched=0 | game frame ms mean=41.0 p50=40.0 | hook ms=1.0 hud=9\n' > "$rt/logs/fg/fg-123.log"
printf 'Frame generation x4; log: here\n' > "$rt/logs/launch.out"
gl="$work/t/prefix/drive_c/Program Files/Roberts Space Industries/StarCitizen/LIVE"; mkdir -p "$gl"
printf '<2026-11-20T10:33:00.000Z> [Notice] a line of the game log\n' > "$gl/Game.log"
: > "$rt/calls.log"
window running 2600 --page console \
    --test-eval '@700 if (gameConsole.active && gameConsole.info.up && gameConsole.info.threads === 191 && gameConsole.text.indexOf("STAT x4 real=24.0/s") >= 0 && gameConsole.text.indexOf("hook ms") < 0) backend.panel("tl")' \
    --test-eval '@900 gameConsole.setSource("game")' --test-eval '@1400 if (gameConsole.text.indexOf("a line of the game log") >= 0) backend.panel("tr")' \
    --test-eval '@1600 gameConsole.setSource("launch")' --test-eval '@2100 if (gameConsole.text.indexOf("Frame generation x4") >= 0) backend.panel("bl")' || bad "the window ran"
has "$rt/calls.log" "fg-panel tl" "the layer's log, its ten-second lines cut to what matters, and the game's process"
has "$rt/calls.log" "fg-panel tr" "the game's own Game.log"
has "$rt/calls.log" "fg-panel bl" "what the last launch said"
printf '    20.0 STAT x3 real=30.0/s generated=60.0/s shown=90.0/s untouched=0 | game frame ms mean=33.0\n' >> "$rt/logs/fg/fg-123.log"; : > "$rt/calls.log"
window running 2600 --page console --test-eval '@700 backend.panel(gameConsole.text.indexOf("x3 real=30.0") >= 0 ? "1" : "2")' || bad "the window ran"
has "$rt/calls.log" "fg-panel 1" "what the layer writes later is there too"
: > "$rt/calls.log"
window idle 2500 --page play --test-eval '@700 backend.panel(gameConsole.active ? "tr" : "br")' || bad "the window ran"
has "$rt/calls.log" "fg-panel br" "another page shown: the Console reads nothing"

echo "21. 5x to 8x, an experimental setting"
fresh; : > "$rt/calls.log"
window idle 2500 --test-eval 'backend.setLimit(6); backend.panel(backend.prefs.limit === 4 ? "tl" : "tr")' || bad "the window ran"
has "$rt/calls.log" "fg-panel tl" "without the experimental setting the highest multiplier stops at 4"
window idle 2500 --test-eval 'backend.setOption("experimental", true); backend.setLimit(6); backend.setMode("auto"); backend.launch()' || bad "the window ran"
sleep 0.5
has "$rt/sc-turbo.conf" "export SC_FG_EXPERIMENTAL=1" "the setting is in sc-turbo.conf, exported for the layer"
grep -q '^limit=6' "$MYRDDIN_CONFIG_DIR/myrddin.conf" && ok "with it the highest multiplier can be 6" || bad "with it the highest multiplier can be 6"
grep -qE '^fg 6( |$)' "$rt/calls.log" && ok "and Auto launches up to x6 (sc-turbo fg 6)" || bad "and Auto launches up to x6 (sc-turbo fg 6)"
window idle 2500 --test-eval 'backend.setOption("experimental", false)' || bad "the window ran"
sleep 0.5
hasnt "$rt/sc-turbo.conf" 'SC_FG_EXPERIMENTAL' "switched off: gone from sc-turbo.conf"
grep -q '^limit=4' "$MYRDDIN_CONFIG_DIR/myrddin.conf" && ok "and a highest multiplier above 4 comes back to 4" || bad "and a highest multiplier above 4 comes back to 4"
fresh; mkdir -p "$MYRDDIN_CONFIG_DIR"; printf '[General]\nwelcomed=true\nmode=auto\nlimit=6\n' > "$MYRDDIN_CONFIG_DIR/myrddin.conf"; : > "$rt/calls.log"
window idle 2500 --test-eval 'backend.panel(backend.prefs.limit === 4 ? "bl" : "br")' || bad "the window ran"
has "$rt/calls.log" "fg-panel bl" "a stored 6 without the setting is read as 4"
# the switch's page on GitHub, under its explanation
mkdir -p "$work/opener"; printf '#!/bin/sh\necho "xdg-open $*" >> "%s/calls.log"\n' "$rt" > "$work/opener/xdg-open"; chmod +x "$work/opener/xdg-open"
: > "$rt/calls.log"
PATH="$work/opener:$PATH" window idle 1800 --page options \
    --test-eval '@600 (function find(o) { if (!o) return null; if (o.text === "Experimental multipliers on GitHub" && typeof o.clicked === "function") return o; for (var i = 0; i < o.children.length; i++) { var r = find(o.children[i]); if (r) return r } return null })(window.contentItem).clicked()' || bad "the window ran"
sleep 0.5
has "$rt/calls.log" "xdg-open https://github.com/Cadwaladr/Myrddin/blob/main/docs/experimental-multipliers.md" "the switch's link opens Experimental multipliers on GitHub"

echo
[ $fails = 0 ] && echo "all checks passed" || echo "$fails check(s) FAILED"
exit $fails
