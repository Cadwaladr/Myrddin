# Optional extras

Nothing on this page is needed for frame generation. Each entry says what it is for and what it
needs. Myrddin installs nothing by itself and never asks for a password: where something needs
root, the commands are yours to run.

## Optional dependencies

Fedora's package names; other distributions have the same things under other names.

| What | What it is for | Package |
|---|---|---|
| MangoHud | a frame-rate display that counts the generated frames ([below](#mangohud)) | `mangohud` |
| Mesa's Vulkan layer `VK_LAYER_MESA_vram_report_limit` | the limit on the graphics memory the game is told about ([below](#graphics-memory-limit)) | `vulkan-mesa-layers` or similar. Fedora has none: `tools/build-vram-layer.sh` builds it, with podman |
| A kernel with sched_ext, `scx-scheds`, `scx-tools` | another CPU scheduler while the game runs ([below](#cpu-scheduler-optional)) | Linux 6.12 or later; the two packages are in the Terra repository on Fedora 44 |
| `msr-tools` | trying the game with the CPU's hardware prefetchers off ([below](#hardware-prefetchers-optional)) | `msr-tools` |
| GTK 4, libadwaita, PyGObject | the small launcher window of a checkout (`./sc-turbo gui`) | `python3-gobject gtk4 libadwaita` |
| FUSE 3 | lets the AppImage mount itself; without it `--appimage-extract-and-run` does | `fuse3` |
| Weston, NumPy, Pillow | the headless tests (`tools/test-chain.sh`, `tools/fg-validate.sh`) | `weston python3-numpy python3-pillow` |
| Qt 6.5 or later | building the AppImage's window outside its container | `qt6-qtbase-devel qt6-qtdeclarative-devel qt6-qtsvg-devel` |
| podman | building the AppImage | `podman` |

## MangoHud

With `./sc-turbo fg 3 hud`, MangoHud sits below the layer and counts every frame that goes out.
Keep MangoHud's own `fps_limit` off, or it holds the layer's frames back. The layer also has a
small panel of its own (shown rate, the game's own rate, a pacing graph) when you ask for it
at launch.

`SC_MANGOHUD=1` in `sc-turbo.conf` does the same at every launch with frame generation.

## Graphics memory limit

On a card with 12 GB or less, a launch tells the game about less graphics memory than the card
has, the LUG wiki's remedy for frame drops when the card's memory runs out. It works through
Mesa's layer named in the table. Without that layer the launch says so and goes on with no limit.
An AppImage carries the layer. [Your launch script](usage.md#your-launch-script) has the sizes,
and `SC_VRAM_LIMIT=off` leaves it out.

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

All on the machine under [Tested configuration](limitations.md#tested-configuration), with frame
generation off, the two schedulers switched back and forth inside one session.

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
[Frame rates and latency](measurements.md#frame-rates-and-latency) the rows dated 2026-11-19 were
measured with scx_lavd, those dated 2026-11-18 with the kernel's own. `scx_bpfland` was tried
once: 3.9 % more frames in that run, and a longest frame of 168 ms against 129 ms.

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
rate](measurements.md#the-games-own-frame-rate)), which is why we tried it. Intel's CPUs have a
register, `0x1A4`, in which every prefetcher has a bit that switches it off. Some BIOS set-ups
show the same switches, under names such as "Hardware Prefetcher" and "Adjacent Cache Line
Prefetch".

### What Myrddin does by itself

Nothing. Writing that register needs root, and Myrddin never asks for a password. `sc-turbo`, the
window and the AppImage neither read the register nor change it.

### What we measured

On the machine under [Tested configuration](limitations.md#tested-configuration), on 2026-11-19
with game build 13220821, frame generation off, standing still. All prefetchers were switched off
and on again inside one running session: ten seconds off, ten seconds on, twelve rounds at each
spot. Every ten seconds off are compared with the ten seconds on before and after them.

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
