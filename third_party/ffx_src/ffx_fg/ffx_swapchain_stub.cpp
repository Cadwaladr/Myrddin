// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// AMD's Vulkan backend points FfxInterface::fpSwapChainConfigureFrameGeneration at its frame
// interpolation swapchain (ffx_vk_swapchain, Windows only and not built here). Lichii presents the
// generated frames itself, and neither frame interpolation nor optical flow calls this.

#include <FidelityFX/host/backends/vk/ffx_vk.h>

FfxErrorCode ffxSetFrameGenerationConfigToSwapchainVK(FfxFrameGenerationConfig const*) {
    return FFX_ERROR_INVALID_ARGUMENT;
}
