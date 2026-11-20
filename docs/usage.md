# Commands and settings

## Commands

These do what the launcher windows do:

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
./sc-turbo fg 3 watch            # launch with a log of the session ("run watch": without frame generation)
./sc-turbo iowatch report        #   afterwards: frame rates and the game's disk and memory use, place by place
./sc-turbo fg 3 sched=lavd       # launch with the scx_lavd CPU scheduler (see Optional extras)
./sc-turbo latency               # where the time goes between a frame and the display, per multiplier
./sc-turbo fg 3 panel            # launch with the layer's own panel available; then:
./sc-turbo fg-panel              #   step the panel (off, rate, both rates, with the graph)
./sc-turbo fg-panel-key install  #   KDE Plasma, COSMIC: Shift+7 steps the panel
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
  `SC_VRAM_LIMIT=off` leaves it out.

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
- `SC_SCHED=lavd`: a sched_ext scheduler at every launch
- `SC_FG_MARKERS=0`: never move HUD elements

What `SC_SCHED` and `SC_MANGOHUD` need is on the page [Optional extras](optional.md).
