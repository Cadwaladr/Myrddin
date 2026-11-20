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
window idle 2500 --page play --test-eval 'backend.prefs.multiplier = 3; backend.prefs.panel = true; backend.conf.mangohud = true; backend.conf.scheduler = "lavd"; backend.launch()' || bad "the window ran"
sleep 0.5
has "$rt/calls.log" "fg 3 panel" "sc-turbo was called with the chosen words"
has "$rt/sc-turbo.conf" "SC_PREFIX='$work/t/prefix'" "the game folder is in sc-turbo.conf"
has "$rt/sc-turbo.conf" "SC_MANGOHUD=1" "MangoHud is in sc-turbo.conf"
has "$rt/sc-turbo.conf" "SC_SCHED=lavd" "the scheduler is in sc-turbo.conf"
bash -n "$rt/sc-turbo.conf" && ok "sc-turbo.conf is valid shell" || bad "sc-turbo.conf is valid shell"
hasnt "$rt/calls.env" '^(APPDIR|APPIMAGE|LD_LIBRARY_PATH|QT_|QML|MYRDDIN_)' "nothing of the window's environment reaches sc-turbo"
grep -q 'Frame generation x3' "$rt/logs/launch.out" 2>/dev/null && ok "sc-turbo's words are kept for the window" || bad "sc-turbo's words are kept for the window"
grep -q '^multiplier=3' "$MYRDDIN_CONFIG_DIR/myrddin.conf" && ok "the multiplier is remembered" || bad "the multiplier is remembered"

echo "2. off: the game as it is; \"Run with logs\""
fresh
window idle 2500 --test-eval 'backend.setMultiplier(1); backend.launch()' || bad "the window ran"
sleep 0.5
has "$rt/calls.log" "run" "\"run\" for Off"
fresh
window idle 2500 --test-eval 'backend.setMultiplier(1); backend.prefs.watch = true; backend.launch()' || bad "the window ran"
sleep 0.5
has "$rt/calls.log" "run watch" "Off with logs: \"run watch\""
fresh
window idle 2500 --test-eval 'backend.prefs.multiplier = 4; backend.prefs.watch = true; backend.conf.markers = false; backend.conf.fill = false; backend.launch()' || bad "the window ran"
sleep 0.5
has "$rt/calls.log" "fg 4 watch" "\"watch\" is passed"
has "$rt/sc-turbo.conf" "SC_FG_MARKERS=0" "markers off is in sc-turbo.conf"
has "$rt/sc-turbo.conf" "export SC_FG_FILL=0" "fill off is exported from sc-turbo.conf"
( . "$rt/sc-turbo.conf"; [ "$(sh -c 'echo $SC_FG_FILL')" = 0 ] ) && ok "a program started after it sees SC_FG_FILL=0" || bad "a program started after it sees SC_FG_FILL=0"
window idle 2500 --test-eval 'backend.conf.targetRate = "100"' || bad "the window ran"
[ "$(grep -c 'SC_FG_FILL' "$rt/sc-turbo.conf")" = 1 ] && ok "fill off is read back and written once" || bad "fill off is read back and written once"
window idle 2500 --test-eval 'backend.conf.fill = true' || bad "the window ran"
hasnt "$rt/sc-turbo.conf" 'SC_FG_FILL' "fill on again takes the line out"

echo "3. nothing is launched while something is in the way"
fresh; rm "$work/t/prefix/sc-launch.sh"
window idle 2500 --test-eval 'backend.launch()' || bad "the window ran"
hasnt "$rt/calls.log" '^(fg|run)' "no launch without a launch script"
fresh
window launcher 2500 --test-eval 'backend.launch()' || bad "the window ran"
hasnt "$rt/calls.log" '^(fg|run)' "no second launch while the launcher is open"

echo "4. in the running game"
fresh
window running 3000 --test-eval 'backend.setMultiplier(3); backend.sendParam("fill", "0"); backend.sendParam("bad;name", "1"); backend.sendParam("fill", "$(id)"); backend.liveSwitch("fg-cap", false); backend.liveSwitch("rm", true); backend.panel("tr"); backend.panel("--help")' || bad "the window ran"
has "$rt/calls.log" "fg-mult 3" "the multiplier is sent"
has "$rt/calls.log" "fg-params fill=0" "a parameter is sent"
has "$rt/calls.log" "fg-cap off" "a switch is sent"
has "$rt/calls.log" "fg-panel tr" "the panel's corner is sent"
hasnt "$rt/calls.log" 'bad|id\)|^rm|help' "words outside the lists are not sent"
hasnt "$rt/calls.log" '^(fg [0-9]|run)' "nothing is launched"
window running 2500 --test-eval 'backend.setMultiplier(1)' || bad "the window ran"
has "$rt/calls.log" "fg-off" "Off stops generation"
[ -e "$rt/logs/fg/OFF" ] && ok "the OFF file is there" || bad "the OFF file is there"

echo "5. a settings file written by hand survives"
fresh
cat > "$rt/sc-turbo.conf" <<CONF
# my own note
SC_PREFIX="\$HOME/somewhere else"
SC_LAUNCH="\$SC_PREFIX/start.sh"
SC_FG_BOOST=1
export WINE_SOMETHING="a b"
CONF
window idle 2500 --test-eval 'backend.conf.targetRate = "144"' || bad "the window ran"
has "$rt/sc-turbo.conf" "SC_PREFIX='$HOME/somewhere else'" "a \$HOME path is kept (written out)"
has "$rt/sc-turbo.conf" "SC_LAUNCH='$HOME/somewhere else/start.sh'" "a \$SC_PREFIX path is kept (written out)"
has "$rt/sc-turbo.conf" "SC_FG_TARGET=144" "the new value is written"
has "$rt/sc-turbo.conf" "SC_FG_BOOST=1" "an unknown setting is kept"
has "$rt/sc-turbo.conf" 'export WINE_SOMETHING="a b"' "an unknown export is kept"
has "$rt/sc-turbo.conf" "# my own note" "a comment is kept"
window idle 2500 --test-eval 'backend.conf.targetRate = ""' || bad "the window ran"
[ "$(grep -c 'my own note' "$rt/sc-turbo.conf")" = 1 ] && [ "$(grep -c 'lines the window does not know' "$rt/sc-turbo.conf")" = 1 ] && ok "nothing doubles on the second save" || bad "nothing doubles on the second save"
hasnt "$rt/sc-turbo.conf" 'SC_FG_TARGET' "an emptied value is taken out"

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
( unset MYRDDIN_RUNTIME; MYRDDIN_PAYLOAD="$pay" "$bin" displays > /dev/null 2>&1 )
[ "$(cat "$inst/VERSION")" = v3 ] && [ -e "$inst/tools/new-tool" ] && ok "a newer version replaces it" || bad "a newer version replaces it"
[ ! -e "$inst/tools/sc-sched" ] && ok "a file the new version has not is removed" || bad "a file the new version has not is removed"
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
python3 "$here/pixel.py" "$work/last.png" -40 -100 250 240 230 && ok "the window is drawn in them (its background is the scheme's)" || bad "the window is drawn in them (its background is the scheme's)"
window idle 2000 || bad "the window ran"
grep -q 'fill="#faf0e6"' "$icon" && bad "another scheme at the next start redraws the menu icon" || ok "another scheme at the next start redraws the menu icon"

echo "11. the first start finds the game"
fresh
window idle 2000 || bad "the window ran"
has "$rt/sc-turbo.conf" "SC_PREFIX='$work/t/prefix'" "found through the LUG Helper's note"

echo
[ $fails = 0 ] && echo "all checks passed" || echo "$fails check(s) FAILED"
exit $fails
