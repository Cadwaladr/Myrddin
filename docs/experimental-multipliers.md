# Experimental multipliers: 5× to 8×

Myrddin's frame generation normally goes up to 4×: up to three generated frames between every
two of the game's own. With the experimental setting it goes up to 8×, seven generated frames
between every two. It is off by default and acts from the next launch.

This is experimental. It has passed the headless tests below.

## When it helps

At 4× the display is only filled when the game itself gets a quarter of the refresh rate: 30
frames a second at 120 Hz. Star Citizen often gets less than that where it is busiest. On the
test machine, the busiest tenth of the time in the cities was 25 to 29 frames a second, and at
Port Tressler 19 to 27, all of it held back by the CPU, not the graphics card. At 20 frames a
second, 4× shows 80 frames on a 120 Hz display; 6× shows 120.

The game's own rate each multiplier needs to fill the display:

| Display | 4× | 5× | 6× | 7× | 8× |
|---|---|---|---|---|---|
| 120 Hz | 30 | 24 | 20 | 17 | 15 |
| 144 Hz | 36 | 29 | 24 | 21 | 18 |
| 165 Hz | 41 | 33 | 28 | 24 | 21 |

Above those rates the extra multipliers do nothing: the shown rate stays at the display's
refresh rate at most, and Auto steps down to fewer generated frames by itself.

## What it costs

- **Response.** Generated frames make motion smoother, not quicker. What you do still takes one
  of the game's own frames to show, and every real frame waits while the generated ones go out
  ahead of it: (N − 1)/N of one of the game's frames at N×. At the same game rate, 8× adds an
  eighth of a game frame to what 4× already waits: about 8 ms at 15 frames a second, 6 ms at
  20. Most of the delay comes from the game's low rate itself, with or without frame generation.
- **The picture.** Every generated frame is a guess between two of the game's frames. The fewer
  frames the game makes, the further apart they are and the more each guess has to cover. Fast
  motion and things that come out from behind others show it first.
- **Graphics memory.** About 75 MB more at 2560×1440: room for seven generated frames instead of
  three, made when the game starts whether or not Auto goes above 4× (19 MB more at 1280×720 in
  the headless test).
- **Graphics card time.** Each generated frame costs some. Where the game is held back by the
  CPU, which is where these multipliers are meant to help, the card has room: at Port Tressler
  it was idle 60 % of the time.

## Turning it on

### In the launcher window

1. Options, "Display and frame generation": switch on **Experimental: 5× to 8×**.
2. "Highest multiplier" now offers 2× to 8×. Pick the most Auto may use.
3. On Play, choose **Auto**. Play's 2×, 3× and 4× hold those multipliers; 5× and above come
   through Auto.
4. Launch. The setting acts from the next launch, because the room for the frames is made when
   the game starts.

Switching it off puts a highest multiplier above 4× back to 4×.

### From the command line

In `sc-turbo.conf`:

```sh
export SC_FG_EXPERIMENTAL=1
```

Then:

```sh
./sc-turbo fg 8                  # Auto: the layer picks the multiplier, up to 8×
./sc-turbo fg 6 fixed            # held at 6×, whatever the game's rate
./sc-turbo fg-mult 8 auto        # in a running game that was launched with the setting
```

Without the setting, `./sc-turbo fg 6` starts nothing and says why.

## How it works with the other settings

- **Limiting the game's own frame rate.** By default the game is never held back and Auto takes
  the most generated frames its rate can feed. With Options' "Limit the game's own frame rate"
  on (`SC_FG_FILL=1`), Auto takes the fewest generated frames that fill the display and holds the
  game to an even part of the display's rate. With 8× that part can be an eighth: 15 frames a
  second at 120 Hz.
- **Loading screens.** While the game shows one, its own rate crawls, and Auto would go to the
  highest multiplier there. Auto starts at 3× and takes 3× at most while a loading screen is up,
  then goes up by itself once the game's rate asks for it ([how it is
  detected](usage.md#what-the-layer-does)).
- **A game launched without the setting.** A multiplier above 4× asked for while it runs (from
  the window or with `fg-mult`) is held at 4×, and the layer's log says so: "this launch has room
  for x4 at most".
- **Swapchain images.** For seven generated frames the layer asks for more swapchain images than
  NVIDIA's display surface allows: 11 against at most 8 on the test machine. It takes the 8 and
  notes it in its log ("swapchain: 11 images wanted, the surface takes 8 at most"). A generated
  frame takes its image just before it is shown, so fewer images only means a smaller buffer.
  Before this limit was respected, NVIDIA's driver crashed when the swapchain was created.

## Checking that it works

- Myrddin's panel or MangoHud shows a shown rate above four times the game's own.
- The Console page, the layer's log: at the start, "generated per frame (experimental: up to x8)";
  later, ten-second lines that begin "STAT x5" to "STAT x8".
- If it stays at 4×: the setting was off when the game started. Switch it on, then launch again.

## Measured so far

Headless, with the test program standing in for the game (`tools/fg-validate.sh experimental`),
at 120 Hz:

| Multiplier | The game's own rate | Generated per game frame | Shown | The frame chain |
|---|---|---|---|---|
| 5×, held | 23.8 a second | 4 | 118.7 a second | intact, nothing lost |
| 6×, held | 19.7 a second | 5 | 117.8 a second | intact, nothing lost |
| 8×, held | 14.5 a second | 7 | 115.3 a second | intact, nothing lost |

Auto up to 8×, with the game at about 20 a second and its frame rate limited ("Limit the game's
own frame rate" on), started at 3× and went to 6× within two seconds. Without the setting, 6×
asked for stayed 4×, with the same swapchain as before the setting existed.
