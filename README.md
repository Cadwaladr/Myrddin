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
  layer, interpolates the picture without it and puts it over every generated frame, to the
  pixel. Now and then a frame comes without a capture: its picture without the HUD is then worked
  out from the frame and the last capture. A picture-based detection is the fallback.
- HUD content that moves over the screen (markers pinned to the world, the compass, a ship's
  flight display) moves at the shown rate too: the layer finds how each group of it moved
  between two of the game's frames and puts it where it is on its way in every generated frame.
  What changes where it stands, like the map at the top left, is left at the game's own rate.
  This costs about 4 % of the game's own frames where the GPU is the limit.
  `sc-turbo fg 3 nomarkers` launches without it, and `sc-turbo fg-params markers=0` switches
  it off in a running game.
- The game's motion vectors are checked against the picture, which cures thin objects that
  flicker because their vectors are wrong.
- Real and generated frames are spaced evenly and limited to the display's refresh rate.
- The shown frame rate, generated frames included, in MangoHud (`sc-turbo fg 3 hud`): the layer
  sits above it, so MangoHud counts every frame that goes out. The layer also has a panel of its
  own (the shown rate, the game's own rate, a pacing graph), off unless asked for at launch.
- Everything is switched from outside while the game runs, and frame dumps and an event
  record are there for diagnosis.

Frame rates on the test machine in the game's cities, on foot, with hangars and flights left out
(2560×1440 at 120 Hz, DLSS rendering at 1280×720, up to ×4; live servers, 2026-11-18). "Typical"
is the middle of the ten-second averages, "busy" the slowest tenth of them
(`tools/city-rates.py` makes this table from a session's logs):

| City | The game's own frames per second: typical (busy) | Shown with frame generation: typical (busy) |
|---|---|---|
| Orison | 27 (20) | 108 (82) |
| Lorville | 29 (25) | 111 (99) |
| Levski | 26 (22) | 102 (88) |

The table comes from three sessions in a row, and rates differ from session to session: in two
sessions earlier the same day Orison typically ran at 39 frames a second and showed 118.

Generated frames are interpolated: they add smoothness, not responsiveness, and each real frame
is shown a little later than it would be without them.

**A launcher window.** One entry per game with its icon: a click launches it, and `−  4x  +`
under it sets the multiplier, before the launch and while the game runs. Everything it does is
also a command of `sc-turbo`.

**A CPU scheduler for the game, optional.** The game is held back by its CPU in the cities.
`sc-turbo fg 3 sched=lavd` runs one of the kernel's sched_ext schedulers while the game runs and
puts the stock one back after it; see [A different CPU scheduler](#a-different-cpu-scheduler-optional).

**Measurement.** A passive probe layer, comparisons of two settings within one session
(`sc-turbo fg-ab`, `sc-turbo threads-ab`, `tools/sc-ab-sched.sh` for the scheduler), where the
time goes between a frame and the display (`sc-turbo latency`), and a headless test chain that
exercises the frame-generation layer without the game.

## Tested configuration

This is the only configuration Myrddin has run on.

| | |
|---|---|
| Distribution | Fedora Linux 44 |
| Kernel | 7.3.4-200.fc44.x86_64 (7.2.15 before 2026-11-19); with the optional CPU scheduler: scx-scheds 1.1.4, scx-tools 1.1.3 |
| Architecture | x86_64 |
| Desktop | KDE Plasma 6.8.3 (KWin 6.8.3-1; 6.8.2 before 2026-11-19), Wayland session; before that COSMIC 1.11.0, where most of the development took place |
| GPU and driver | NVIDIA GeForce RTX 4070 SUPER, 12 GB; driver 620.70.06 (615.78.08 before 2026-11-19) |
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
- **A runner that ships vkd3d-proton.** The Star Citizen LUG's "experimental" runners do
  (`lug-wine-tkg-experimental-wayland-git-…`), and they are the ones Myrddin works with. On
  them the game, though it renders with Vulkan, presents its finished frames through
  Direct3D 12, and that is the swapchain Myrddin puts the generated frames into. On a runner
  without vkd3d-proton (`lug-wine-tkg-git-11.21-1`, `lug-wine-tkg-wayland-git-11.22-1`) the
  game presents through a Vulkan swapchain of its own and does not survive a layer changing
  it: the game closed while starting up. Myrddin leaves that swapchain as it is, so on those
  runners the game runs without generated frames, and `sc-turbo` says so at launch. The game
  has only been played with Wine's Wayland driver; X11 and Proton are untested.
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
- **Known gaps.** Moving HUD content is placed group by group: things that touch are one
  group, which moves as a whole, by one smooth motion, or not at all. A small part that moves
  by itself inside a group (a pointer on a scale) is put where the group's motion takes it,
  and in a very fast turn part of a group can stay at its old place for a frame. In play a
  little flicker of the flight display is left now and then. The map at the top left is not
  moved: it updates at the game's own rate, because it turns inside its frame and moving it as
  a piece doubled its lines and tore its picture. A frame that comes without a HUD
  capture shows the HUD of the last capture in its generated frames, so what moves on the HUD
  (markers) stands still for that frame. Where many frames in a row, or every second one, come
  without (the game's menus), frames are interpolated as they are presented, HUD included, until
  captures come again. In the pause menu, or while
  the view stands still, the generated rate can drop until the view moves; a fallback for that
  is in the source and not yet validated.
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
./sc-turbo fg 3 nomarkers        # launch with moving HUD content left at the game's own rate
./sc-turbo fg 3 watch            # launch and record the game's disk, memory and GPU memory use
./sc-turbo fg 3 sched=lavd       # launch with the scx_lavd CPU scheduler while the game runs (see below)
./sc-turbo iowatch report        #   afterwards: that, frame rates and the game's log, place by place
./sc-turbo latency               # where the time goes between a frame and the display, per multiplier
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

### A different CPU scheduler (optional)

In the cities the game is held back by its CPU, not by the GPU: its main thread and its job
system keep most of the CPU's threads busy, and threads wait for a free one. Linux can run a
scheduler of its own choosing for that (sched_ext), and the [scx](https://github.com/sched-ext/scx)
project has several made for games. `sched=NAME` starts one for as long as the game runs:

```sh
./sc-turbo fg 3 sched=lavd           # scx_lavd in scx_loader's "gaming" mode
./sc-turbo fg 3 sched=bpfland:gaming # another scheduler, or another of its modes
./sc-turbo run                       # SC_SCHED applies here too
```

`SC_SCHED=lavd` in `sc-turbo.conf` does it at every launch (`sched=stock`: not this time). When
the game has ended the stock scheduler comes back (`logs/sched.out` says when). If anything goes
wrong with a sched_ext scheduler, the kernel throws it out and goes back to the stock one by
itself.

Measured on the test machine, in a city on foot, with frame generation off for the test and the
two schedulers switched back and forth in one session (25 seconds each, four times, twice):
scx_lavd gave about 3 % more of the game's own frames than the stock scheduler, and fewer long
frames (the 99th percentile of the frame time 2 to 7 ms shorter). scx_bpfland gave about the same
frame rate, with one long hitch. The frame rate varies by more than that from one stretch to the
next, so the gain is small and not certain; in play scx_lavd felt more responsive.

It needs:

- a kernel with sched_ext (Linux 6.12 or later; Fedora's has it);
- `scx-scheds` and `scx-tools` (the schedulers, `scx_loader` and `scxctl`; on Fedora 44 from the
  Terra repository);
- switching without a password. `scx_loader` asks for an administrator's password every time,
  and a dialog over the game will not do, so a polkit rule lets your own user, in your own
  active local session, start and stop sched_ext schedulers (nothing else):

  ```sh
  tools/sc-sched rule > /tmp/sc-sched.rules
  sudo install -m 644 /tmp/sc-sched.rules /etc/polkit-1/rules.d/49-sc-sched.rules
  ```

  Removing that file undoes it. Without the rule, `sched=` says so at launch and the game runs
  with the stock scheduler.

`tools/sc-sched state` shows what runs, `tools/sc-sched set stock` stops a sched_ext scheduler
by hand, and `tools/sc-ab-sched.sh lavd` compares one with the stock scheduler in a running game
(hold a steady view for about four minutes).

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
| `proto/hud-motion/` | the reference that the layer's passes for moving HUD content are checked against, and its test scenes |
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
