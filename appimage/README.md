# The AppImage

One file with the window ([`app/`](../app/README.md)), `sc-turbo`, its tools and the layer, for an
x86-64 Linux with glibc 2.35 or newer. Everything is built in Ubuntu 22.04 containers with the
project mounted read-only; results go to `appimage/out`, everything else to `appimage/work`
(about 2 GB, both left out of git).

```sh
appimage/build-layer.sh       # the two Vulkan layers, built to load on any such system
appimage/verify-layer.sh      # their symbol versions and exports; loading in bare containers
appimage/make-payload.sh      # what the image carries of the project: out/payload
appimage/build-appimage.sh    # the window with Qt 6.11, and the image: out/Myrddin-x86_64.AppImage
appimage/test-appimage.sh     # the image here and in Ubuntu, Debian, Fedora and Arch containers
```

They need podman (rootless), `curl`, `sha256sum`, and the network the first time. Each script's
header says what it pins by checksum, what its options are and what it checks.
`test-layer-headless.sh` compares the portable layer with the one from `./build.sh` on the GPU, in
a compositor of its own that draws nowhere; it needs that native build and an NVIDIA card.

How the pieces fit:

- The layer is loaded into the game by the Vulkan loader, so it must not depend on the image's
  libraries or on the system's C++ library. `build-layer.sh` links libstdc++ and libgcc into it
  and exports only the Vulkan entry point.
- `AppRun` sets one variable, `MYRDDIN_PAYLOAD`, and starts the window. Qt finds its own files
  through `RUNPATH` and `qt.conf`. Nothing else is put into the environment, because the game is
  started from the window and must not inherit any of it; the window also removes the AppImage
  runtime's own variables before it starts `sc-turbo`. `test-appimage.sh` checks both.
- At its first start the window copies the payload to `~/.local/share/myrddin/runtime`. The
  layer's files have to stay in place while the game runs, and the image's mount is gone as soon
  as the window closes.
- `packaging/deploy.py` fills the AppDir by hand (no linuxdeploy): the Qt libraries, plugins and
  QML modules the window uses, the libraries those need that a desktop cannot be relied on to
  have, and the licence texts of all of it.
