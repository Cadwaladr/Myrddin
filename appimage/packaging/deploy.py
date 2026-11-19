#!/usr/bin/env python3
"""Fills an AppDir for Myrddin: the installed app, the Qt plugins and QML modules it needs, and the
closure of shared libraries that the AppImage excludelist does not leave to the host.

Runs inside the build container (Ubuntu 22.04), called by ../build-appimage.sh:

    deploy.py --qt QTDIR --stage STAGE --appdir APPDIR --excludelist FILE --payload DIR
              --qml-root DIR [--qml-root DIR ...] [--styles Basic] [--report FILE]

Nothing is put into the environment: the binary finds its libraries through its RUNPATH
($ORIGIN/../lib), every carried library through its own ($ORIGIN, or the way back to usr/lib for
plugins), and Qt finds plugins and QML modules through usr/bin/qt.conf.
"""
import argparse
import fnmatch
import json
import os
import re
import shutil
import subprocess
import sys

# Qt plugins carried (category -> file patterns). Platforms: Wayland and X11 for users, offscreen
# for the tests. The rest: SVG pictures and icons, compose/dead keys and IBus input, the portal
# platform theme (file dialogs through xdg-desktop-portal), Wayland shells and client decorations,
# EGL/GLX glue for XCB.
PLUGINS = {
    "platforms": ["libqxcb.so", "libqwayland*.so", "libqoffscreen.so"],   # Wayland: -egl/-generic in 6.8, one in 6.9+
    "xcbglintegrations": ["*.so"],
    "wayland-shell-integration": ["libxdg-shell.so"],
    "wayland-decoration-client": ["*.so"],
    "wayland-graphics-integration-client": ["libqt-plugin-wayland-egl.so", "libdmabuf-server.so",
                                            "libshm-emulation-server.so", "libdrm-egl-server.so"],
    "imageformats": ["libqsvg.so"],
    "iconengines": ["libqsvgicon.so"],
    "platforminputcontexts": ["*.so"],
    "platformthemes": ["libqxdgdesktopportal.so"],
}
# Styles of QtQuick.Controls that exist in Qt; all but the ones asked for are left out.
ALL_STYLES = {"Basic", "Fusion", "Material", "Imagine", "Universal", "FluentWinUI3", "Windows", "macOS", "iOS"}

ELF_MAGIC = b"\x7fELF"


def is_elf(path):
    try:
        with open(path, "rb") as f:
            return f.read(4) == ELF_MAGIC
    except OSError:
        return False


def needed(path):
    out = subprocess.run(["readelf", "-dW", path], capture_output=True, text=True, check=True).stdout
    return re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]", out)


def read_excludelist(path):
    names = set()
    with open(path) as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if line:
                names.add(line)
    return names


def ldconfig_cache():
    """soname -> path of the x86-64 libraries the container's dynamic linker knows."""
    out = subprocess.run(["ldconfig", "-p"], capture_output=True, text=True, check=True).stdout
    cache = {}
    for line in out.splitlines()[1:]:
        m = re.match(r"\s*(\S+) \(([^)]*)\) => (\S+)", line)
        if m and "x86-64" in m.group(2):
            cache.setdefault(m.group(1), m.group(3))
    return cache


def copy_file(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(os.path.realpath(src), dst)   # symlinks resolved: the AppDir holds plain files


def set_runpath(path, runpath):
    subprocess.run(["patchelf", "--set-rpath", runpath, path], check=True)


def rel_runpath(file_dir, lib_dir):
    rel = os.path.relpath(lib_dir, file_dir)
    return "$ORIGIN" if rel == "." else "$ORIGIN/" + rel


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qt", required=True)
    ap.add_argument("--stage", required=True)
    ap.add_argument("--appdir", required=True)
    ap.add_argument("--excludelist", required=True)
    ap.add_argument("--payload", required=True)
    ap.add_argument("--qml-root", action="append", default=[])
    ap.add_argument("--styles", default="Basic")
    ap.add_argument("--packaging", required=True)
    ap.add_argument("--report", default=None)
    ap.add_argument("--app-module", default=None, help="the build tree's folder of the app's QML module")
    a = ap.parse_args()

    qt, appdir = os.path.abspath(a.qt), os.path.abspath(a.appdir)
    usr = os.path.join(appdir, "usr")
    lib_dir = os.path.join(usr, "lib")
    excluded = read_excludelist(a.excludelist)
    keep_styles = {s.strip() for s in a.styles.split(",") if s.strip()}

    if os.path.exists(appdir):
        shutil.rmtree(appdir)
    # 1. The installed app (bin/myrddin, desktop file, AppStream file, icon).
    shutil.copytree(os.path.join(a.stage, "usr"), usr, symlinks=True)
    os.makedirs(lib_dir, exist_ok=True)

    roots = []   # ELF files whose NEEDED entries are followed
    for name in sorted(os.listdir(os.path.join(usr, "bin"))):
        p = os.path.join(usr, "bin", name)
        if os.path.isfile(p) and is_elf(p):
            roots.append(p)
    binaries = list(roots)

    # 2. Plugins.
    plugins = []
    for cat, patterns in PLUGINS.items():
        src_dir = os.path.join(qt, "plugins", cat)
        if not os.path.isdir(src_dir):
            sys.exit(f"deploy: Qt has no plugin folder {cat}")
        for name in sorted(os.listdir(src_dir)):
            if any(fnmatch.fnmatch(name, p) for p in patterns):
                dst = os.path.join(usr, "plugins", cat, name)
                copy_file(os.path.join(src_dir, name), dst)
                plugins.append(dst)
        for p in patterns:   # every pattern must find something
            if not any(fnmatch.fnmatch(n, p) for n in os.listdir(src_dir)):
                sys.exit(f"deploy: no plugin {cat}/{p} in Qt")
    roots += plugins

    # 3. QML modules: what qmlimportscanner finds in the app's QML (plus packaging/qml-extra),
    #    without the QtQuick.Controls styles that were not asked for.
    cmd = [os.path.join(qt, "libexec", "qmlimportscanner")]
    for r in a.qml_root:
        cmd += ["-rootPath", r]
    cmd += ["-importPath", os.path.join(qt, "qml")]
    scan = json.loads(subprocess.run(cmd, capture_output=True, text=True, check=True).stdout)
    modules = []
    for m in scan:
        name, rel = m.get("name", ""), m.get("relativePath")
        if m.get("type") != "module" or not rel:
            continue
        parts = name.split(".")
        if parts[:2] == ["QtQuick", "Controls"] and len(parts) >= 3 and parts[2] in ALL_STYLES \
                and parts[2] not in keep_styles:
            continue
        modules.append((name, rel))
    qml_files = []
    for name, rel in sorted(set(modules)):
        src_dir = os.path.join(qt, "qml", rel)
        dst_dir = os.path.join(usr, "qml", rel)
        os.makedirs(dst_dir, exist_ok=True)
        for f in sorted(os.listdir(src_dir)):   # the module's own files; sub-folders are other modules
            sp = os.path.join(src_dir, f)
            if os.path.isfile(sp):
                copy_file(sp, os.path.join(dst_dir, f))
                if is_elf(sp):
                    qml_files.append(os.path.join(dst_dir, f))
    roots += qml_files

    # 3b. The app's own QML module must live at qrc:/qt/qml/Myrddin (on the default import path). Built
    #     with NO_RESOURCE_TARGET_PATH it sits at qrc:/ and loadFromModule only works in the build tree
    #     (build/Myrddin/qmldir beside the binary, "prefer :/"): stop here rather than ship that.
    if a.app_module and os.path.isfile(os.path.join(a.app_module, "qmldir")):
        with open(os.path.join(a.app_module, "qmldir")) as f:
            if re.search(r"^prefer :/$", f.read(), re.M):
                sys.exit("deploy: the app's QML module is at qrc:/ (NO_RESOURCE_TARGET_PATH in app/CMakeLists.txt): "
                         "an installed binary cannot find it")

    # 4. Library closure. Excluded names are the host's (and their own dependencies with them).
    cache = ldconfig_cache()
    qt_lib = os.path.join(qt, "lib")
    carried, host = {}, {}
    queue = list(roots)
    while queue:
        f = queue.pop()
        for so in needed(f):
            if so in carried or so in host:
                continue
            if so in excluded:
                host[so] = f
                continue
            src = os.path.join(qt_lib, so) if os.path.exists(os.path.join(qt_lib, so)) else cache.get(so)
            if not src:
                sys.exit(f"deploy: {so} (needed by {f}) not found in Qt or the container")
            dst = os.path.join(lib_dir, so)
            copy_file(src, dst)
            carried[so] = os.path.realpath(src)
            queue.append(dst)

    # 5. RUNPATHs: every ELF file finds usr/lib on its own.
    for root, _dirs, files in os.walk(usr):
        for f in files:
            p = os.path.join(root, f)
            if not os.path.islink(p) and is_elf(p):
                set_runpath(p, rel_runpath(root, lib_dir))
    # The app binary is not stripped by a Release build; Qt's and Ubuntu's libraries already are.
    for p in binaries:
        subprocess.run(["strip", "--strip-unneeded", p], check=True)

    # 6. qt.conf beside the binary (Prefix is relative to the binary's folder).
    shutil.copy2(os.path.join(a.packaging, "qt.conf"), os.path.join(usr, "bin", "qt.conf"))

    # 7. Payload: copied as it is (its libraries are loaded into the game, never by this program).
    pdst = os.path.join(usr, "share", "myrddin", "payload")
    shutil.copytree(a.payload, pdst, symlinks=True)

    # 8. AppDir top: AppRun, desktop file, icon, .DirIcon.
    app_id = "io.github.cadwaladr.Myrddin"
    shutil.copy2(os.path.join(a.packaging, "AppRun"), os.path.join(appdir, "AppRun"))
    os.chmod(os.path.join(appdir, "AppRun"), 0o755)
    shutil.copy2(os.path.join(usr, "share", "applications", app_id + ".desktop"),
                 os.path.join(appdir, app_id + ".desktop"))
    svg = os.path.join(usr, "share", "icons", "hicolor", "scalable", "apps", app_id + ".svg")
    shutil.copy2(svg, os.path.join(appdir, app_id + ".svg"))
    png_dir = os.path.join(usr, "share", "icons", "hicolor", "256x256", "apps")
    os.makedirs(png_dir, exist_ok=True)
    png = os.path.join(png_dir, app_id + ".png")
    subprocess.run(["rsvg-convert", "-w", "256", "-h", "256", "-o", png, svg], check=True)
    shutil.copy2(png, os.path.join(appdir, ".DirIcon"))

    report = {
        "carried": {so: carried[so] for so in sorted(carried)},
        "host": sorted(host),
        "plugins": sorted(os.path.relpath(p, usr) for p in plugins),
        "qml_modules": sorted(n for n, _ in set(modules)),
    }
    if a.report:
        with open(a.report, "w") as f:
            json.dump(report, f, indent=1)
    print(f"deploy: {len(carried)} libraries carried, {len(host)} left to the host, "
          f"{len(plugins)} plugins, {len(set(modules))} QML modules")


if __name__ == "__main__":
    main()
