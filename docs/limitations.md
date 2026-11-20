# Limitations

What Myrddin needs, where it stops, what is known to go wrong, and the one configuration it has
run on.

## Requirements and limitations

- An NVIDIA GPU. Frame generation reads what the game gives DLSS through NVIDIA-only Vulkan
  extensions (`VK_NVX_image_view_handle`, `VK_NVX_binary_import`), so DLSS must be the upscaler
  selected in the game. Under Wine that needs NVAPI (`nvapi64.dll` from dxvk-nvapi) and the
  driver's NGX libraries in the prefix: the LUG Helper installs the first, the LUG's runners link
  in the second, and `sc-turbo fg` says before the launch when either is missing. AMD and Intel
  GPUs are not supported. A fallback without the game's vectors exists (`SC_FG_FALLBACK=1`) and has
  never been tested.
- Linux, with the game running under Wine. The layer works on the host side of Wine and relies
  on Wine passing the game's Vulkan calls through to the host. The game has only been played
  with Wine's Wayland driver. X11 and Proton are untested.
- A runner that ships vkd3d-proton. The Star Citizen LUG's "experimental" runners
  (`lug-wine-tkg-experimental-wayland-git-…`) do. On them the game renders with Vulkan but
  presents its finished frames through Direct3D 12, and that is the swapchain Myrddin puts the
  generated frames into. On a runner without vkd3d-proton (`lug-wine-tkg-git-11.21-1`,
  `lug-wine-tkg-wayland-git-11.22-1`) the game presents through a Vulkan swapchain of its own,
  and it closed while starting up when a layer changed that swapchain. Myrddin leaves it alone,
  so on those runners the game runs without generated frames, and `sc-turbo` says so at launch.
- The current game build. Finding the HUD layer and matching frames to their inputs depend on
  how Alpha 4.10 builds a frame. A patch can break either. The layer then falls back, or you
  switch it off with `./sc-turbo fg-off`.
- The display's refresh rate is read from the desktop at launch on KDE Plasma and COSMIC
  (`./sc-turbo displays` shows what was found). Elsewhere 120 Hz is assumed unless you set
  `SC_FG_TARGET`. The game has been played at 120 Hz and 143.99 Hz. Other rates and variable
  refresh are untested.
- GPU memory. The layer keeps buffers of its own, and their size has not been reduced yet. On
  the 12 GB test card the game runs close to the limit. Switching the upscaler to native
  resolution in a running session exhausted it, turned generation off and was followed by a
  driver hang. Leave headroom, and do not change the upscaling mode during a session.
- Desktop integration (the panel's key, naming the display the game should use, reading its
  refresh rate) is written for KDE Plasma and COSMIC. Elsewhere, bind `./sc-turbo fg-panel` to a
  key yourself and set `SC_MONITOR` and `SC_FG_TARGET`.
- No installer and no packages yet. x86_64 only.

## Known faults

- Moving HUD elements are placed group by group, and things that touch form one group. A small
  part that moves inside a group, such as a pointer on a scale, goes where the group goes, and in
  a very fast turn part of a group can stay at its old place for a frame. A little flicker of
  the flight display is left now and then.
- The map at the top left updates at the game's own rate: it turns inside its frame, and moving
  it as one piece doubled its lines and tore its picture. A shop's purchase window has also been
  seen at the game's own rate.
- A frame that comes without a HUD capture shows the HUD of the last capture, so moving markers
  stand still for that frame. Where captures are missing for many frames in a row (the game's
  menus), frames are interpolated as presented, HUD included, until captures come back.
- In the pause menu, or while the view stands still, the generated rate can drop until the view
  moves.
- 3 of 24 launches on 2026-11-18 and 19 froze in the game's front end within the first 20
  seconds, with the layer loaded. The layer's record showed no thread inside it. It is not known
  whether this happens without the layer.

## Tested configuration

This is the only configuration Myrddin has run on.

| | |
|---|---|
| Distribution | Fedora Linux 44 |
| Kernel | 7.3.4-200.fc44.x86_64 (7.2.15 before 2026-11-19). For the optional CPU scheduler: scx-scheds 1.1.4, scx-tools 1.1.3 |
| Architecture | x86_64 |
| Desktop | KDE Plasma 6.8.3 (KWin 6.8.3; 6.8.2 before 2026-11-19), Wayland session. Before 2026-11-14 COSMIC 1.11.0, where most of the development took place |
| GPU and driver | NVIDIA GeForce RTX 4070 SUPER, 12 GB; driver 620.70.06 (615.78.08 before 2026-11-19) |
| CPU | Intel Core i5-13600K (6 P-cores with SMT, 8 E-cores) |
| Memory | 64 GB DDR4 in two kits (2 × 16 GB rated 3200, 2 × 16 GB rated 2666), at 2666 MT/s since 2026-11-19, 2133 before |
| Wine runner | the Star Citizen LUG's, unchanged: `lug-wine-tkg-experimental-wayland-git-11.22-1` and `-11.20-2` (Wine 11.22 and 11.20, Wayland driver) |
| Game | Star Citizen Alpha 4.10 on the LIVE servers, Vulkan renderer, DLSS: build 13220821 (version 4.10.201.48085) since the patch of 2026-11-19; before it build 13165946 (version 4.10.200.58746), on which the frame rates under [Measurements](measurements.md) were measured |
| Display | 2560×1440 at 143.99 Hz and at 120 Hz, no variable refresh |
