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
> Experimental, under active development, and tested on one setup only (see
> [Requirements](#requirements)). Star Citizen uses Easy Anti-Cheat. Myrddin does not modify the
> game, its files or its memory and sends it no input, but it is not affiliated with or endorsed
> by Cloud Imperium Games. Use it at your own risk.

## Quick start

You need an NVIDIA GPU and the game on a Wine runner with vkd3d-proton (see
[Requirements](#requirements)). In the game, set the renderer to Vulkan and the upscaler to DLSS.
Then build and launch:

```sh
git clone https://github.com/Cadwaladr/Myrddin.git
cd Myrddin
./build.sh install    # builds the layer and puts it in place
./sc-turbo fg 4       # launch with frame generation, up to 4×
```

`sc-turbo` starts the game with a copy of your launch script (by default the LUG Helper's, in
`~/Games/star-citizen`) and never changes the script itself. The packages the build needs and
the launcher window are in the [install guide](docs/install.md); the first launch is walked
through in the [quick start guide](docs/quickstart.md).

## Features

- 2×, 3× or 4× frame generation from the game's own motion vectors and depth (5× to 8× as an
  [experimental setting](docs/experimental-multipliers.md)): held at that multiplier, or
  following the game's frame rate up to it (Auto). It can be changed while the game runs.
- A sharp HUD: it is taken from the game's own HUD layer and put back over every generated
  frame. Markers, the compass and other HUD elements that move across the screen move at the
  shown rate.
- Even frame pacing, capped at the display's refresh rate.
- A launcher window with the settings, the session's frame rates and Stop and Kill buttons, and
  the `sc-turbo` command line for everything the window does.
- A frame rate overlay: MangoHud, or Myrddin's own panel (Shift+7 in the game).

## Frame rates

On the test machine, on foot in the cities on 2026-11-19: 2560×1440 at 120 Hz, DLSS rendering at
1280×720, frame generation up to 4×. Typical is the median of the ten-second averages, busy the
slowest tenth of them.

| City | The game's own frames per second, typical (busy) | Shown with frame generation, typical (busy) |
|---|---|---|
| New Babbage | 30 (26) | 116 (103) |
| Lorville | 36 (29) | 117 (110) |
| Orison | 29 (25) | 114 (92) |

Generated frames make motion smoother but delay every real frame: at 4×, with the game at 26 to
29 frames a second, a real frame reached the display 75 to 77 ms after the game handed its
inputs to DLSS. [Measurements](docs/measurements.md) has more sessions and where that time goes.

## Requirements

- An NVIDIA GPU, with Vulkan as the game's renderer and DLSS as its upscaler. AMD and Intel GPUs
  are not supported.
- Linux on x86_64, with the game under Wine on a runner that ships vkd3d-proton, such as the
  Star Citizen LUG's "experimental" runners.
- Tested on one setup: Fedora Linux 44, kernel 7.3.4, KDE Plasma 6.8.3 on Wayland, an RTX 4070
  SUPER with driver 620.70.06, game build 13220821. Known faults are listed in
  [Limitations](docs/limitations.md).

## Documentation

| Page | Contents |
|---|---|
| [Install guide](docs/install.md) | building from source, the launcher window, the AppImage |
| [Quick start guide](docs/quickstart.md) | the first launch, the launcher window's pages, the first commands |
| [Commands and settings](docs/usage.md) | every command, `sc-turbo.conf`, what the layer does in detail |
| [Limitations](docs/limitations.md) | requirements, known faults, the tested configuration |
| [Optional extras](docs/optional.md) | MangoHud, the graphics memory limit, optional packages |
| [Experimental multipliers](docs/experimental-multipliers.md) | 5× to 8×: when they help, what they cost, how to switch them on |
| [Advanced performance tweaking](docs/advanced-performance-tweaking.md) | A CPU scheduler, the CPU's prefetchers and huge pages: what they gave, how to switch them |
| [Troubleshooting](docs/troubleshooting.md) | a game that stands still, keys that do nothing, tearing |
| [Measurements](docs/measurements.md) | frame rates per city, latency, what limits the game's own frame rate |
| [Third-party code](docs/third-party.md) | where the approach came from, every component and its licence |

## License

Myrddin is free software under the [GNU General Public License, version 3 or later](LICENSE).
`fg/hud_font.h`, which holds the panel's letters, is under the SIL Open Font License 1.1.

The code, the tests and the documents were written by Claude, the AI model made by
[Anthropic](https://www.anthropic.com), working in Claude Code. Cadwaladr directed the work,
decided what it should do and tested it in the game.

The frame generation from the game's motion vectors is our own implementation. The approach was
found in a fork of the PS5 emulator KytyPS5,
[chenxiao07/KytyPS5](https://github.com/chenxiao07/KytyPS5) (pull requests
[#117](https://github.com/chenxiao07/KytyPS5/pull/117) and
[#118](https://github.com/chenxiao07/KytyPS5/pull/118) by Bios2000), and used as a guide; none of
its code was taken. [Third-party code](docs/third-party.md) has the details, every component and
its licence, and the trademarks.

Star Citizen is a trademark of Cloud Imperium Games, which is not affiliated with Myrddin and does
not endorse it. Thanks to the [Star Citizen LUG](https://github.com/starcitizen-lug), whose
runners and Helper Myrddin builds on.
