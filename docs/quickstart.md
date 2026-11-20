# Quick start

This page begins where the [Install guide](install.md) ends: the layer is built and in place, or
you have the AppImage.

## Before the first launch

- In the game's graphics settings, Vulkan is the renderer and DLSS the upscaler. Frame generation
  works from what the game gives DLSS.
- The game starts through your install's launch script, by default the LUG Helper's
  `sc-launch.sh` in `~/Games/star-citizen`. A game somewhere else is named once: on the AppImage's
  Setup page, or as `SC_PREFIX` in `sc-turbo.conf` ([Settings](usage.md#settings)). That script
  is never changed.
- The Wine runner is one of the Star Citizen LUG's "experimental" ones. `sc-turbo` says at launch
  when the runner, NVAPI or the driver's DLSS libraries are missing.

## With the AppImage

```sh
chmod +x Myrddin-x86_64.AppImage
./Myrddin-x86_64.AppImage              # the window
./Myrddin-x86_64.AppImage fg 4 hud     # or any command from the list below, without the window
```

The first start asks where the game is (it looks where the LUG Helper puts it) and checks what a
launch needs: the launch script, a runner with vkd3d-proton, the NVIDIA driver, the display.
After that the window has four pages:

- Play: the multiplier, the performance overlay and the launch, and below them the session's
  frame rates while the game runs. The multiplier is Off, 2×, 3×, 4× or Auto. 2×, 3× and 4× hold
  that multiplier whatever the game's frame rate. With Auto the layer picks the multiplier as the
  game's own rate changes, 2× at the least and no more than the highest multiplier set in Options
  (4× unless you change it). "Performance" is Auto's: on, the game's own frames are never
  limited, which is the fastest response; off (the default), they are limited so that every
  refresh of the display gets a frame, a smoother picture with more latency. A change acts at
  once in a running game. The overlay is MangoHud, Myrddin's own panel or none, and what is
  chosen is drawn with frame generation Off too; the panel is there from the start of the game,
  and Shift+7 in the game steps through it. "Run with logs" is the tab under the
  launch button. While the RSI Launcher or the game is up, the launch button says "Stop" under
  the pointer: a click asks the session to end. If something of it is still there two seconds
  later, the button offers "Kill", which ends it at once.
- Setup: the game's folder and launch script, the system check with its fixes (the runner, NVAPI
  in the prefix, whether the game is set to DLSS, the graphics driver, the display), an entry in
  the application menu, a
  `myrddin` command for the terminal, and the launch as it is set, as a command to copy.
- Options: display and refresh rate, the multiplier, the highest one Auto may pick and
  "Performance" with what it does, the
  performance overlay with the panel's corner and its key (click the key and press another to
  change it), how the HUD is treated in generated
  frames, the CPU scheduler, running with logs, and the reports `sc-turbo` can make. The
  multiplier and the HUD and performance switches are kept for the next launch and also act at
  once in a running game.
- About: versions and licences.

With "Run with logs" switched on (Play or Options), the window offers the session's log once the
game has closed: read it, copy it, delete it or keep it. The log is one text file with what the
launch said, the frame rates, and what the game used of the disk, memory and graphics memory, read
from outside the game. Your home folder's name is not in it.

The window is drawn in your desktop's own colours, its mark included: on KDE Plasma the colour
scheme's, elsewhere light or dark as the desktop is set, with its accent colour where it has one.

## From a checkout

```sh
./sc-turbo gui            # a small launcher window
./sc-turbo gui install    # put it in the desktop's application menu ("Myrddin")
```

This window has one entry per game. `−  4x  +` under the icon sets the multiplier (1x: no frame
generation), and a click on the icon launches the game with it. While the game runs the buttons
act at once, and the line under them shows what the layer is doing: its multiplier at the
moment, the shown frame rate and the game's own. The games are listed in
[`gui/games.ini`](../gui/games.ini). The icon is looked up on the machine (the LUG Helper installs
one for Star Citizen), and none is part of this project.

## From a terminal

```sh
./sc-turbo fg 4                  # launch with frame generation: the layer picks the multiplier, up to ×4
./sc-turbo fg 3 fixed            # launch with ×3, held there whatever the game's frame rate
./sc-turbo run                   # launch without it
./sc-turbo fg-mult 3 fixed       # in a running game: ×3, held ("fg-mult 4 auto": the layer picks again, up to ×4)
./sc-turbo fg-off                # frames pass through untouched; fg-on brings generation back
```

`./sc-turbo` alone lists every command, and [Commands and settings](usage.md) goes through the
ones worth knowing.

## Where to go from here

- [Commands and settings](usage.md): MangoHud, the layer's own panel, a session's log, `sc-turbo.conf`.
- [Optional extras](optional.md): what is not needed and may still help.
- [Advanced performance tweaking](advanced-performance-tweaking.md): a CPU scheduler, the CPU's
  prefetchers and huge pages: what each gave, how Myrddin switches them, and how to by hand.
- [Limitations](limitations.md) and [Troubleshooting](troubleshooting.md) when something is not
  as this page says.
