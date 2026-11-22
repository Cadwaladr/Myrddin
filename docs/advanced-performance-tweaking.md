# Advanced performance tweaking

Changes to the computer that can make Star Citizen run faster, and that Myrddin does not make by
itself: they need root or extra packages, and Myrddin never asks for a password. All of them are
optional, and frame generation works without any of them. Each one can be used by hand, or set
up once so that Myrddin switches it on when it launches the game and back once the game has
closed.

| Tweak | On the test machine | What it needs | Setting |
|---|---|---|---|
| [A sched_ext CPU scheduler](#a-sched_ext-cpu-scheduler) | about 3 % more frames, within what runs differ by, and shorter slow frames | Linux 6.12 or later, `scx-scheds`, `scx-tools`, a polkit rule | `SC_SCHED=lavd` |
| [The CPU's prefetchers off](#the-cpus-hardware-prefetchers) | 4 to 7 % more frames at two busy spots | an Intel Core CPU of the 12th to 14th generation, Myrddin's root helper | `SC_PREFETCH=off` |
| [Huge pages](#huge-pages) | about half of the game's memory in huge pages; the frame rate within what runs differ by | Myrddin's root helper | `SC_THP=on` |

The settings go in `sc-turbo.conf`, or are switched in the window under Options, "Advanced
performance tweaks". They can be switched on before the tweak is set up: they act once it is,
and until then a launch leaves the computer as it is and says so.

## Setting up the automation

This is done once, with root, and undone by removing the files it puts in place. Afterwards no
password is needed: Myrddin asks in a way that cannot open a password dialog, so where the set-up
is missing nothing is switched and nothing pops up over the game. Nothing switched this way
outlasts a reboot.

### The polkit rule for the CPU scheduler

`scx_loader`, the service that loads sched_ext schedulers (it comes with `scx-tools`), asks for an
administrator's password at every switch, and a password dialog over a starting game will not do.
This rule lets your own user, in your own active local session, start and stop sched_ext
schedulers and nothing else:

```sh
tools/sc-sched rule > /tmp/sc-sched.rules      # prints the rule; read it
sudo install -m 644 /tmp/sc-sched.rules /etc/polkit-1/rules.d/49-sc-sched.rules
```

To undo it, remove that file.

### Myrddin's root helper, for the prefetchers and huge pages

One small program run as root, `tools/sc-tune-root`, which does four things and nothing else:
prefetchers off and back, huge pages on and back. systemd runs it as the unit
`myrddin-tune@ACTION.service`, which takes only the words `prefetch-off`, `prefetch-on`, `thp-on`
and `thp-off`, and a polkit rule lets your user, in your own active local session, start exactly
those four units. What it changed is kept in `/run/myrddin-tune` until the next boot. From the
project's folder, or the AppImage's lasting folder (`~/.local/share/myrddin/runtime`):

```sh
tools/sc-tune unit > /tmp/myrddin-tune@.service
tools/sc-tune rule > /tmp/49-myrddin-tune.rules     # names you as the one user it lets through
sudo install -D -o root -g root -m 755 tools/sc-tune-root /usr/local/libexec/myrddin/sc-tune-root
sudo install -o root -g root -m 644 /tmp/myrddin-tune@.service /etc/systemd/system/
sudo install -o root -g root -m 644 /tmp/49-myrddin-tune.rules /etc/polkit-1/rules.d/
sudo systemctl daemon-reload
tools/sc-tune set prefetch off && tools/sc-tune set prefetch on     # a test: no password asked
```

To undo it, remove the three files and run `sudo systemctl daemon-reload`. `tools/sc-tune state`
says what is set.

### What happens at launch

With a tweak's setting on, a launch from `sc-turbo` or the window switches the tweak on, starts
the game, and leaves a small watcher behind that waits for it (30 minutes at most for the game to
start). When the game has ended the tweak is switched back. While the RSI Launcher stays open the
watcher waits ten minutes first, because a game started again from the launcher's own button does
not come through `sc-turbo`. Only the newest launch's watcher acts. `logs/sched.out` and
`logs/tune.out` say what was done and when.

## A sched_ext CPU scheduler

### What it is

The CPU scheduler is the part of the kernel that decides which thread runs on which core, and for
how long. Linux's own is called EEVDF. Since Linux 6.12 the kernel can also take a scheduler as a
small program that is loaded while the system runs and removed again without a reboot: that
mechanism is sched_ext, and the [scx](https://github.com/sched-ext/scx) project publishes
schedulers for it. Myrddin has been tested with `scx_lavd`, which was written with games in mind:
it works out which threads others are waiting for and lets those run first. Star Citizen runs
close to two hundred threads and keeps some fourteen cores busy on the test machine, so the order
they run in is not a detail. A sched_ext scheduler that misbehaves is removed by the kernel
itself, which goes back to its own.

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

In short: a small gain at best, mostly in how even the frames are.

### Automated by Myrddin

Install `scx-scheds` and `scx-tools` (on Fedora 44 from the Terra repository), put [the polkit
rule](#the-polkit-rule-for-the-cpu-scheduler) in place, and choose a scheduler: in the window
under Options, or `SC_SCHED=lavd` in `sc-turbo.conf` for every launch. A launch then runs it in
`scx_loader`'s "gaming" mode for as long as the game runs.

```sh
./sc-turbo fg 3 sched=lavd           # scx_lavd for this launch
./sc-turbo fg 3 sched=bpfland:gaming # another scheduler, or another of its modes
./sc-turbo fg 3 sched=stock          # the kernel's own for this launch, whatever sc-turbo.conf says
./sc-turbo run                       # SC_SCHED applies here too
```

### By hand

With the two packages installed, `scxctl` switches schedulers. Without the polkit rule, starting
and stopping one asks for an administrator's password.

```sh
scxctl start -s lavd -m gaming   # before you launch the game
scxctl get                       # what runs now
scxctl stop                      # afterwards: the kernel's own again
```

In the source folder, `tools/sc-ab-sched.sh lavd` compares a scheduler with the kernel's
own in a running game (hold a steady view for about four minutes).

## The CPU's hardware prefetchers

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
  neither, so treat this as a setting for the time you play, not one to keep for good, which is
  how Myrddin uses it.
- Anything about other machines. This is one CPU with memory at 2666 MT/s. With another CPU or
  faster memory the result may differ, in either direction.

With frame generation the gain shows only where the game runs below the rate the layer holds it
at (the display's rate divided by the multiplier: 30 at 4× on 120 Hz).

### Automated by Myrddin

Set up [Myrddin's root helper](#myrddins-root-helper-for-the-prefetchers-and-huge-pages) and
switch on "CPU prefetchers off while playing" in Options (`SC_PREFETCH=off` in `sc-turbo.conf`).
A launch then switches the prefetchers off and puts the register back as it was once the game has
closed. The helper works on Intel's 12th to 14th generation Core CPUs (Alder Lake and Raptor Lake)
only; on others it leaves the register alone and says why. Like `wrmsr` below, it needs a kernel
that lets root write the register (see the lockdown note there).

### By hand

The values below are for those same CPUs. Other Intel CPUs use the same register with other bits,
and AMD's use other registers, which we have not looked at: there, leave it alone or look in the
BIOS. You need the package `msr-tools` (its two commands `rdmsr` and `wrmsr`) and a kernel that
lets root write such registers. With Secure Boot most kernels refuse:
`cat /sys/kernel/security/lockdown` must show `[none]`.

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

To repeat the measurement, read `tools/base-ab-root.py` (the part that needs root: it switches on
request and puts everything back when it ends) and `tools/base-ab.py` (which reads the layer's
frame-rate lines and asks for the switches in time with them), in the source folder; both
say at their top how they are run.

## Huge pages

### What they are

The game keeps its memory, 28 to 42 GB of it on the test machine (it grows over a long session),
in pages of 4 KiB, the smallest the CPU knows. Every address it uses has to be looked up in the
page tables, and the CPU keeps only a few thousand of those lookups at hand. On the test machine
13 to 17 % of the P-cores' cycles in a city went into such lookups. With huge pages (2 MiB each)
one lookup covers 512 times as much memory. Wine never asks the kernel for huge pages, so the game
gets none unless the kernel's setting is "always" (Fedora's default is "madvise": only for
programs that ask). The setting applies to every program while it is on, not only to the game.

### What we measured

Two sessions on the test machine with the kernel's setting at "always", frame generation off,
standing still:

- 27 to 37 % of the game's memory was in huge pages in the first minutes of a session, and 48 %
  after 33 minutes. The rest stays in small pages: Wine hands the game its memory in pieces, and
  the kernel packs only whole 2 MiB blocks.
- The share of the CPU's time spent on those lookups fell by 2 to 3 points (on the game's main
  thread from 16.2 to 12.7 % in the first session).
- The frame rate did not move by more than the runs' own spread (about 9 %): 29.6, 31.2 and 30.0
  frames a second against 30.2, 32.5 and 29.8 without huge pages.
- No hitches were noticed in two long sessions.

So far: no gain we could measure, and no harm seen. Getting nearly all of the game's memory into
huge pages would need a change in Wine, which is not part of Myrddin.

### Automated by Myrddin

Set up [Myrddin's root helper](#myrddins-root-helper-for-the-prefetchers-and-huge-pages) and
switch on "Huge pages while playing" in Options (`SC_THP=on` in `sc-turbo.conf`). A launch then
sets the settings below and puts them back as they were once the game has closed. Memory that is
in huge pages by then stays so, which does no harm.

### By hand

The kernel's settings for it are files under `/sys/kernel/mm/transparent_hugepage`. These are the
ones the helper sets: huge pages for all memory; a fault takes one when one is free and otherwise
leaves the packing to the kernel's collector (khugepaged); the collector packs only blocks that
have at most 64 of their 512 pages unused, so that little memory is wasted, and looks at 128 MiB
a second.

```sh
cd /sys/kernel/mm/transparent_hugepage
echo always        | sudo tee enabled
echo defer+madvise | sudo tee defrag
echo 64            | sudo tee khugepaged/max_ptes_none
echo 32768         | sudo tee khugepaged/pages_to_scan
echo 1000          | sudo tee khugepaged/scan_sleep_millisecs
echo 5000          | sudo tee khugepaged/alloc_sleep_millisecs
grep AnonHugePages /proc/meminfo   # how much memory is in huge pages now
```

Fedora's own values, to put back: `madvise`, `madvise`, `511`, `4096`, `10000` and `60000`, in
that order. A reboot puts them back too.

## Tried without a gain

- The kernel's preemption model "full" instead of "lazy", switched back and forth in a running
  game the same way as the prefetchers: +0.8 %, which is nothing.
