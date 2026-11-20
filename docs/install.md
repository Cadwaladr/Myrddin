# Install guide

Myrddin runs from a checkout of this repository or as an AppImage, one file with a window. There
is no download yet, so both begin with a build from source.
[Limitations](limitations.md#requirements-and-limitations) says what the GPU, the game and the
Wine runner must be, and [Optional extras](optional.md#optional-dependencies) lists what is only
needed for single features.

## From a checkout

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

The small launcher window of a checkout (`./sc-turbo gui`) needs GTK 4, libadwaita and PyGObject
(Fedora: `python3-gobject gtk4 libadwaita`).

## The AppImage

`Myrddin-x86_64.AppImage` is one file with a window, `sc-turbo`, its tools and the layer.

### Building it

The window of the AppImage is a Qt 6 program in [`app/`](../app/README.md) (Qt 6.5 or later: `sudo
dnf install qt6-qtbase-devel qt6-qtdeclarative-devel qt6-qtsvg-devel`):

```sh
cmake -S app -B build/app -G Ninja && cmake --build build/app
build/app/myrddin --runtime .    # the window, on the sc-turbo of this checkout
```

The AppImage itself is built in containers, with podman, by the scripts in
[`appimage/`](../appimage/README.md): the layer once more so that it loads on any distribution,
the window with Qt's official libraries, then the image and its tests.

### What it needs

Besides what [Requirements and limitations](limitations.md#requirements-and-limitations) lists,
the AppImage needs:

- x86-64 and glibc 2.35 or newer (Ubuntu 22.04, Debian 12, Fedora 36 and later).
- FUSE 3 to mount itself: `/dev/fuse` and `fusermount3`, package `fuse3`. Without it,
  `./Myrddin-x86_64.AppImage --appimage-extract-and-run` unpacks about 110 MB into `/tmp` and
  starts from there; the copy stays until `/tmp` is cleared.
- A desktop's usual libraries, on Wayland or X11: libglvnd (OpenGL, EGL), libX11 and libxcb,
  libwayland-client, fontconfig, FreeType, and the GPU driver's OpenGL. Qt, the window's font and
  everything else are inside.
- `bash` and `python3` for `sc-turbo` and its tools, as from a checkout.

### Starting it

```sh
chmod +x Myrddin-x86_64.AppImage
./Myrddin-x86_64.AppImage              # the window
./Myrddin-x86_64.AppImage fg 4 hud     # or any sc-turbo command, without the window
```

### Where it keeps its files

The window does not talk to the game. It writes `sc-turbo.conf`, runs `sc-turbo` and reads the
layer's log. At its first start it copies `sc-turbo`, the tools and the layer to
`~/.local/share/myrddin/runtime`: the layer is loaded into the game, the game can outlive the
window, and the AppImage's own files are gone when it closes. Logs and `sc-turbo.conf` stay there
across updates, and a newer AppImage replaces the rest only while the game and the RSI Launcher
are closed. The window's own choices are in `~/.config/myrddin/myrddin.conf`. It has been tested
on the machine under [Tested configuration](limitations.md#tested-configuration), and in Ubuntu
22.04, Debian 12, Fedora and Arch containers without a game.

## Where the game is

Both launcher windows and `sc-turbo`'s commands start the game through your install's launch
script, by default the LUG Helper's `sc-launch.sh` in `~/Games/star-citizen`. If your game is
somewhere else, name its folder on the AppImage's Setup page or set `SC_PREFIX` (see
[Settings](usage.md#settings)).

## Repository layout

- `docs/`: these pages
- `fg/`: the frame-generation layer (`sc_fg.cpp`), its compute shaders and the panel
- `app/`: the AppImage's window (`myrddin`, Qt 6 Quick) and its tests
- `appimage/`: the scripts that build and test the AppImage
- `gui/`: the small launcher window (`myrddin-gui`, GTK 4 and libadwaita) and its list of games
- `probe/`: the passive probe layer and the test programs, among them the two-device test chain
- `tools/`: tests and validation, in-session comparisons, log readers, thread and display tools,
  and the `base-*` tools the measurements under
  [The game's own frame rate](measurements.md#the-games-own-frame-rate) were made with
- `proto/hud-motion/`: the reference the layer's passes for moving HUD elements are checked
  against, and its test scenes
- `third_party/`: AMD FidelityFX SDK 1.1.4, the recipe that builds it for Linux, and the panel's
  font licence
- `sc-turbo`, `build.sh`: the launcher and the build
