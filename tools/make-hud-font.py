#!/usr/bin/env python3
"""The performance panel's letters: the printable ASCII characters of a font, drawn once at the
sizes the panel uses, as a header the layer is built with (fg/hud_font.h). The layer has no
font library; it copies these pictures.

    tools/make-hud-font.py [FONT.ttf]     (default: Adwaita Sans, the desktop's own, SIL OFL 1.1)

Run again only to change the font or the sizes; the header is kept in the tree, so a build needs
neither this script nor the font. Needs Pillow.
"""
import os, sys
from PIL import Image, ImageDraw, ImageFont

here = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
path = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/fonts/adwaita-sans-fonts/AdwaitaSans-Regular.ttf"
# name, pixel size, weight. Sizes are for a 1440-line picture; the panel has them at two scales
# (1x up to 1800 lines, 1.5x above).
FACES = [("BIG", 25, 650), ("LABEL", 13, 700), ("SMALL", 12, 550)]
SCALES = [1.0, 1.5]
CHARS = [chr(c) for c in range(32, 127)] + ["×"] # (the multiplication sign: code 127 in the table)
WIDTH = 512


def face(size, weight):
    font = ImageFont.truetype(path, size)
    try:
        axes = font.get_variation_axes()
        font.set_variation_by_axes([weight if a["name"] in (b"Weight", "Weight") else a["default"] for a in axes])
    except OSError:
        pass # (not a variable font: the weight it has)
    return font


def main():
    glyphs, rows = [], []
    x = y = row_h = 0
    tables = []
    for scale in SCALES:
        for name, size, weight in FACES:
            font = face(round(size * scale), weight)
            ascent, descent = font.getmetrics()
            table = []
            for ch in CHARS:
                x0, y0, x1, y1 = font.getbbox(ch, anchor="ls")
                w, h = max(x1 - x0, 0), max(y1 - y0, 0)
                if x + w + 1 > WIDTH:
                    x, y, row_h = 0, y + row_h + 1, 0
                if w and h:
                    picture = Image.new("L", (w, h), 0)
                    ImageDraw.Draw(picture).text((-x0, -y0), ch, fill=255, font=font, anchor="ls")
                    glyphs.append((x, y, picture))
                table.append((x, y, w, h, x0, y0, font.getlength(ch)))
                x += w + 1
                row_h = max(row_h, h)
            digits = max(font.getlength(d) for d in "0123456789")
            tables.append((f"{name}_{str(scale).replace('.', '_')}", round(size * scale), ascent, descent, digits, table))
    height = y + row_h + 1
    atlas = Image.new("L", (WIDTH, height), 0)
    for gx, gy, picture in glyphs:
        atlas.paste(picture, (gx, gy))
    data = atlas.tobytes()
    out = [f"// The performance panel's letters. Made by tools/make-hud-font.py from {os.path.basename(path)}",
           "// (Adwaita Sans, SIL Open Font License 1.1); not edited by hand.",
           "#pragma once", "#include <cstdint>", "", "namespace hud_font {", "",
           "/// Where a character's picture is in ATLAS, where it goes from the pen (left, top: from the base",
           "/// line, top negative above it), and how far the pen moves on.",
           "struct Glyph {", "    uint16_t x, y;", "    uint8_t w, h;", "    int8_t left, top;", "    float advance;", "};",
           "/// One size of the font: its glyphs for the codes 32 to 127 (127: the multiplication sign); `digit`: the",
           "/// widest figure's advance (figures are set that far apart, so that numbers do not shift as they change).",
           "struct Face {", "    int size, ascent, descent;", "    float digit;", "    Glyph glyphs[96];", "};", "",
           f"inline constexpr int ATLAS_W = {WIDTH}, ATLAS_H = {height};", f"inline constexpr int SCALES = {len(SCALES)};",
           f"inline constexpr float SCALE[SCALES] = {{{', '.join(f'{s}f' for s in SCALES)}}};",
           f"enum {{ {', '.join(n for n, _, _ in FACES)}, FACES }};", "",
           "inline constexpr Face FACE[SCALES][FACES] = {"]
    i = 0
    for scale in SCALES:
        out.append("    {")
        for _ in FACES:
            name, size, ascent, descent, digits, table = tables[i]
            i += 1
            out.append(f"        {{{size}, {ascent}, {descent}, {digits:.3f}f, {{ // {name}")
            for k in range(0, len(table), 4):
                out.append("            " + " ".join(f"{{{gx}, {gy}, {w}, {h}, {l}, {t}, {a:.3f}f}}," for gx, gy, w, h, l, t, a in table[k:k + 4]))
            out.append("        }},")
        out.append("    },")
    out += ["};", "", "/// Coverage, 0 to 255, ATLAS_W x ATLAS_H.", "inline constexpr uint8_t ATLAS[ATLAS_W * ATLAS_H] = {"]
    for k in range(0, len(data), 32):
        out.append("    " + ",".join(str(b) for b in data[k:k + 32]) + ",")
    out += ["};", "", "} // namespace hud_font", ""]
    target = os.path.join(here, "fg", "hud_font.h")
    with open(target, "w") as f:
        f.write("\n".join(out))
    atlas.save(os.path.join(here, "build", "hud_font_atlas.png")) if os.path.isdir(os.path.join(here, "build")) else None
    print(f"{target}: {WIDTH}x{height} atlas, {len(tables)} faces, {os.path.getsize(target) // 1024} KB")


main()
