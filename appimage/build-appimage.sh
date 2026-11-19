#!/usr/bin/env bash
# Builds appimage/out/Myrddin-x86_64.AppImage from app/ (the Qt 6 Quick window) and the payload
# folder, in one command, from nothing: a podman container with Ubuntu 22.04 (glibc 2.35, the
# oldest system the AppImage has to run on), Qt 6.11 from the official binaries (aqtinstall),
# the app built with CMake, an AppDir filled by packaging/deploy.py, and appimagetool with the
# static type-2 runtime. Later runs reuse the container, the downloads, Qt and the build tree:
# after an edit in app/ a run takes about a minute.
#
#   appimage/build-appimage.sh
#
# Options, through the environment only:
#   APP_SRC=DIR      the app's source folder (default: app/). Must lie inside the project folder.
#   PAYLOAD=DIR      the payload carried in usr/share/myrddin/payload (default: appimage/out/payload
#                    when it exists and is not empty, otherwise a stub made in work/app/stub-payload:
#                    a VERSION file and an sc-turbo that prints its arguments). Inside the project.
#   QT_VERSION=X     Qt version (default 6.11.3, the line the app is developed on; archive checksums
#                    are pinned for 6.11.3 and 6.8.3, the last open-source 6.8 LTS; other versions are
#                    checked by aqt against download.qt.io only, and their sums are printed).
#   QT_MIRROR=URL    base URL of a download.qt.io mirror for aqt (default: download.qt.io).
#   QT_STYLES=LIST   QtQuick.Controls styles carried, comma separated (default: Basic).
#   CLEAN=1          the app's build tree is made anew (downloads, Qt and the container are kept).
#   RECREATE=1       the container is removed and made (and provisioned) anew.
#   JOBS=N           parallel compile jobs (default 6). PODMAN=CMD (default podman).
#   RUNTIME_URL=URL RUNTIME_SHA256=SUM   another type-2 runtime (e.g. the tagged release 20251108,
#                    sha256 2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d, should the
#                    continuous build be replaced; delete work/app/dl/runtime/ first).
#
# Result: appimage/out/Myrddin-x86_64.AppImage, with the licence texts of everything it carries and
# THIRD-PARTY-APPIMAGE.txt in usr/share/licenses/myrddin/ (packaging/licenses.py); what went into it
# (library list, plugins, QML modules, sizes, highest GLIBC version needed) in
# appimage/work/app/report.txt, the licences found in appimage/work/app/licenses.json.
# Needs podman (rootless), curl, sha256sum and the network the first time.
#
# Pinned and checked by sha256: appimagetool 1.9.1, the type-2 runtime (continuous build of
# 2026-10-19: it has the 0700 extraction folder fix that the last tagged release 20251108 lacks),
# the AppImage excludelist (pkg2appimage commit 15a64c2), the Qt 6.11.3 and 6.8.3 archives, Qt 6.11.3's
# source archives (for their LICENSES/ folders), the licence files of ICU and of the runtime's parts.
set -euo pipefail

here="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"   # appimage/
project="$(dirname "$here")"
work="$here/work/app"
out="$here/out"
name=myrddin-app-build
image=docker.io/library/ubuntu:22.04
mirror=mirrors.edge.kernel.org/ubuntu/
jobs="${JOBS:-6}"
podman="${PODMAN:-podman}"
qt_version="${QT_VERSION:-6.11.3}"
qt_mirror="${QT_MIRROR:-}"
styles="${QT_STYLES:-Basic}"
clean="${CLEAN:-0}"
provision_version=1   # raise when the package list below changes: the container is provisioned again

tool_url=https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage
tool_sha=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
runtime_url="${RUNTIME_URL:-https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64}"
runtime_sha="${RUNTIME_SHA256:-617aa77314d57c8b8e4fae9c3f0b35af7b30d5529cb69e98f64f92510ed98a46}"
excl_url=https://raw.githubusercontent.com/AppImageCommunity/pkg2appimage/15a64c20dc23a0154622ba25829364323903b6b5/excludelist
excl_sha=50db0f894f34b169c47a5cbc0c17dbab61e9edebca5bc8269a1e6ac1bf4bdad9
# sha256 of the Qt archives aqt fetches (file: sum), per version
qt_sums_6_11_3="icu-linux-Rhel8.6-x86_64.7z:16f1e8f409bd9f0b248e36d10deab498d0c1b33d25e0a515a076b1f6b5a1ab28
qtbase-Linux-RHEL_9_6-GCC-Linux-RHEL_9_6-X86_64.7z:ce282da4d8a83dc216ed96b715202872d655177189c16205dc5fab09b41a9f09
qtdeclarative-Linux-RHEL_9_6-GCC-Linux-RHEL_9_6-X86_64.7z:424a4012edd475e21c108df6e297ab034cb786ec91f5fe451d20e3a5540e486d
qtsvg-Linux-RHEL_9_6-GCC-Linux-RHEL_9_6-X86_64.7z:7b71eea32674d6ac8e9b5b1d3819f510c6081619ca440b6cac86fd970942cf7a
qtwayland-Linux-RHEL_9_6-GCC-Linux-RHEL_9_6-X86_64.7z:510a61a415699e83a9d0431c7e3d4621e50776c7f57aefbe0f9a841132e2dd99"
qt_sums_6_8_3="icu-linux-Rhel8.6-x86_64.7z:a2543496e1280c6fa348e0791def5026d5bde9e5749d9fc2244866ab0bee8aed
qtbase-Linux-RHEL_8_10-GCC-Linux-RHEL_8_10-X86_64.7z:d8fe882cae71284a7ca906b70b42840376882fe96f22fce1ba5a7a70ceeec799
qtdeclarative-Linux-RHEL_8_10-GCC-Linux-RHEL_8_10-X86_64.7z:6330a5ed0138393468be3ddcca458606e14a607f7191d8dbad3978004bdf29ee
qtsvg-Linux-RHEL_8_10-GCC-Linux-RHEL_8_10-X86_64.7z:e330eb3abed3e0b9c9a278da971ec67306969f35020a3d54350a6ee86d57983d
qtwayland-Linux-RHEL_8_10-GCC-Linux-RHEL_8_10-X86_64.7z:da40cc32c95aa9bf96fcf3e2bfe19055c93cc1ae297bd7cd3cf532aaef634cff"
case "$qt_version" in
6.11.3) qt_sums="$qt_sums_6_11_3" ;;
6.8.3) qt_sums="$qt_sums_6_8_3" ;;
*) qt_sums="" ;;
esac

if [ $# -gt 0 ]; then sed -n '2,/^set -e/{/^set -e/d;s/^# \{0,1\}//;p}' "$0"; exit 1; fi
start=$(date +%s)

# Host paths -> paths in the container (the project is mounted read-only at /src).
in_container() {
    local p
    p="$(readlink -f "$1")"
    case "$p" in
    "$project") echo /src ;;
    "$project"/*) echo "/src/${p#"$project"/}" ;;
    *) echo "outside the project folder: $1" >&2; return 1 ;;
    esac
}

# Downloads go to work/app/dl/NAME/ and are checked against the pinned sum.
fetch() {   # fetch DIR FILE URL SHA256
    local dir="$work/dl/$1" file="$work/dl/$1/$2"
    mkdir -p "$dir"
    if [ ! -f "$file" ]; then
        echo "download: $3"
        curl -fsSL -m 600 -o "$file.part" "$3"
        mv -f "$file.part" "$file"
    fi
    if ! echo "$4  $file" | sha256sum -c --quiet -; then
        echo "checksum of $file does not match the pinned one: remove it to download again, or check the source"
        exit 1
    fi
}
mkdir -p "$work" "$out"
fetch appimagetool appimagetool-x86_64.AppImage "$tool_url" "$tool_sha"
fetch runtime runtime-x86_64 "$runtime_url" "$runtime_sha"
fetch excludelist excludelist "$excl_url" "$excl_sha"

# Licence texts the AppImage carries besides those in Qt's sources (packaging/licenses.py): ICU as Qt
# ships it, the AppImage runtime and what it links statically (pinned at the runtime's commit's versions).
while read -r f url sum; do
    fetch licenses "$f" "$url" "$sum"
done <<'LIC'
ICU-73.2-LICENSE https://raw.githubusercontent.com/unicode-org/icu/release-73-2/icu4c/LICENSE f3005e195ff74d8812cc1f182a1c446fab678d70a10e3dada497585befee5416
type2-runtime-LICENSE https://raw.githubusercontent.com/AppImage/type2-runtime/fedfad51acc2860b2916a1eef32ba4a500ff31ef/LICENSE aa154fc9070614bbe7921f89db11efd1dba7a1f3a41685958110e2230f9c0ca1
libfuse-3.15.0-LGPL-2.1.txt https://raw.githubusercontent.com/libfuse/libfuse/fuse-3.15.0/LGPL2.txt dc626520dcd53a22f727af3ee42c770e56c97a64fe3adb063799d8ab032fe551
squashfuse-0.5.2-LICENSE https://raw.githubusercontent.com/vasi/squashfuse/0.5.2/LICENSE 9e909cc8a8ba27b1a649c964cf9eda37de911f3138c2b64e6d2f01703904ac13
zstd-1.5.6-LICENSE https://raw.githubusercontent.com/facebook/zstd/v1.5.6/LICENSE 7055266497633c9025b777c78eb7235af13922117480ed5c674677adc381c9d8
zlib-1.3.1-LICENSE https://raw.githubusercontent.com/madler/zlib/v1.3.1/LICENSE 845efc77857d485d91fb3e0b884aaa929368c717ae8186b66fe1ed2495753243
mimalloc-2.1.7-LICENSE https://raw.githubusercontent.com/microsoft/mimalloc/v2.1.7/LICENSE 19c99805e7a44a34b297a75d1edea9985e300066dfc024d5c99d4236d4573b5d
musl-1.2.5-COPYRIGHT https://git.musl-libc.org/cgit/musl/plain/COPYRIGHT?h=v1.2.5 f9bc4423732350eb0b3f7ed7e91d530298476f8fec0c6c427a1c04ade22655af
LIC

# Qt's source archives of the same version, for the LICENSES/ folder of each carried module (only
# those folders are unpacked, into work/app/dl/qt-src/VERSION/LICENSES-MODULE/). Pinned for 6.11.3;
# for other versions checked against download.qt.io's md5sums.txt and their sha256 printed.
qt_src_sums_6_11_3="qtbase:46de0710b594945a5d446ae44354daa386141c6903c53781b2ca547bc62f8f8f
qtdeclarative:6d6db328a02ae3bb0b1e6944aeef8dfe44cd6b55ab59b1708bbf0400f5759cb5
qtsvg:9bdf6fa4b5a2d91c3eaa2edd12263fdff0ae19d0ac25d3c99946cd4189a07123
qtwayland:7f6185eb6e7d75202c42dfaf18dc54ae96f2d9a7a265358d061a6f85366f6c97"
qsrc="$work/dl/qt-src/$qt_version"
qsrc_url="https://download.qt.io/archive/qt/${qt_version%.*}/$qt_version/submodules"
for m in qtbase qtdeclarative qtsvg qtwayland; do
    [ -d "$qsrc/LICENSES-$m" ] && continue
    f="$m-everywhere-src-$qt_version.tar.xz"
    if [ "$qt_version" = 6.11.3 ]; then
        fetch "qt-src/$qt_version" "$f" "$qsrc_url/$f" "$(echo "$qt_src_sums_6_11_3" | sed -n "s/^$m://p")"
    else
        mkdir -p "$qsrc"
        [ -f "$qsrc/md5sums.txt" ] || curl -fsSL -m 60 -o "$qsrc/md5sums.txt" "$qsrc_url/md5sums.txt"
        [ -f "$qsrc/$f" ] || { echo "download: $qsrc_url/$f"; curl -fsSL -m 900 -o "$qsrc/$f.part" "$qsrc_url/$f"; mv -f "$qsrc/$f.part" "$qsrc/$f"; }
        (cd "$qsrc" && grep " $f\$" md5sums.txt | md5sum -c --quiet -) || { echo "$f: md5 differs from download.qt.io's"; exit 1; }
        echo "not pinned: $(cd "$qsrc" && sha256sum "$f")"
    fi
    rm -rf "$qsrc/.x"; mkdir -p "$qsrc/.x"
    tar -xJf "$qsrc/$f" -C "$qsrc/.x" --wildcards "*/LICENSES/*"
    mv "$qsrc/.x/$m-everywhere-src-$qt_version/LICENSES" "$qsrc/LICENSES-$m"
    rm -rf "$qsrc/.x"
done

app_src="$(in_container "${APP_SRC:-$project/app}")"
[ -f "${APP_SRC:-$project/app}/CMakeLists.txt" ] || { echo "no CMakeLists.txt in ${APP_SRC:-$project/app}"; exit 1; }
payload_host="${PAYLOAD:-$here/out/payload}"
if [ -z "${PAYLOAD:-}" ] && { [ ! -d "$payload_host" ] || [ -z "$(ls -A "$payload_host" 2>/dev/null)" ]; }; then
    # No payload yet: a stub, so that the layout and the app's look-up can be tested.
    payload_host="$work/stub-payload"
    rm -rf "$payload_host"
    mkdir -p "$payload_host"
    echo "stub $(date -u +%Y-%m-%d)" > "$payload_host/VERSION"
    printf '#!/bin/sh\n# Stub of the payload'"'"'s sc-turbo (the real payload was not built yet).\necho "sc-turbo (stub): $*"\n' \
        > "$payload_host/sc-turbo"
    chmod 755 "$payload_host/sc-turbo"
    echo "payload: stub ($payload_host)"
else
    [ -d "$payload_host" ] || { echo "PAYLOAD $payload_host is not a folder"; exit 1; }
    echo "payload: $payload_host ($(head -1 "$payload_host/VERSION" 2>/dev/null || echo 'no VERSION file'))"
fi
payload="$(in_container "$payload_host")"

# The container: made once, then reused; one with other mounts (the project moved) is made anew.
mounts_ok() {
    local m
    m="$("$podman" inspect --format '{{range .Mounts}}{{.Source}}={{.Destination}}:{{.RW}} {{end}}' "$name" 2>/dev/null)" || return 1
    case " $m " in *" $project=/src:false "*) ;; *) return 1 ;; esac
    case " $m " in *" $work=/work:true "*) ;; *) return 1 ;; esac
}
if "$podman" container exists "$name" && { [ "${RECREATE:-0}" = 1 ] || ! mounts_ok; }; then
    echo "container $name: made anew"
    "$podman" rm -f "$name" > /dev/null
fi
if ! "$podman" container exists "$name"; then
    "$podman" image exists "$image" || nice -n 19 "$podman" pull -q "$image" > /dev/null
    "$podman" run -d --name "$name" --security-opt label=disable \
        -v "$project:/src:ro" -v "$work:/work" -w /work "$image" sleep infinity > /dev/null
    echo "container $name made ($image)"
elif [ "$("$podman" inspect --format '{{.State.Running}}' "$name")" != true ]; then
    "$podman" start "$name" > /dev/null
fi

"$podman" exec -i -e JOBS="$jobs" -e CLEAN="$clean" -e PROVISION="$provision_version" -e MIRROR="$mirror" \
    -e QTV="$qt_version" -e QT_MIRROR="$qt_mirror" -e QT_SUMS="$qt_sums" -e APP_SRC="$app_src" \
    -e PAYLOAD="$payload" -e STYLES="$styles" -e LC_ALL=C.UTF-8 "$name" bash -s <<'INSIDE'
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
nice="nice -n 19"

# --- provisioning (once per provision_version) ---
if [ "$(cat /opt/provisioned 2>/dev/null)" != "$PROVISION" ]; then
    # Ubuntu's own servers were unreachable from the build machine; kernel.org's mirror is used.
    # First over http (apt checks the signed Release files) to get the CA certificates, then https.
    sed -i -E "s#https?://(archive|security)\.ubuntu\.com/ubuntu/?#http://$MIRROR#" /etc/apt/sources.list
    $nice apt-get -qq update
    $nice apt-get -qq install -y --no-install-recommends ca-certificates > /dev/null
    sed -i -E "s#http://$MIRROR#https://$MIRROR#" /etc/apt/sources.list
    $nice apt-get -qq update
    # Build: g++ 11, cmake 3.22, ninja; GL/EGL/xkbcommon headers for Qt6Gui's CMake package.
    # Runtime libraries the Qt plugins link (the deploy step copies those not on the excludelist).
    # Tools: patchelf, readelf/objdump/strip, rsvg-convert (the PNG icon), desktop-file-validate,
    # appstreamcli; fontconfig and a font for a smoke test of the AppDir.
    $nice apt-get -qq install -y --no-install-recommends \
        python3 python3-venv g++ make cmake ninja-build patchelf file binutils \
        libgl-dev libegl-dev libopengl-dev libxkbcommon-dev \
        libfontconfig1 libfreetype6 libglib2.0-0 libdbus-1-3 libx11-6 libx11-xcb1 libxkbcommon-x11-0 \
        libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-randr0 libxcb-render-util0 \
        libxcb-shape0 libxcb-shm0 libxcb-sync1 libxcb-xfixes0 libxcb-xkb1 libxcb-render0 libxcb-glx0 \
        libwayland-client0 libwayland-cursor0 libwayland-egl1 \
        librsvg2-bin desktop-file-utils appstream fontconfig fonts-dejavu-core > /dev/null
    python3 -m venv /opt/aqt
    $nice /opt/aqt/bin/pip install -q aqtinstall
    echo "$PROVISION" > /opt/provisioned
    echo "container provisioned"
fi

# --- Qt (official binaries, once per version) ---
Q=/work/qt/$QTV/gcc_64
if [ ! -f "/work/qt/.installed-$QTV" ]; then
    rm -rf "/work/qt/$QTV"
    mkdir -p /work/qt "/work/dl/qt/$QTV"   # one folder per version: some archive names (icu) repeat
    base=()
    [ -n "$QT_MIRROR" ] && base=(-b "$QT_MIRROR")
    (cd /work/qt && $nice /opt/aqt/bin/aqt install-qt "${base[@]}" linux desktop "$QTV" linux_gcc_64 \
        --outputdir /work/qt --archives icu qtbase qtdeclarative qtsvg qtwayland \
        --keep --archive-dest "/work/dl/qt/$QTV" > /work/qt/aqt-install.log 2>&1) \
        || { tail -20 /work/qt/aqt-install.log; exit 1; }
    if [ -n "$QT_SUMS" ]; then
        n=0
        while IFS=: read -r f sum; do
            echo "$sum  /work/dl/qt/$QTV/$f" | sha256sum -c --quiet - || { echo "Qt archive $f: checksum differs"; exit 1; }
            n=$((n + 1))
        done <<< "$QT_SUMS"
        [ "$n" = "$(ls /work/dl/qt/$QTV/*.7z | wc -l)" ] || { echo "Qt $QTV: other archives than the pinned ones"; exit 1; }
    else
        echo "Qt $QTV: no pinned checksums (aqt checked them against download.qt.io); they are:"
        (cd "/work/dl/qt/$QTV" && sha256sum *.7z | awk '{print $2 ":" $1}')
    fi
    touch "/work/qt/.installed-$QTV"
    echo "Qt $QTV installed (aqtinstall $(/opt/aqt/bin/aqt version 2>&1 | grep -o 'v[0-9][0-9.]*' | head -1))"
fi

# --- appimagetool, extracted once (no FUSE in the container) ---
T=/work/tools/appimagetool
if [ ! -x "$T/AppRun" ] || [ "$(cat $T/.sha 2>/dev/null)" != "$(sha256sum /work/dl/appimagetool/appimagetool-x86_64.AppImage | cut -d' ' -f1)" ]; then
    rm -rf "$T" /work/tools/squashfs-root
    mkdir -p /work/tools
    chmod +x /work/dl/appimagetool/appimagetool-x86_64.AppImage
    (cd /work/tools && /work/dl/appimagetool/appimagetool-x86_64.AppImage --appimage-extract > /dev/null)
    mv /work/tools/squashfs-root "$T"
    sha256sum /work/dl/appimagetool/appimagetool-x86_64.AppImage | cut -d' ' -f1 > "$T/.sha"
fi

# --- the app ---
B=/work/build
if [ "$CLEAN" = 1 ] || [ "$(cat $B/.src 2>/dev/null)" != "$APP_SRC $QTV" ]; then
    rm -rf "$B"
fi
if [ ! -f "$B/build.ninja" ]; then
    mkdir -p "$B"
    $nice cmake -S "$APP_SRC" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$Q" \
        -DCMAKE_INSTALL_PREFIX=/usr -Wno-dev > "$B/configure.log" 2>&1 || { tail -30 "$B/configure.log"; exit 1; }
    echo "$APP_SRC $QTV" > "$B/.src"
fi
$nice cmake --build "$B" -j "$JOBS" > "$B/build.log" 2>&1 || { grep -E 'error|Error' "$B/build.log" | head -30; tail -5 "$B/build.log"; exit 1; }
rm -rf /work/stage
DESTDIR=/work/stage cmake --install "$B" > /dev/null

# --- the AppDir ---
qml_roots=(--qml-root /src/appimage/packaging/qml-extra)
[ -d "$APP_SRC/qml" ] && qml_roots+=(--qml-root "$APP_SRC/qml")
python3 -I /src/appimage/packaging/deploy.py --qt "$Q" --stage /work/stage --appdir /work/AppDir \
    --excludelist /work/dl/excludelist/excludelist --payload "$PAYLOAD" --styles "$STYLES" \
    --packaging /src/appimage/packaging "${qml_roots[@]}" --app-module "$B/Myrddin" --report /work/deploy.json
python3 -I /src/appimage/packaging/licenses.py --appdir /work/AppDir --deploy-json /work/deploy.json \
    --qt "$Q" --qt-version "$QTV" --qt-licenses "/work/dl/qt-src/$QTV" --texts /work/dl/licenses \
    --project-license /src/LICENSE --app-src "$APP_SRC" --payload "$PAYLOAD" --summary /work/licenses.json

# --- checks of the metadata (informational; appimagetool repeats the AppStream one) ---
desktop-file-validate /work/AppDir/*.desktop || true
appstreamcli validate --no-net --explain /work/AppDir/usr/share/metainfo/*.xml > /work/appstream.txt 2>&1 || true

# --- the AppImage ---
rm -f /work/Myrddin-x86_64.AppImage
ARCH=x86_64 $nice "$T/AppRun" --no-appstream --runtime-file /work/dl/runtime/runtime-x86_64 \
    --comp zstd --mksquashfs-opt -processors --mksquashfs-opt "$JOBS" \
    /work/AppDir /work/Myrddin-x86_64.AppImage > /work/appimagetool.log 2>&1 \
    || { tail -30 /work/appimagetool.log; exit 1; }

# --- report ---
python3 -I - <<'PY'
import json, os, re, subprocess
d = json.load(open("/work/deploy.json"))
usr = "/work/AppDir/usr"
def elf(p):
    try:
        return open(p, "rb").read(4) == b"\x7fELF"
    except OSError:
        return False
def versions(p):
    out = subprocess.run(["objdump", "-p", p], capture_output=True, text=True).stdout
    return set(re.findall(r"\b(GLIBC_[0-9.]+|GLIBCXX_[0-9.]+|CXXABI_[0-9.]+)\b", out.split("Version References:")[-1] if "Version References:" in out else ""))
def key(v):
    return tuple(int(x) for x in v.split("_", 1)[1].split("."))
def scan(top, skip=None):
    found = {}
    for root, dirs, files in os.walk(top):
        if skip and os.path.abspath(root).startswith(skip):
            continue
        for f in files:
            p = os.path.join(root, f)
            if not os.path.islink(p) and elf(p):
                for v in versions(p):
                    found.setdefault(v, []).append(os.path.relpath(p, "/work/AppDir"))
    return found
def top(found, prefix):
    vs = sorted((v for v in found if v.startswith(prefix + "_")), key=key)
    return (vs[-1], sorted(set(found[vs[-1]]))[:4]) if vs else ("none", [])
def du(p):
    t = 0
    for root, dirs, files in os.walk(p):
        for f in files:
            fp = os.path.join(root, f)
            if not os.path.islink(fp):
                t += os.path.getsize(fp)
    return t
payload_dir = os.path.join(usr, "share/myrddin/payload")
app = scan("/work/AppDir", skip=payload_dir)
pay = scan(payload_dir)
lines = []
size = os.path.getsize("/work/Myrddin-x86_64.AppImage")
lines.append(f"AppImage: {size/1e6:.1f} MB; AppDir {du('/work/AppDir')/1e6:.1f} MB unpacked")
for part in ("bin", "lib", "plugins", "qml", "share/myrddin/payload"):
    lines.append(f"  usr/{part}: {du(os.path.join(usr, part))/1e6:.1f} MB")
libs = sorted(((os.path.getsize(os.path.join(usr, 'lib', so)), so) for so in d["carried"]), reverse=True)
lines.append("largest libraries: " + ", ".join(f"{so} {s/1e6:.1f}" for s, so in libs[:6]) + " (MB)")
for prefix in ("GLIBC", "GLIBCXX", "CXXABI"):
    v, who = top(app, prefix)
    lines.append(f"highest {prefix} needed by Qt/app files: {v} ({', '.join(who)})")
    v, who = top(pay, prefix)
    lines.append(f"highest {prefix} needed by payload files: {v} ({', '.join(who)})")
lines.append("carried libraries (" + str(len(d["carried"])) + "):")
for so, src in d["carried"].items():
    lines.append(f"  {so}  <- {src}")
lines.append("left to the host (excludelist): " + " ".join(d["host"]))
lines.append("plugins: " + " ".join(d["plugins"]))
lines.append("QML modules: " + " ".join(d["qml_modules"]))
open("/work/report.txt", "w").write("\n".join(lines) + "\n")
print("\n".join(lines[:12]))
PY
INSIDE

cp -f "$work/Myrddin-x86_64.AppImage" "$out/Myrddin-x86_64.AppImage.part"
mv -f "$out/Myrddin-x86_64.AppImage.part" "$out/Myrddin-x86_64.AppImage"
echo "built $out/Myrddin-x86_64.AppImage in $(( $(date +%s) - start )) s (report: $work/report.txt)"
