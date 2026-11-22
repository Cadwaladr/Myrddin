# Quick start

This page begins where the [Install guide](install.md) ends: you have the AppImage, or Myrddin is
built from source with the layer in place.

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

Download `Myrddin-x86_64.AppImage` from the
[latest release](https://github.com/Cadwaladr/Myrddin/releases/latest), then:

```sh
chmod +x Myrddin-x86_64.AppImage
./Myrddin-x86_64.AppImage
```

The first start asks where the game is (it looks where the LUG Helper puts it) and checks what a
launch needs: the launch script, a runner with vkd3d-proton, the NVIDIA driver, the display.

The window has five pages:

- Play: the multiplier, the overlay and the launch button, and below them the session's frame
  rates while the game runs.
  - The multiplier is Off, 2×, 3×, 4× or Auto. 2×, 3× and 4× hold that multiplier; Auto follows
    the game's own rate, from 2× up to the highest multiplier set in Options. A change acts at
    once in a running game.
  - The overlay is MangoHud, Myrddin's own panel (Shift+7 in the game steps through it) or none.
  - "Run with logs" is the tab under the launch button. While the game runs, the button says
    "Stop" under the pointer; if the game is still there two seconds after a Stop, it offers
    "Kill".
- Console: what the game's process is doing (CPU, memory, threads), the GPU, and the end of the
  logs.
- Setup: the game's folder and launch script, the system check with its fixes, an entry in the
  application menu, and the launch as a command to copy.
- Options: the display and its refresh rate, the multiplier and the highest one Auto may pick,
  "Limit the game's own frame rate", the overlay with the panel's corner and key, how the HUD is
  treated in generated frames, the advanced performance tweaks, running with logs and the
  reports. Everything there is kept for the next launch; the multiplier, the limit on the game's
  frame rate and the HUD and performance switches also act at once in a running game.
- About: versions and licences.

With "Run with logs" on, the window offers the session's log once the game has closed: read it,
copy it, delete it or keep it. Your home folder's name is not in it.

## Built from source

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

The window's choices, as commands of the source folder (with the AppImage, see
[Commands through the AppImage](usage.md#commands-through-the-appimage)):

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
