#!/usr/bin/env bash
# What a build of the frame-generation layer has to pass before it is installed. Everything runs
# headless (tools/test-chain.sh); results are appended to logs/test/_validate.txt and printed.
#
#   tools/fg-validate.sh [all | regression | hose | limiter [A|B|C|D] | multiplier | gpuwait | soak | panel | markers]
#
#   regression   the frame chain's cases: chain intact (unlinked=0, shift=0, off=0; "early": shift=1, off=1)
#                ("own": a swapchain of the game's own renderer, left as it is: presents = frames, nothing generated;
#                "gaps": most frames without a HUD capture, generation goes on with the HUD in the picture;
#                "gapfill": one frame in three without one, the HUD stays apart all the same; "gapcross": the HUD
#                pass in a command buffer of its own; tools/gap-check.py says whether the pictures are right)
#                "lost": frames copied out of the shared image only when the game's next frame was
#                finished, so that the copy took the wrong picture (the layer counts them, from the moment
#                generation is set up). Nothing but 0 will do, anywhere, except: in the runs of the
#                limiter as it was ("...old"); in "early" (the count takes the usual order for granted);
#                and one where the queue thread is made to stop ("stall": no present call comes for most
#                of a second, and the game is not held that long). "late": copied out after the next
#                frame was submitted, but in time; that happens (a GPU that is the slower one, the frame
#                after a long one) and is no fault.
#                Lines that say a wait was given up ("did not come", "no guard") belong in "early" and "e4"
#                (a game that hands over before it submits) and where the GPU is the slower one (C: the
#                present call of the frame before begins when the GPU is done with it, not when the timer
#                says; frames held to bring them back in step do not stay there, and it is tried less and
#                less often).
#   hose         a dump of the game replayed: the generated picture against the installed build's (the same
#                but for a few pixels when the vector check is intact), and with the check off (must differ)
#   limiter      A steady cases, B irregular frame times, C a GPU as slow as the rest, D the game's own size;
#                C and D run the limiter as it was in turn ("...old": SC_FG_GATE=0 SC_FG_GUARD=0 SC_FG_FIRST=0
#                SC_FG_GPUWAIT=0).
#                Frames in the same time must not be fewer than old's beyond what runs differ by (GPU-bound
#                cases: 2-4 %), and "off" (mismatches found) not more.
#   multiplier   a game whose main thread is the slower one, with slow stretches, a scene that changes:
#                one or two changes of the multiplier and then none (the build before this did 26 in 40 s);
#                generation off above 92 % of the display's rate and on again only below 80 %;
#                and a GPU that is the slower one ("gpm..."): no going back and forth there either
#   gpuwait      the wait for the GPU (SC_FG_GPUWAIT=1, off by default): cases where the GPU is the slower
#                one, or nearly, as they are and, in turn, with it ("...w"). With it: lost 0; the picture
#                newer ("age") where the GPU is the slower one; frames not fewer than without it by more
#                than was measured when it was made (steady cases 1-3 %, a hitch every second 5 %).
#   soak         three minutes of that at the game's own size
#   markers      HUD content that moves over the screen (SC_FG_MARKERS=1, which sc-turbo sets at launch; not part of "all"):
#                a test shape (an opaque marker that crosses HUD that stays) must be shown on its way, whole,
#                with what stays untouched (tools/marker-check.py; shapes with a known answer, filled and half
#                transparent ones among them, are in proto/hud-motion/shapes.py); captures of the game's HUD layer replayed
#                (HUD_CAPTURES=PREFIX, seven in a row from "sc-turbo fg-dump 7"; default logs/fg/dump-650085)
#                must come out as the reference says, pass by pass and in the picture
#                (tools/hud-motion-check.py), with nothing of the game's map moved, whether it turns in its
#                frame or sways with it (HUD_SWAY=PREFIX, three in a row; default logs/fg/dump-147674), and
#                nothing of a ship's HUD held back, slow or fast (HUD_SHIP=PREFIX, eight in a row; default
#                logs/fg/dump-1326375); and what the passes cost: a GPU-bound game with them once, eight
#                times over and not at all, in turn.
#   FG_TARGET=RATE: the display rate the limiter, multiplier, gpuwait and soak cases run at (120).
#   panel        the performance panel (off by default; all the above run without it): the chain's cases
#                with it on, intact as without; frames that pass through from the first one, with the
#                panel on them, then generation on (the panel must be seen in the dumps of both); and at
#                the game's own size with it and without, in turn, twice ("...p": with it): frames not
#                fewer beyond what runs differ by.
set -u
here="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
cd "$here" || exit 1
out="$here/logs/test/_validate.txt"; mkdir -p "$here/logs/test"
say() { echo "$@" | tee -a "$out"; }
replay="--replay logs/fg/dump-236448-000"   # a dump of the game (sc-turbo fg-dump): any will do
old="SC_FG_GATE=0 SC_FG_GUARD=0 SC_FG_FIRST=0 SC_FG_GPUWAIT=0"
run() { local n=$1 secs=$2 e=$3; shift 3; tools/test-chain.sh "$n" "$secs" "SC_FG_TARGET=${FG_TARGET:-120} $e" "$@" > "logs/test/_$n.out" 2>&1; }
late() { grep -ho 'late copies=[0-9]* lost=[0-9]*' logs/test/$1/fg-*.log | awk -F'[= ]' '{ l += $3; x += $5 } END { printf "%d/%d", x, l }'; } # lost/late
line() { local n=$1; printf "%-10s %s | %s off, %s lost/late | %s\n" "$n" "$(tail -1 logs/test/$n/out.txt)" "$(grep -c 'CHAIN OFF' logs/test/$n/fg-*.log)" "$(late "$n")" \
    "$(grep STAT logs/test/$n/fg-*.log | tail -1 | grep -o 'gate=.*first=[0-9]* ms=[0-9.]*' | sed 's/ own ms=[0-9.]*//')"; }
table() { python3 tools/fg-summ.py "$@" | tee -a "$out"; for n in "$@"; do line "$n" | tee -a "$out"; done; }

reg() { local n=$1 e=$2; shift 2; tools/test-chain.sh "$n" "${REG_SECS:-12}" "$e" "$@" > "logs/test/_$n.out" 2>&1
    local st; st=$(grep STAT logs/test/$n/fg-*.log | tail -1)
    say "$(printf "%-6s %s | %s | off=%s lost/late=%s | %s" "$n" "$(tail -1 logs/test/$n/out.txt)" \
        "$(echo "$st" | grep -o 'chain=[0-9]* dup=[0-9]* unlinked=[0-9]* nohud=[0-9]* multi=[0-9]* shift=[-0-9]*')" \
        "$(grep -c 'CHAIN OFF' logs/test/$n/fg-*.log)" "$(late "$n")" "$(echo "$st" | grep -o 'real=[0-9.]*/s generated=[0-9.]*/s')")"; }
regression() {
    say "## regression"
    reg plain ""; reg dup "" --dup 70; reg nohud "SC_FG_TEST_LAYER=1" --nohud 50; reg early "" --early
    reg stall "" --stall 150 900; reg wild "" --wild
    # A HUD pass left without its frame copy, its layer destroyed, and a copy without a pass in the same
    # command buffer later: the copy must not be captured as that old pass's frame (nothing but "0 taken").
    reg stale "" --stale-hud 7
    say "stale: $(grep -h 'stale-hud' logs/test/stale/out.txt); $(grep -c 'layer no longer exists' logs/test/stale/fg-*.log) taken for an old pass, $(grep -c 'never came back\|^ *[0-9.]* lock ' logs/test/stale/fg-*.log) lock lines"
    # Captures that come with gaps (every second frame without one, as in the game's menus): interpolated as
    # presented, so the generated rate stays near the real one's; one "come with gaps" line.
    reg gaps "SC_FG_TEST_LAYER=1" --nohud 2
    say "gaps: $(grep -c 'captures come with gaps' logs/test/gaps/fg-*.log) hold(s) begun, $(grep -c 'every frame has its HUD capture again' logs/test/gaps/fg-*.log) ended"
    # Frames with their HUD but without a capture (every third one), as the game has them now and then: the
    # scene given to the interpolation stays free of the HUD, every generated frame has it exact, none is lost.
    # And a HUD pass recorded into a command buffer of its own: a capture all the same ("cross", nohud=0).
    TEST_AT="5:DUMP=8" reg gapfill "SC_FG_MARKERS=1" --game-ms 30 --marker 0 0 --gap 3
    say "gapfill: $(python3 tools/gap-check.py logs/test/gapfill --expect-gaps | tail -1), $(grep -c 'HUD layer mode off' logs/test/gapfill/fg-*.log) times out of layer mode, $(grep STAT logs/test/gapfill/fg-*.log | tail -1 | grep -o 'hud gaps filled=[0-9]*')"
    TEST_AT="5:DUMP=6" reg gapcross "SC_FG_MARKERS=1" --game-ms 30 --marker 0 0 --split-hud 3
    say "gapcross: $(python3 tools/gap-check.py logs/test/gapcross | tail -1), $(grep STAT logs/test/gapcross/fg-*.log | tail -1 | grep -o 'hud gaps filled=[0-9]* cross=[0-9]*')"
    # The locks' own test: a holder that ended (taken over), one at work (waited for, not taken over),
    # a call that never came back (let go by its thread, with and without another thread waiting).
    reg locks "SC_FG_TEST_LOCKS=1"
    say "locks: $(grep -h 'locks test' logs/test/locks/fg-*.log | sed 's/^ *[0-9.]* locks test//' | tr '\n' ';')"
    # The game's own swapchain (its renderer's, "StarEngine": what it presents through where the runner
    # has no vkd3d-proton) is left as the game asked: as many presents as frames, and the one line saying so.
    reg own "" --engine StarEngine
    say "own: $(grep -c 'swapchain: .*(untouched)' logs/test/own/fg-*.log) swapchain left as asked, $(grep -c "this one is the game's own" logs/test/own/fg-*.log) line saying so"
    # Presents as vkd3d-proton makes them where the driver reports when frames reach the display: the layer
    # takes the reports in as they pass (reports about as many as frames, unmatched 0). The GPU's boost
    # switched on and off in the running program: two lines, result 0.
    TEST_AT="4:PARAMS=boost=1 8:PARAMS=boost=0" reg timing "" --timing
    say "timing: $(grep STAT logs/test/timing/fg-*.log | tail -1 | grep -o 'present to display ms.*unmatched=[0-9]*'), $(grep -c "the GPU's boost is o[nf]* *f* *(result 0)" logs/test/timing/fg-*.log) boost switches, $(grep -c 'the display reports when' logs/test/timing/fg-*.log) line about the reports"
    # Presents as the game makes them, without vsync: no id, no request for the time, the present mode given with
    # each. The layer's own requests (timing=1), switched on in the running program: reports for real and generated
    # frames, unmatched 0, and no present that failed.
    TEST_AT="3:PARAMS=timing=1" reg unlocked "" --unlocked
    say "unlocked: $(grep STAT logs/test/unlocked/fg-*.log | tail -1 | grep -o 'present to display ms.*unmatched=[0-9]*'); $(grep STAT logs/test/unlocked/fg-*.log | tail -1 | grep -o 'generated frames to display.*'); $(grep -c 'that asked for its time failed' logs/test/unlocked/fg-*.log) failed"
    # ... and with another thread taking some of the reports, as something in the game's process does: frames must
    # still be paired with their own reports (the delay stays what it was, it does not grow).
    TEST_AT="3:PARAMS=timing=1" reg stolen "" --unlocked --steal 700
    say "stolen: $(grep STAT logs/test/stolen/fg-*.log | tail -1 | grep -o 'present to display ms.*unmatched=[0-9]*'); $(grep STAT logs/test/stolen/fg-*.log | tail -1 | grep -o 'generated frames to display.*'); $(grep -c 'that asked for its time failed' logs/test/stolen/fg-*.log) failed"
}
panel() {
    say "## panel: the performance panel on (it is off in everything above)"
    TEST_PANEL=3 reg p_plain ""; TEST_PANEL="2 br" reg p_dup "" --dup 70
    TEST_PANEL="3 tr" reg p_nohud "SC_FG_TEST_LAYER=1" --nohud 50; TEST_PANEL="1 bl" reg p_stall "" --stall 150 900
    # Generation off from the first frame: they pass through with the panel on them (the layer sets
    # itself up for that alone). Then generation on, the panel moved, and off.
    REG_SECS=16 TEST_PANEL="2 tr" TEST_AT="pre:OFF 4:PANELDUMP 6:-OFF 9:PANELDUMP 11:PANEL=3,bl 12:PANELDUMP 13:PANEL=0" reg p_pass ""
    say "p_pass: $(grep -c 'panel dump' logs/test/p_pass/fg-*.log) panel dumps (3 expected), the panel in them:"
    python3 tools/panel-seen.py logs/test/p_pass/paneldump-*.ppm | sed "s|$here/||" | tee -a "$out"
    # The game's own size, with the panel's largest step and without, in turn.
    local n names=
    for n in rq3 rq3p rr3 rr3p; do
        # shellcheck disable=SC2086
        TEST_SIZE=2560x1440 TEST_PANEL=$( [ "${n%p}" != "$n" ] && echo 3 || echo 0 ) run "$n" 20 "SC_FG_MULT=3" $replay --game-ms 12 --jitter 6 --gpu-load 2 --unprotected
        names="$names $n"
    done
    # shellcheck disable=SC2086
    table $names
}
hose() {
    say "## hose replay"
    # The replay shows the dump's two pictures in turn, and a dump catches the one or the other on
    # top: a run is repeated until the real picture in its dump is the one in the reference's.
    hrun() { TEST_SIZE=2560x1440 TEST_DUMP=6 tools/test-chain.sh "$1" 12 "$2" $replay > "logs/test/_$1.out" 2>&1; }
    pic() { ls logs/test/$1/dump-*-$2.ppm 2>/dev/null | head -1; }
    [ -f build/fg/libsc_fg.so ] && TEST_LIB=build/fg/libsc_fg.so hrun hoseref ""
    local n e try same
    for n in hoseon hoseoff; do
        e=; [ "$n" = hoseoff ] && e="SC_FG_FIX_VECTORS=0"
        for try in 1 2 3 4 5 6; do
            hrun "$n" "$e"
            same=$( [ -n "$(pic "$n" real)" ] && [ -n "$(pic hoseref real)" ] && python3 tools/ppmdiff.py "$(pic "$n" real)" "$(pic hoseref real)" | awk '{ print ($2 + 0 < 0.05) ? "yes" : "no" }')
            [ "$same" = yes ] && break
        done
        say "$n against the installed build (same pair of pictures: ${same:-no dump}, run $try): $( [ -n "$(pic "$n" generated)" ] && [ -n "$(pic hoseref generated)" ] && python3 tools/ppmdiff.py "$(pic "$n" generated)" "$(pic hoseref generated)" || echo 'no dump')"
    done
}
limiter() {
    local X="SC_FG_MULT=4 SC_FG_TEST_WAIT_MS=7" U="--gpu-load 4 --unprotected" names=
    pairs() { # "name|environment|options" ...: each as it is now and, in turn, as the limiter was
        local c n e o; for c in "$@"; do IFS='|' read -r n e o <<< "$c"
            # shellcheck disable=SC2086
            { run "$n" 20 "$e" $o --unprotected; run "${n}old" 20 "$e $old" $o --unprotected; }
            names="$names $n ${n}old"; done; }
    case "$1" in
    A)  say "## limiter A: steady, at the hooks' limit or the timer's"
        run x4gate 30 "$X" --game-ms 5 $U; run x4lite 20 "SC_FG_MULT=4" --game-ms 5 $U
        run x3wait 20 "SC_FG_MULT=3 SC_FG_TEST_WAIT_MS=5" --game-ms 5 $U; run x2g 20 "SC_FG_MULT=2" --game-ms 5 $U
        run jb3 20 "SC_FG_MULT=3 SC_FG_TEST_WAIT_MS=5" --game-ms 22 $U; run jb4 20 "SC_FG_MULT=4" --game-ms 30 $U
        run gb4 20 "SC_FG_MULT=4" --game-ms 40 $U; run x4prot 20 "$X" --game-ms 5 --gpu-load 4
        run stall4 20 "$X" --game-ms 5 $U --stall 150 900
        run x4gateold 30 "$X $old" --game-ms 5 $U; run jb3old 20 "SC_FG_MULT=3 SC_FG_TEST_WAIT_MS=5 $old" --game-ms 22 $U
        run stall4old 20 "$X $old" --game-ms 5 $U --stall 150 900
        names="x4gate x4gateold x4lite x3wait x2g jb3 jb3old jb4 gb4 x4prot stall4 stall4old" ;;
    B)  say "## limiter B: irregular frame times"
        local P="--jitter 8 --gpu-load 4"
        pairs "j4|$X|--game-ms 12 $P" "j3|SC_FG_MULT=3|--game-ms 16 $P" "j2|SC_FG_MULT=2|--game-ms 12 --jitter 6 --gpu-load 4" \
              "sp4|$X|--game-ms 8 --spike 40 60 --gpu-load 4" "big3|SC_FG_MULT=3|--game-ms 12 --jitter 6 --gpu-load 2" \
              "e4|$X|--game-ms 5 --gpu-load 4 --early" ;; # (a game that hands over before it submits: the waits must give themselves up)
    C)  say "## limiter C: the GPU as slow as the rest, or slower"
        pairs "gp4|SC_FG_MULT=4|--game-ms 3 --gpu-load 14" "gp3|SC_FG_MULT=3|--game-ms 3 --gpu-load 12" "gp2|SC_FG_MULT=2|--game-ms 3 --gpu-load 8" \
              "gpj3|SC_FG_MULT=3|--game-ms 14 --jitter 8 --gpu-load 9" "gpj2|SC_FG_MULT=2|--game-ms 10 --jitter 6 --gpu-load 6" ;;
    D)  say "## limiter D: the game's own size (a dump replayed)"
        export TEST_SIZE=2560x1440
        pairs "rep4|SC_FG_MULT=4|$replay --game-ms 12 --jitter 6 --gpu-load 2" "rep3|SC_FG_MULT=3|$replay --game-ms 12 --jitter 6 --gpu-load 2" \
              "rep2|SC_FG_MULT=2|$replay --game-ms 10 --jitter 5 --gpu-load 2" "reph3|SC_FG_MULT=3|$replay --game-ms 8 --jitter 4 --gpu-load 6" \
              "repg4|SC_FG_MULT=4|$replay --game-ms 30 --jitter 8 --gpu-load 2" "repg2|SC_FG_MULT=2|$replay --game-ms 17 --jitter 5 --gpu-load 2"
        unset TEST_SIZE ;;
    *)  for b in A B C D; do limiter $b; done; return ;;
    esac
    # shellcheck disable=SC2086
    table $names
}
scenario() { # NAME SECONDS "ENV" options...: the multiplier's changes, and where it ended up
    local n=$1 secs=$2 e=$3; shift 3
    run "$n" "$secs" "SC_FG_MULT=4 SC_FG_MAX_BASE=62 $e" "$@" --unprotected
    say "== $n: $(grep -c 'multiplier x' logs/test/$n/fg-*.log) changes, $(tail -1 logs/test/$n/out.txt), off $(grep -c 'CHAIN OFF' logs/test/$n/fg-*.log), lost/late $(late "$n"), probes $(grep -ho 'probes=[0-9]*' logs/test/$n/fg-*.log | awk -F= '{ s += $2 } END { print s + 0 }')"
    grep 'multiplier x\|did not hold' logs/test/$n/fg-*.log | cut -c1-120 | head -30 | tee -a "$out"
    grep STAT logs/test/$n/fg-*.log | tail -1 | sed -E 's/^( *[0-9.]+ STAT x[0-9] real=[0-9.]+\/s generated=[0-9.]+\/s shown=[0-9.]+\/s).*(game frame ms mean=[0-9.]+ p50=[0-9.]+ p95=[0-9.]+ p99=[0-9.]+ max=[0-9.]+).*(limit [0-9]+ of [0-9]+ sleep ms=[0-9.]+).*(free=[^|]*)\|.*/\1 | \2 | \3 | \4/' | tee -a "$out"
}
multiplier() {
    say "## multiplier (a main thread of 21 ms is a game that could do 48 a second)"
    local G="--game-ms 7 --jitter 1 --gpu-load 3"
    scenario m_dips 40 "" --sim-ms 21 --sim-jitter 3 --sim-dip 3 400 12 $G            # x3 and stays (fill)
    scenario m_dips0 40 "SC_FG_FILL=0" --sim-ms 21 --sim-jitter 3 --sim-dip 3 400 12 $G  # x2 and stays
    scenario m_faster 50 "" --sim-ms 27 --sim-jitter 3 --sim-change 20 12 $G           # x4 or x3 (37 a second is at the edge), x2 after 20 s
    scenario m_slower 50 "" --sim-ms 12 --sim-jitter 2 --sim-change 20 27 $G           # x2, then x3 after 20 s
    scenario m_fast 35 "" --sim-ms 6 --sim-jitter 1 --game-ms 5 --jitter 1 --gpu-load 1   # x1 and stays
    scenario m_near 60 "" --sim-ms 17.5 --sim-jitter 3 $G                              # x3 or x2, no going back and forth
    # A light game (3 ms on its own thread): the frame after a probe would be drawn over the probe's
    # before that is copied out, were it not kept back. By the wait for the generated frames: lost must
    # be 0. Or, that switched off (m_lightg), by the guard: then frames are lost as they were before
    # there was that wait, where they queue behind the presents (the first second, a hitch), but in the
    # seconds without that none, probes or no probes.
    local L="--game-ms 3 --jitter 1 --gpu-load 1"
    scenario m_light 40 "" --sim-ms 14 --sim-jitter 2 $L
    scenario m_lightg 40 "SC_FG_FIRST=0" --sim-ms 14 --sim-jitter 2 $L
    # Generation off (x1) for a game near the display's rate; it stays off at 100 a second (above 80 %
    # of 120), and comes back on at 80.
    scenario m_x1keep 45 "" --sim-ms 6 --sim-jitter 1 --sim-change 20 10 --game-ms 5 --jitter 1 --gpu-load 1
    scenario m_x1back 45 "" --sim-ms 6 --sim-jitter 1 --sim-change 20 12.5 --game-ms 5 --jitter 1 --gpu-load 1
    # A GPU that is the slower one (30 ms for a frame: x4 and stays), and one that is nearly (x2 or x3
    # at the display's rate, or just under it).
    scenario gpm14 40 "" --sim-ms 12 --sim-jitter 2 --game-ms 3 --jitter 1 --gpu-load 14
    scenario gpm5 40 "" --sim-ms 12 --sim-jitter 2 --game-ms 3 --jitter 1 --gpu-load 5
}
gpuwait() {
    say "## gpuwait: the wait for the GPU, off by default (\"...w\": with SC_FG_GPUWAIT=1)"
    local names= W="SC_FG_GPUWAIT=1"
    both() { # NAME SECONDS "ENV" options...: as it is and, in turn, with the wait
        local n=$1 secs=$2 e=$3; shift 3
        run "$n" "$secs" "$e" "$@" --unprotected; run "${n}w" "$secs" "$e $W" "$@" --unprotected
        names="$names $n ${n}w"; }
    both gpx4 20 "SC_FG_MULT=4" --game-ms 3 --gpu-load 14
    both gpx2 20 "SC_FG_MULT=2" --game-ms 3 --gpu-load 8
    both gpy2 20 "SC_FG_MULT=2" --game-ms 10 --jitter 6 --gpu-load 6
    both gps4 30 "SC_FG_MULT=4" --game-ms 10 --jitter 6 --gpu-load 14 --spike 25 80   # a hitch every second
    # shellcheck disable=SC2086
    TEST_SIZE=2560x1440 both gpr2 20 "SC_FG_MULT=2" $replay --game-ms 10 --jitter 5 --gpu-load 2   # the game's size, near the display's rate
    # shellcheck disable=SC2086
    table $names
    local n; for n in $names; do say "$(printf "%-8s %s | %s" "$n" "$(grep STAT logs/test/$n/fg-*.log | tail -1 | grep -o 'age ms p50=[0-9.]* p99=[0-9.]*')" "$(grep STAT logs/test/$n/fg-*.log | tail -1 | grep -o 'kept back for the GPU.*')")"; done
    scenario gpm14w 40 "$W" --sim-ms 12 --sim-jitter 2 --game-ms 3 --jitter 1 --gpu-load 14   # (against gpm14 of "multiplier")
}
soak() {
    say "## soak (the game's own size)"
    TEST_SIZE=2560x1440 scenario r_soak 180 "" $replay --sim-ms 22 --sim-jitter 4 --sim-dip 7 500 15 --sim-change 90 14 --game-ms 8 --jitter 2 --gpu-load 4 --spike 400 120
    grep -h 'did not come\|no guard' logs/test/r_soak/fg-*.log | head -5 | tee -a "$out"
}
given_up() { # waits that gave themselves up, anywhere but in the runs they belong in (this run's logs only)
    say "## waits given up (none expected; those of early, e4 and where the GPU is the slower one, \"gp...\", are not listed)"
    find logs/test -mindepth 2 -maxdepth 2 -name 'fg-*.log' -newer "$stamp" -exec grep -l 'did not come\|no guard' {} + 2>/dev/null | sort | grep -v '/early/\|/e4/\|old/\|/gp' | while read -r f; do
        say "$(basename "$(dirname "$f")"): $(grep -h 'did not come\|no guard' "$f" | head -2 | cut -c1-150)"; done
}
stamp="$here/logs/test/_validate.start"; touch "$stamp"
say "# $(date '+%F %H:%M') build $(sha256sum "${TEST_LIB:-$( [ -f build/fg/libsc_fg.new.so ] && echo build/fg/libsc_fg.new.so || echo build/fg/libsc_fg.so)}" | cut -c1-16)"
markers() {
    say "## markers: HUD content that moves over the screen (SC_FG_MARKERS=1)"
    local c n v captures=${HUD_CAPTURES:-logs/fg/dump-650085} sway=${HUD_SWAY:-logs/fg/dump-147674} ship=${HUD_SHIP:-logs/fg/dump-1326375}
    for c in "mk_across|14 0" "mk_down|0 9" "mk_both|11 7" "mk_fast|90 40"; do
        IFS='|' read -r n v <<< "$c"
        # shellcheck disable=SC2086
        TEST_AT="5:DUMP=3" tools/test-chain.sh "$n" 10 "SC_FG_MARKERS=1" --game-ms 30 --marker $v > "logs/test/_$n.out" 2>&1
        python3 tools/marker-check.py "logs/test/$n/dump-" 2 | sed "s|$here/||" | tee -a "$out"
    done
    if ls "$captures"-000-hud-*.bin > /dev/null 2>&1; then
        TEST_SIZE=2560x1440 TEST_AT="5:DUMP=8" tools/test-chain.sh mk_game 12 "SC_FG_MARKERS=1 SC_FG_MARKER_CAP=100000" \
            --game-ms 30 --hud-files "$captures" 0 7 > logs/test/_mk_game.out 2>&1
        HUD_STILL="0 1 7 7" python3 tools/hud-motion-check.py logs/test/mk_game "$captures" 0 7 2 | sed "s|$here/||" | tee -a "$out"
        if ls "$sway"-000-hud-*.bin > /dev/null 2>&1; then
            TEST_SIZE=2560x1440 TEST_AT="5:DUMP=4" tools/test-chain.sh mk_sway 12 "SC_FG_MARKERS=1 SC_FG_MARKER_CAP=100000" \
                --game-ms 30 --hud-files "$sway" 0 3 > logs/test/_mk_sway.out 2>&1
            HUD_STILL="0 1 7 7" python3 tools/hud-motion-check.py logs/test/mk_sway "$sway" 0 3 2 | sed "s|$here/||" | tee -a "$out"
        fi
        if ls "$ship"-000-hud-*.bin > /dev/null 2>&1; then
            TEST_SIZE=2560x1440 TEST_AT="5:DUMP=8" tools/test-chain.sh mk_ship 12 "SC_FG_MARKERS=1 SC_FG_MARKER_CAP=100000" \
                --game-ms 30 --hud-files "$ship" 0 8 > logs/test/_mk_ship.out 2>&1
            HUD_PICTURES=0 python3 tools/hud-motion-check.py logs/test/mk_ship "$ship" 0 8 2 | sed "s|$here/||" | tee -a "$out"
        fi
        # what the passes cost: the game's own size, the GPU the slower one; in turn, twice
        local names=
        for n in mc0 mc1 mc8 mc0b mc1b mc8b; do
            v=0; case "$n" in mc1*) v=1 ;; mc8*) v=8 ;; esac
            TEST_SIZE=2560x1440 run "$n" 16 "SC_FG_MULT=3 SC_FG_MARKER_CAP=100000 SC_FG_MARKERS=$(( v > 0 )) SC_FG_TEST_MARKERS=$(( v > 0 ? v : 1 ))" \
                --game-ms 3 --gpu-load 10 --unprotected --hud-files "$captures" 0 7
            names="$names $n"
        done
        # shellcheck disable=SC2086
        table $names
        say "moving hud: $(grep -ho 'moving hud frames=[0-9]* tiles=[0-9]* found=[0-9]* pieces=[0-9]* over=[0-9]*' logs/test/mc1/fg-*.log | tail -1)"
    else
        say "no captures of the game's HUD layer ($captures-000-hud-*.bin): only the test shape was checked"
    fi
}
case "${1:-all}" in
all) regression; hose; limiter all; multiplier; gpuwait; soak; panel; given_up ;;
regression|hose|multiplier|gpuwait|soak|panel|markers) "$1" ;;
limiter) limiter "${2:-all}" ;;
*) awk 'NR > 1 { if (/^#/) print; else exit }' "$0"; exit 1 ;;
esac
left="$(pgrep -a weston)"; say "${left:-no weston running}"
