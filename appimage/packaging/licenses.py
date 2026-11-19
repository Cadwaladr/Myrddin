#!/usr/bin/env python3
"""Puts the licence texts of everything the AppImage redistributes into usr/share/licenses/myrddin/ of
the AppDir and writes THIRD-PARTY-APPIMAGE.txt there (one line per component: name, version,
licence, where the source is). Runs in the build container after deploy.py (../build-appimage.sh):

    licenses.py --appdir APPDIR --deploy-json FILE --qt QTDIR --qt-version X --qt-licenses DIR
                --texts DIR --project-license FILE --app-src DIR --payload DIR --summary FILE

--qt-licenses holds the LICENSES/ folders of Qt's source archives (LICENSES-qtbase/ ...);
--texts the pinned licence files of ICU and of the AppImage runtime and what it links statically.
Which third-party code is inside the carried Qt files comes from Qt's own SBOM (QTDIR/sbom/*.spdx.json).
Ubuntu libraries: the owning package (dpkg -S) and its /usr/share/doc/PACKAGE/copyright.
Nothing of the build machine (paths, names) goes into the text.
"""
import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys

QT_MODULES = ("qtbase", "qtdeclarative", "qtsvg", "qtwayland")
RUNTIME = {
    "commit": "fedfad51acc2860b2916a1eef32ba4a500ff31ef",
    "parts": [   # statically linked into the type-2 runtime (its Makefile and install-dependencies.sh)
        ("libfuse", "3.15.0", "LGPL-2.1 (the library)", "https://github.com/libfuse/libfuse/releases/tag/fuse-3.15.0",
         "libfuse-3.15.0-LGPL-2.1.txt"),
        ("squashfuse", "0.5.2", "BSD-2-Clause", "https://github.com/vasi/squashfuse/releases/tag/0.5.2",
         "squashfuse-0.5.2-LICENSE"),
        ("zstd", "1.5.6 (Alpine 3.21)", "BSD-3-Clause", "https://github.com/facebook/zstd", "zstd-1.5.6-LICENSE"),
        ("zlib", "1.3.1 (Alpine 3.21)", "Zlib", "https://github.com/madler/zlib", "zlib-1.3.1-LICENSE"),
        ("mimalloc", "2.1.7 (Alpine 3.21)", "MIT", "https://github.com/microsoft/mimalloc", "mimalloc-2.1.7-LICENSE"),
        ("musl", "1.2.5 (Alpine 3.21)", "MIT", "https://musl.libc.org", "musl-1.2.5-COPYRIGHT"),
    ],
}


# The licence of the carried LIBRARY of each Ubuntu source package (read in its copyright file; the
# file itself, with every term of the whole source package, is carried next to it).
UBUNTU_MAIN = {
    "brotli": "MIT", "libcap2": "BSD-3-Clause OR GPL-2.0-only", "dbus": "AFL-2.1 OR GPL-2.0-or-later",
    "libgcrypt20": "LGPL-2.1-or-later", "glib2.0": "LGPL-2.1-or-later", "krb5": "MIT (Kerberos licence, BSD-style parts)",
    "keyutils": "LGPL-2.0-or-later", "lz4": "BSD-2-Clause", "xz-utils": "public domain (liblzma)",
    "pcre3": "BSD-3-Clause", "systemd": "LGPL-2.1-or-later", "wayland": "MIT (X11)", "libxcb": "MIT (X11)",
    "xcb-util": "MIT (X11)", "xcb-util-cursor": "MIT (X11)", "xcb-util-image": "MIT (X11)",
    "xcb-util-keysyms": "MIT (X11)", "xcb-util-renderutil": "MIT (X11)", "xcb-util-wm": "MIT (X11)",
    "libxkbcommon": "MIT (X11)", "libzstd": "BSD-3-Clause OR GPL-2.0-only",
}


def font_version(path):
    """nameID 5 ("Version x.yyy;...") of a TrueType/OpenType font."""
    import struct
    data = open(path, "rb").read()
    num = struct.unpack(">H", data[4:6])[0]
    for i in range(num):
        tag, _sum, off, _len = struct.unpack(">4sIII", data[12 + 16 * i:28 + 16 * i])
        if tag == b"name":
            _fmt, count, strings = struct.unpack(">HHH", data[off:off + 6])
            for j in range(count):
                plat, _enc, _lang, nid, ln, so = struct.unpack(">HHHHHH", data[off + 6 + 12 * j:off + 18 + 12 * j])
                if nid == 5:
                    raw = data[off + strings + so:off + strings + so + ln]
                    text = raw.decode("utf-16-be" if plat in (0, 3) else "latin-1", "replace")
                    m = re.search(r"(\d+\.\d+)", text)
                    return m.group(1) if m else text
    return "?"


def spdx_ids(expr):
    return {t for t in re.split(r"[\s()]+", expr or "") if t and t not in ("AND", "OR", "WITH", "NOASSERTION", "NONE")}


def qt_parts(qt, carried_rel):
    """(name, version, licence) of the third-party code inside the carried Qt files, from the SBOM."""
    pkgs, files, edges = {}, {}, {}
    for doc in glob.glob(os.path.join(qt, "sbom", "*.spdx.json")):
        d = json.load(open(doc))
        for p in d.get("packages", []):
            pkgs[p["SPDXID"]] = p
        for f in d.get("files", []):
            files[f["SPDXID"]] = os.path.normpath(f.get("fileName", ""))
        for r in d.get("relationships", []):
            a, b = r["spdxElementId"].split(":")[-1], r["relatedSpdxElement"].split(":")[-1]
            if r["relationshipType"] in ("DEPENDS_ON", "CONTAINS"):
                edges.setdefault(a, set()).add(b)
    owner = {}
    for a, bs in edges.items():
        for b in bs:
            if b in files:
                owner.setdefault(files[b], set()).add(a)
    start, unknown = set(), []
    for rel in carried_rel:
        o = owner.get(os.path.normpath(rel))
        if o:
            start |= o
        else:
            unknown.append(rel)
    seen, stack = set(), list(start)
    while stack:
        s = stack.pop()
        if s in seen:
            continue
        seen.add(s)
        stack += [b for b in edges.get(s, ()) if b in pkgs]
    parts, modules = {}, {}
    for s in seen:
        p = pkgs[s]
        lic = p.get("licenseConcluded") or p.get("licenseDeclared") or "NOASSERTION"
        if "-qt-3rdparty-sources-" in s or "-qt-bundled-3rdparty-module-" in s:
            name = re.sub(r"^.*_Attribution_", "", p["name"])
            name = re.sub(r"^Bundled", "", name)
            if name.endswith("Private"):
                continue   # the same code as its Bundled... package
            parts[(name.lower(), p.get("versionInfo", "?"))] = lic
        elif ("-qt-module-" in s or "-qt-plugin-" in s) and not re.search(r"(Private|Internal)$|^Platform$", p["name"]):
            modules[p["name"]] = lic
    return parts, modules, unknown


def dpkg_owner(path):
    for cand in (path, path.replace("/usr/lib/", "/lib/", 1)):
        r = subprocess.run(["dpkg", "-S", cand], capture_output=True, text=True)
        if r.returncode == 0:
            return r.stdout.split(":", 1)[0].split(",")[0].strip()
    return None


def copyright_licenses(text):
    """The licence names of a Debian (DEP-5) copyright file."""
    found = []
    for m in re.finditer(r"^License:\s*(\S.*)$", text, re.M):
        for part in re.split(r"\s+(?:or|and)\s+|\s*,\s*", m.group(1).strip()):
            part = part.strip()
            if part and part not in found:
                found.append(part)
    return found


def main():
    ap = argparse.ArgumentParser()
    for opt in ("appdir", "deploy-json", "qt", "qt-version", "qt-licenses", "texts", "project-license",
                "app-src", "payload", "summary"):
        ap.add_argument("--" + opt, required=True)
    a = ap.parse_args()
    usr = os.path.join(a.appdir, "usr")
    out = os.path.join(usr, "share", "licenses", "myrddin")
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out)
    dep = json.load(open(a.deploy_json))
    qt = os.path.realpath(a.qt)
    lines, summary, missing = [], {"qt": {}, "ubuntu": {}, "other": {}}, []

    # this program
    shutil.copy2(a.project_license, os.path.join(out, "LICENSE"))
    version = "?"
    try:
        with open(os.path.join(a.app_src, "CMakeLists.txt")) as f:
            m = re.search(r"project\(\s*myrddin\s+VERSION\s+([0-9.]+)", f.read())
            version = m.group(1) if m else version
    except OSError:
        pass
    lines.append(f"Myrddin (the window and its runtime files) | {version} | GPL-3.0-or-later | "
                 "https://github.com/Cadwaladr/Myrddin | LICENSE")
    payload_lic = os.path.join(a.payload, "licenses")
    if os.path.isdir(payload_lic):
        shutil.copytree(payload_lic, os.path.join(out, "payload"))
    lines.append("  The runtime files in usr/share/myrddin/payload name their third-party parts in its "
                 "THIRD-PARTY.md; their licence texts are in payload/ here")

    # Qt
    qv = a.qt_version
    carried_rel = []
    for so, src in dep["carried"].items():
        if os.path.realpath(src).startswith(qt + os.sep):
            carried_rel.append(os.path.relpath(os.path.realpath(src), qt))
    carried_rel += dep["plugins"]   # plugins/CAT/NAME, the same path under the Qt prefix
    for root, _d, fs in os.walk(os.path.join(usr, "qml")):
        for f in fs:
            if f.endswith(".so"):
                carried_rel.append(os.path.relpath(os.path.join(root, f), usr))
    parts, modules, unknown = qt_parts(qt, carried_rel)
    qt_ids = set()
    for lic in list(parts.values()) + list(modules.values()):
        qt_ids |= spdx_ids(lic)
    qt_ids |= {"LGPL-3.0-only", "GPL-2.0-only", "GPL-3.0-only"}
    qt_ids.discard("LicenseRef-Qt-Commercial")   # the open-source licences are the ones used here
    qdir = os.path.join(out, "Qt")
    os.makedirs(qdir)
    for ident in sorted(qt_ids):
        src = None
        for m in QT_MODULES:
            cand = os.path.join(a.qt_licenses, "LICENSES-" + m, ident + ".txt")
            if os.path.isfile(cand):
                src = cand
                break
        if src:
            shutil.copy2(src, os.path.join(qdir, ident + ".txt"))
        else:
            missing.append(f"Qt: no text for {ident}")
    short = qv.rsplit(".", 1)[0]
    urls = ", ".join(f"https://download.qt.io/archive/qt/{short}/{qv}/submodules/{m}-everywhere-src-{qv}.tar.xz"
                     for m in QT_MODULES)
    lines.append(f"Qt | {qv} | LGPL-3.0-only (Qt is also offered under GPL-2.0-only and GPL-3.0-only) | {urls} | Qt/")
    lines.append("  The Qt libraries are linked dynamically: usr/lib/libQt6*.so.6, the plugins in usr/plugins and "
                 "the QML modules in usr/qml can be replaced by your own builds of the same Qt version "
                 "(the program finds them through RUNPATH and usr/bin/qt.conf).")
    lines.append("  Qt modules and plugins carried: " + ", ".join(sorted(modules)))
    lines.append("  Third-party code inside the carried Qt files (from Qt's SBOM): " +
                 "; ".join(f"{n} {v} ({l})" for (n, v), l in sorted(parts.items())))
    summary["qt"] = {"parts": {f"{n} {v}": l for (n, v), l in sorted(parts.items())},
                     "modules": sorted(modules), "files without SBOM entry": unknown,
                     "licences": sorted(qt_ids)}

    # ICU as Qt ships it
    icu = [so for so in dep["carried"] if so.startswith("libicu")]
    if icu:
        ver = re.sub(r"^.*\.so\.", "", os.path.realpath(dep["carried"][icu[0]]))
        shutil.copy2(os.path.join(a.texts, "ICU-73.2-LICENSE"), os.path.join(out, f"ICU-{ver}-LICENSE"))
        lines.append(f"ICU (built by The Qt Company, shipped with Qt) | {ver} | Unicode-DFS-2016 AND ICU (and the "
                     f"third-party terms in the file) | https://github.com/unicode-org/icu/releases/tag/release-"
                     f"{ver.replace('.', '-')} | ICU-{ver}-LICENSE")
        summary["other"]["ICU " + ver] = "Unicode-DFS-2016 AND ICU (+ third-party terms: see file)"

    # Inter
    inter = os.path.join(a.app_src, "assets", "Inter-LICENSE.txt")
    if os.path.isfile(inter):
        shutil.copy2(inter, os.path.join(out, "Inter-LICENSE.txt"))
        fonts = glob.glob(os.path.join(a.app_src, "assets", "Inter*.ttf"))
        ver = font_version(fonts[0]) if fonts else "?"
        lines.append(f"Inter (font, inside the program) | {ver} | OFL-1.1 | https://github.com/rsms/inter | Inter-LICENSE.txt")
        summary["other"]["Inter " + ver] = "OFL-1.1"
    else:
        missing.append("Inter-LICENSE.txt not in the app's assets")

    # Ubuntu 22.04 libraries
    udir = os.path.join(out, "ubuntu")
    os.makedirs(udir)
    by_pkg = {}
    for so, src in sorted(dep["carried"].items()):
        if os.path.realpath(src).startswith(qt + os.sep):
            continue
        pkg = dpkg_owner(src)
        if not pkg:
            missing.append(f"no package owns {so}")
            continue
        by_pkg.setdefault(pkg, []).append(so)
    for pkg, sos in sorted(by_pkg.items()):
        q = subprocess.run(["dpkg-query", "-W", "-f", "${Version}\t${source:Package}", pkg],
                           capture_output=True, text=True, check=True).stdout.split("\t")
        version, source = q[0], q[1] or pkg
        cfile = os.path.join("/usr/share/doc", pkg, "copyright")
        if os.path.isfile(cfile):
            shutil.copy2(os.path.realpath(cfile), os.path.join(udir, pkg + "-copyright"))
            lics = copyright_licenses(open(cfile, errors="replace").read())
        else:
            missing.append(f"{pkg}: no copyright file")
            lics = []
        lic = UBUNTU_MAIN.get(source) or (", ".join(l for l in lics if len(l.split()) <= 4) or "see the copyright file")
        if source not in UBUNTU_MAIN:
            missing.append(f"{pkg}: licence not reviewed (source package {source} not in UBUNTU_MAIN)")
        lines.append(f"{pkg} ({' '.join(sos)}) | {version} | {lic} | Ubuntu 22.04 archive (apt source {source}) | "
                     f"ubuntu/{pkg}-copyright")
        summary["ubuntu"][f"{pkg} {version}"] = {"library": lic, "copyright file": lics}

    # the AppImage runtime
    rdir = os.path.join(out, "appimage-runtime")
    os.makedirs(rdir)
    shutil.copy2(os.path.join(a.texts, "type2-runtime-LICENSE"), os.path.join(rdir, "type2-runtime-LICENSE"))
    lines.append(f"AppImage type-2 runtime (the start of the file) | continuous, commit {RUNTIME['commit'][:7]} | MIT | "
                 f"https://github.com/AppImage/type2-runtime | appimage-runtime/type2-runtime-LICENSE")
    summary["other"]["AppImage type-2 runtime"] = "MIT"
    for name, ver, lic, url, f in RUNTIME["parts"]:
        shutil.copy2(os.path.join(a.texts, f), os.path.join(rdir, f))
        lines.append(f"  linked into the runtime: {name} | {ver} | {lic} | {url} | appimage-runtime/{f}")
        summary["other"][f"runtime: {name} {ver}"] = lic

    head = ["Third-party components in Myrddin-x86_64.AppImage, and where their sources are.",
            "Fields: component | version | licence | source | licence text (relative to this folder).",
            "Made when the AppImage was built (appimage/packaging/licenses.py in Myrddin's repository).", ""]
    with open(os.path.join(out, "THIRD-PARTY-APPIMAGE.txt"), "w") as fh:
        fh.write("\n".join(head + lines) + "\n")
    summary["missing"] = missing
    with open(a.summary, "w") as fh:
        json.dump(summary, fh, indent=1)
    print(f"licenses: {len(lines)} lines, {len(by_pkg)} Ubuntu packages, {len(qt_ids)} Qt licence texts, "
          f"{len(missing)} missing" + (": " + "; ".join(missing) if missing else ""))


if __name__ == "__main__":
    main()
