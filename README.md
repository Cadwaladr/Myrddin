# Myrddin

**Made by Claude, Anthropic's AI model, and directed by Cadwaladr.**

Myrddin gives you motion-vector-based frame generation for Star Citizen on Linux, without
touching the game's files, patching its code or memory, or injecting a DLL.

It works on the host side of Wine. The game hands its motion vectors and depth to DLSS through
Vulkan, and Wine passes those calls on to the host's Vulkan. Myrddin is a Vulkan layer that sits
there, like MangoHud, but it also copies the motion vectors, the depth and the game's HUD layer,
runs a modified FSR 3.1 frame interpolation on them, and presents the generated frames in the
game's window between the real ones, with the HUD put over them sharp. NVIDIA GPUs only, with
DLSS and Vulkan selected in the game.

> [!WARNING]
> Myrddin is experimental and under active development. It is built and tested on one hardware
> and software stack, listed under
> [Tested configuration](docs/limitations.md#tested-configuration). Anything else is untested,
> and a game patch, a driver update, another GPU or another desktop can break it.
>
> Star Citizen is protected by Easy Anti-Cheat. Myrddin does not modify the game, its files or
> its memory and sends it no input. It is not affiliated with or endorsed by Cloud Imperium Games
> and comes with no guarantee of any kind. You use it at your own risk.

## What it does

The layer, `sc_fg`, loads the way MangoHud does. Between each pair of the game's frames it runs
AMD's FSR 3.1 frame interpolation on the motion vectors and depth the game gives DLSS, and shows
one, two or three generated frames.

- You set the multiplier (×2, ×3 or ×4) at launch and can change it while the game runs. Up to
  that limit it follows the game's frame rate and never slows the game down to fit more
  generated frames in (`SC_FG_FILL=1`, the window's "Performance" switched off, limits the
  game's frames instead so that the display is filled evenly). If the game alone gets above
  92 % of the refresh rate, generation pauses until it drops below 80 %. With the word `fixed`
  (the launcher window's 2×, 3× and 4×) the multiplier is held instead, whatever the game's
  frame rate.
- The HUD stays sharp. The game draws its HUD into a layer of its own, and `sc_fg` captures it,
  interpolates the picture without it and puts the HUD back over every generated frame. A frame
  that arrives without a capture gets its HUD-free picture worked out from the frame and the
  last capture. A picture-based detection is the fallback.
- HUD elements that move across the screen (markers pinned to the world, the compass, a ship's
  flight display) move at the shown rate as well. The map at the top left stays at the game's
  own rate. In a headless test where the GPU was the limit this cost 3 to 4 % of the game's
  frames. `./sc-turbo fg 3 nomarkers` launches without it, and `./sc-turbo fg-params markers=0`
  turns it off in a running game.
- The game's motion vectors are checked against the picture. This stops thin objects from
  flickering where their vectors are wrong.
- Real and generated frames are spaced evenly and capped at the display's refresh rate.
- With `./sc-turbo fg 3 hud`, MangoHud sits below the layer and counts every frame that goes out.
  Keep MangoHud's own `fps_limit` off, or it holds the layer's frames back. The layer also has a
  small panel of its own (shown rate, the game's own rate, a pacing graph) when you ask for it
  at launch; Shift+7 in the game steps through it.

Around the layer there is a launcher window, an optional CPU scheduler for the time the game
runs, and tools for measuring: two settings compared within one session, the time from a frame
to the display, a recorder for the game's disk and memory use, and a headless test chain that
runs the layer without the game.

## Frame rates

Measured on the test machine in the game's cities, on foot, with hangars and flights left out:
2560×1440 at 120 Hz, DLSS rendering at 1280×720, frame generation up to ×4, live servers.
"Typical" is the median of the ten-second averages, "busy" the slowest tenth of them.

| City | Date | The game's own frames per second: typical (busy) | Shown with frame generation: typical (busy) |
|---|---|---|---|
| New Babbage | 2026-11-19 | 30 (26) | 116 (103) |
| Lorville | 2026-11-19 | 36 (29) | 117 (110) |
| Orison | 2026-11-19 | 29 (25) | 114 (92) |

Rates differ from session to session by more than most settings do. Generated frames make motion
smoother and delay every real frame: at ×4, with the game at 26 to 29 frames a second, a real
frame reached the display 75 to 77 ms after the game handed its inputs to DLSS.
[Measurements](docs/measurements.md) has the earlier sessions, Levski among them, how the
figures were taken and where that time goes.

## Quick start

There is no download yet, so the first step is a build. The [Install guide](docs/install.md) has
the packages it needs and the AppImage.

```sh
git clone https://github.com/Cadwaladr/Myrddin.git
cd Myrddin
./build.sh            # AMD's library (once), both layers, the test program
./build.sh install    # puts the new layer in place; not while the game runs
./sc-turbo fg 3       # launch with frame generation, up to ×3 (2, 3 or 4; "fg 3 fixed" holds it at ×3)
./sc-turbo fg-off     # in a running game: frames pass through untouched; fg-on brings generation back
```

In the game, select Vulkan as the renderer and DLSS as the upscaler. `sc-turbo` starts the game
through your install's launch script, by default the LUG Helper's in `~/Games/star-citizen`, and
never changes that script. [Quick start](docs/quickstart.md) goes through the first launch and
the launcher windows.

## Requirements in short

- An NVIDIA GPU, with DLSS as the upscaler and Vulkan as the renderer in the game. AMD and Intel
  GPUs are not supported.
- Linux on x86_64, with the game under Wine on a runner that ships vkd3d-proton. The Star Citizen
  LUG's "experimental" runners do.
- One tested configuration: Fedora Linux 44, kernel 7.3.4, KDE Plasma 6.8.3 on Wayland, an RTX
  4070 SUPER with driver 620.70.06, game build 13220821.

[Limitations](docs/limitations.md) has the full list, the known faults and the tested
configuration in detail.

## Documentation

| Page | What is in it |
|---|---|
| [Install guide](docs/install.md) | building from source, the AppImage, what each needs |
| [Quick start](docs/quickstart.md) | the first launch, the launcher windows, the commands you need first |
| [Commands and settings](docs/usage.md) | the other commands, `sc-turbo.conf`, what a launch places in the copy of your launch script |
| [Limitations](docs/limitations.md) | requirements, known faults, the tested configuration |
| [Optional extras](docs/optional.md) | optional dependencies and what each is for; the CPU scheduler and the hardware prefetchers, with what we measured and how to set them up |
| [Troubleshooting](docs/troubleshooting.md) | a game that stands still, keys that do not act, tearing |
| [Measurements](docs/measurements.md) | frame rates per city, latency, what limits the game's own frame rate |

## Licence and third-party code

Myrddin is free software under the [GNU General Public License, version 3 or later](LICENSE).
`fg/hud_font.h`, which holds the panel's letters, is under the SIL Open Font License 1.1
instead, like the font it is drawn from.

The code, the tests and the documents in this repository were written by Claude, the AI model
made by [Anthropic](https://www.anthropic.com), working in Claude Code. Cadwaladr directed the
work, decided what it should do and tested it in the game.

The frame generation from the game's motion vectors is our own implementation. The approach,
building AMD's FSR 3 frame interpolation from its sources on Linux and giving it a game's own
depth and motion vectors with a HUD-free picture, is one we found in a fork of the PS5 emulator
KytyPS5: [chenxiao07/KytyPS5](https://github.com/chenxiao07/KytyPS5), pull requests
[#117](https://github.com/chenxiao07/KytyPS5/pull/117) and
[#118](https://github.com/chenxiao07/KytyPS5/pull/118) by Bios2000. We used it as a guide and
took none of its code, which is GPL-2.0-only. Our changes to AMD's SDK that give several
generated frames per pair, and the recipe that builds the SDK on Linux, were first written for
our own modified build of Lichii, an emulator made to run Nintendo Switch 2 games, and carried
over. The parts that come from elsewhere are listed below, with the licence of what we carried
over.

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

Star Citizen is a trademark of Cloud Imperium Games. FidelityFX and FSR are trademarks of AMD,
DLSS of NVIDIA, Nintendo Switch of Nintendo, PS5 of Sony Interactive Entertainment, and Claude
and Claude Code of Anthropic. Myrddin is an independent project, and none of them is affiliated
with it or endorses it.

Thanks to the [Star Citizen LUG](https://github.com/starcitizen-lug), whose runners and Helper
Myrddin builds on.
