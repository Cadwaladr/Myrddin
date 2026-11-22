# Optional extras

Nothing on this page is needed for frame generation. Each entry says what it is for and what it
needs. Myrddin installs nothing by itself and never asks for a password: where something needs
root, the commands are yours to run. The changes to the computer that need root (a CPU scheduler,
the CPU's prefetchers, huge pages) have a page of their own: [Advanced performance
tweaking](advanced-performance-tweaking.md).

## Optional dependencies

Fedora's package names; other distributions have the same things under other names.

| What | What it is for | Package |
|---|---|---|
| MangoHud | a frame-rate display that counts the generated frames ([below](#mangohud)) | `mangohud` |
| Mesa's Vulkan layer `VK_LAYER_MESA_vram_report_limit` | the limit on the graphics memory the game is told about ([below](#graphics-memory-limit)) | the AppImage carries it; Arch: `vulkan-mesa-layers`. Fedora, Debian and Ubuntu do not package it: `tools/build-vram-layer.sh` builds it, with podman |
| A kernel with sched_ext, `scx-scheds`, `scx-tools` | another CPU scheduler while the game runs ([Advanced performance tweaking](advanced-performance-tweaking.md#a-sched_ext-cpu-scheduler)) | Linux 6.12 or later; the two packages are in the Terra repository on Fedora 44 |
| `msr-tools` | switching the CPU's hardware prefetchers by hand ([Advanced performance tweaking](advanced-performance-tweaking.md#the-cpus-hardware-prefetchers)) | `msr-tools` |
| GTK 4, libadwaita, PyGObject | the small launcher window of a source build (`./sc-turbo gui`) | `python3-gobject gtk4 libadwaita` |
| FUSE 3 | lets the AppImage mount itself; without it `--appimage-extract-and-run` does | `fuse3` |
| Weston, NumPy, Pillow | the headless tests (`tools/test-chain.sh`, `tools/fg-validate.sh`) | `weston python3-numpy python3-pillow` |
| Qt 6.5 or later | building the full window from source ([Install guide](install.md#building-from-source)) | `qt6-qtbase-devel qt6-qtdeclarative-devel qt6-qtsvg-devel` |

## MangoHud

In the window, choose "MangoHud" as the overlay on the Play page or in Options; from a terminal,
`./sc-turbo fg 3 hud`. MangoHud then sits below the layer and counts every frame that goes out.
Its own `fps_limit` would hold the layer's frames back, so a launch with frame generation sets it
to none and leaves the rest of its settings as they are. The layer also has a small panel of its
own (shown rate, the game's own rate, a pacing graph) when you ask for it at launch; Shift+7 in
the game steps through it.

`SC_MANGOHUD=1` in `sc-turbo.conf` does the same at every launch with frame generation.

## Graphics memory limit

On a card with 4 to 12 GB, a launch tells the game about less graphics memory than the card
has, the LUG wiki's remedy for frame drops when the card's memory runs out. It works through
Mesa's layer named in the table. Without that layer, or where `nvidia-smi` cannot name the card,
the launch says so and goes on with no limit. An AppImage carries the layer.
[Your launch script](usage.md#your-launch-script) has the sizes. The window's Options page
switches it off ("Graphics memory limit"), as does `SC_VRAM_LIMIT=off`.
