# Measurements

All of it on the one machine under [Tested configuration](limitations.md#tested-configuration).

## Frame rates and latency

Measured on the test machine in the game's cities and space stations, on foot, with hangars and
flights left out: 2560×1440 at 120 Hz, DLSS rendering at 1280×720, frame generation up to ×4,
live servers. "Typical" is the median of the ten-second averages, "busy" the slowest tenth of
them.

| Place | Date | The game's own frames per second: typical (busy) | Shown with frame generation: typical (busy) |
|---|---|---|---|
| New Babbage | 2026-11-19 | 30 (26) | 116 (103) |
| Lorville | 2026-11-19 | 36 (29) | 117 (110) |
| Orison | 2026-11-19 | 29 (25) | 114 (92) |
| Pyro Gateway | 2026-11-20 | 31 (28) | 117 (109) |
| Port Tressler | 2026-11-20 | 24 (17) | 83 (59) |
| Seraphim Station (during IAE) | 2026-11-19 | 40 (38) | 119 (112) |
| Lorville | 2026-11-18 | 29 (25) | 111 (99) |
| Orison | 2026-11-18 | 27 (20) | 108 (82) |
| Levski | 2026-11-18 | 26 (22) | 102 (88) |

The two days differ in more than the date. On 2026-11-18 the test machine's memory ran at 2133
MT/s and the kernel's own CPU scheduler was in use; since 2026-11-19 the memory runs at 2666 and
the rows were measured with scx_lavd (see [A sched_ext CPU scheduler](advanced-performance-tweaking.md#a-sched_ext-cpu-scheduler)).

Rates differ from session to session by more than most settings do. In two sessions earlier on
2026-11-18, Orison typically ran at 39 and showed 118.

When these were measured, the layer held the game to the refresh rate divided by the multiplier
whenever the game could go faster: 30 frames a second at ×4, 40 at ×3 (now the default is not
to: Options, "Limit the game's own frame rate"). In the city stretches of 2026-11-19 it did so in
39 to 87 % of the ten-second windows, so the typical figure for the game's own rate is often that
step, below what the game could do. Among the slowest tenth of the windows almost none were
held, except in Lorville, where about half were. The stations rest on five to seven minutes on
foot each, the Pyro Gateway on thirty.

The shown rate counts the frames sent to the display. With tearing switched off in KWin, about
8 % of them were replaced before they reached the screen (×4, Lorville, 2026-11-18).
`tools/city-rates.py PID ...` makes a table like the one above from the logs of the sessions
you name.

Generated frames make motion smoother and delay every real frame. At ×4 in Lorville, with the
game at 26 to 29 frames a second (2026-11-18, tearing off), a real frame reached the
display 75 to 77 ms after the game handed its inputs to DLSS. Of that, 25 to 29 ms passed before
the game's present call reached the layer, about 8 ms went to generating, 26 to 29 ms was the
real frame waiting while the three generated ones went out ahead of it, and 11 to 15 ms was the
compositor. Myrddin's own part is the generating and that wait, 34 to 37 ms: the rest is there
without frame generation too. The wait is three quarters of a game frame at ×4, and any
interpolation has it. At ×2, with the game near 60, the stages before the compositor came to 24
to 27 ms, about 14 of them Myrddin's (5 generating, 9 waiting). The game's work
before DLSS and the monitor's response are not included. `./sc-turbo latency` prints these
figures for your last session, the compositor's part only after `./sc-turbo fg-params timing=1`
in the running game. With tearing allowed that part fell from 13.5 to 2.7 ms in a test.

## The game's own frame rate

Frame generation multiplies what the game delivers, so the game's own rate decides both the
shown rate and the delay. These figures come from one spot in Orison on the test machine,
standing still, frame generation off, measured from outside the game with the CPU's performance
counters and the kernel's scheduler trace (`perf`). Nothing was attached to the game.

The game is limited by memory there. About 60 % of the P-cores' cycles stalled waiting for RAM
and 13 to 17 % went to walking page tables. The game's own code took 92 % of the CPU samples and
Wine 2 to 3 %, and the CPU stayed below its power limit (90 to 94 W of 125 W).

Placing the game's threads on P-cores or E-cores by kind, tried with a patched runner, lost 4 to
6 % against leaving every thread free (2026-11-14, about 19 frames a second, generation off).
For the CPU schedulers, see [A sched_ext CPU scheduler](advanced-performance-tweaking.md#a-sched_ext-cpu-scheduler). Switching the CPU's
hardware prefetchers off gave 4 to 7 % at two spots: see
[The CPU's hardware prefetchers](advanced-performance-tweaking.md#the-cpus-hardware-prefetchers).

Still open: transparent huge pages, switched on for the whole system, covered only 27 to 48 % of
the game's memory, took 2 to 3 points off the page walks and moved the frame rate by less than
the spread. Covering the rest needs a change in how the Wine runner commits memory, not made yet.
