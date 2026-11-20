# The window (`myrddin`)

A Qt 6 Quick window for `sc-turbo`: the multiplier, the launch, the settings file, the system check, live
figures and the switches of a running session. It is what the AppImage starts. It does not talk to the
game: it writes `sc-turbo.conf`, runs `sc-turbo`'s commands and reads the layer's log.

```sh
cmake -S app -B build/app -G Ninja        # Qt 6.5 or later: Core, Gui, Qml, Quick, QuickControls2 (Svg optional)
cmake --build build/app
build/app/myrddin --runtime .             # the window, driving the sc-turbo of this checkout
build/app/myrddin fg 4 hud                # words that do not start with "-" go to sc-turbo as they are
```

Without `--runtime` (or `MYRDDIN_RUNTIME`) the runtime is `~/.local/share/myrddin/runtime`. Started from the
AppImage, the runtime the image carries (`sc-turbo`, its tools, the layer) is copied there when it is missing
or of another version, and never while the game or the RSI Launcher runs: the layer's files must outlive the
image's mount. `logs/` and `sc-turbo.conf` there are kept. The window's own choices are in
`~/.config/myrddin/myrddin.conf`.

- `src/backend.*`: everything the pages need from the system
- `src/gameconsole.*`: the Console page: the game's process and its logs, read from outside it
- `src/main.cpp`: the command line, and the window
- `qml/`: the pages (Play, Console, Setup, Options, About, the first-start welcome) and their parts;
  `Theme.qml` holds the colours, sizes, timings and icons
- `assets/`: the icon and the Inter font (SIL OFL 1.1, `Inter-LICENSE.txt`)
- `packaging/`: desktop entry, AppStream data, icon

## Tests

Both draw nowhere (no window appears) and work on a stand-in runtime made by `tests/fake-runtime.sh`, whose
`sc-turbo` writes down how it was called. Neither starts the game.

```sh
app/tests/run-tests.sh build/app/myrddin /tmp/myrddin-tests     # what the window does: launch words, the
                                                                # settings file, live switches, the menu entry,
                                                                # the runtime's installation
app/tests/shots.sh build/app/myrddin /tmp/myrddin-shots         # a picture of every page
```

`MYRDDIN_FAKE=idle|launcher|running|plain` stands in for the game's state, and
`myrddin --screenshot FILE --page NAME [--test-eval CODE]` takes one picture.
