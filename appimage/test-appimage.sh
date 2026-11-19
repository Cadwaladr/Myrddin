#!/usr/bin/env bash
# Tests appimage/out/Myrddin-x86_64.AppImage. No window opens anywhere: every run is offscreen
# (QT_QPA_PLATFORM=offscreen, no DISPLAY/WAYLAND_DISPLAY) or inside a container, and every run gets a
# home of its own under appimage/out/test/ (the app copies its payload into the home and writes its
# settings there: nothing of the user's own is read or written).
#
#   appimage/test-appimage.sh
#
#   licences       usr/share/licenses/myrddin: the texts named in THIRD-PARTY-APPIMAGE.txt are there, every
#                  carried non-Qt library has a line, the payload's licences are copied, no machine paths
#   host-fuse      the AppImage as a user starts it (FUSE mount), a screenshot
#   host-extract   the same with --appimage-extract-and-run (no FUSE)
#   env-*          brief point 2: /proc/PID/environ of the running app compared with the environment it
#                  was started with (FUSE and extract-and-run; once with nothing set, once with
#                  LD_LIBRARY_PATH, QT_PLUGIN_PATH, QML_IMPORT_PATH, QML2_IMPORT_PATH,
#                  QT_QPA_PLATFORM_PLUGIN_PATH, XDG_DATA_DIRS set to test values that must come through
#                  unchanged); and the environment of the programs the app itself starts (a stand-in
#                  runtime records it)
#   probe-DISTRO   bare image: which libraries are missing (must all be ones the excludelist leaves to
#                  the host)
#   DISTRO         image with only the packages listed below (the requirements): extract-and-run, a
#                  screenshot, the payload installed into the home
#   nofuse-DISTRO  (INFO) what the AppImage says without FUSE and without the flag
#   x11-DISTRO     the xcb platform in Xvfb (Mesa llvmpipe, Qt Quick on OpenGL through GLX)
#   wayland-DISTRO the Wayland platform in a headless weston (Qt Quick software, shared memory)
#   wayland-egl-DISTRO  Wayland with EGL (weston's GL renderer; SKIP where weston has none: 9.0)
#   apptests-DISTRO app/tests/run-tests.sh with the AppImage as the binary and with its usr/bin/myrddin
#   shots-DISTRO   app/tests/shots.sh with the AppImage: every picture a good PNG
#                  (these two need python3 and desktop-file-utils in the image: test tools, not requirements)
# A screenshot is good when it is a PNG of the window's size with more than one colour in it. The
# test images are made once (podman build) and kept: localhost/myrddin-apptest-*.
# Prints a table, writes it to out/test/results.txt, exits 1 when anything failed.
#
# Environment: APPIMAGE_FILE (default out/Myrddin-x86_64.AppImage), DISTROS (default
# "ubuntu22 debian12 fedora arch"; empty: no containers), PLATFORM_DISTROS (default "ubuntu22 arch",
# a subset of DISTROS), APP_TESTS_DISTROS (default: all four; empty: none), HOST=0 (no host runs), EXPECT_SIZE=WxH (default: the root width/height in
# app/qml/Main.qml; "any" for no size check), SETTLE (ms before the screenshot, default 1500),
# PODMAN (default podman).
set -uo pipefail

here="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"   # appimage/
project="$(dirname "$here")"
img="$(readlink -f "${APPIMAGE_FILE:-$here/out/Myrddin-x86_64.AppImage}")"
tdir="$here/out/test"
podman="${PODMAN:-podman}"
distros="${DISTROS-ubuntu22 debian12 fedora arch}"
settle="${SETTLE:-1500}"
excludelist="$here/work/app/dl/excludelist/excludelist"
mirror=mirrors.edge.kernel.org/ubuntu/
forbidden="LD_LIBRARY_PATH QT_PLUGIN_PATH QML_IMPORT_PATH QML2_IMPORT_PATH QT_QPA_PLATFORM_PLUGIN_PATH XDG_DATA_DIRS"
from_runtime="APPDIR APPIMAGE ARGV0 OWD"   # set by the AppImage runtime; MYRDDIN_PAYLOAD by AppRun
from_shell="PWD SHLVL"                     # what any /bin/sh adds (AppRun is a shell script)

[ $# -eq 0 ] || { sed -n '2,/^set -u/{/^set -u/d;s/^# \{0,1\}//;p}' "$0"; exit 1; }
[ -x "$img" ] || { echo "no AppImage at $img (build it with appimage/build-appimage.sh)"; exit 1; }
mkdir -p "$tdir"
results=()
record() { results+=("$1|$2|$3"); printf '  %-22s %-4s %s\n' "$1" "$2" "$3"; }

expect="${EXPECT_SIZE:-}"
if [ -z "$expect" ]; then
    w=$(grep -m1 -E '^    width: *[0-9]+ *$' "$project/app/qml/Main.qml" 2>/dev/null | grep -oE '[0-9]+')
    h=$(grep -m1 -E '^    height: *[0-9]+ *$' "$project/app/qml/Main.qml" 2>/dev/null | grep -oE '[0-9]+')
    expect=$([ -n "$w" ] && [ -n "$h" ] && echo "${w}x$h" || echo any)
fi
shot_ok() { python3 -I "$here/packaging/pngcheck.py" "$1" "$expect"; }

# The payload's VERSION as the AppImage carries it.
x="$tdir/.extract"; rm -rf "$x"; mkdir -p "$x"
(cd "$x" && "$img" --appimage-extract > /dev/null 2>&1)
payload_version="$(head -1 "$x/squashfs-root/usr/share/myrddin/payload/VERSION" 2>/dev/null)"
cp -f "$x/squashfs-root/usr/share/myrddin/payload/VERSION" "$tdir/payload-VERSION" 2>/dev/null || : > "$tdir/payload-VERSION"
echo "AppImage: $img ($(du -h "$img" | cut -f1)), payload ${payload_version:-?}, expected window $expect"

# --- the licence texts (usr/share/licenses/myrddin, made by packaging/licenses.py) ---
lic_verdict="$(python3 -I - "$x/squashfs-root/usr" <<'PY'
import os, re, sys
usr = sys.argv[1]
d = os.path.join(usr, "share", "licenses", "myrddin")
bad, n = [], 0
for f in ("LICENSE", "Qt/LGPL-3.0-only.txt", "Qt/GPL-3.0-only.txt", "Qt/GPL-2.0-only.txt", "Inter-LICENSE.txt",
          "THIRD-PARTY-APPIMAGE.txt", "appimage-runtime/type2-runtime-LICENSE"):
    if not os.path.isfile(os.path.join(d, f)):
        bad.append("missing " + f)
pl = os.path.join(usr, "share", "myrddin", "payload", "licenses")
if os.path.isdir(pl):
    for f in os.listdir(pl):
        if not os.path.isfile(os.path.join(d, "payload", f)):
            bad.append("missing payload/" + f)
tp = os.path.join(d, "THIRD-PARTY-APPIMAGE.txt")
text = open(tp).read() if os.path.isfile(tp) else ""
for line in text.splitlines():
    parts = [p.strip() for p in line.split("|")]
    if len(parts) == 5 and not line.startswith("Fields:"):
        n += 1
        if not os.path.exists(os.path.join(d, parts[4])):
            bad.append("no text " + parts[4])
# every carried library that is not Qt's must have a line
names = " ".join(l for l in text.splitlines() if "Ubuntu 22.04 archive" in l)
for so in os.listdir(os.path.join(usr, "lib")):
    if not so.startswith(("libQt6", "libicu")) and so not in names:
        bad.append("no line for " + so)
user = os.environ.get("USER") or os.environ.get("LOGNAME") or ""
if re.search(r"/(mnt|home|work|src|tmp)/", text) or (user and re.search(r"\b" + re.escape(user) + r"\b", text)):
    bad.append("a machine path or user name in THIRD-PARTY-APPIMAGE.txt")
print(("FAIL|" + "; ".join(bad[:8]) + "; " if bad else "PASS|") + f"{n} components listed, "
      f"{len(os.listdir(os.path.join(d, 'ubuntu'))) if os.path.isdir(os.path.join(d, 'ubuntu')) else 0} Ubuntu copyright files, "
      f"{len(os.listdir(os.path.join(d, 'Qt'))) if os.path.isdir(os.path.join(d, 'Qt')) else 0} Qt licence texts")
PY
)"
rm -rf "$x"
record licences "${lic_verdict%%|*}" "${lic_verdict#*|}"

# A clean environment for host runs: nothing of the desktop's, a home under DIR.
run_env() {   # DIR -> NAME=VALUE words
    local d=$1
    mkdir -p "$d/home" "$d/tmp" "$d/run"; chmod 700 "$d/run"
    printf '%s\n' PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 HOME="$d/home" \
        XDG_CONFIG_HOME="$d/home/.config" XDG_DATA_HOME="$d/home/.local/share" XDG_CACHE_HOME="$d/home/.cache" \
        XDG_STATE_HOME="$d/home/.local/state" XDG_RUNTIME_DIR="$d/run" TMPDIR="$d/tmp" QT_QPA_PLATFORM=offscreen
}

# --- (a) host: FUSE and extract-and-run, a screenshot each ---
host_shot() {   # NAME [runtime flag]
    local name=$1 d="$tdir/$1" code detail
    rm -rf "$d"; mkdir -p "$d"
    mapfile -t e < <(run_env "$d")
    timeout 90 env -i "${e[@]}" "$img" ${2:+"$2"} --screenshot "$tdir/$name.png" --settle "$settle" > "$d/log.txt" 2>&1
    code=$?
    detail="$(shot_ok "$tdir/$name.png")"; local good=$?
    local installed; installed="$(head -1 "$d/home/.local/share/myrddin/runtime/VERSION" 2>/dev/null)"
    detail="exit $code, $detail, payload installed: ${installed:-no}"
    if grep -q "$d/tmp" /proc/mounts; then detail="$detail, MOUNT LEFT BEHIND"; good=1; fi
    # The type-2 runtime never removes its extraction (it calls nftw() with nopenfd 0, which musl, the
    # runtime's libc, answers with "done" at once): noted, not a failure; removed here.
    if compgen -G "$d/tmp/appimage_extracted_*" > /dev/null; then
        detail="$detail, extraction left in TMPDIR ($(du -sh "$d"/tmp/appimage_extracted_* | cut -f1 | head -1), runtime bug)"
        rm -rf "$d"/tmp/appimage_extracted_*
    fi
    [ "$(grep -cvE '^QProcess: Destroyed|^$' "$d/log.txt")" -gt 0 ] && detail="$detail, $(grep -cvE '^QProcess: Destroyed|^$' "$d/log.txt") log lines ($name/log.txt)"
    [ $code -eq 0 ] && [ $good -eq 0 ] && [ "$installed" = "$payload_version" ] && record "$name" PASS "$detail" || record "$name" FAIL "$detail"
}

# --- (c) the environment rule ---
find_app() {   # MARK -> pid of the running usr/bin/myrddin started with that mark
    local p exe
    for p in /proc/[0-9]*; do
        exe=$(readlink "$p/exe" 2>/dev/null) || continue
        case "$exe" in */usr/bin/myrddin) ;; *) continue ;; esac
        tr '\0' '\n' < "$p/environ" 2>/dev/null | grep -qxF "MYRDDIN_TEST_MARK=$1" && { echo "${p#/proc/}"; return 0; }
    done
    return 1
}
env_check() {   # NAME FLAG(or "") SENTINELS(0/1)
    local name=$1 flag=$2 sentinel=$3 d="$tdir/$1" mark pid="" i
    rm -rf "$d"; mkdir -p "$d"
    mark="$name-$$-$RANDOM"
    # a stand-in runtime with the payload's VERSION, so the app keeps it and runs its sc-turbo and
    # sc-sched, which write down the environment they get
    local rt="$d/runtime"; mkdir -p "$rt/tools"
    cp "$tdir/payload-VERSION" "$rt/VERSION"
    printf '#!/bin/sh\nenv > "$(dirname "$0")/child-env-sc-turbo-${1:-none}.txt"\n[ "$1" = displays ] && echo "TEST-1 1920x1080 at 60.000 Hz, at 0,0" && echo "choice: TEST-1"\nexit 0\n' > "$rt/sc-turbo"
    printf '#!/bin/sh\nenv > "$(dirname "$0")/../child-env-sc-sched-${1:-none}.txt"\nexit 1\n' > "$rt/tools/sc-sched"
    chmod 755 "$rt/sc-turbo" "$rt/tools/sc-sched"
    mapfile -t e < <(run_env "$d"; echo "MYRDDIN_TEST_MARK=$mark"; echo "MYRDDIN_RUNTIME=$rt"
                     if [ "$sentinel" = 1 ]; then
                         for v in $forbidden; do echo "$v=/test-$v-must-stay"; done
                     fi)
    printf '%s\n' "${e[@]}" | sort > "$d/started-with.txt"
    timeout 90 env -i "${e[@]}" "$img" ${flag:+"$flag"} --screenshot "$d/shot.png" --settle 8000 > "$d/log.txt" 2>&1 &
    local job=$!
    for i in $(seq 1 60); do pid=$(find_app "$mark") && break; sleep 0.25; done
    if [ -z "$pid" ]; then wait $job; record "$name" FAIL "the running app was not found (log: $name/log.txt)"; return; fi
    tr '\0' '\n' < "/proc/$pid/environ" | sort > "$d/app-environ.txt"
    wait $job
    local verdict
    rm -rf "$d"/tmp/appimage_extracted_*
    verdict="$(python3 -I - "$d/started-with.txt" "$d/app-environ.txt" "$forbidden" "$from_runtime" "$rt" "$sentinel" "$from_shell" <<'PY'
import glob, os, sys
def load(p):
    out = {}
    for line in open(p, errors="replace"):
        line = line.rstrip("\n")
        if "=" in line:
            k, v = line.split("=", 1)
            out[k] = v
    return out
start, app = load(sys.argv[1]), load(sys.argv[2])
forbidden, runtime_set, rt, sentinel = sys.argv[3].split(), sys.argv[4].split(), sys.argv[5], sys.argv[6] == "1"
shell_set = sys.argv[7].split()
bad, notes, shell = [], [], []
for k in sorted(set(start) | set(app)):
    if start.get(k) == app.get(k):
        continue
    what = f"{k} {'added' if k not in start else 'removed' if k not in app else 'changed'}"
    if k in forbidden or k not in runtime_set + shell_set + ["MYRDDIN_PAYLOAD"]:
        bad.append(what)
    elif k in shell_set:
        shell.append(k)
    else:
        notes.append(k)
kids = sorted(glob.glob(os.path.join(rt, "child-env-*.txt")))
child = []
for f in kids:
    env = load(f)
    for k in forbidden:
        if k in env and env.get(k) != start.get(k):
            bad.append(f"{os.path.basename(f)[10:-4]} got {k}")
    child += [k for k in runtime_set + ["MYRDDIN_PAYLOAD"] if k in env]
line = "app: only " + (" ".join(notes) or "nothing") + " added" + (f" (and {' '.join(shell)} by AppRun's shell)" if shell else "")
line += f"; {len(kids)} child programs: " + ("clean" if not child else "have " + " ".join(sorted(set(child)))) if kids else "; no child program seen"
if sentinel:
    line += "; test values of the 6 kept"
print(("FAIL " + "; ".join(bad) + " | " if bad else "PASS ") + line)
PY
)"
    record "$name" "${verdict%% *}" "pid $pid: ${verdict#* }"
}

if [ "${HOST:-1}" = 1 ]; then
    echo "host runs (offscreen, own home under $tdir):"
    if [ -e /dev/fuse ] && { command -v fusermount3 || command -v fusermount; } > /dev/null; then
        host_shot host-fuse
        env_check env-fuse "" 0
        env_check env-fuse-set "" 1
    else
        record host-fuse SKIP "no /dev/fuse or fusermount on this host"
    fi
    host_shot host-extract --appimage-extract-and-run
    env_check env-extract --appimage-extract-and-run 0
fi

# --- (b) containers ---
distro_base() {
    case $1 in
    ubuntu22) echo docker.io/library/ubuntu:22.04 ;;
    debian12) echo docker.io/library/debian:12 ;;
    fedora) echo registry.fedoraproject.org/fedora:latest ;;
    arch) echo docker.io/library/archlinux:latest ;;
    *) return 1 ;;
    esac
}
# What had to be added to the bare images: the libraries the excludelist leaves to the host (GL/EGL
# through libglvnd, X11/xcb, Wayland's client library, fontconfig and FreeType). No font: the app carries Inter
# (without any system font only its monospace line falls back to Inter).
distro_pkgs() {
    case $1 in
    ubuntu22|debian12) echo libgl1 libegl1 libopengl0 libx11-6 libx11-xcb1 libwayland-client0 libfontconfig1 libfreetype6 ;;
    fedora) echo libglvnd-glx libglvnd-egl libglvnd-opengl libX11 libX11-xcb libwayland-client fontconfig freetype ;;
    arch) echo libglvnd libx11 wayland fontconfig freetype2 ;;
    esac
}
distro_install() {   # the shell line that installs PKGS in that image
    case $1 in
    ubuntu22) echo "sed -i -E 's#https?://(archive|security)\.ubuntu\.com/ubuntu/?#http://$mirror#' /etc/apt/sources.list && apt-get -qq update && DEBIAN_FRONTEND=noninteractive apt-get -qq install -y --no-install-recommends $2 >/dev/null && rm -rf /var/lib/apt/lists/*" ;;
    debian12) echo "apt-get -qq update && DEBIAN_FRONTEND=noninteractive apt-get -qq install -y --no-install-recommends $2 >/dev/null && rm -rf /var/lib/apt/lists/*" ;;
    fedora) echo "dnf -q -y install --setopt=install_weak_deps=False $2 && dnf clean all" ;;
    arch) echo "pacman -Syu --noconfirm --needed $2 >/dev/null && pacman -Scc --noconfirm >/dev/null" ;;
    esac
}
inside='export HOME=/t/home XDG_CONFIG_HOME=/t/home/.config XDG_DATA_HOME=/t/home/.local/share XDG_CACHE_HOME=/t/home/.cache XDG_RUNTIME_DIR=/t/run QT_QPA_PLATFORM=offscreen LANG=C.UTF-8
mkdir -p "$HOME" "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"
/a/Myrddin-x86_64.AppImage --appimage-extract-and-run --screenshot /t/shot.png --settle "$SETTLE" > /t/log.txt 2>&1
echo $? > /t/exit
head -1 "$XDG_DATA_HOME/myrddin/runtime/VERSION" > /t/runtime-version 2>/dev/null
/a/Myrddin-x86_64.AppImage --version > /t/nofuse.txt 2>&1; true'
probe='cd /tmp && /a/Myrddin-x86_64.AppImage --appimage-extract > /dev/null 2>&1 || { echo EXTRACT-FAILED; exit 0; }
for f in squashfs-root/usr/bin/myrddin squashfs-root/usr/lib/*.so* squashfs-root/usr/plugins/*/*.so squashfs-root/usr/qml/*/*.so squashfs-root/usr/qml/*/*/*.so squashfs-root/usr/qml/*/*/*/*.so squashfs-root/usr/qml/*/*/*/*/*.so; do
  [ -f "$f" ] && ldd "$f" 2>/dev/null | grep "not found"; done | awk "{print \$1}" | sort -u'

for distro in $distros; do
    base="$(distro_base "$distro")" || { record "$distro" FAIL "unknown distro name"; continue; }
    d="$tdir/$distro"; rm -rf "$d"; mkdir -p "$d"
    # bare image: what is missing must all be on the excludelist (left to the host on purpose)
    "$podman" image exists "$base" || nice -n 19 "$podman" pull -q "$base" > /dev/null 2>&1 \
        || { record "probe-$distro" FAIL "cannot pull $base"; continue; }
    missing="$("$podman" run --rm --timeout 300 --security-opt label=disable -v "$img:/a/Myrddin-x86_64.AppImage:ro" "$base" sh -c "$probe" 2>&1)"
    echo "$missing" > "$d/missing-in-bare-image.txt"
    if [ -f "$excludelist" ]; then
        unexpected="$(for so in $missing; do sed 's/#.*//' "$excludelist" | grep -qxF "$so" || echo "$so"; done | tr '\n' ' ')"
    else
        unexpected=""
    fi
    if [ -n "$unexpected" ]; then
        record "probe-$distro" FAIL "missing and not on the excludelist: $unexpected"
    else
        record "probe-$distro" PASS "bare $base lacks only host libraries: $(echo $missing)"
    fi
    # the test image: the bare one plus the requirements
    pkgs="$(distro_pkgs "$distro")"
    tag="localhost/myrddin-apptest-$distro:$(echo "$base $pkgs" | sha256sum | cut -c1-12)"
    if ! "$podman" image exists "$tag"; then
        echo "  ($distro: making $tag)"
        printf 'FROM %s\nRUN %s\n' "$base" "$(distro_install "$distro" "$pkgs")" \
            | nice -n 19 "$podman" build -q -t "$tag" -f - "$d" > "$d/image-build.log" 2>&1 \
            || { record "$distro" FAIL "image build failed ($distro/image-build.log)"; continue; }
    fi
    "$podman" run --rm --timeout 300 --security-opt label=disable -e SETTLE="$settle" \
        -v "$img:/a/Myrddin-x86_64.AppImage:ro" -v "$d:/t" "$tag" sh -c "$inside" > "$d/podman.log" 2>&1
    code="$(cat "$d/exit" 2>/dev/null || echo none)"
    detail="$(shot_ok "$d/shot.png")"; good=$?
    cp -f "$d/shot.png" "$tdir/$distro.png" 2>/dev/null
    installed="$(cat "$d/runtime-version" 2>/dev/null)"
    detail="exit $code, $detail, payload installed: ${installed:-no}; packages: $pkgs"
    lines="$(grep -cvE '^QProcess: Destroyed|^$' "$d/log.txt" 2>/dev/null)"
    [ "${lines:-0}" -gt 0 ] && detail="$detail; $lines log lines ($distro/log.txt)"
    [ "$code" = 0 ] && [ $good -eq 0 ] && [ "$installed" = "$payload_version" ] && record "$distro" PASS "$detail" || record "$distro" FAIL "$detail"
    [ "$distro" = "${distros%% *}" ] && [ -s "$d/nofuse.txt" ] \
        && record "nofuse-$distro" INFO "no FUSE, no flag: \"$(grep -m1 -i 'cannot mount' "$d/nofuse.txt")\" (all: $distro/nofuse.txt)"
done

# --- the real platforms, in containers: X11 (Xvfb; Mesa's llvmpipe through GLX, Qt Quick on OpenGL),
#     Wayland (headless weston; Qt Quick's software renderer into shared memory) and, where weston's
#     headless backend has a GL renderer (weston 10 and later), Wayland with EGL (Qt Quick on OpenGL) ---
platform_pkgs() {
    case $1 in
    ubuntu22|debian12) echo xvfb weston libgl1-mesa-dri libegl-mesa0 libglx-mesa0 ;;
    fedora) echo xorg-x11-server-Xvfb weston mesa-dri-drivers mesa-libEGL mesa-libGL ;;
    arch) echo xorg-server-xvfb weston mesa ;;
    esac
}
platform_inside='export HOME=/t/home XDG_CONFIG_HOME=/t/home/.config XDG_DATA_HOME=/t/home/.local/share XDG_CACHE_HOME=/t/home/.cache LANG=C.UTF-8
export XDG_RUNTIME_DIR=/tmp/xdg; mkdir -p "$HOME" "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"
app() { /a/Myrddin-x86_64.AppImage --appimage-extract-and-run --screenshot "/t/$1.png" --settle "$SETTLE" > "/t/$1.log" 2>&1; echo $? > "/t/$1.exit"; }
Xvfb :99 -screen 0 1920x1080x24 -nolisten tcp > /t/xvfb.log 2>&1 & x=$!
for i in $(seq 1 50); do [ -e /tmp/.X11-unix/X99 ] && break; sleep 0.1; done
DISPLAY=:99 QT_QPA_PLATFORM=xcb app xcb; kill $x
weston_up() {   # SOCKET [renderer option]: weston 10+ names the backend "headless", 9 "headless-backend.so"
    for b in headless headless-backend.so; do
        weston --backend=$b $2 --socket=$1 --idle-time=0 > "/t/weston-$1.log" 2>&1 & w=$!
        for i in $(seq 1 30); do [ -e "$XDG_RUNTIME_DIR/$1" ] && return 0; kill -0 $w 2>/dev/null || break; sleep 0.1; done
        kill $w 2>/dev/null
    done
    return 1
}
if weston_up wl-shm; then WAYLAND_DISPLAY=wl-shm QT_QPA_PLATFORM=wayland QT_QUICK_BACKEND=software app wayland; kill $w; fi
if weston_up wl-gl --renderer=gl; then WAYLAND_DISPLAY=wl-gl QT_QPA_PLATFORM=wayland-egl app wayland-egl; kill $w; else echo skip > /t/wayland-egl.exit; fi
true'
for distro in ${PLATFORM_DISTROS-ubuntu22 arch}; do
    case " $distros " in *" $distro "*) ;; *) continue ;; esac   # needs its test image from above
    base="$(distro_base "$distro")"; pkgs="$(distro_pkgs "$distro")"
    parent="localhost/myrddin-apptest-$distro:$(echo "$base $pkgs" | sha256sum | cut -c1-12)"
    extra="$(platform_pkgs "$distro")"
    tag="localhost/myrddin-apptest-$distro-platforms:$(echo "$parent $extra" | sha256sum | cut -c1-12)"
    d="$tdir/$distro-platforms"; rm -rf "$d"; mkdir -p "$d"
    if ! "$podman" image exists "$tag"; then
        echo "  ($distro: making $tag)"
        printf 'FROM %s\nRUN %s\n' "$parent" "$(distro_install "$distro" "$extra")" \
            | nice -n 19 "$podman" build -q -t "$tag" -f - "$d" > "$d/image-build.log" 2>&1 \
            || { record "x11-$distro" FAIL "image build failed ($distro-platforms/image-build.log)"; continue; }
    fi
    "$podman" run --rm --timeout 300 --security-opt label=disable --userns=keep-id -e SETTLE="$settle" \
        -v "$img:/a/Myrddin-x86_64.AppImage:ro" -v "$d:/t" "$tag" sh -c "$platform_inside" > "$d/podman.log" 2>&1
    for p in xcb wayland wayland-egl; do
        name=$([ $p = xcb ] && echo "x11-$distro" || echo "$p-$distro")
        code="$(cat "$d/$p.exit" 2>/dev/null || echo none)"
        if [ "$code" = skip ]; then record "$name" SKIP "weston here has no GL renderer for its headless backend"; continue; fi
        detail="$(shot_ok "$d/$p.png")"; good=$?
        cp -f "$d/$p.png" "$tdir/$name.png" 2>/dev/null
        lines="$(grep -cvE '^QProcess: Destroyed|^$' "$d/$p.log" 2>/dev/null)"
        detail="exit $code, $detail"
        [ "${lines:-0}" -gt 0 ] && detail="$detail; $lines log lines ($distro-platforms/$p.log)"
        [ "$code" = 0 ] && [ $good -eq 0 ] && record "$name" PASS "$detail" || record "$name" FAIL "$detail"
    done
done

# --- the app's own tests (app/tests/run-tests.sh, shots.sh) against the AppImage, in the containers:
#     run-tests once with the AppImage itself as the binary (APPIMAGE_EXTRACT_AND_RUN=1: no FUSE in a
#     container) and once with its extracted usr/bin/myrddin. Section 8 of run-tests hands the binary a
#     stand-in payload through MYRDDIN_PAYLOAD, which AppRun replaces with the AppImage's own on purpose:
#     those checks can only pass with the bare binary, and are reported, not failed, through AppRun. ---
tools_pkgs() {
    case $1 in
    arch) echo python desktop-file-utils ;;
    *) echo python3 desktop-file-utils ;;
    esac
}
apptests_inside='export HOME=/t/home LANG=C.UTF-8; mkdir -p "$HOME"
APPIMAGE_EXTRACT_AND_RUN=1 /src/tests/run-tests.sh /a/Myrddin-x86_64.AppImage /t/rt-appimage > /t/run-tests-appimage.log 2>&1
echo $? > /t/run-tests-appimage.exit
(cd /tmp && /a/Myrddin-x86_64.AppImage --appimage-extract > /dev/null 2>&1)
/src/tests/run-tests.sh /tmp/squashfs-root/usr/bin/myrddin /t/rt-binary > /t/run-tests-binary.log 2>&1
echo $? > /t/run-tests-binary.exit
APPIMAGE_EXTRACT_AND_RUN=1 /src/tests/shots.sh /a/Myrddin-x86_64.AppImage /t/shots > /t/shots.log 2>&1
echo $? > /t/shots.exit; true'
check_failures() {   # LOG -> "N checks, M failed" and the failed ones, by design or not
    awk '/^[0-9]+\. /{sec=$0} /^  (ok|FAIL)  /{n++} /^  FAIL  /{f++; sub(/^  FAIL  /,""); if (sec ~ /runtime an AppImage carries/) d=d"; "$0; else b=b"; "$0}
         END{printf "%d|%d|%s|%s\n", n, f, substr(d,3), substr(b,3)}' "$1"
}
if [ -d "$project/app/tests" ]; then
for distro in ${APP_TESTS_DISTROS-ubuntu22 debian12 fedora arch}; do
    case " $distros " in *" $distro "*) ;; *) continue ;; esac
    base="$(distro_base "$distro")"; pkgs="$(distro_pkgs "$distro")"
    parent="localhost/myrddin-apptest-$distro:$(echo "$base $pkgs" | sha256sum | cut -c1-12)"
    extra="$(tools_pkgs "$distro")"
    tag="localhost/myrddin-apptest-$distro-tools:$(echo "$parent $extra" | sha256sum | cut -c1-12)"
    d="$tdir/$distro-apptests"; rm -rf "$d"; mkdir -p "$d"
    if ! "$podman" image exists "$tag"; then
        echo "  ($distro: making $tag)"
        printf 'FROM %s\nRUN %s\n' "$parent" "$(distro_install "$distro" "$extra")" \
            | nice -n 19 "$podman" build -q -t "$tag" -f - "$d" > "$d/image-build.log" 2>&1 \
            || { record "apptests-$distro" FAIL "image build failed ($distro-apptests/image-build.log)"; continue; }
    fi
    "$podman" run --rm --timeout 1200 --security-opt label=disable -v "$img:/a/Myrddin-x86_64.AppImage:ro" \
        -v "$project/app/tests:/src/tests:ro" -v "$d:/t" "$tag" sh -c "$apptests_inside" > "$d/podman.log" 2>&1
    IFS='|' read -r n1 f1 design1 bad1 <<< "$(check_failures "$d/run-tests-appimage.log")"
    IFS='|' read -r n2 f2 design2 bad2 <<< "$(check_failures "$d/run-tests-binary.log")"
    detail="AppImage: $n1 checks, $f1 failed${design1:+ (by design, AppRun sets its own payload: $design1)}${bad1:+; FAILED: $bad1}; usr/bin/myrddin: $n2 checks, $f2 failed${bad2:+: $bad2}${design2:+: $design2}"
    [ "${n1:-0}" -gt 0 ] && [ "${n2:-0}" -gt 0 ] && [ -z "$bad1" ] && [ "${f2:-1}" = 0 ] \
        && record "apptests-$distro" PASS "$detail" || record "apptests-$distro" FAIL "$detail (logs: $distro-apptests/)"
    # the pictures: every one a good PNG, no "FAILED:" line
    shots=0; badshots=""
    for f in "$d"/shots/*.png; do
        [ -f "$f" ] || continue
        shots=$((shots + 1))
        python3 -I "$here/packaging/pngcheck.py" "$f" any > /dev/null || badshots="$badshots $(basename "$f")"
    done
    failed_shots="$(grep -c '^FAILED:' "$d/shots.log" 2>/dev/null)"
    warn="$(grep -cvE '^FAILED:|^[0-9]+ /|^$' "$d/shots.log" 2>/dev/null)"
    detail="$shots pictures (in $distro-apptests/shots/), exit $(cat "$d/shots.exit" 2>/dev/null)${badshots:+, not good:$badshots}, ${failed_shots:-0} FAILED lines, ${warn:-0} log lines"
    [ "$shots" -gt 0 ] && [ -z "$badshots" ] && [ "${failed_shots:-0}" = 0 ] && [ "$(cat "$d/shots.exit" 2>/dev/null)" = 0 ] \
        && record "shots-$distro" PASS "$detail" || record "shots-$distro" FAIL "$detail"
done
fi

# --- (d) the table ---
{
    printf '%-22s %-4s %s\n' TEST RES DETAIL
    for r in "${results[@]}"; do IFS='|' read -r n s t <<< "$r"; printf '%-22s %-4s %s\n' "$n" "$s" "$t"; done
} > "$tdir/results.txt"
echo
cat "$tdir/results.txt"
for r in "${results[@]}"; do case "$r" in *"|FAIL|"*) exit 1 ;; esac; done
exit 0
