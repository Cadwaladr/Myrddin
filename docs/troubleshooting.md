# Troubleshooting

## The game stands still or closes by itself

The layer keeps a record that outlives the game: the session's log (`logs/fg/fg-<pid>.log`)
and, for every thread that called into it, the hook it was in and the locks it held or waited
for.

```sh
./sc-turbo fg-where     # the last session: where each thread was inside the layer
```

In the window: Options, Reports, "Where the layer stands".

A line in the log that begins with `lock` means a thread waited more than a second for one of
the layer's locks, or that a lock left behind by a call that never returned was taken over.
Both, with the output of `fg-where`, are what a bug report needs. Frame generation Off (on the
window's Play page, or `./sc-turbo fg-off`) tells you whether the layer is involved at all.

## Only some keys act in the game (KDE Plasma)

Seen on Plasma 6.8: W moved the character and A, S and D did not, while all of them typed into
the chat. Plasma's on-screen keyboard service offers accented letters when a physical key is
held, and it holds those keys back from any program that takes text input. Wine's Wayland driver
always takes text input, so the letters that have accents arrive as text. `sc-turbo` says so at
launch when it finds the option on. To switch it off:

```sh
kwriteconfig6 --file plasmakeyboardrc --group General --key diacriticsPopupEnabled --type bool --notify false
```

## The picture tears (KDE Plasma)

KWin lets fullscreen windows tear by default. On Plasma 6.8 the game's picture tore, generated
frames included, until that was switched off. `sc-turbo` says so at launch:

```sh
kwriteconfig6 --file kwinrc --group Compositing --key AllowTearing --type bool false
busctl --user call org.kde.KWin /KWin org.kde.KWin reconfigure
```

In a test at ×4, allowing tearing took the time from sending a frame to seeing it from 13.5 to
2.7 ms. With it allowed, tear lines showed when spawning in Orison.
