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
> **Experimental and under active development.** Myrddin has been built and tested on exactly
> one machine, listed under [Tested configuration](#tested-configuration). Everything else is
> untested: a game patch, a driver update, another GPU or another desktop can break it.
>
> Star Citizen is protected by Easy Anti-Cheat. Myrddin does not modify the game, its files or
> its memory and sends it no input, but it is not affiliated with or endorsed by Cloud Imperium
> Games and comes with no guarantee of any kind. You use it at your own risk.

## What it does

**Frame generation.** `sc_fg` is a Vulkan layer, loaded the way MangoHud is. It takes the
motion vectors and depth the game hands to DLSS, runs AMD's FSR 3.1 frame interpolation
between each pair of the game's frames, and presents one to three generated frames in between.

- Multipliers ×2, ×3 and ×4, set at launch and changed while the game runs. The multiplier
  follows the game's rate: the fewest generated frames that fill the display.
- The HUD stays sharp. The game draws its HUD into a layer of its own; `sc_fg` captures that
  layer and puts it over every generated frame instead of interpolating it. A picture-based
  detection is the fallback.
- The game's motion vectors are checked against the picture, which cures thin objects that
  flicker because their vectors are wrong.
- Real and generated frames are spaced evenly and limited to the display's refresh rate.
- The shown frame rate, generated frames included, in MangoHud (`sc-turbo fg 3 hud`): the layer
  sits above it, so MangoHud counts every frame that goes out. The layer also has a panel of its
  own (the shown rate, the game's own rate, a pacing graph), off unless asked for at launch.
- Everything is switched from outside while the game runs, and frame dumps and an event
  record are there for diagnosis.

On the test machine, a heavy hangar scene that the game renders at 24 to 28 frames a second is
shown at 97 to 110 with ×4. Generated frames are interpolated: they add smoothness, not
responsiveness, and each real frame is shown a little later than it would be without them.

**A launcher window.** One entry per game with its icon: a click launches it, and `−  4x  +`
under it sets the multiplier, before the launch and while the game runs. Everything it does is
also a command of `sc-turbo`.

**Measurement.** A passive probe layer, comparisons of two settings within one session
(`sc-turbo fg-ab`, `sc-turbo threads-ab`), and a headless test chain that exercises the
frame-generation layer without the game.

## Tested configuration

This is the only configuration Myrddin has run on.

| | |
|---|---|
| Distribution | Fedora Linux 44 |
| Kernel | 7.2.15-200.fc44.x86_64 |
| Architecture | x86_64 |
| Desktop | KDE Plasma 6.8.2 (KWin 6.8.2-3), Wayland session; before that COSMIC 1.11.0, where most of the development took place |
| GPU and driver | NVIDIA GeForce RTX 4070 SUPER, 12 GB; driver 615.78.08 |
| CPU | Intel Core i5-13600K (6 P-cores with SMT, 8 E-cores) |
| Wine runner | the Star Citizen LUG's, unchanged: `lug-wine-tkg-experimental-wayland-git-11.22-1` and `-11.20-2` (Wine 11.22 and 11.20, Wayland driver) |
| Game | Star Citizen Alpha 4.10.3 (LIVE), Vulkan renderer, DLSS |
| Display | 2560×1440 at 143.99 Hz and at 120 Hz, no variable refresh |

## Requirements and limitations

- **NVIDIA GPUs only.** Frame generation reads what the game gives to DLSS, through
  NVIDIA-only Vulkan extensions (`VK_NVX_image_view_handle`, `VK_NVX_binary_import`). DLSS
  must be the upscaler selected in the game, which under Wine needs NVAPI (`nvapi64.dll` from
  dxvk-nvapi) and the driver's NGX libraries in the prefix; the LUG Helper can set both up.
  AMD and Intel GPUs are not supported. A fallback without the game's vectors exists
  (`SC_FG_FALLBACK=1`) and has never been tested.
- **One renderer, one kind of runner.** The game's Vulkan renderer under Wine with the Wayland
  driver, on the LUG's experimental Wayland runner as it comes. X11, Proton and other runners
  are untested. With two other LUG runners (`lug-wine-tkg-git-11.21-1`,
  `lug-wine-tkg-wayland-git-11.22-1`) the game closed while starting up; whether Myrddin has a
  part in that is not known yet.
- **Tied to the current game build.** Finding the HUD layer and matching frames to their
  inputs rely on how Alpha 4.10 builds a frame. A patch can break either; the layer then falls
  back or has to be switched off (`sc-turbo fg-off`).
- **The display's refresh rate** is asked of the desktop at launch on KDE Plasma and COSMIC
  (`sc-turbo displays` shows what was found). Elsewhere 120 Hz is assumed: set `SC_FG_TARGET`.
  The game has been played at 120 Hz and at 143.99 Hz; other rates and variable refresh are
  untested.
- **GPU memory.** The layer keeps buffers of its own, and their size has not been reduced yet.
  On the 12 GB test card the game runs close to the limit: switching the upscaler to native
  resolution in a running session exhausted it, turned generation off, and a driver hang
  followed. Leave headroom and do not change the upscaling mode during a session.
- **Known gaps.** Markers pinned to the world and the ship's flight display move at the game's
  frame rate, not the shown one. A first attempt is in the layer, off by default
  (`sc-turbo fg-params markers=1`); the version meant to replace it is being worked out in
  [`proto/hud-motion`](proto/hud-motion). In the pause menu, or while the view stands still,
  the generated rate can drop until the view moves; a fallback for that is in the source and
  not yet validated.
- **Desktop integration is for KDE Plasma and COSMIC.** Setting up the panel's key, naming the
  display the game should use and reading its refresh rate work there. Elsewhere, bind
  `sc-turbo fg-panel` to a key by hand and set `SC_MONITOR` and `SC_FG_TARGET`.
- **No installer, no packages.** x86_64 only.

## Building from source

Fedora package names; other distributions have the same tools under other names.

```sh
sudo dnf install clang gcc cmake ninja-build glslang python3 \
                 vulkan-headers vulkan-loader-devel wayland-devel wayland-protocols-devel

git clone https://github.com/Cadwaladr/Myrddin.git
cd Myrddin
./build.sh            # AMD's library (once), both layers, the test program
./build.sh install    # puts the new layer in place; not while the game runs
```

`build.sh` writes only into `build/`. The Wayland packages are needed for the test program
alone; the tests themselves (`tools/test-chain.sh`, `tools/fg-validate.sh`) run in a headless
Weston and need `weston`, `python3-numpy` and `python3-pillow`.

## Running

Myrddin starts the game through the launch script of your install, by default the LUG
Helper's `sc-launch.sh` in `~/Games/star-citizen`. If your game is somewhere else:

```sh
cp sc-turbo.conf.example sc-turbo.conf     # then set SC_PREFIX in it
```

In the game, select DLSS as the upscaler.

### With the launcher window

```sh
./sc-turbo gui            # open it
./sc-turbo gui install    # put it in the desktop's application menu ("Myrddin")
```

One entry per game. `−  4x  +` under the icon sets the multiplier (1x: no frame generation), and
a click on the icon launches the game with it. While the game runs the buttons act at once, and
the line under them says what the layer is doing: its multiplier at the moment, the shown frame
rate and the game's own. The games are listed in [`gui/games.ini`](gui/games.ini); the icon is
looked up on the machine (the LUG Helper installs one for Star Citizen), none is part of this
project. The window needs GTK 4, libadwaita and PyGObject (Fedora: `python3-gobject gtk4
libadwaita`).

### From a terminal

The window does nothing that these do not:

```sh
./sc-turbo fg 3                  # launch with frame generation, ×3 (2, 3 or 4)
./sc-turbo run                   # launch without it
./sc-turbo fg-mult 4             # change the multiplier while the game runs
./sc-turbo fg-off                # frames pass through untouched;  fg-on  brings it back
```

And more than the window has:

```sh
./sc-turbo fg 3 hud              # launch with MangoHud: its frame rate is the shown one
./sc-turbo fg 3 panel            # launch with the layer's own panel available; then:
./sc-turbo fg-panel              #   step the panel (off, rate, both rates, with the graph)
./sc-turbo fg-panel-key install  #   KDE Plasma, COSMIC: Ctrl+Shift+` steps the panel
./sc-turbo displays              # the displays, the one the game takes and its refresh rate
./sc-turbo fg-watch              # the session's log at a glance
./sc-turbo fg-where              # after a freeze: what each thread was doing inside the layer
./sc-turbo                       # every command
```

At every launch `sc-turbo` asks the desktop which display is the primary one and at what rate
it runs, names that display to the runner (left to itself, the LUG runner's Wayland driver takes
the display with the most pixels: a TV beside the monitor) and tells the layer the rate.
`SC_MONITOR` and `SC_FG_TARGET`, in the environment or in `sc-turbo.conf`, override both.

Logs and dumps go to `logs/` inside the project.

### If the game stands still or closes by itself

The layer keeps a record that outlives the game: the session's log (`logs/fg/fg-<pid>.log`) and,
for every thread that called into it, the hook it was in and the locks it held or waited for.

```sh
./sc-turbo fg-where     # the last session: where each thread was inside the layer
```

A line in the log that begins with `lock` means a thread waited more than a second for one of
the layer's locks, or that a lock left behind by a call that never returned was taken over.
Both, with the output of `fg-where`, are what a bug report needs. `./sc-turbo fg-off` (or
launching without `fg`) tells whether the layer is involved at all.

### If only some keys act in the game (KDE Plasma)

Seen on Plasma 6.8: W moved the character, A, S and D did not, while all of them typed into
the chat. The cause is not this project: Plasma's on-screen keyboard service offers accented
letters when a physical key is held, and it holds those keys back from any program that takes
text input, which Wine's Wayland driver always does. Letters with accents then arrive as text.
`sc-turbo` says so at launch when it finds the option on. To switch it off:

```sh
kwriteconfig6 --file plasmakeyboardrc --group General --key diacriticsPopupEnabled --type bool --notify false
```

### If the picture tears (KDE Plasma)

KWin allows fullscreen windows to tear by default. On Plasma 6.8 the game's picture tore,
generated frames included, until that was switched off (`sc-turbo` says so at launch):

```sh
kwriteconfig6 --file kwinrc --group Compositing --key AllowTearing --type bool false
busctl --user call org.kde.KWin /KWin org.kde.KWin reconfigure
```

## Repository

| Path | Contents |
|---|---|
| `fg/` | the frame-generation layer (`sc_fg.cpp`), its compute shaders and the panel |
| `gui/` | the launcher window (`myrddin-gui`, GTK 4 and libadwaita) and its list of games |
| `probe/` | the passive probe layer and the test programs, among them the two-device test chain |
| `tools/` | tests and validation, in-session comparisons, log readers, thread and display tools, the panel's key |
| `proto/hud-motion/` | reference prototype for moving world-pinned HUD content on generated frames |
| `third_party/` | AMD FidelityFX SDK 1.1.4 and the recipe that builds it for Linux |
| `sc-turbo`, `build.sh` | the launcher, and the build |

## Licence and third-party code

Myrddin is free software under the [GNU General Public License, version 3 or later](LICENSE).

The code, the tests and the documents in this repository were written by Claude, the AI model
made by [Anthropic](https://www.anthropic.com), working in Claude Code. Cadwaladr directed the
work, decided what it should do and tested it in the game. The parts that come from elsewhere
are listed below.

| Component | Licence |
|---|---|
| AMD FidelityFX SDK 1.1.4, with changes that yield several generated frames per pair | MIT (`third_party/ffx_src/FidelityFX-SDK/LICENSE.txt`) |
| Linux build recipe for the SDK and Vulkan test helpers, from the Lichii project | GPL-3.0-or-later |
| Adwaita Sans, the panel's letters, baked into `fg/hud_font.h` | SIL Open Font License 1.1 |

Star Citizen is a trademark of Cloud Imperium Games; FidelityFX and FSR are AMD's; DLSS is
NVIDIA's; Claude and Claude Code are Anthropic's. Myrddin is an independent project: none of
them is affiliated with it or endorses it. It owes its footing to the
[Star Citizen LUG](https://github.com/starcitizen-lug), whose runners and Helper it builds on.
