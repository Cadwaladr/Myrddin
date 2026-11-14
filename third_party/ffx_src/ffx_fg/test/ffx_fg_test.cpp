// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Runs AMD's FidelityFX optical flow and frame interpolation (ffx_fg) on synthetic frames on the
// first Vulkan GPU and checks the generated frames: a square moving 32 pixels a frame must be where
// it is at each generated frame's time, and a HUD bar present only in the backbuffer must survive.
// Frame 1 generates AMD's one middle frame, frame 2 three (4x), frame 3 two (3x).
//
// usage: ffx_fg_test [OUT_DIR]   (OUT_DIR gets the frames and the result as .ppm)
// FFX_FG_TEST_NO_VALIDATION=1 skips the Khronos validation layer.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_opticalflow.h>

#include "ffx_fg_gpu.h"

namespace {

using namespace ffx_fg_gpu;

constexpr uint32_t DISPLAY_W = 256, DISPLAY_H = 256;
constexpr uint32_t RENDER_W = 128, RENDER_H = 128;
constexpr uint32_t SCALE = DISPLAY_W / RENDER_W;
constexpr uint32_t SQUARE = 32, SQUARE_Y = 112; // display pixels
constexpr uint32_t HUD_Y0 = 8, HUD_Y1 = 16;
constexpr float FRAME_MS = 33.3f;

uint16_t Half(float value) {
    // Exact for the values used here (0 and small whole numbers).
    if (value == 0.0f) {
        return 0;
    }
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    const uint32_t sign = (bits >> 16) & 0x8000;
    const int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xff) - 127 + 15;
    const uint32_t mantissa = (bits >> 13) & 0x3ff;
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10) | mantissa);
}

struct Rgba {
    uint8_t r, g, b, a;
};

constexpr Rgba RED{230, 40, 30, 255};
constexpr Rgba HUD{255, 255, 255, 255};

/// The textured backdrop (a checker the optical flow can lock onto) at a display pixel.
Rgba Backdrop(uint32_t x, uint32_t y) {
    const bool light = ((x / 16) + (y / 16)) % 2 == 0;
    const uint8_t base = light ? 150 : 90;
    return {base, static_cast<uint8_t>(base + 10), static_cast<uint8_t>(base + 30), 255};
}

bool InSquare(uint32_t x, uint32_t y, uint32_t square_x) {
    return x >= square_x && x < square_x + SQUARE && y >= SQUARE_Y && y < SQUARE_Y + SQUARE;
}

struct FrameData {
    std::vector<Rgba> backbuffer, hudless;
    std::vector<float> depth;
    std::vector<uint16_t> motion; // RG16F, render pixels, current -> previous (FSR convention)
};

FrameData MakeFrame(uint32_t square_x, int32_t motion_render_px) {
    FrameData frame;
    frame.hudless.resize(DISPLAY_W * DISPLAY_H);
    for (uint32_t y = 0; y < DISPLAY_H; ++y) {
        for (uint32_t x = 0; x < DISPLAY_W; ++x) {
            frame.hudless[y * DISPLAY_W + x] = InSquare(x, y, square_x) ? RED : Backdrop(x, y);
        }
    }
    frame.backbuffer = frame.hudless;
    for (uint32_t y = HUD_Y0; y < HUD_Y1; ++y) {
        for (uint32_t x = 16; x < DISPLAY_W - 16; ++x) {
            frame.backbuffer[y * DISPLAY_W + x] = HUD;
        }
    }
    frame.depth.resize(RENDER_W * RENDER_H);
    frame.motion.resize(RENDER_W * RENDER_H * 2);
    for (uint32_t y = 0; y < RENDER_H; ++y) {
        for (uint32_t x = 0; x < RENDER_W; ++x) {
            const bool square = InSquare(x * SCALE, y * SCALE, square_x);
            frame.depth[y * RENDER_W + x] = square ? 0.5f : 0.999f;
            frame.motion[(y * RENDER_W + x) * 2] =
                Half(square ? static_cast<float>(motion_render_px) : 0.0f);
            frame.motion[(y * RENDER_W + x) * 2 + 1] = 0;
        }
    }
    return frame;
}

void WritePpm(const std::string& path, const uint8_t* rgba) {
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) {
        return;
    }
    std::fprintf(file, "P6\n%u %u\n255\n", DISPLAY_W, DISPLAY_H);
    for (uint32_t i = 0; i < DISPLAY_W * DISPLAY_H; ++i) {
        std::fwrite(rgba + i * 4, 1, 3, file);
    }
    std::fclose(file);
}

bool IsRed(const uint8_t* pixel) {
    return pixel[0] > 180 && pixel[1] < 100 && pixel[2] < 100;
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    const std::string out_dir = argc > 1 ? argv[1] : "";
    Gpu gpu = CreateGpu("ffx_fg_test");

    Image backbuffer = CreateImage(gpu, VK_FORMAT_R8G8B8A8_UNORM, 4, DISPLAY_W, DISPLAY_H);
    Image hudless = CreateImage(gpu, VK_FORMAT_R8G8B8A8_UNORM, 4, DISPLAY_W, DISPLAY_H);
    Image outputs[FFX_FRAMEINTERPOLATION_MAX_GENERATED_FRAMES];
    for (Image& output : outputs) {
        output = CreateImage(gpu, VK_FORMAT_R8G8B8A8_UNORM, 4, DISPLAY_W, DISPLAY_H);
    }
    Image depth = CreateImage(gpu, VK_FORMAT_R32_SFLOAT, 4, RENDER_W, RENDER_H);
    Image motion = CreateImage(gpu, VK_FORMAT_R16G16_SFLOAT, 4, RENDER_W, RENDER_H);
    {
        VkCommandBuffer cmd = Begin(gpu);
        for (const Image* image : {&backbuffer, &hudless, &depth, &motion}) {
            ToGeneral(cmd, *image);
        }
        for (const Image& output : outputs) {
            ToGeneral(cmd, output);
        }
        Submit(gpu, cmd);
    }

    // One backend for the shared resources, optical flow and frame interpolation.
    VkDeviceContext device_context{gpu.device, gpu.physical, vkGetDeviceProcAddr};
    const FfxDevice ffx_device = ffxGetDeviceVK(&device_context);
    constexpr size_t MAX_CONTEXTS = 4;
    const size_t scratch_size = ffxGetScratchMemorySizeVK(gpu.physical, MAX_CONTEXTS);
    // Zeroed (the backend reads its reference count before clearing it), on a cache line.
    const size_t scratch_alloc = (scratch_size + 63) / 64 * 64;
    void* scratch = std::aligned_alloc(64, scratch_alloc);
    std::memset(scratch, 0, scratch_alloc);
    FfxInterface backend{};
    CHECK_FFX(ffxGetInterfaceVK(&backend, ffx_device, scratch, scratch_size, MAX_CONTEXTS));
    std::printf("ffx scratch: %zu KiB\n", scratch_size / 1024);

    FfxUInt32 shared_context = 0;
    CHECK_FFX(backend.fpCreateBackendContext(&backend, FFX_EFFECT_SHAREDRESOURCES, nullptr,
                                             &shared_context));

    FfxOpticalflowContext optical_flow{};
    FfxOpticalflowContextDescription of_description{};
    of_description.backendInterface = backend;
    of_description.resolution = {DISPLAY_W, DISPLAY_H};
    CHECK_FFX(ffxOpticalflowContextCreate(&optical_flow, &of_description));

    FfxFrameInterpolationContext interpolation{};
    FfxFrameInterpolationContextDescription fi_description{};
    fi_description.backendInterface = backend;
    fi_description.flags = 0; // render-size motion vectors, no jitter in them, depth 0 near 1 far
    fi_description.maxRenderSize = {RENDER_W, RENDER_H};
    fi_description.displaySize = {DISPLAY_W, DISPLAY_H};
    fi_description.backBufferFormat = FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;
    fi_description.previousInterpolationSourceFormat = FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;
    CHECK_FFX(ffxFrameInterpolationContextCreate(&interpolation, &fi_description));
    std::printf("contexts created\n");

    FfxOpticalflowSharedResourceDescriptions of_shared{};
    CHECK_FFX(ffxOpticalflowGetSharedResourceDescriptions(&optical_flow, &of_shared));
    FfxFrameInterpolationSharedResourceDescriptions fi_shared{};
    CHECK_FFX(ffxFrameInterpolationGetSharedResourceDescriptions(&interpolation, &fi_shared));
    FfxResourceInternal of_vector{}, of_scd{}, dilated_depth{}, dilated_motion{}, previous_depth{};
    CHECK_FFX(backend.fpCreateResource(&backend, &of_shared.opticalFlowVector, shared_context,
                                       &of_vector));
    CHECK_FFX(backend.fpCreateResource(&backend, &of_shared.opticalFlowSCD, shared_context,
                                       &of_scd));
    CHECK_FFX(backend.fpCreateResource(&backend, &fi_shared.dilatedDepth, shared_context,
                                       &dilated_depth));
    CHECK_FFX(backend.fpCreateResource(&backend, &fi_shared.dilatedMotionVectors, shared_context,
                                       &dilated_motion));
    CHECK_FFX(backend.fpCreateResource(&backend, &fi_shared.reconstructedPrevNearestDepth,
                                       shared_context, &previous_depth));

    struct Step {
        uint32_t square_x;
        std::vector<float> times; ///< empty: AMD's single middle frame, through `output`
    };
    const Step steps[] = {
        {64, {}},
        {96, {}},
        {128, {0.25f, 0.5f, 0.75f}},
        {160, {1.0f / 3.0f, 2.0f / 3.0f}},
    };
    bool ok = true;
    for (uint64_t frame_id = 0; frame_id < std::size(steps); ++frame_id) {
        const Step& step = steps[frame_id];
        const uint32_t previous_x = frame_id > 0 ? steps[frame_id - 1].square_x : step.square_x;
        const FrameData frame = MakeFrame(
            step.square_x, -static_cast<int32_t>((step.square_x - previous_x) / SCALE));
        Upload(gpu, backbuffer, frame.backbuffer.data());
        Upload(gpu, hudless, frame.hudless.data());
        Upload(gpu, depth, frame.depth.data());
        Upload(gpu, motion, frame.motion.data());
        if (!out_dir.empty()) {
            WritePpm(out_dir + "/frame" + std::to_string(frame_id) + ".ppm",
                     reinterpret_cast<const uint8_t*>(frame.backbuffer.data()));
        }
        const bool reset = frame_id == 0;

        VkCommandBuffer cmd = Begin(gpu);
        const FfxCommandList command_list = ffxGetCommandListVK(cmd);

        FfxFrameInterpolationPrepareDescription prepare{};
        prepare.commandList = command_list;
        prepare.renderSize = {RENDER_W, RENDER_H};
        prepare.jitterOffset = {0.0f, 0.0f};
        prepare.motionVectorScale = {1.0f, 1.0f};
        prepare.frameTimeDelta = FRAME_MS;
        prepare.cameraNear = 0.1f;
        prepare.cameraFar = 1000.0f;
        prepare.viewSpaceToMetersFactor = 1.0f;
        prepare.cameraFovAngleVertical = 1.0f;
        prepare.depth = External(depth, L"Depth");
        prepare.motionVectors = External(motion, L"MotionVectors");
        prepare.frameID = frame_id;
        prepare.dilatedDepth = backend.fpGetResource(&backend, dilated_depth);
        prepare.dilatedMotionVectors = backend.fpGetResource(&backend, dilated_motion);
        prepare.reconstructedPrevDepth = backend.fpGetResource(&backend, previous_depth);
        CHECK_FFX(ffxFrameInterpolationPrepare(&interpolation, &prepare));

        FfxOpticalflowDispatchDescription of_dispatch{};
        of_dispatch.commandList = command_list;
        of_dispatch.color = External(hudless, L"HudLess");
        of_dispatch.opticalFlowVector = backend.fpGetResource(&backend, of_vector);
        of_dispatch.opticalFlowSCD = backend.fpGetResource(&backend, of_scd);
        of_dispatch.reset = reset;
        of_dispatch.backbufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        of_dispatch.minMaxLuminance = {0.0f, 1.0f};
        CHECK_FFX(ffxOpticalflowContextDispatch(&optical_flow, &of_dispatch));

        FfxFrameInterpolationDispatchDescription dispatch{};
        dispatch.commandList = command_list;
        dispatch.displaySize = {DISPLAY_W, DISPLAY_H};
        dispatch.renderSize = {RENDER_W, RENDER_H};
        dispatch.currentBackBuffer = External(backbuffer, L"Backbuffer");
        dispatch.currentBackBuffer_HUDLess = External(hudless, L"HudLess");
        dispatch.output = External(outputs[0], L"Output");
        dispatch.generatedFrameCount = static_cast<uint32_t>(step.times.size());
        for (size_t i = 0; i < step.times.size(); ++i) {
            dispatch.generatedFrames[i] = External(outputs[i], L"Output");
            dispatch.generatedFrameTimes[i] = step.times[i];
        }
        dispatch.interpolationRect = {0, 0, static_cast<int32_t>(DISPLAY_W),
                                      static_cast<int32_t>(DISPLAY_H)};
        dispatch.opticalFlowVector = backend.fpGetResource(&backend, of_vector);
        dispatch.opticalFlowSceneChangeDetection = backend.fpGetResource(&backend, of_scd);
        dispatch.opticalFlowBlockSize = 8;
        dispatch.opticalFlowScale = {1.0f / DISPLAY_W, 1.0f / DISPLAY_H};
        dispatch.cameraNear = prepare.cameraNear;
        dispatch.cameraFar = prepare.cameraFar;
        dispatch.cameraFovAngleVertical = prepare.cameraFovAngleVertical;
        dispatch.viewSpaceToMetersFactor = prepare.viewSpaceToMetersFactor;
        dispatch.frameTimeDelta = FRAME_MS;
        dispatch.reset = reset;
        dispatch.backBufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        dispatch.minMaxLuminance[0] = 0.0f;
        dispatch.minMaxLuminance[1] = 1.0f;
        dispatch.frameID = frame_id;
        if (const char* flags = std::getenv("FFX_FG_TEST_DISPATCH_FLAGS")) {
            dispatch.flags = static_cast<uint32_t>(std::strtoul(flags, nullptr, 0));
        }
        dispatch.dilatedDepth = prepare.dilatedDepth;
        dispatch.dilatedMotionVectors = prepare.dilatedMotionVectors;
        dispatch.reconstructedPrevDepth = prepare.reconstructedPrevDepth;
        CHECK_FFX(ffxFrameInterpolationDispatch(&interpolation, &dispatch));
        Submit(gpu, cmd);
        if (frame_id == 0) {
            continue;
        }

        // Each generated frame: the square's columns on its middle row, and the HUD bar.
        const std::vector<float> times = step.times.empty() ? std::vector<float>{0.5f} : step.times;
        for (size_t i = 0; i < times.size(); ++i) {
            const std::vector<uint8_t> result = Download(gpu, outputs[i]);
            if (!out_dir.empty()) {
                WritePpm(out_dir + "/frame" + std::to_string(frame_id) + "_generated" +
                             std::to_string(i) + ".ppm",
                         result.data());
            }
            const uint32_t row = SQUARE_Y + SQUARE / 2;
            int32_t first = -1, last = -1;
            for (uint32_t x = 0; x < DISPLAY_W; ++x) {
                if (IsRed(&result[(row * DISPLAY_W + x) * 4])) {
                    first = first < 0 ? static_cast<int32_t>(x) : first;
                    last = static_cast<int32_t>(x);
                }
            }
            uint32_t hud_white = 0;
            for (uint32_t x = 16; x < DISPLAY_W - 16; ++x) {
                const uint8_t* pixel = &result[(((HUD_Y0 + HUD_Y1) / 2) * DISPLAY_W + x) * 4];
                hud_white += pixel[0] > 230 && pixel[1] > 230 && pixel[2] > 230;
            }
            const float expected =
                static_cast<float>(previous_x) +
                times[i] * static_cast<float>(step.square_x - previous_x);
            const bool square_ok = first >= 0 && std::abs(first - expected) <= 2.0f &&
                                   std::abs(last - (expected + SQUARE - 1)) <= 2.0f;
            const bool hud_ok = hud_white >= (DISPLAY_W - 32) * 95 / 100;
            ok = ok && square_ok && hud_ok;
            std::printf("frame %llu t=%.3f: square x %d..%d (expected %.1f..%.1f) %s, HUD %u/%u "
                        "%s\n",
                        static_cast<unsigned long long>(frame_id), times[i], first, last,
                        expected, expected + SQUARE - 1, square_ok ? "ok" : "WRONG", hud_white,
                        DISPLAY_W - 32, hud_ok ? "ok" : "WRONG");
        }
    }
    std::printf("validation errors: %d\n", g_validation_errors);

    ffxFrameInterpolationContextDestroy(&interpolation);
    ffxOpticalflowContextDestroy(&optical_flow);
    for (FfxResourceInternal resource : {of_vector, of_scd, dilated_depth, dilated_motion,
                                         previous_depth}) {
        backend.fpDestroyResource(&backend, resource, shared_context);
    }
    backend.fpDestroyBackendContext(&backend, shared_context);
    std::free(scratch);
    vkDeviceWaitIdle(gpu.device);
    for (Image* image : {&backbuffer, &hudless, &depth, &motion, &outputs[0], &outputs[1],
                         &outputs[2]}) {
        vkDestroyImage(gpu.device, image->image, nullptr);
        vkFreeMemory(gpu.device, image->memory, nullptr);
    }
    vkDestroyCommandPool(gpu.device, gpu.pool, nullptr);
    vkDestroyDevice(gpu.device, nullptr);
    if (gpu.messenger) {
        const auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(gpu.instance, "vkDestroyDebugUtilsMessengerEXT"));
        destroy(gpu.instance, gpu.messenger, nullptr);
    }
    vkDestroyInstance(gpu.instance, nullptr);

    ok = ok && g_validation_errors == 0;
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
