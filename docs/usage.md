# Commands and settings

## Commands

These do what the launcher windows do:

```sh
./sc-turbo fg 4                  # launch with frame generation: the layer picks the multiplier, up to ×4 (the window's Auto)
./sc-turbo fg 3 fixed            # launch with ×3, held there whatever the game's frame rate (the window's ×3)
./sc-turbo run                   # launch without it
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
./sc-turbo fg-panel key=Ctrl+F9  #   another key for the running game (SC_FG_PANEL_KEY keeps it)
./sc-turbo fg-panel              #   the same step from a terminal
./sc-turbo stop                  # end the session: the game, the RSI Launcher, the rest of the prefix
./sc-turbo kill                  #   the same without asking, for a session that "stop" did not end
./sc-turbo displays              # the displays, the one the game takes and its refresh rate
./sc-turbo fg-watch              # the session's log at a glance
./sc-turbo fg-where              # after a freeze: what each thread was doing inside the layer
./sc-turbo                       # every command, with the switches for tuning and diagnosis
```

At every launch `sc-turbo` asks the desktop which display is the primary one and at what rate it
runs. It names that display to the runner, whose Wayland driver otherwise takes the display with
the most pixels (a TV beside the monitor, for instance), and tells the layer the rate. Logs and
dumps go to `logs/` inside the project.

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
  `SC_DLSS_OVERRIDE=0` leaves it out.
- A limit on the graphics memory the game is told about, the wiki's remedy for frame drops when
  the card's memory runs out: 9216 MiB on a 12 GB card, 8192 on 10, 6144 on 8, 4096 on 6, 2048
  on 4, and larger cards are left alone. It works through Mesa's Vulkan layer
  `VK_LAYER_MESA_vram_report_limit` (package `vulkan-mesa-layers` or similar). Fedora has no
  package for it, and `tools/build-vram-layer.sh` builds it into `build/vram-limit` there.
  `SC_VRAM_LIMIT=off` leaves it out. The card and its memory are asked of `nvidia-smi`: an 11 GB
  card gets the 10 GB figure, 9 GB the 8 GB one and so on. Where `nvidia-smi` cannot name the
  card, no limit is set and the launch says so. With several NVIDIA cards of different kinds the
  limit is for the first one `nvidia-smi` names, and the launch says that too.

A script that finds its files from its own location is started as it is, and so is any script
with `SC_LAUNCH_COPY=0`.

## Settings

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
- `SC_FG_PANEL_KEY=Ctrl+F9`: the key that steps the layer's panel in the game, in place of
  Shift+7. Ctrl, Shift and Alt with one key: a letter, a digit, F1 to F12, Tab, Home, End,
  PageUp, PageDown, Insert, Delete, Pause, ScrollLock or one of ``- = [ ] ; ' ` \ , . /``. Keys
  go by their place on a US keyboard. The layer reads the key inside the game while the game
  has the keyboard, so nothing is set up on the desktop; the game gets the key as well, so pick
  one it does not use
- `SC_SCHED=lavd`: a sched_ext scheduler at every launch
- `SC_FG_MARKERS=0`: never move HUD elements
- `SC_FG_KEEP_HUD=0`: the HUD is not kept apart from what is generated (`sc-turbo fg-hud on`
  switches that on in the running game)
- `export SC_FG_GAPFILL=0`, `export SC_FG_PICTURES=0`, `export SC_FG_BOOST=1`: the layer's own
  settings, which it reads from the environment, so they are exported: frames that come without
  the HUD are not bridged, the map is moved like the markers, the GPU's speed is held up
- `SC_FG_FILL=1`: with a multiplier that follows the game's rate, the game's own frames are
  limited to an even part of the display's rate and the layer takes the fewest generated frames
  that fill the display: a smoother picture, with more latency. Without the line the game is
  never limited and the layer takes the most generated frames it can feed (the window's
  "Performance" switch; `sc-turbo fg-params fill=0|1` in a running game)

The launcher window writes the same file: what its Options page sets is there under these names,
and lines it does not know are kept as they are.

What `SC_SCHED` and `SC_MANGOHUD` need is on the page [Optional extras](optional.md).
