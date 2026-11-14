// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Runs AMD's optical flow and frame interpolation (ffx_fg) again on frames Lichii dumped (FSR 3.1
// frame generation debug flag 16), with the calls and settings Lichii's FfxFrameGenerator makes,
// so a change to the interpolation can be compared on real frames without the game.
//
// usage: ffx_fg_replay DUMP_DIR OUT_DIR GENERATED FRAME FRAME...
//   FRAMEs are consecutive dumped frame numbers (f<N>_* files: depth, motion, camera, info,
//   hudless, presented); the first only primes AMD's history. For each later frame it writes
//   OUT_DIR/f<N>_replay<i>_t<i+1>of<GENERATED+1>.ppm, the frames between it and the one before.
// FFX_FG_TEST_NO_VALIDATION=1 skips the Khronos validation layer.
// FFX_FG_REPLAY_PRIME=N repeats the first frame N times (default 10): AMD's interpolation uses
//   only the game's motion vectors for the first 10 frames after a reset, as in the game.
// FFX_FG_REPLAY_MV_SCALE=S scales the motion vectors, e.g. 2 to interpolate across a skipped
//   frame and compare the middle generated frame with the real one.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_opticalflow.h>

#include "ffx_fg_gpu.h"

namespace {

using namespace ffx_fg_gpu;

constexpr uint32_t DISPLAY_W = 3840, DISPLAY_H = 2160;
constexpr uint32_t RENDER_W = 2560, RENDER_H = 1440;
constexpr uint32_t OPTICAL_FLOW_BLOCK_SIZE = 8; // as vk_ffx_frame_gen.cpp

std::vector<char> ReadFile(const std::string& path) {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        std::fprintf(stderr, "cannot read %s\n", path.c_str());
        std::exit(2);
    }
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

/// A binary PPM (P6, maxval 255) of the display size, as RGBA.
std::vector<uint8_t> ReadPpm(const std::string& path) {
    const std::vector<char> bytes = ReadFile(path);
    std::istringstream header{std::string(bytes.begin(),
                                          bytes.begin() + std::min<size_t>(bytes.size(), 64))};
    std::string magic;
    uint32_t width = 0, height = 0, maxval = 0;
    header >> magic >> width >> height >> maxval;
    const size_t offset = static_cast<size_t>(header.tellg()) + 1;
    if (magic != "P6" || width != DISPLAY_W || height != DISPLAY_H || maxval != 255 ||
        bytes.size() < offset + size_t{width} * height * 3) {
        std::fprintf(stderr, "%s: not a %ux%u P6 image\n", path.c_str(), DISPLAY_W, DISPLAY_H);
        std::exit(2);
    }
    std::vector<uint8_t> rgba(size_t{width} * height * 4);
    for (size_t i = 0; i < size_t{width} * height; ++i) {
        std::memcpy(&rgba[i * 4], &bytes[offset + i * 3], 3);
        rgba[i * 4 + 3] = 255;
    }
    return rgba;
}

void WritePpm(const std::string& path, const std::vector<uint8_t>& rgba) {
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        std::exit(2);
    }
    std::fprintf(file, "P6\n%u %u\n255\n", DISPLAY_W, DISPLAY_H);
    for (size_t i = 0; i < size_t{DISPLAY_W} * DISPLAY_H; ++i) {
        std::fwrite(&rgba[i * 4], 1, 3, file);
    }
    std::fclose(file);
}

struct Camera {
    float near_plane = 0.1f, far_plane = 25000.0f, vertical_fov = 1.0f;
};

/// f<N>_info.txt: "frame N frame_id I reset R generated G camera near A far B fov C".
Camera ReadCamera(const std::string& path) {
    const std::vector<char> bytes = ReadFile(path);
    std::istringstream info{std::string(bytes.begin(), bytes.end())};
    Camera camera;
    std::string word;
    while (info >> word) {
        if (word == "near") {
            info >> camera.near_plane;
        } else if (word == "far") {
            info >> camera.far_plane;
        } else if (word == "fov") {
            info >> camera.vertical_fov;
        }
    }
    return camera;
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc < 6) {
        std::fprintf(stderr, "usage: %s DUMP_DIR OUT_DIR GENERATED FRAME FRAME...\n", argv[0]);
        return 2;
    }
    const std::string dump_dir = argv[1];
    const std::string out_dir = argv[2];
    const uint32_t generated = static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 10));
    if (generated < 1 || generated > FFX_FRAMEINTERPOLATION_MAX_GENERATED_FRAMES) {
        std::fprintf(stderr, "GENERATED must be 1-%u\n", FFX_FRAMEINTERPOLATION_MAX_GENERATED_FRAMES);
        return 2;
    }
    std::vector<std::string> frames(argv + 4, argv + argc);

    Gpu gpu = CreateGpu("ffx_fg_replay");
    Image presented = CreateImage(gpu, VK_FORMAT_R8G8B8A8_UNORM, 4, DISPLAY_W, DISPLAY_H);
    Image hudless = CreateImage(gpu, VK_FORMAT_R8G8B8A8_UNORM, 4, DISPLAY_W, DISPLAY_H);
    Image outputs[FFX_FRAMEINTERPOLATION_MAX_GENERATED_FRAMES];
    for (Image& output : outputs) {
        output = CreateImage(gpu, VK_FORMAT_R8G8B8A8_UNORM, 4, DISPLAY_W, DISPLAY_H);
    }
    Image depth = CreateImage(gpu, VK_FORMAT_R32_SFLOAT, 4, RENDER_W, RENDER_H);
    Image motion = CreateImage(gpu, VK_FORMAT_R16G16_SFLOAT, 4, RENDER_W, RENDER_H);
    {
        VkCommandBuffer cmd = Begin(gpu);
        for (const Image* image : {&presented, &hudless, &depth, &motion}) {
            ToGeneral(cmd, *image);
        }
        for (const Image& output : outputs) {
            ToGeneral(cmd, output);
        }
        Submit(gpu, cmd);
    }

    VkDeviceContext device_context{gpu.device, gpu.physical, vkGetDeviceProcAddr};
    const FfxDevice ffx_device = ffxGetDeviceVK(&device_context);
    constexpr size_t MAX_CONTEXTS = 4;
    const size_t scratch_size = ffxGetScratchMemorySizeVK(gpu.physical, MAX_CONTEXTS);
    const size_t scratch_alloc = (scratch_size + 63) / 64 * 64;
    void* scratch = std::aligned_alloc(64, scratch_alloc);
    std::memset(scratch, 0, scratch_alloc);
    FfxInterface backend{};
    CHECK_FFX(ffxGetInterfaceVK(&backend, ffx_device, scratch, scratch_size, MAX_CONTEXTS));
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
    fi_description.flags = 0;
    fi_description.maxRenderSize = {RENDER_W, RENDER_H};
    fi_description.displaySize = {DISPLAY_W, DISPLAY_H};
    fi_description.backBufferFormat = FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;
    fi_description.previousInterpolationSourceFormat = FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;
    CHECK_FFX(ffxFrameInterpolationContextCreate(&interpolation, &fi_description));

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

    const char* prime_env = std::getenv("FFX_FG_REPLAY_PRIME");
    const size_t prime = prime_env ? std::strtoul(prime_env, nullptr, 10) : 10;
    const char* scale_env = std::getenv("FFX_FG_REPLAY_MV_SCALE");
    const float mv_scale = scale_env ? std::strtof(scale_env, nullptr) : 1.0f;
    std::vector<size_t> sequence((std::max<size_t>)(prime, 1), 0);
    for (size_t index = 1; index < frames.size(); ++index) {
        sequence.push_back(index);
    }

    for (uint64_t frame_id = 0; frame_id < sequence.size(); ++frame_id) {
        const size_t input = sequence[frame_id];
        const std::string prefix = dump_dir + "/f" + frames[input] + "_";
        const Camera camera = ReadCamera(prefix + "info.txt");
        const std::vector<char> camera_bin = ReadFile(prefix + "camera.bin");
        std::array<float, 2> jitter{};
        if (camera_bin.size() >= 34 * sizeof(float)) {
            std::memcpy(jitter.data(), camera_bin.data() + 32 * sizeof(float), sizeof(jitter));
        }
        const std::vector<char> depth_data =
            ReadFile(prefix + "depth_d32_" + std::to_string(RENDER_W) + "x" +
                     std::to_string(RENDER_H) + ".raw");
        const std::vector<char> motion_data =
            ReadFile(prefix + "motion_rg16f_" + std::to_string(RENDER_W) + "x" +
                     std::to_string(RENDER_H) + ".raw");
        if (depth_data.size() != size_t{RENDER_W} * RENDER_H * 4 ||
            motion_data.size() != size_t{RENDER_W} * RENDER_H * 4) {
            std::fprintf(stderr, "frame %s: depth or motion of the wrong size\n",
                         frames[input].c_str());
            return 2;
        }
        Upload(gpu, depth, depth_data.data());
        Upload(gpu, motion, motion_data.data());
        Upload(gpu, presented, ReadPpm(prefix + "presented.ppm").data());
        Upload(gpu, hudless, ReadPpm(prefix + "hudless.ppm").data());
        const bool reset = frame_id == 0;

        VkCommandBuffer cmd = Begin(gpu);
        const FfxCommandList command_list = ffxGetCommandListVK(cmd);

        FfxFrameInterpolationPrepareDescription prepare{};
        prepare.commandList = command_list;
        prepare.renderSize = {RENDER_W, RENDER_H};
        prepare.jitterOffset = {jitter[0], jitter[1]};
        prepare.motionVectorScale = {mv_scale, mv_scale};
        prepare.frameTimeDelta = 33.3f;
        prepare.cameraNear = camera.near_plane;
        prepare.cameraFar = camera.far_plane;
        prepare.viewSpaceToMetersFactor = 1.0f;
        prepare.cameraFovAngleVertical = camera.vertical_fov;
        prepare.depth = External(depth, L"Depth");
        prepare.motionVectors = External(motion, L"MotionVectors");
        prepare.frameID = frame_id;
        prepare.dilatedDepth = backend.fpGetResource(&backend, dilated_depth);
        prepare.dilatedMotionVectors = backend.fpGetResource(&backend, dilated_motion);
        prepare.reconstructedPrevDepth = backend.fpGetResource(&backend, previous_depth);
        CHECK_FFX(ffxFrameInterpolationPrepare(&interpolation, &prepare));
        Barrier(cmd);

        FfxOpticalflowDispatchDescription flow{};
        flow.commandList = command_list;
        flow.color = External(hudless, L"HudLess");
        flow.opticalFlowVector = backend.fpGetResource(&backend, of_vector);
        flow.opticalFlowSCD = backend.fpGetResource(&backend, of_scd);
        flow.reset = reset;
        flow.backbufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        flow.minMaxLuminance = {0.0f, 1.0f};
        CHECK_FFX(ffxOpticalflowContextDispatch(&optical_flow, &flow));

        FfxFrameInterpolationDispatchDescription dispatch{};
        dispatch.commandList = command_list;
        dispatch.displaySize = {DISPLAY_W, DISPLAY_H};
        dispatch.renderSize = {RENDER_W, RENDER_H};
        dispatch.currentBackBuffer = External(presented, L"Presented");
        dispatch.currentBackBuffer_HUDLess = External(hudless, L"HudLess");
        dispatch.generatedFrameCount = generated;
        for (uint32_t i = 0; i < generated; ++i) {
            dispatch.generatedFrames[i] = External(outputs[i], L"Generated");
            dispatch.generatedFrameTimes[i] =
                static_cast<float>(i + 1) / static_cast<float>(generated + 1);
        }
        dispatch.output = dispatch.generatedFrames[0];
        dispatch.interpolationRect = {0, 0, static_cast<int32_t>(DISPLAY_W),
                                      static_cast<int32_t>(DISPLAY_H)};
        dispatch.opticalFlowVector = flow.opticalFlowVector;
        dispatch.opticalFlowSceneChangeDetection = flow.opticalFlowSCD;
        dispatch.opticalFlowBlockSize = OPTICAL_FLOW_BLOCK_SIZE;
        dispatch.opticalFlowScale = {1.0f / DISPLAY_W, 1.0f / DISPLAY_H};
        dispatch.cameraNear = camera.near_plane;
        dispatch.cameraFar = camera.far_plane;
        dispatch.cameraFovAngleVertical = camera.vertical_fov;
        dispatch.viewSpaceToMetersFactor = 1.0f;
        dispatch.frameTimeDelta = 33.3f;
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
        if (input == 0) {
            continue;
        }
        for (uint32_t i = 0; i < generated; ++i) {
            const std::string path = out_dir + "/f" + frames[input] + "_replay" +
                                     std::to_string(i) + "_t" + std::to_string(i + 1) + "of" +
                                     std::to_string(generated + 1) + ".ppm";
            WritePpm(path, Download(gpu, outputs[i]));
            std::printf("%s\n", path.c_str());
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
    for (Image* image : {&presented, &hudless, &depth, &motion, &outputs[0], &outputs[1],
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
    return g_validation_errors == 0 ? 0 : 1;
}
