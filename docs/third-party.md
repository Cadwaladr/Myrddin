# Third-party code

Myrddin is free software under the [GNU General Public License, version 3 or later](../LICENSE).
`fg/hud_font.h`, which holds the panel's letters, is under the SIL Open Font License 1.1
instead, like the font it is drawn from.

The code, the tests and the documents in this repository were written by Claude, the AI model
made by [Anthropic](https://www.anthropic.com), working in Claude Code. Cadwaladr directed the
work, decided what it should do and tested it in the game.

## Where the approach came from

The frame generation from the game's motion vectors is our own implementation. The approach,
building AMD's FSR 3 frame interpolation from its sources on Linux and giving it a game's own
depth and motion vectors with a HUD-free picture, is one we found in a fork of the PS5 emulator
KytyPS5: [chenxiao07/KytyPS5](https://github.com/chenxiao07/KytyPS5), pull requests
[#117](https://github.com/chenxiao07/KytyPS5/pull/117) and
[#118](https://github.com/chenxiao07/KytyPS5/pull/118) by Bios2000. We used it as a guide and
took none of its code, which is GPL-2.0-only. Our changes to AMD's SDK that give several
generated frames per pair, and the recipe that builds the SDK on Linux, were first written for
our own modified build of Lichii, an emulator made to run Nintendo Switch 2 games, and carried
over.

## Components and their licences

| Component | Licence |
|---|---|
| AMD FidelityFX SDK 1.1.4, with our changes that yield several generated frames per pair | MIT, Copyright (C) 2024 Advanced Micro Devices, Inc. (`third_party/ffx_src/FidelityFX-SDK/LICENSE.txt`) |
| Our Linux build recipe for the SDK and our Vulkan test helpers, first written for our build of Lichii | GPL-3.0-or-later. The files carry the notice of the project they were written in: Copyright 2026 Eden Emulator Project |
| Adwaita Sans, a variation of Inter, drawn into `fg/hud_font.h` for the panel | SIL Open Font License 1.1 (copyright notices and licence text in `third_party/adwaita-sans/LICENSE`) |
| Inter, the font of the AppImage's window (`app/assets/InterVariable.ttf`) | SIL Open Font License 1.1, Copyright (c) 2016 The Inter Project Authors (`app/assets/Inter-LICENSE.txt`) |
| Qt 6, which the AppImage's window is built on (linked, not in this repository) | LGPL-3.0 |
| Mesa's Vulkan layer `VK_LAYER_MESA_vram_report_limit`, built unchanged by `tools/build-vram-layer.sh` (not in this repository; an AppImage carries it) | MIT, Copyright 2017 Google, 2019 Red Hat, 2024 Igalia S.L. |

An AppImage also carries Qt itself, ICU and a number of system libraries. Their licence texts
are inside it under `usr/share/licenses/myrddin`, with a list of every part, its version and
where its source is (`THIRD-PARTY-APPIMAGE.txt`; the About page opens the folder). Portions of
that software are copyright © The FreeType Project (www.freetype.org).

## Trademarks

Star Citizen is a trademark of Cloud Imperium Games. FidelityFX and FSR are trademarks of AMD,
DLSS of NVIDIA, Nintendo Switch of Nintendo, PS5 of Sony Interactive Entertainment, and Claude
and Claude Code of Anthropic. Myrddin is an independent project, and none of them is affiliated
with it or endorses it.

Thanks to the [Star Citizen LUG](https://github.com/starcitizen-lug), whose runners and Helper
Myrddin builds on.
