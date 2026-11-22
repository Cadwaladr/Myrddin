# Commands and settings

The window has switches for most of what this page lists: the multiplier and the overlay on its
Play page, the rest on Options and Setup. This page is for the terminal, for scripts and for
`sc-turbo.conf`.

## Commands

In the source folder `sc-turbo` is run as `./sc-turbo`; with the AppImage, put
`./Myrddin-x86_64.AppImage` in its place (see [Commands through the AppImage](#commands-through-the-appimage)).
These do what the launcher windows do:

```sh
./sc-turbo fg 4                  # launch with frame generation: the layer picks the multiplier, up to ×4 (the window's Auto)
./sc-turbo fg 3 fixed            # launch with ×3, held there whatever the game's frame rate (the window's ×3)
./sc-turbo run                   # launch without it ("run hud": with MangoHud)
./sc-turbo fg-mult 3 fixed       # in a running game: ×3, held ("fg-mult 4 auto": the layer picks again, up to ×4)
./sc-turbo fg-off                # frames pass through untouched; fg-on brings generation back
```

These go further:

```sh
./sc-turbo fg 3 hud              # launch with MangoHud: its frame rate counts the generated frames
./sc-turbo fg 3 nomarkers        # launch with moving HUD elements left at the game's own rate
./sc-turbo fg 3 watch            # launch with a log of the session ("run watch": without frame generation)
./sc-turbo iowatch report        #   afterwards: frame rates and the game's disk and memory use, place by place
./sc-turbo fg 3 sched=lavd       # launch with the scx_lavd CPU scheduler (see Optional extras)
./sc-turbo latency               # where the time goes between a frame and the display, per multiplier
./sc-turbo fg 3 panel            # launch with the layer's own panel; Shift+7 in the game steps it:
                                 #   the shown rate, both rates, both with the pacing graph, off
./sc-turbo fg 3 panel off        #   the panel with generation off at the start (fg-on: up to x3)
./sc-turbo fg-panel key=Ctrl+F9  #   another key for the running game (SC_FG_PANEL_KEY keeps it)
./sc-turbo fg-panel              #   the same step from a terminal
./sc-turbo stop                  # end the session: the game, the RSI Launcher, the rest of the prefix
./sc-turbo kill                  #   the same without asking, for a session that "stop" did not end
./sc-turbo displays              # the displays, the one the game takes and its refresh rate
./sc-turbo others                # other frame generators and frame limits your setup asks for
./sc-turbo fg-watch              # the session's log at a glance
./sc-turbo fg-where              # after a freeze: what each thread was doing inside the layer
./sc-turbo                       # every command, with the switches for tuning and diagnosis
```

At every launch `sc-turbo` asks the desktop which display is the primary one and at what rate it
runs. It names that display to the runner, whose Wayland driver otherwise takes the display with
the most pixels (a TV beside the monitor, for instance), and tells the layer the rate. Logs and
dumps go to `logs/` inside the project.

## Commands through the AppImage

Started with words after it, the AppImage opens no window and hands them to `sc-turbo`:

```sh
./Myrddin-x86_64.AppImage fg 4 hud          # launch with frame generation up to ×4 and MangoHud
./Myrddin-x86_64.AppImage fg-mult 3 fixed    # in a running game: ×3, held
./Myrddin-x86_64.AppImage displays          # any other command of this page
```

`sc-turbo` then runs from the AppImage's copy in `~/.local/share/myrddin/runtime`, so its
settings file is `~/.local/share/myrddin/runtime/sc-turbo.conf` (the one the window writes) and
its logs are in `logs/` there.

## Your launch script

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
  `SC_DLSS_OVERRIDE=0` leaves it out (the window: Options, "DLSS 4").
- A limit on the graphics memory the game is told about, the wiki's remedy for frame drops when
  the card's memory runs out: 9216 MiB on a 12 GB card, 8192 on 10, 6144 on 8, 4096 on 6, 2048
  on 4, and larger cards are left alone. It works through Mesa's Vulkan layer
  `VK_LAYER_MESA_vram_report_limit`, which the AppImage carries. Of the distributions, Arch has it
  (package `vulkan-mesa-layers`); Fedora, Debian and Ubuntu do not package it, and
  `tools/build-vram-layer.sh` builds it into `build/vram-limit` for a source folder.
  `SC_VRAM_LIMIT=off` leaves it out (the window: Options, "Graphics memory limit"). The card
  and its memory are asked of `nvidia-smi`: an 11 GB card gets the 10 GB figure, 9 GB the 8 GB
  one and so on. Where `nvidia-smi` cannot name the card, no limit is set and the launch says so.
  With several NVIDIA cards of different kinds the limit is for the first one `nvidia-smi` names,
  and the launch says that too.

### Other frame generators and frame limits

A second frame generator would work on the frames Myrddin presents, generated ones included, and
a frame limit would hold the game's frames or every frame that goes out. At a launch with frame
generation, `sc-turbo` keeps them off and names each one your launch script or your environment
asks for:

- NVIDIA Smooth Motion (`NVPRESENT_ENABLE_SMOOTH_MOTION=1`), lsfg-vk (Lossless Scaling's frame
  generation, both its 1.x and 2.x versions, also when its own settings name the game),
  libstrangle and LatencyFleX are switched off with the Vulkan loader's switch for each, which
  wins over the one that switches it on.
- `VKD3D_FRAME_RATE`, `DXVK_FRAME_RATE` and `MANGOHUD_FPS_LIMIT` are set to 0, none, in the copy.
- MangoHud's own `fps_limit` is none at every launch with frame generation, with the rest of its
  settings as they are (`MANGOHUD_CONFIG=read_cfg,fps_limit=0`).
- A Vulkan layer list (`VK_INSTANCE_LAYERS`, `VK_LOADER_LAYERS_ENABLE`, `VK_LOADER_LAYERS_ALLOW`)
  that names one of them keeps it on whatever `sc-turbo` sets: the launch says so.
- gamescope paces every frame itself and cannot be switched off from inside: the launch says so.

`./sc-turbo others` lists what it finds, and the window's system check shows it. "Off" on the
window's Play page (`./sc-turbo run`) starts the game without Myrddin's layer and leaves all of
these as you set them. For a limit on the game's own frame rate, use Options, "Limit the game's
own frame rate".

Without a copy, your script is started as it is, with none of the settings above placed in it,
in two cases: when no copy of it can be made (it is not a shell script, or it looks for its files
next to itself, which a copy in another folder would break), and when `sc-turbo.conf` says
`SC_LAUNCH_COPY=0`. What `sc-turbo` exports still reaches it.

## Settings

`sc-turbo` reads `sc-turbo.conf` beside it, which is not tracked by git. Start from the example.
Every line is optional, and the same names in the environment take precedence:

```sh
cp sc-turbo.conf.example sc-turbo.conf
```

The window's Setup and Options pages write most of these lines; where a line has a switch in the
window, it is named.

- `SC_PREFIX`: the Wine prefix the game is installed in (default `~/Games/star-citizen`; the
  window: Setup, "Game folder")
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
  desktop cannot be asked (the window: Options, "Display" and "Refresh rate")
- `SC_MANGOHUD=1`: MangoHud at every launch, with frame generation or without (the window's
  overlay choice "MangoHud")
- `SC_FG_PANEL_KEY=Ctrl+F9`: the key that steps the layer's panel in the game, in place of
  Shift+7. Ctrl, Shift and Alt with one key: a letter, a digit, F1 to F12, Tab, Home, End,
  PageUp, PageDown, Insert, Delete, Pause, ScrollLock or one of ``- = [ ] ; ' ` \ , . /``. Keys
  go by their place on a US keyboard. The layer reads the key inside the game while the game
  has the keyboard, so nothing is set up on the desktop; the game gets the key as well, so pick
  one it does not use (the window: Options, "Panel corner and key")
- `SC_SCHED=lavd`: a sched_ext scheduler at every launch (the window: Options, "CPU scheduler")
- `SC_PREFETCH=off`, `SC_THP=on`: the CPU's prefetchers off and huge pages on while the game runs
  (the window: Options, "Advanced performance tweaks")
- `SC_FG_MARKERS=0`: never move HUD elements (the window: Options, "Move HUD markers")
- `SC_FG_KEEP_HUD=0`: the HUD is not kept apart from what is generated (`sc-turbo fg-hud on`
  switches that on in the running game; the window: Options, "Keep the HUD sharp")
- `export SC_FG_GAPFILL=0`, `export SC_FG_PICTURES=0`, `export SC_FG_BOOST=1`: the layer's own
  settings, which it reads from the environment, so they are exported: frames that come without
  the HUD are not bridged, the map is moved like the markers, the GPU's speed is held up (the
  window: Options, "Smooth over frames without the HUD", "Leave the map at the game's rate"
  and "Keep the GPU's speed up")
- `SC_FG_FILL=1`: with a multiplier that follows the game's rate, the game's own frames are
  limited to an even part of the display's rate and the layer takes the fewest generated frames
  that fill the display, so that every refresh gets a frame, with more latency. Without the line
  they are never limited and the layer takes the most generated frames it can feed: the quickest
  response (the window's Options, "Limit the game's own frame rate", off by default;
  `sc-turbo fg-params fill=1|0` in a running game)
- `export SC_FG_EXPERIMENTAL=1`: experimental, off by default. Multipliers from ×5 to ×8
  (`sc-turbo fg 6`, `fg-mult 6`; the window's Options, "Experimental: 5× to 8×"), for places where
  the game's own rate falls below 30 frames a second. The layer makes room for that many
  generated frames when the game starts (about 75 MB more graphics memory at 2560x1440), so the
  setting acts from the next launch. Every generated frame is a guess between two of the game's
  and what you do still takes one of the game's frames to show: more of them make a smoother
  picture, not a quicker one. [Experimental multipliers](experimental-multipliers.md) has when
  they help, what they cost and what was measured

The launcher window writes the same file: what its Options page sets is there under these names,
and lines it does not know are kept as they are.

What `SC_SCHED`, `SC_PREFETCH` and `SC_THP` need is on the page [Advanced performance
tweaking](advanced-performance-tweaking.md), what `SC_MANGOHUD` needs on [Optional extras](optional.md).

## What the layer does

The layer, `sc_fg`, loads the way MangoHud does. Between each pair of the game's frames it runs
AMD's FSR 3.1 frame interpolation on the motion vectors and depth the game gives DLSS, and shows
one, two or three generated frames (up to seven with the [experimental
setting](experimental-multipliers.md)).

- The multiplier is set at launch and can be changed while the game runs. Up to that limit it
  follows the game's frame rate without slowing the game down (`SC_FG_FILL=1`, the window's
  "Limit the game's own frame rate", limits the game's frames to an even part of the display's
  rate instead, so that the display is filled evenly). If the game alone gets above
  92 % of the refresh rate, generation pauses until it drops below 80 %. With the word `fixed`
  (the window's 2×, 3× and 4×) the multiplier is held instead, whatever the game's frame rate.
- Loading screens. While the game loads, its own frame rate crawls (9 to 12 frames a second at
  the end of loading into the universe), and a multiplier that follows it would go to its limit.
  So a multiplier that follows the game's rate starts at 3× and takes 3× at most while the game
  shows a loading screen; once the screen has gone it follows the rate again, up to its limit.
  The game writes in its own log (`Game.log`) when a loading screen opens and closes:
  `tools/sc-loading`, which every `fg` launch starts, reads that log from outside the game and
  tells the layer (its notes: `logs/loading.out`). A held multiplier stays where it is.
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
  Its own `fps_limit` would hold the layer's frames back, so a launch sets it to none. The layer
  also has a small panel of its own (shown rate, the game's own rate, a pacing graph) when you
  ask for it at launch; Shift+7 in the game steps through it.

Around the layer there is a launcher window, an optional CPU scheduler for the time the game
runs, and tools for measuring: two settings compared within one session (`sc-turbo fg-ab`), the
time from a frame to the display (`sc-turbo latency`), a recorder for the game's disk and memory
use (`sc-turbo iowatch`), and a headless test chain that runs the layer without the game
(`tools/test-chain.sh`).
