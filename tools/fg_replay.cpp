// fg_replay: runs FSR 3.1 frame interpolation (the FidelityFX build snapshotted from the
// Lichii project) on frames captured from Star Citizen by sc-probe, offline and with no window.
//
// A capture holds what DLSS was given: linear HDR colour, motion vectors and depth at render
// size.  The game's final picture (tone-mapped, with HUD) is not in it, so the "backbuffer"
// given to the interpolator here is the captured colour, tone-mapped and sRGB-encoded.
//
// usage: fg_replay OUT_DIR GENERATED FRAME.bin FRAME.bin [FRAME.bin ...]
//   GENERATED: frames to generate between each pair (1..3)
//   Writes OUT_DIR/real<i>.ppm and OUT_DIR/gen<i>_<k>.ppm (generated between real i-1 and i).
// Environment (all optional):
//   SC_FG_NEAR (0.1)      camera near plane, metres
//   SC_FG_FOV (1.0)       vertical field of view, radians
//   SC_FG_FLAGS           context flags; default: inverted | infinite depth (3)
//   SC_FG_MV_SCALE (1)    factor applied to the vectors
//   SC_FG_EXPOSURE (8)    exposure of the synthetic backbuffer
//   SC_FG_DISPATCH_FLAGS  AMD's debug dispatch flags (4 = debug view)
//   FFX_FG_TEST_NO_VALIDATION=1  skip the Khronos validation layer

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_opticalflow.h>

#include "../third_party/lichii/ffx_fg_gpu.h"

using namespace ffx_fg_gpu;

namespace {

float EnvFloat(const char* name, float fallback) {
    const char* value = std::getenv(name);
    return value && *value ? std::strtof(value, nullptr) : fallback;
}

float HalfToFloat(uint16_t h) {
    const int exponent = (h >> 10) & 31, mantissa = h & 1023;
    float value;
    if (exponent == 0) {
        value = std::ldexp(static_cast<float>(mantissa), -24);
    } else if (exponent == 31) {
        value = mantissa ? 0.0f : 65504.0f; // NaN -> 0, infinity -> largest half
    } else {
        value = std::ldexp(static_cast<float>(mantissa + 1024), exponent - 25);
    }
    return (h & 0x8000) ? -value : value;
}

struct Capture {
    uint32_t width{}, height{};
    std::vector<uint8_t> backbuffer; // RGBA8, sRGB-encoded
    std::vector<uint16_t> motion;    // RG16F
    std::vector<float> depth;
};

Capture Load(const std::string& path, float exposure, float mv_scale) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        std::exit(2);
    }
    char header[257]{};
    Capture capture;
    if (std::fread(header, 1, 256, file) != 256 ||
        std::sscanf(header, "SCCAP1 w=%u h=%u", &capture.width, &capture.height) != 2) {
        std::fprintf(stderr, "%s is not a capture\n", path.c_str());
        std::exit(2);
    }
    const size_t pixels = static_cast<size_t>(capture.width) * capture.height;
    std::vector<uint16_t> colour(pixels * 4);
    capture.motion.resize(pixels * 2);
    capture.depth.resize(pixels);
    if (std::fread(colour.data(), 8, pixels, file) != pixels ||
        std::fread(capture.motion.data(), 4, pixels, file) != pixels ||
        std::fread(capture.depth.data(), 4, pixels, file) != pixels) {
        std::fprintf(stderr, "%s is too short\n", path.c_str());
        std::exit(2);
    }
    std::fclose(file);
    capture.backbuffer.resize(pixels * 4);
    for (size_t i = 0; i < pixels; ++i) {
        for (int c = 0; c < 3; ++c) {
            float v = std::fmax(HalfToFloat(colour[i * 4 + c]), 0.0f) * exposure;
            v = v / (1.0f + v);
            capture.backbuffer[i * 4 + c] =
                static_cast<uint8_t>(std::lround(std::pow(v, 1.0f / 2.2f) * 255.0f));
        }
        capture.backbuffer[i * 4 + 3] = 255;
    }
    if (mv_scale != 1.0f) {
        // Rescaled through float; only used for experiments.
        for (uint16_t& h : capture.motion) {
            const float v = HalfToFloat(h) * mv_scale;
            // Smallest correct encoder for the range in use: via the GPU-compatible bit layout.
            uint32_t bits;
            std::memcpy(&bits, &v, 4);
            const uint32_t sign = (bits >> 16) & 0x8000;
            const int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xff) - 127 + 15;
            const uint32_t mantissa = (bits >> 13) & 0x3ff;
            h = (v == 0.0f || exponent <= 0) ? static_cast<uint16_t>(sign)
                : exponent >= 31 ? static_cast<uint16_t>(sign | 0x7bff)
                : static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10) | mantissa);
        }
    }
    return capture;
}

void WritePpm(const std::string& path, const uint8_t* rgba, uint32_t width, uint32_t height) {
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) {
        return;
    }
    std::fprintf(file, "P6\n%u %u\n255\n", width, height);
    for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
        std::fwrite(rgba + i * 4, 1, 3, file);
    }
    std::fclose(file);
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc < 5) {
        std::fprintf(stderr, "usage: %s OUT_DIR GENERATED FRAME.bin FRAME.bin [...]\n", argv[0]);
        return 2;
    }
    const std::string out_dir = argv[1];
    const uint32_t generated = static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 10));
    if (generated < 1 || generated > FFX_FRAMEINTERPOLATION_MAX_GENERATED_FRAMES) {
        std::fprintf(stderr, "GENERATED must be 1..%d\n", FFX_FRAMEINTERPOLATION_MAX_GENERATED_FRAMES);
        return 2;
    }
    const float exposure = EnvFloat("SC_FG_EXPOSURE", 8.0f);
    const float mv_scale = EnvFloat("SC_FG_MV_SCALE", 1.0f);
    const float camera_near = EnvFloat("SC_FG_NEAR", 0.1f);
    const float camera_fov = EnvFloat("SC_FG_FOV", 1.0f);
    const uint32_t context_flags = static_cast<uint32_t>(EnvFloat(
        "SC_FG_FLAGS", FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INVERTED | FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INFINITE));
    const uint32_t dispatch_flags = static_cast<uint32_t>(EnvFloat("SC_FG_DISPATCH_FLAGS", 0));

    std::vector<Capture> captures;
    for (int i = 3; i < argc; ++i) {
        captures.push_back(Load(argv[i], exposure, mv_scale));
    }
    const uint32_t width = captures[0].width, height = captures[0].height;
    std::printf("%zu frames of %ux%u, %u generated per pair, context flags %u, near %.3f, fov %.2f\n",
                captures.size(), width, height, generated, context_flags, camera_near, camera_fov);

    Gpu gpu = CreateGpu("fg_replay");
    Image backbuffer = CreateImage(gpu, VK_FORMAT_R8G8B8A8_UNORM, 4, width, height);
    Image outputs[FFX_FRAMEINTERPOLATION_MAX_GENERATED_FRAMES];
    for (Image& output : outputs) {
        output = CreateImage(gpu, VK_FORMAT_R8G8B8A8_UNORM, 4, width, height);
    }
    Image depth = CreateImage(gpu, VK_FORMAT_R32_SFLOAT, 4, width, height);
    Image motion = CreateImage(gpu, VK_FORMAT_R16G16_SFLOAT, 4, width, height);
    {
        VkCommandBuffer cmd = Begin(gpu);
        for (const Image* image : {&backbuffer, &depth, &motion, &outputs[0], &outputs[1], &outputs[2]}) {
            ToGeneral(cmd, *image);
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
    CHECK_FFX(backend.fpCreateBackendContext(&backend, FFX_EFFECT_SHAREDRESOURCES, nullptr, &shared_context));

    FfxOpticalflowContext optical_flow{};
    FfxOpticalflowContextDescription of_description{};
    of_description.backendInterface = backend;
    of_description.resolution = {width, height};
    CHECK_FFX(ffxOpticalflowContextCreate(&optical_flow, &of_description));

    FfxFrameInterpolationContext interpolation{};
    FfxFrameInterpolationContextDescription fi_description{};
    fi_description.backendInterface = backend;
    fi_description.flags = context_flags;
    fi_description.maxRenderSize = {width, height};
    fi_description.displaySize = {width, height};
    fi_description.backBufferFormat = FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;
    fi_description.previousInterpolationSourceFormat = FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;
    CHECK_FFX(ffxFrameInterpolationContextCreate(&interpolation, &fi_description));

    FfxOpticalflowSharedResourceDescriptions of_shared{};
    CHECK_FFX(ffxOpticalflowGetSharedResourceDescriptions(&optical_flow, &of_shared));
    FfxFrameInterpolationSharedResourceDescriptions fi_shared{};
    CHECK_FFX(ffxFrameInterpolationGetSharedResourceDescriptions(&interpolation, &fi_shared));
    FfxResourceInternal of_vector{}, of_scd{}, dilated_depth{}, dilated_motion{}, previous_depth{};
    CHECK_FFX(backend.fpCreateResource(&backend, &of_shared.opticalFlowVector, shared_context, &of_vector));
    CHECK_FFX(backend.fpCreateResource(&backend, &of_shared.opticalFlowSCD, shared_context, &of_scd));
    CHECK_FFX(backend.fpCreateResource(&backend, &fi_shared.dilatedDepth, shared_context, &dilated_depth));
    CHECK_FFX(backend.fpCreateResource(&backend, &fi_shared.dilatedMotionVectors, shared_context, &dilated_motion));
    CHECK_FFX(backend.fpCreateResource(&backend, &fi_shared.reconstructedPrevNearestDepth, shared_context,
                                       &previous_depth));

    for (uint64_t frame_id = 0; frame_id < captures.size(); ++frame_id) {
        const Capture& capture = captures[frame_id];
        Upload(gpu, backbuffer, capture.backbuffer.data());
        Upload(gpu, depth, capture.depth.data());
        Upload(gpu, motion, capture.motion.data());
        WritePpm(out_dir + "/real" + std::to_string(frame_id) + ".ppm", capture.backbuffer.data(), width, height);
        const bool reset = frame_id == 0;

        VkCommandBuffer cmd = Begin(gpu);
        const FfxCommandList command_list = ffxGetCommandListVK(cmd);

        FfxFrameInterpolationPrepareDescription prepare{};
        prepare.commandList = command_list;
        prepare.renderSize = {width, height};
        prepare.jitterOffset = {0.0f, 0.0f};
        prepare.motionVectorScale = {1.0f, 1.0f};
        prepare.frameTimeDelta = 33.3f;
        prepare.cameraNear = camera_near;
        prepare.cameraFar = 3.402823466e+38f; // infinite far plane
        prepare.viewSpaceToMetersFactor = 1.0f;
        prepare.cameraFovAngleVertical = camera_fov;
        prepare.depth = External(depth, L"Depth");
        prepare.motionVectors = External(motion, L"MotionVectors");
        prepare.frameID = frame_id;
        prepare.dilatedDepth = backend.fpGetResource(&backend, dilated_depth);
        prepare.dilatedMotionVectors = backend.fpGetResource(&backend, dilated_motion);
        prepare.reconstructedPrevDepth = backend.fpGetResource(&backend, previous_depth);
        CHECK_FFX(ffxFrameInterpolationPrepare(&interpolation, &prepare));

        FfxOpticalflowDispatchDescription of_dispatch{};
        of_dispatch.commandList = command_list;
        of_dispatch.color = External(backbuffer, L"Backbuffer");
        of_dispatch.opticalFlowVector = backend.fpGetResource(&backend, of_vector);
        of_dispatch.opticalFlowSCD = backend.fpGetResource(&backend, of_scd);
        of_dispatch.reset = reset;
        of_dispatch.backbufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        of_dispatch.minMaxLuminance = {0.0f, 1.0f};
        CHECK_FFX(ffxOpticalflowContextDispatch(&optical_flow, &of_dispatch));

        FfxFrameInterpolationDispatchDescription dispatch{};
        dispatch.commandList = command_list;
        dispatch.displaySize = {width, height};
        dispatch.renderSize = {width, height};
        dispatch.currentBackBuffer = External(backbuffer, L"Backbuffer");
        dispatch.output = External(outputs[0], L"Output");
        dispatch.generatedFrameCount = generated;
        for (uint32_t i = 0; i < generated; ++i) {
            dispatch.generatedFrames[i] = External(outputs[i], L"Output");
            dispatch.generatedFrameTimes[i] = static_cast<float>(i + 1) / static_cast<float>(generated + 1);
        }
        dispatch.interpolationRect = {0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)};
        dispatch.opticalFlowVector = backend.fpGetResource(&backend, of_vector);
        dispatch.opticalFlowSceneChangeDetection = backend.fpGetResource(&backend, of_scd);
        dispatch.opticalFlowBlockSize = 8;
        dispatch.opticalFlowScale = {1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height)};
        dispatch.cameraNear = prepare.cameraNear;
        dispatch.cameraFar = prepare.cameraFar;
        dispatch.cameraFovAngleVertical = prepare.cameraFovAngleVertical;
        dispatch.viewSpaceToMetersFactor = 1.0f;
        dispatch.frameTimeDelta = 33.3f;
        dispatch.reset = reset;
        dispatch.backBufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        dispatch.minMaxLuminance[0] = 0.0f;
        dispatch.minMaxLuminance[1] = 1.0f;
        dispatch.frameID = frame_id;
        dispatch.flags = dispatch_flags;
        dispatch.dilatedDepth = prepare.dilatedDepth;
        dispatch.dilatedMotionVectors = prepare.dilatedMotionVectors;
        dispatch.reconstructedPrevDepth = prepare.reconstructedPrevDepth;
        CHECK_FFX(ffxFrameInterpolationDispatch(&interpolation, &dispatch));
        Submit(gpu, cmd);
        if (frame_id == 0) {
            continue;
        }
        for (uint32_t i = 0; i < generated; ++i) {
            const std::vector<uint8_t> result = Download(gpu, outputs[i]);
            WritePpm(out_dir + "/gen" + std::to_string(frame_id) + "_" + std::to_string(i) + ".ppm", result.data(),
                     width, height);
        }
        std::printf("frame %llu: %u generated\n", static_cast<unsigned long long>(frame_id), generated);
    }
    std::printf("validation errors: %d\n", g_validation_errors);

    ffxFrameInterpolationContextDestroy(&interpolation);
    ffxOpticalflowContextDestroy(&optical_flow);
    for (FfxResourceInternal resource : {of_vector, of_scd, dilated_depth, dilated_motion, previous_depth}) {
        backend.fpDestroyResource(&backend, resource, shared_context);
    }
    backend.fpDestroyBackendContext(&backend, shared_context);
    std::free(scratch);
    vkDeviceWaitIdle(gpu.device);
    return 0;
}
