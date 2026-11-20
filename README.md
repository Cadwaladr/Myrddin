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
> and software stack, listed under [Tested configuration](#tested-configuration). Anything else is
> untested, and a game patch, a driver update, another GPU or another desktop can break it.
>
> Star Citizen is protected by Easy Anti-Cheat. Myrddin does not modify the game, its files or
> its memory and sends it no input. It is not affiliated with or endorsed by Cloud Imperium Games
> and comes with no guarantee of any kind. You use it at your own risk.

## What it does

The layer, `sc_fg`, loads the way MangoHud does. Between each pair of the game's frames it runs
AMD's FSR 3.1 frame interpolation on the motion vectors and depth the game gives DLSS, and shows
one, two or three generated frames.

- You set the multiplier (×2, ×3 or ×4) at launch and can change it while the game runs. Up to
  that limit it follows the game's frame rate and uses the fewest generated frames that fill the
  display. If the game alone gets above 92 % of the refresh rate, generation pauses until it
  drops below 80 %.
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
  at launch.

Around the layer there is a launcher window, an optional CPU scheduler for the time the game
runs, and tools for measuring: two settings compared within one session, the time from a frame
to the display, a recorder for the game's disk and memory use, and a headless test chain that
runs the layer without the game.

## Frame rates and latency

Measured on the test machine in the game's cities, on foot, with hangars and flights left out:
2560×1440 at 120 Hz, DLSS rendering at 1280×720, frame generation up to ×4, live servers.
"Typical" is the median of the ten-second averages, "busy" the slowest tenth of them. The memory
speed changed between the two days (see [The game's own frame rate](#the-games-own-frame-rate)).

| City | Memory, date | The game's own frames per second: typical (busy) | Shown with frame generation: typical (busy) |
|---|---|---|---|
| New Babbage | DDR4-2666, 2026-11-19 | 30 (26) | 116 (103) |
| Lorville | DDR4-2666, 2026-11-19 | 36 (29) | 117 (110) |
| Orison | DDR4-2666, 2026-11-19 | 29 (25) | 114 (92) |
| Lorville | DDR4-2133, 2026-11-18 | 29 (25) | 111 (99) |
| Orison | DDR4-2133, 2026-11-18 | 27 (20) | 108 (82) |
| Levski | DDR4-2133, 2026-11-18 | 26 (22) | 102 (88) |

Rates differ from session to session by more than most settings do. In two sessions earlier on
2026-11-18, Orison typically ran at 39 and showed 118.

While it generates frames, the layer holds the game to the refresh rate divided by the
multiplier whenever the game could go faster: 30 frames a second at ×4, 40 at ×3. In the
stretches above it did so in 39 to 87 % of the ten-second windows, so the typical figure for the
game's own rate is often that step, below what the game could do. Among the slowest tenth
of the windows almost none were held, except in Lorville, where about half were. With generation
off, at one spot in Orison, the game made 25.0 frames a second at DDR4-2133 and 30.8 at
DDR4-2666.

The shown rate counts the frames sent to the display. With tearing switched off in KWin, about
8 % of them were replaced before they reached the screen (×4, Lorville, 2026-11-18).
`tools/city-rates.py PID ...` makes a table like the one above from the logs of the sessions
you name.

Generated frames make motion smoother and delay every real frame. At ×4 in Lorville, with the
game at 26 to 29 frames a second (2026-11-18, DDR4-2133, tearing off), a real frame reached the
display 75 to 77 ms after the game handed its inputs to DLSS. Of that, 25 to 29 ms passed before
the game's present call reached the layer, about 8 ms went to generating, 26 to 29 ms was the
real frame waiting while the three generated ones went out ahead of it, and 11 to 15 ms was the
compositor. That wait is three quarters of a game frame at ×4, and any interpolation has it. At
×2, with the game near 60, the stages before the compositor came to 24 to 27 ms. The game's work
before DLSS and the monitor's response are not included. `./sc-turbo latency` prints these
figures for your last session, the compositor's part only after `./sc-turbo fg-params timing=1`
in the running game. With tearing allowed that part fell from 13.5 to 2.7 ms in a test.

## Tested configuration

This is the only configuration Myrddin has run on.

| | |
|---|---|
| Distribution | Fedora Linux 44 |
| Kernel | 7.3.4-200.fc44.x86_64 (7.2.15 before 2026-11-19). For the optional CPU scheduler: scx-scheds 1.1.4, scx-tools 1.1.3 |
| Architecture | x86_64 |
| Desktop | KDE Plasma 6.8.3 (KWin 6.8.3; 6.8.2 before 2026-11-19), Wayland session. Before 2026-11-14 COSMIC 1.11.0, where most of the development took place |
| GPU and driver | NVIDIA GeForce RTX 4070 SUPER, 12 GB; driver 620.70.06 (615.78.08 before 2026-11-19) |
| CPU | Intel Core i5-13600K (6 P-cores with SMT, 8 E-cores) |
| Memory | 64 GB DDR4 in two kits (2 × 16 GB rated 3200, 2 × 16 GB rated 2666), at 2666 MT/s since 2026-11-19, 2133 before |
| Wine runner | the Star Citizen LUG's, unchanged: `lug-wine-tkg-experimental-wayland-git-11.22-1` and `-11.20-2` (Wine 11.22 and 11.20, Wayland driver) |
| Game | Star Citizen Alpha 4.10 on the LIVE servers, Vulkan renderer, DLSS: build 13220821 (version 4.10.201.48085) since the patch of 2026-11-19; before it build 13165946 (version 4.10.200.58746), on which the figures here were measured |
| Display | 2560×1440 at 143.99 Hz and at 120 Hz, no variable refresh |

## Requirements and limitations

- An NVIDIA GPU. Frame generation reads what the game gives DLSS through NVIDIA-only Vulkan
  extensions (`VK_NVX_image_view_handle`, `VK_NVX_binary_import`), so DLSS must be the upscaler
  selected in the game. Under Wine that needs NVAPI (`nvapi64.dll` from dxvk-nvapi) and the
  driver's NGX libraries in the prefix: the LUG Helper installs the first, the LUG's runners link
  in the second, and `sc-turbo fg` says before the launch when either is missing. AMD and Intel
  GPUs are not supported. A fallback without the game's vectors exists (`SC_FG_FALLBACK=1`) and has
  never been tested.
- Linux, with the game running under Wine. The layer works on the host side of Wine and relies
  on Wine passing the game's Vulkan calls through to the host. The game has only been played
  with Wine's Wayland driver. X11 and Proton are untested.
- A runner that ships vkd3d-proton. The Star Citizen LUG's "experimental" runners
  (`lug-wine-tkg-experimental-wayland-git-…`) do. On them the game renders with Vulkan but
  presents its finished frames through Direct3D 12, and that is the swapchain Myrddin puts the
  generated frames into. On a runner without vkd3d-proton (`lug-wine-tkg-git-11.21-1`,
  `lug-wine-tkg-wayland-git-11.22-1`) the game presents through a Vulkan swapchain of its own,
  and it closed while starting up when a layer changed that swapchain. Myrddin leaves it alone,
  so on those runners the game runs without generated frames, and `sc-turbo` says so at launch.
- The current game build. Finding the HUD layer and matching frames to their inputs depend on
  how Alpha 4.10 builds a frame. A patch can break either. The layer then falls back, or you
  switch it off with `./sc-turbo fg-off`.
- The display's refresh rate is read from the desktop at launch on KDE Plasma and COSMIC
  (`./sc-turbo displays` shows what was found). Elsewhere 120 Hz is assumed unless you set
  `SC_FG_TARGET`. The game has been played at 120 Hz and 143.99 Hz. Other rates and variable
  refresh are untested.
- GPU memory. The layer keeps buffers of its own, and their size has not been reduced yet. On
  the 12 GB test card the game runs close to the limit. Switching the upscaler to native
  resolution in a running session exhausted it, turned generation off and was followed by a
  driver hang. Leave headroom, and do not change the upscaling mode during a session.
- Desktop integration (the panel's key, naming the display the game should use, reading its
  refresh rate) is written for KDE Plasma and COSMIC. Elsewhere, bind `./sc-turbo fg-panel` to a
  key yourself and set `SC_MONITOR` and `SC_FG_TARGET`.
- No installer and no packages yet. x86_64 only.

## Known faults

- Moving HUD elements are placed group by group, and things that touch form one group. A small
  part that moves inside a group, such as a pointer on a scale, goes where the group goes, and in
  a very fast turn part of a group can stay at its old place for a frame. A little flicker of
  the flight display is left now and then.
- The map at the top left updates at the game's own rate: it turns inside its frame, and moving
  it as one piece doubled its lines and tore its picture. A shop's purchase window has also been
  seen at the game's own rate.
- A frame that comes without a HUD capture shows the HUD of the last capture, so moving markers
  stand still for that frame. Where captures are missing for many frames in a row (the game's
  menus), frames are interpolated as presented, HUD included, until captures come back.
- In the pause menu, or while the view stands still, the generated rate can drop until the view
  moves.
- 3 of 24 launches on 2026-11-18 and 19 froze in the game's front end within the first 20
  seconds, with the layer loaded. The layer's record showed no thread inside it. It is not known
  whether this happens without the layer.

## Building from source

Fedora package names. Other distributions have the same tools under other names.

```sh
sudo dnf install clang gcc cmake ninja-build glslang python3 \
                 vulkan-headers vulkan-loader-devel wayland-devel wayland-protocols-devel

git clone https://github.com/Cadwaladr/Myrddin.git
cd Myrddin
./build.sh            # AMD's library (once), both layers, the test program
./build.sh install    # puts the new layer in place; not while the game runs
```

`build.sh` writes into `build/`, and regenerates a shader's header in `fg/` when you have changed
its source. The Wayland packages are only needed for the test program. The tests
(`tools/test-chain.sh`, `tools/fg-validate.sh`) run in a headless Weston and need `weston`,
`python3-numpy` and `python3-pillow`.

The window of the AppImage is a Qt 6 program in [`app/`](app/README.md) (Qt 6.5 or later: `sudo
dnf install qt6-qtbase-devel qt6-qtdeclarative-devel qt6-qtsvg-devel`):

```sh
cmake -S app -B build/app -G Ninja && cmake --build build/app
build/app/myrddin --runtime .    # the window, on the sc-turbo of this checkout
```

The AppImage itself is built in containers, with podman, by the scripts in
[`appimage/`](appimage/README.md): the layer once more so that it loads on any distribution,
the window with Qt's official libraries, then the image and its tests.

## Running

### With the AppImage

`Myrddin-x86_64.AppImage` is one file with a window, `sc-turbo`, its tools and the layer. There
is no download yet: build it as described [above](#building-from-source).

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
  elements, the CPU scheduler, recording, and the switches that act on a running game.
- Session: frame rates over the last fifteen minutes, the reports `sc-turbo` can make, and what
  the last launch printed.
- About: versions and licences.

Besides what [Requirements and limitations](#requirements-and-limitations) lists, the AppImage
needs:

- x86-64 and glibc 2.35 or newer (Ubuntu 22.04, Debian 12, Fedora 36 and later).
- FUSE 3 to mount itself: `/dev/fuse` and `fusermount3`, package `fuse3`. Without it,
  `./Myrddin-x86_64.AppImage --appimage-extract-and-run` unpacks about 110 MB into `/tmp` and
  starts from there; the copy stays until `/tmp` is cleared.
- A desktop's usual libraries, on Wayland or X11: libglvnd (OpenGL, EGL), libX11 and libxcb,
  libwayland-client, fontconfig, FreeType, and the GPU driver's OpenGL. Qt, the window's font and
  everything else are inside.
- `bash` and `python3` for `sc-turbo` and its tools, as from a checkout.

The window does not talk to the game. It writes `sc-turbo.conf`, runs `sc-turbo` and reads the
layer's log. At its first start it copies `sc-turbo`, the tools and the layer to
`~/.local/share/myrddin/runtime`: the layer is loaded into the game, the game can outlive the
window, and the AppImage's own files are gone when it closes. Logs and `sc-turbo.conf` stay
there across updates, and a newer AppImage replaces the rest only while the game and the RSI
Launcher are closed. The window's own choices are in `~/.config/myrddin/myrddin.conf`. It has
been tested on the machine under [Tested configuration](#tested-configuration), and in Ubuntu
22.04, Debian 12, Fedora and Arch containers without a game.

### From a checkout

```sh
./sc-turbo gui            # a small launcher window
./sc-turbo gui install    # put it in the desktop's application menu ("Myrddin")
```

This window has one entry per game. `−  4x  +` under the icon sets the multiplier (1x: no frame
generation), and a click on the icon launches the game with it. While the game runs the buttons
act at once, and the line under them shows what the layer is doing: its multiplier at the
moment, the shown frame rate and the game's own. The games are listed in
[`gui/games.ini`](gui/games.ini). The icon is looked up on the machine (the LUG Helper installs
one for Star Citizen), and none is part of this project. It needs GTK 4, libadwaita and
PyGObject (Fedora: `python3-gobject gtk4 libadwaita`).

Both windows and the commands below start the game through your install's launch script, by
default the LUG Helper's `sc-launch.sh` in `~/Games/star-citizen`. If your game is somewhere
else, name its folder on the AppImage's Setup page or set `SC_PREFIX` (see
[Settings](#settings)). In the game, select DLSS as the upscaler.

Your launch script is never changed. At every launch `sc-turbo` writes a copy of it to
`logs/launch/` and starts that copy, with its own settings placed just before the line that
starts the game: whatever `SC_LAUNCH_SET` and `SC_RUNNER` name (see [Settings](#settings)), and
the NVIDIA settings of the LUG wiki's
[Nvidia page](https://wiki.starcitizen-lug.org/Troubleshooting/nvidia), each only where your
script or your environment has not set it:

- The driver's shader cache as the LUG's script has it: on, 10 GiB, not cleaned at start.
- "DLSS 4": NVIDIA's updater, which fetches the current DLSS from NVIDIA into the prefix, and
  dxvk-nvapi's override that makes the game use it, with its newest preset, in place of the DLSS
  the game came with. That is for the picture, not for the frame rate.
  `SC_DLSS_OVERRIDE=0` leaves it out.
- A limit on the graphics memory the game is told about, the wiki's remedy for frame drops when
  the card's memory runs out: 9216 MiB on a 12 GB card, 8192 on 10, 6144 on 8, 4096 on 6, 2048
  on 4, and larger cards are left alone. It works through Mesa's Vulkan layer
  `VK_LAYER_MESA_vram_report_limit` (package `vulkan-mesa-layers` or similar). Fedora has no
  package for it, and `tools/build-vram-layer.sh` builds it into `build/vram-limit` there.
  `SC_VRAM_LIMIT=off` leaves it out.

A script that finds its files from its own location is started as it is, and so is any script
with `SC_LAUNCH_COPY=0`.

### From a terminal

These do what the windows do:

```sh
./sc-turbo fg 3                  # launch with frame generation, ×3 (2, 3 or 4; the number comes first)
./sc-turbo run                   # launch without it
./sc-turbo fg-mult 4             # change the multiplier while the game runs
./sc-turbo fg-off                # frames pass through untouched; fg-on brings generation back
```

These go further:

```sh
./sc-turbo fg 3 hud              # launch with MangoHud: its frame rate counts the generated frames
./sc-turbo fg 3 nomarkers        # launch with moving HUD elements left at the game's own rate
./sc-turbo fg 3 watch            # launch and record the game's disk, memory and GPU memory use
./sc-turbo iowatch report        #   afterwards: that, frame rates and the game's log, place by place
./sc-turbo fg 3 sched=lavd       # launch with the scx_lavd CPU scheduler (see below)
./sc-turbo latency               # where the time goes between a frame and the display, per multiplier
./sc-turbo fg 3 panel            # launch with the layer's own panel available; then:
./sc-turbo fg-panel              #   step the panel (off, rate, both rates, with the graph)
./sc-turbo fg-panel-key install  #   KDE Plasma, COSMIC: Ctrl+Shift+` steps the panel
./sc-turbo displays              # the displays, the one the game takes and its refresh rate
./sc-turbo fg-watch              # the session's log at a glance
./sc-turbo fg-where              # after a freeze: what each thread was doing inside the layer
./sc-turbo                       # every command, with the switches for tuning and diagnosis
```

At every launch `sc-turbo` asks the desktop which display is the primary one and at what rate it
runs. It names that display to the runner, whose Wayland driver otherwise takes the display with
the most pixels (a TV beside the monitor, for instance), and tells the layer the rate. Logs and
dumps go to `logs/` inside the project.

### Settings

`sc-turbo` reads `sc-turbo.conf` beside it, which is not tracked by git. Start from the example.
Every line is optional, and the same names in the environment take precedence:

```sh
cp sc-turbo.conf.example sc-turbo.conf
```

- `SC_PREFIX`: the Wine prefix the game is installed in (default `~/Games/star-citizen`)
- `SC_LAUNCH`: the script that starts the game (default `sc-launch.sh` in the prefix)
- `SC_LAUNCH_SET="NAME=value NAME=value"`: environment settings of your own for the game, placed
  in the copy of the launch script, where they take precedence over the script's
- `SC_RUNNER`: the folder of another Wine runner to start the game with (the one holding
  `bin/wine`)
- `SC_NVIDIA_FLAGS=0`: leave every NVIDIA setting out of the copy
- `SC_DLSS_OVERRIDE=0`: leave the DLSS 4 settings out
- `SC_DLSS_INDICATOR=1`: DLSS's own indicator in the game for one launch (its version and
  preset); the launch after it switches it off again. It is a check, to be read while standing
  still: with frame generation the text smears when the picture moves
- `SC_VRAM_LIMIT=off`, or a number of MiB: no limit on the graphics memory the game is told
  about, or a limit of your own
- `SC_LAUNCH_COPY=0`: start the launch script itself, with nothing placed
- `SC_MONITOR`, `SC_FG_TARGET`: the display the game should use and its refresh rate, where the
  desktop cannot be asked
- `SC_MANGOHUD=1`: MangoHud at every launch with frame generation
- `SC_SCHED=lavd`: a sched_ext scheduler at every launch
- `SC_FG_MARKERS=0`: never move HUD elements

## CPU scheduler (optional)

Myrddin can run the game under a different CPU scheduler and put the kernel's own back
afterwards. This is optional. Everything else works without it, it is off until you ask for it,
and Myrddin neither installs anything for it nor ever asks for a password.

### What it is

The CPU scheduler is the part of the kernel that decides which thread runs on which core, and for
how long. Linux's own is called EEVDF. Since Linux 6.12 the kernel can also take a scheduler as a
small program that is loaded while the system runs and removed again without a reboot: that
mechanism is sched_ext, and the [scx](https://github.com/sched-ext/scx) project publishes
schedulers for it. Myrddin has been tested with `scx_lavd`, which was written with games in mind:
it works out which threads others are waiting for and lets those run first. Star Citizen runs
close to two hundred threads and keeps some fourteen cores busy on the test machine, so the order
they run in is not a detail.

### What Myrddin does by itself

Once you have chosen a scheduler (in the window: Options, CPU scheduler; from a terminal:
`sched=lavd` on the command line, or `SC_SCHED=lavd` in `sc-turbo.conf` for every launch), each
launch goes like this:

1. `sc-turbo` asks the system whether your user may switch schedulers without a password. If not,
   it says so and starts the game with the kernel's own scheduler. No dialog appears.
2. If so, it starts the scheduler, in `scx_loader`'s "gaming" mode, and then the game.
3. A small watcher waits for the game, for 30 minutes at most. When the game has ended, it puts
   the kernel's own scheduler back. While the RSI Launcher stays open it waits ten minutes first,
   because a game started again from the launcher's own button does not come through `sc-turbo`.
4. If something else changed the scheduler in the meantime, the watcher leaves it alone, and only
   the newest launch's watcher acts.

`logs/sched.out` says what was done and when. Two things hold whatever Myrddin does: a sched_ext
scheduler that misbehaves is removed by the kernel itself, which goes back to its own, and
nothing Myrddin starts outlasts a reboot.

```sh
./sc-turbo fg 3 sched=lavd           # scx_lavd for this launch
./sc-turbo fg 3 sched=bpfland:gaming # another scheduler, or another of its modes
./sc-turbo fg 3 sched=stock          # the kernel's own for this launch, whatever sc-turbo.conf says
./sc-turbo run                       # SC_SCHED applies here too
```

### What we measured

All on the machine under [Tested configuration](#tested-configuration), with frame generation
off, the two schedulers switched back and forth inside one session.

| | Kernel's own | scx_lavd | |
|---|---|---|---|
| City, on foot, run 1: frames a second | 21.6 | 22.6 | +4.6 % |
| 99th-percentile frame time | 77.4 ms | 70.7 ms | |
| City, on foot, run 2 (order reversed): frames a second | 22.9 | 23.3 | +1.9 % |
| 99th-percentile frame time | 61.3 ms | 59.4 ms | |
| Orison, one spot, a later session: frames a second | 23.6 and 25.9 | 25.4 | within the drift |

In the two city runs (25 seconds a side, four rounds each) scx_lavd was ahead in seven of eight
pairs, by 3.3 % on average. Stretches of the same scheduler differed by 5 to 10 % in those runs,
so the frame rate alone does not prove a gain. The slow frames were shorter in both runs, and the
tester found the game more responsive with it and has played with it since: in the table under
[Frame rates and latency](#frame-rates-and-latency) the rows dated 2026-11-19 were measured with
scx_lavd, those dated 2026-11-18 with the kernel's own.
`scx_bpfland` was tried once: 3.9 % more frames in that run, and a longest frame of 168 ms against
129 ms.

In short: a small gain at best, mostly in how even the frames are. Try it if you are curious, and
leave it out without missing anything.

### Setting it up

It needs a kernel with sched_ext (Linux 6.12 or later; Fedora's has it) and the packages
`scx-scheds` and `scx-tools` (on Fedora 44 from the Terra repository). It also needs your user to
be allowed to switch schedulers without a password: `scx_loader`, the service that loads them,
asks for an administrator's password at every switch otherwise, and a password dialog over a
starting game will not do. Myrddin does not arrange that for you. If you want the option, you
allow it once yourself, with a polkit rule that lets your own user, in your own active local
session, start and stop sched_ext schedulers and nothing else:

```sh
tools/sc-sched rule > /tmp/sc-sched.rules      # prints the rule; read it
sudo install -m 644 /tmp/sc-sched.rules /etc/polkit-1/rules.d/49-sc-sched.rules
```

Removing that file undoes it. Without the rule the option does nothing: `sc-turbo` says so at
launch and the game runs with the kernel's own scheduler.

`tools/sc-sched state` shows what runs, `tools/sc-sched set stock` stops a sched_ext scheduler by
hand, and `tools/sc-ab-sched.sh lavd` compares one with the kernel's own in a running game (hold
a steady view for about four minutes).

## Hardware prefetchers (optional)

On the test machine the game ran 4 to 7 % faster with the CPU's hardware prefetchers switched
off. This is optional, and everything else works without it. Switching them needs root, so
Myrddin does not do it: this section says what we measured and how you can try it yourself.

### What they are

A prefetcher is a part of the CPU that watches which memory a program reads and fetches what it
expects to be read next, before the program asks for it. A program that goes through memory in
order gains a great deal from that. A program that jumps about in memory gains little, and what
was fetched in vain still takes its share of the memory's bandwidth and pushes other data out of
the caches. Star Citizen is limited by memory on the test machine (see [The game's own frame
rate](#the-games-own-frame-rate)), which is why we tried it. Intel's CPUs have a register,
`0x1A4`, in which every prefetcher has a bit that switches it off. Some BIOS set-ups show the
same switches, under names such as "Hardware Prefetcher" and "Adjacent Cache Line Prefetch".

### What Myrddin does by itself

Nothing. Writing that register needs root, and Myrddin never asks for a password. `sc-turbo`, the
window and the AppImage neither read the register nor change it.

### What we measured

On the machine under [Tested configuration](#tested-configuration), on 2026-11-19 with game build
13220821, frame generation off, standing still. All prefetchers were switched off and on again
inside one running session: ten seconds off, ten seconds on, twelve rounds at each spot. Every
ten seconds off are compared with the ten seconds on before and after them.

| | Prefetchers on | All off | | Rounds ahead |
|---|---|---|---|---|
| Orison, one spot: frames a second | 30.0 | 31.3 | +4.5 % | 12 of 12 |
| Median and 99th-percentile frame time | 33.2 and 40.7 ms | 32.4 and 40.7 ms | | |
| A second, slower spot: frames a second | 26.7 | 28.5 | +7.4 % | 11 of 12 |
| Median and 99th-percentile frame time | 36.3 and 65.4 ms | 33.7 and 61.3 ms | | |

Compared in the same way with itself, "on" came out at +0.05 % and -0.25 %. The gain is small,
and it was there in 23 of 24 rounds. What these figures do not tell:

- Which prefetchers matter. Only all of them off together showed a gain. In a first pass of
  three rounds each, the adjacent-line prefetcher alone (-0.3 %), the two of the second-level
  cache alone (-1.9 %) and the two of the first-level cache alone (+0.3 %) changed nothing that
  stands out from a spread of 4 %. The fifth, which Intel calls AMP, was not tried alone. So we
  cannot say whether the switches a BIOS offers are enough.
- How large the gain is at the Orison spot. With the prefetchers on, the game stood at 30.0
  frames a second there in most ten-second stretches. We have not found what holds it at that
  figure, and the gain may be cut short by it.
- What it does to loading times and to other programs. Programs that read memory in order
  (copying, compressing, compiling) usually get slower without prefetchers. We have measured
  neither, so treat this as a setting for the time you play, not one to keep for good.
- Anything about other machines. This is one CPU with memory at 2666 MT/s. With another CPU or
  faster memory the result may differ, in either direction.

With frame generation the gain shows only where the game runs below the rate the layer holds it
at (the display's rate divided by the multiplier: 30 at 4× on 120 Hz). The kernel's preemption
model was tried in the same way, "full" instead of "lazy": +0.8 %, which is nothing.

In short: a few per cent where the game is slowest, bought with a setting that may slow other
programs down. Try it if you are curious, and leave it out without missing much.

### Trying it

The values below are for Intel's Core CPUs of the 12th to 14th generation (Alder Lake and Raptor
Lake), which is what we tested. Other Intel CPUs use the same register with other bits, and AMD's
use other registers, which we have not looked at: there, leave it alone or look in the BIOS. You
need the package `msr-tools` (its two commands `rdmsr` and `wrmsr`) and a kernel that lets root
write such registers. With Secure Boot most kernels refuse: `cat /sys/kernel/security/lockdown`
must show `[none]`.

```sh
sudo modprobe msr                 # the kernel's access to such registers (Fedora has it built in)
sudo rdmsr -a 0x1a4               # what is set now, one line per CPU: normally 0 everywhere. Note it
cat /sys/devices/cpu_core/cpus    # the numbers of your P-cores' CPUs: 0-11 on the test machine
cat /sys/devices/cpu_atom/cpus    # those of your E-cores: 12-19 on the test machine

# all off: 2f on the P-cores, 2d on the E-cores (they have one prefetcher fewer). Use your numbers
for c in $(seq 0 11);  do sudo wrmsr -p $c 0x1a4 0x2f; done
for c in $(seq 12 19); do sudo wrmsr -p $c 0x1a4 0x2d; done

# all on again (if rdmsr showed something other than 0 before, write that value instead)
sudo wrmsr -a 0x1a4 0
```

On a CPU of those generations without E-cores the two `cat` commands find nothing, and one line
does it: `sudo wrmsr -a 0x1a4 0x2f`.

It takes effect at once and can be switched while the game runs. It is a setting of the CPU, the
same one a BIOS switch sets, and touches nothing of the game. A reboot puts everything back; we
have not checked whether a suspend does. At the first such write the kernel puts a note in its
log and marks itself "tainted" (flag S) until the next boot. That mark is there for people who
read kernel bug reports and changes nothing else.

We wrote the register with our own measuring tool, not with `wrmsr`: the values are the ones
tested, the two commands above are the standard way to write them. To repeat the measurement,
read `tools/base-ab-root.py` (the part that needs root: it switches on request and puts
everything back when it ends) and `tools/base-ab.py` (which reads the layer's frame-rate lines
and asks for the switches in time with them); both say at their top how they are run.

## Troubleshooting

### The game stands still or closes by itself

The layer keeps a record that outlives the game: the session's log (`logs/fg/fg-<pid>.log`)
and, for every thread that called into it, the hook it was in and the locks it held or waited
for.

```sh
./sc-turbo fg-where     # the last session: where each thread was inside the layer
```

A line in the log that begins with `lock` means a thread waited more than a second for one of
the layer's locks, or that a lock left behind by a call that never returned was taken over.
Both, with the output of `fg-where`, are what a bug report needs. `./sc-turbo fg-off`, or a
launch without `fg`, tells you whether the layer is involved at all.

### Only some keys act in the game (KDE Plasma)

Seen on Plasma 6.8: W moved the character and A, S and D did not, while all of them typed into
the chat. Plasma's on-screen keyboard service offers accented letters when a physical key is
held, and it holds those keys back from any program that takes text input. Wine's Wayland driver
always takes text input, so the letters that have accents arrive as text. `sc-turbo` says so at
launch when it finds the option on. To switch it off:

```sh
kwriteconfig6 --file plasmakeyboardrc --group General --key diacriticsPopupEnabled --type bool --notify false
```

### The picture tears (KDE Plasma)

KWin lets fullscreen windows tear by default. On Plasma 6.8 the game's picture tore, generated
frames included, until that was switched off. `sc-turbo` says so at launch:

```sh
kwriteconfig6 --file kwinrc --group Compositing --key AllowTearing --type bool false
busctl --user call org.kde.KWin /KWin org.kde.KWin reconfigure
```

In a test at ×4, allowing tearing took the time from sending a frame to seeing it from 13.5 to
2.7 ms. With it allowed, the tester saw tear lines when spawning in Orison.

## The game's own frame rate

Frame generation multiplies what the game delivers, so the game's own rate decides both the
shown rate and the delay. These figures come from one spot in Orison on the test machine,
standing still, frame generation off, measured from outside the game with the CPU's performance
counters and the kernel's scheduler trace (`perf`). Nothing was attached to the game.

The game is limited by memory there. About 60 % of the P-cores' cycles stalled waiting for RAM
and 13 to 17 % went to walking page tables. The game's own code took 92 % of the CPU samples and
Wine 2 to 3 %, and the CPU stayed below its power limit (90 to 94 W of 125 W).

The memory had been running at DDR4-2133 with XMP off. With XMP loaded and the speed set to
2666, the highest all four modules are rated for, read bandwidth went from 16-19 to 29-36 GB/s
and a random load from 134 to 87.5 ns. The game's own rate at the spot went from 25.0 to 30.8
frames a second (+23 %, three readings each, spread within each about 9 %), and the shown rate
from about 96 to about 117. Check the speed your memory runs at:

```sh
udevadm info -e | grep CONFIGURED_SPEED_MTS
tools/base-ramtest.sh     # that, and what it does: read bandwidth and the time of a random load (needs gcc, AVX2)
```

DDR4 at 2133 or DDR5 at 4800, when the modules are rated higher, usually means XMP or EXPO is
off. The game's start-up CPU benchmark in `Game.log` does not show the difference: it was no
faster at 2666 (27.7 to 33.3 ms) than at 2133 (27.7 to 28.4 ms). Test any memory change before
you play. Here a try at 3200, past the rating of one of the two kits, failed:
`stressapptest -M 44000 -s 7200 -m 12 -W` found the first error after 40 seconds and 949 in two
minutes. The same test, run for an hour at 2666, found none.

Placing the game's threads on P-cores or E-cores by kind, tried with a patched runner, lost 4 to
6 % against leaving every thread free (2026-11-14, about 19 frames a second, generation off).
For the CPU schedulers, see [CPU scheduler](#cpu-scheduler-optional). Switching the CPU's
hardware prefetchers off gave 4 to 7 % at two spots: see
[Hardware prefetchers](#hardware-prefetchers-optional).

Still open: transparent huge pages, switched on for the whole system, covered only 27 to 48 % of
the game's memory, took 2 to 3 points off the page walks and moved the frame rate by less than
the spread. Covering the rest needs a change in how the Wine runner commits memory, not made yet.

## Repository layout

- `fg/`: the frame-generation layer (`sc_fg.cpp`), its compute shaders and the panel
- `app/`: the AppImage's window (`myrddin`, Qt 6 Quick) and its tests
- `appimage/`: the scripts that build and test the AppImage
- `gui/`: the small launcher window (`myrddin-gui`, GTK 4 and libadwaita) and its list of games
- `probe/`: the passive probe layer and the test programs, among them the two-device test chain
- `tools/`: tests and validation, in-session comparisons, log readers, thread and display tools,
  the panel's key, and the `base-*` tools the measurements under
  [The game's own frame rate](#the-games-own-frame-rate) were made with
- `proto/hud-motion/`: the reference the layer's passes for moving HUD elements are checked
  against, and its test scenes
- `third_party/`: AMD FidelityFX SDK 1.1.4, the recipe that builds it for Linux, and the panel's
  font licence
- `sc-turbo`, `build.sh`: the launcher and the build

## Licence and third-party code

Myrddin is free software under the [GNU General Public License, version 3 or later](LICENSE).
`fg/hud_font.h`, which holds the panel's letters, is under the SIL Open Font License 1.1
instead, like the font it is drawn from.

The code, the tests and the documents in this repository were written by Claude, the AI model
made by [Anthropic](https://www.anthropic.com), working in Claude Code. Cadwaladr directed the
work, decided what it should do and tested it in the game. The parts that come from elsewhere
are listed below.

| Component | Licence |
|---|---|
| AMD FidelityFX SDK 1.1.4, with changes that yield several generated frames per pair | MIT, Copyright (C) 2024 Advanced Micro Devices, Inc. (`third_party/ffx_src/FidelityFX-SDK/LICENSE.txt`) |
| Linux build recipe for the SDK and Vulkan test helpers, from the Lichii project | GPL-3.0-or-later, Copyright 2026 Eden Emulator Project |
| Adwaita Sans, a variation of Inter, drawn into `fg/hud_font.h` for the panel | SIL Open Font License 1.1 (copyright notices and licence text in `third_party/adwaita-sans/LICENSE`) |
| Inter, the font of the AppImage's window (`app/assets/InterVariable.ttf`) | SIL Open Font License 1.1, Copyright (c) 2016 The Inter Project Authors (`app/assets/Inter-LICENSE.txt`) |
| Qt 6, which the AppImage's window is built on (linked, not in this repository) | LGPL-3.0 |
| Mesa's Vulkan layer `VK_LAYER_MESA_vram_report_limit`, built unchanged by `tools/build-vram-layer.sh` (not in this repository; an AppImage carries it) | MIT, Copyright 2017 Google, 2019 Red Hat, 2024 Igalia S.L. |

An AppImage also carries Qt itself, ICU and a number of system libraries. Their licence texts
are inside it under `usr/share/licenses/myrddin`, with a list of every part, its version and
where its source is (`THIRD-PARTY-APPIMAGE.txt`; the About page opens the folder). Portions of
that software are copyright © The FreeType Project (www.freetype.org).

Star Citizen is a trademark of Cloud Imperium Games. FidelityFX and FSR are trademarks of AMD,
DLSS of NVIDIA, and Claude and Claude Code of Anthropic. Myrddin is an independent project, and
none of them is affiliated with it or endorses it.

Thanks to the [Star Citizen LUG](https://github.com/starcitizen-lug), whose runners and Helper
Myrddin builds on.
