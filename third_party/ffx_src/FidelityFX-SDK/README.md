# FidelityFX SDK (subset)

Parts of AMD's FidelityFX SDK used by Lichii:

- the FSR 3.1 upscaler's GPU code, used in place of a game's DLSS evaluation
  (`src/video_core/renderer_vulkan/vk_fsr3_upscaler.cpp`);
- frame interpolation and optical flow (the frame generation half of FSR 3.1), host code, Vulkan
  backend and GPU code, built into `ffx_fg` by `src/video_core/ffx_fg/` (Linux only; AMD ships
  this as Windows DLLs only).

- Source: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, tag `v1.1.4`
  (commit `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`), FSR 3.1.4.
- Licence: MIT, `LICENSE.txt` (the SDK's `sdk/LICENSE.txt`).

## Files

Upscaler:

- `include/FidelityFX/gpu/`: from `sdk/include/FidelityFX/gpu/` (the core headers, `fsr3upscaler/`
  without the HLSL callbacks and the debug view, `spd/ffx_spd.h`, `fsr1/ffx_fsr1.h`).
- `shaders/fsr3upscaler/`: the Vulkan pass files from `sdk/src/backends/vk/shaders/fsr3upscaler/`
  (the eight upscaling passes; not the autogen-reactive or debug-view pass).

Frame interpolation and optical flow, same paths as in `sdk/`:

- `include/FidelityFX/gpu/frameinterpolation/`, `include/FidelityFX/gpu/opticalflow/`,
  `include/FidelityFX/gpu/ffx_core_cpu.h`.
- `shaders/frameinterpolation/`, `shaders/opticalflow/`: from `sdk/src/backends/vk/shaders/`.
- `include/FidelityFX/host/`: `ffx_assert.h`, `ffx_breadcrumbs.h`, `ffx_error.h`,
  `ffx_frameinterpolation.h`, `ffx_interface.h`, `ffx_message.h`, `ffx_opticalflow.h`,
  `ffx_types.h`, `ffx_util.h`, `backends/vk/ffx_vk.h`.
- `src/components/frameinterpolation/`, `src/components/opticalflow/`: the effects' host code.
- `src/backends/vk/ffx_vk.cpp`: the Vulkan backend.
- `src/backends/shared/`: `ffx_shader_blobs.{cpp,h}` and the frame interpolation and optical flow
  blob accessors.
- `src/shared/`: assert, breadcrumbs list, message and object management.

No prebuilt binary or AMD tool from the SDK is used. The shaders are compiled to SPIR-V by
`src/video_core/host_shaders` (upscaler) and `src/video_core/ffx_fg` (frame generation, whose
`ffx_permutations.py` writes the permutation tables AMD's Windows-only FidelityFX-SC would).

## Changes

The upscaler files are unmodified. Changes to the frame generation files, each marked `Lichii:`
in the source:

- `include/FidelityFX/host/ffx_frameinterpolation.h`, `ffx_opticalflow.h`: the context sizes are
  doubled, as the contexts hold resource names in `wchar_t`, which is 4 bytes on Linux (2 on
  Windows).
- `src/backends/vk/ffx_vk.cpp`:
  - the scratch memory's arrays start on 64-byte boundaries (they were packed at 4 bytes, but
    clang reads and writes their elements with aligned vector moves; MSVC does not);
  - `vkGetBufferMemoryRequirements2` is looked up by its core name when the device has not
    enabled `VK_KHR_get_memory_requirements2` (the KHR name alone gave a null pointer);
  - the descriptor pool counts all six of its pool sizes (it passed 5, leaving out the storage
    buffers).
- `include/FidelityFX/gpu/frameinterpolation/ffx_frameinterpolation_optical_flow_vector_field.h`:
  two loads of `r_optical_flow_confidence` whose result was unused are removed. Nothing binds that
  texture, which reads zero on D3D12 but is an unwritten descriptor on Vulkan.
- More than one generated frame per pair of frames (3x and 4x frame generation): AMD's passes
  generate the frame in the middle only.
  - `ffx_frameinterpolation.h` (host): `FfxFrameInterpolationDispatchDescription` gains
    `generatedFrameCount`, `generatedFrames[]` and `generatedFrameTimes[]` (up to
    `FFX_FRAMEINTERPOLATION_MAX_GENERATED_FRAMES`, 3); with a count of 0 a dispatch works as
    AMD's.
  - `ffx_frameinterpolation.cpp`, `ffx_frameinterpolation_private.h`: the dispatch runs the passes
    from setup to inpainting once per generated frame, with its time and index in the constants
    and its own output, and stores the interpolation source for the next frame once.
  - GPU (`gpu/frameinterpolation/`): the constants gain `interpolationTime` and
    `generatedFrameIndex` (GLSL callbacks only; the HLSL ones are not built here). The vector
    fields still hold half the motion; the game and optical flow vectors are scattered to where a
    pixel is at the frame's time, the reconstructed depth, disocclusion mask and the interpolation
    pass reach the previous frame at `2t` and the current one at `2(1 - t)` times a field vector,
    and the colours blend at `t` (all were fixed at 0.5). At `t = 0.5` the arithmetic is AMD's.
    The setup pass counts frames since a reset once per pair of frames.
