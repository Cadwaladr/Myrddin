# Wine runner work

Star Citizen under Wine is bound by the CPU: the main thread and the job system. This folder
holds what was built and measured around that: a Wine patch that implements the CPU-set and
EcoQoS calls the game makes on Windows, tools that place and observe the game's threads from
outside the process, and the scripts that build and test the patch.

**Where it stands:** the patch works and is tested, and by measurement it is switched off by
default. On the one machine it was compared on, leaving every thread on every CPU was the
faster arrangement (see [What the comparison said](#what-the-comparison-said)). It stays here
as an option and as a way to see what the game asks for.

## What was measured

Test machine: Intel Core i5-13600K (P-cores are CPUs 0-11, E-cores 12-19), runner
`lug-wine-tkg-experimental-wayland-git-11.20-2`. The profile figures come from a single
session and are indicative only.

- **Wine's own cost is small.** Wine's code paths were about 7-9 % of user-space CPU time,
  the Vulkan translation (winevulkan) about 0.3 %. The largest single item was Wine's builtin
  SSE2 `memmove` at about 2.2 %.
- **Linux places the game's threads with no regard for P-cores and E-cores.** Every thread
  group spent 40-48 % of its time on E-cores, which is simply the E-cores' share of the CPUs.
- **Three things that steer threads on Windows are missing under Wine 11.20:**
  1. `SetThreadSelectedCpuSets` and `SetProcessDefaultCpuSets` are stubs, and EcoQoS
     (`SetThreadInformation(ThreadPowerThrottling)`) does nothing. `StarCitizen.exe` imports
     these.
  2. Thread priorities become nice values only when `RLIMIT_NICE` allows it
     (see [Thread priorities](#thread-priorities)).
  3. There is no Thread Director. With SMT on, the kernel reports the same capacity for
     every CPU.

### What the comparison said

In-game, 2026-11-14, a heavy scene at about 19 frames a second, frame generation off,
placements switched back and forth within one session (`sc-turbo threads-ab`, 4 x 20 s a side):

| Placement | Frames a second |
|---|---|
| every thread on every CPU (stock Wine) | 19.9 / 19.6 / 19.0 in three tests |
| `WINE_CPUSETS=class WINE_ECOQOS=1` + `sc-hybrid main` | 18.8 |
| the same, background workers at nice 19 | 18.9 |
| every CPU, but the main thread on the P-cores | 18.1 |

Each margin is inside the noise between stretches (9-13 %), but all three comparisons point
the same way, and the 95th and 99th percentile frame times agree. The thread data explains
it: with the class placement the batch workers on the E-cores waited for a CPU 0.8 times as
long as they worked. Getting a CPU at once matters more than which CPU it is.

So the defaults are `WINE_CPUSETS=0 WINE_ECOQOS=0 SC_HYBRID=off`. Do not add placement or
priority changes on belief: compare them in a session.

## Contents

| Path | What it is |
|---|---|
| `patches/0001-kernelbase-…​.patch` | CPU sets and EcoQoS for kernelbase; applies to wine-11.20 |
| `build/build-kernelbase.sh` | builds only `kernelbase.dll` in a rootless podman container |
| `build/make-patched-runner.sh` | makes a copy of a LUG runner with that DLL swapped in |
| `build/test/` | Windows test programs and their harness, run in a separate throwaway prefix |
| `config/sc-launch.sh` | the LUG Helper's launch script with this project's TUNING section, as an example |
| `tools/sc-hybrid` | places threads from outside (main thread to P-cores, service threads to E-cores), measures where they run, undoes it |
| `tools/sc-monitor` | names the display the game should take as its main one (COSMIC) |
| `tools/sc-levelstats` | summarizes the game's own `level_stats_*.csv` per session |
| `tools/sc-schedlog` | summarizes a `WINE_SCHED_LOG=1` launch log: which scheduling calls the game made |

The tools find the game through `SC_PREFIX` (see `sc-turbo.conf.example` in the project root).

## The kernelbase patch

- **Class mode.** A CPU-set selection becomes a thread affinity for the whole *efficiency
  class* it names: ask for one P-core and the thread may run on all of them. The game's intent
  is kept and Linux still balances within the class. `WINE_CPUSETS=exact` pins to the exact
  CPUs instead, which suits Linux less.
- **Windows rules are kept.** A thread's own selection beats the process default, NULL clears
  it, an explicit `SetThreadAffinityMask` that conflicts wins, new threads inherit the default.
- **EcoQoS.** A thread that asks for it and has no selection of its own goes to the E-cores.
- **CPUs that are not hybrid.** Nothing changes.
- **Switches,** read once per process: `WINE_CPUSETS=class|exact|0`, `WINE_ECOQOS=0`,
  `WINE_SCHED_LOG=1` (logs every CPU-set, priority, ideal-processor and thread-name call with
  the thread's name, whatever `WINEDEBUG` says).
- **Verification.** Built from the runner's own commit (`7298a818b79`, wine-11.20): the export
  table is identical (1302 names) and the image base matches. Tested in a separate prefix
  against the stock DLL in every mode, including thread IDs that Wine reuses after about 450
  threads.

  ```
  stock         : all requests ignored                       (every thread on 0-19)
  patched class : P request -> 0-11, E request -> 12-19, EcoQoS -> 12-19, affinity conflict -> affinity wins
  patched exact : P request -> 0,    E request -> 12
  WINE_CPUSETS=0: identical to stock
  ```

## Building and using it

Needs podman and a LUG runner of Wine 11.20. Only the one DLL is built; the runner is copied,
never changed.

```
build/build-kernelbase.sh                                  # -> build/out/kernelbase.dll
build/make-patched-runner.sh /path/to/prefix/runners/lug-wine-tkg-experimental-wayland-git-11.20-2
build/test/run_test.sh runners/lug-wine-tkg-experimental-wayland-git-11.20-2-cpusets WINE_CPUSETS=class
```

The copy keeps the original as `kernelbase.dll.lug-orig`. To use it, point `wine_path` of your
launch script at the copy's `bin`, as `config/sc-launch.sh` does; that script also starts
`sc-hybrid`, records each launch's settings in `logs/launches.log` and picks the display.
Everything is switched in its TUNING section or per launch:

```
WINE_CPUSETS=class WINE_ECOQOS=1 SC_HYBRID=main ./sc-launch.sh
```

The LUG Helper writes a fresh `sc-launch.sh` when it updates, so keep your changed copy.

## Thread priorities

Wine maps Windows thread priorities to nice values (time-critical -10 to below-normal +2) only
when the user may lower nice values. With gamemode installed:

```
sudo usermod -aG gamemode "$USER"     # then log out and back in
ulimit -e                              # must print 30
```

This is upstream Wine behaviour (`server/thread.c`) and needs no patch.

## Comparing placements

All from outside, within one session, with the game untouched:

| Command | What it tells |
|---|---|
| `sc-turbo threads` | where each kind of thread may run right now |
| `tools/sc-threads load 15` | per kind of thread: time at work, time waiting for a CPU, share on E-cores, nice value |
| `sc-turbo threads-ab asis all` | the launched placement against none, switched every 20 s, frame rates of each side |
| `tools/sc-cpuwatch` | the CPU's clock under the game's load: held back by the power limit or by heat? |

Hold a steady view while a comparison runs. What can only be set at launch needs two sessions,
and between sessions the server and the place move the frame rate more than a placement does.

## Displays under Wine Wayland

Wayland has no main display a program could ask for. The LUG runner's Wayland driver takes the
display with the most pixels unless `WAYLANDDRV_PRIMARY_MONITOR` names one: with a 4K
television beside a 1440p monitor the game renders 3840x2160 at the television's refresh rate
onto the monitor. `tools/sc-monitor` finds the right name (COSMIC's primary display, through
`cosmic-randr`) and the example launch script exports it; `SC_MONITOR=NAME|off` overrides.
`sc-turbo displays check` shows what Wine then tells a program.

## Capturing the game's window

The `-wayland` LUG runners make the game a native Wayland window whose Vulkan frames go to a
subsurface. Tools that capture X11 windows do not see it. `SC_X11=1` in the launch script runs
the game through XWayland instead (`winewayland.drv=d`), at the cost of native-Wayland HDR and
with XWayland's mouse-warp caveats.

## Not done

- **memmove.** Wine's builtin is SSE2. An AVX2/ERMS version, or the game's own
  `vcruntime140.dll` (`vcruntime140=n,b`), are untested ideas.
- **Other CPU-set calls.** `GetThreadSelectedCpuSets` and `GetProcessDefaultCpuSets` are not
  exported by Wine 11.20 and `SetThreadIdealProcessorEx` is a stub.

## Sources

- LUG Wine build and patches: <https://github.com/starcitizen-lug/lug-wine-experimental>,
  <https://github.com/starcitizen-lug/patches>
- LUG knowledge base: <https://github.com/starcitizen-lug/knowledge-base>
- wine-tkg: <https://github.com/Frogging-Family/wine-tkg-git>
- CPU sets: <https://learn.microsoft.com/en-us/windows/win32/procthread/cpu-sets>
- EcoQoS: <https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreadinformation>
- Intel's hybrid placement sample: <https://github.com/GameTechDev/HybridDetect>
- Wine 11.20: `dlls/kernelbase/memory.c`, `server/thread.c`, `dlls/winewayland.drv/wayland_surface.c`
