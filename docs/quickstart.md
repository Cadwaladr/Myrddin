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
After that the window has five pages:

- Play: the multiplier (Off, ×2, ×3, ×4) and the launch. While the game runs it shows the shown
  frame rate, the game's own and the frame time, and a change of multiplier acts at once.
- Setup: the game's folder and launch script, the system check with its fixes, an entry in the
  application menu, and a `myrddin` command for the terminal.
- Options: display and refresh rate, MangoHud, the panel with its corner and key, moving HUD
  elements, the CPU scheduler, running with logs, and the switches that act on a running game.
- Session: frame rates over the last fifteen minutes, the reports `sc-turbo` can make, and what
  the last launch printed.
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
./sc-turbo fg 3                  # launch with frame generation, ×3 (2, 3 or 4; the number comes first)
./sc-turbo run                   # launch without it
./sc-turbo fg-mult 4             # change the multiplier while the game runs
./sc-turbo fg-off                # frames pass through untouched; fg-on brings generation back
```

`./sc-turbo` alone lists every command, and [Commands and settings](usage.md) goes through the
ones worth knowing.

## Where to go from here

- [Commands and settings](usage.md): MangoHud, the layer's own panel, a session's log, `sc-turbo.conf`.
- [Optional extras](optional.md): what is not needed and may still help, with what we measured.
- [Limitations](limitations.md) and [Troubleshooting](troubleshooting.md) when something is not
  as this page says.
