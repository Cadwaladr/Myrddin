// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The Vulkan side of the ffx_fg GPU tools (ffx_fg_test, ffx_fg_replay): a device with every core
// feature, images kept in GENERAL, uploads and downloads through host buffers, and the Khronos
// validation layer unless FFX_FG_TEST_NO_VALIDATION=1.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <vulkan/vulkan.h>

#include <FidelityFX/host/backends/vk/ffx_vk.h>

namespace ffx_fg_gpu {

inline int g_validation_errors = 0;

#define CHECK_VK(call)                                                                             \
    do {                                                                                           \
        const VkResult result_ = (call);                                                           \
        if (result_ != VK_SUCCESS) {                                                               \
            std::fprintf(stderr, "%s failed: %d (line %d)\n", #call, result_, __LINE__);           \
            std::exit(2);                                                                          \
        }                                                                                          \
    } while (0)

#define CHECK_FFX(call)                                                                            \
    do {                                                                                           \
        const FfxErrorCode error_ = (call);                                                        \
        if (error_ != FFX_OK) {                                                                    \
            std::fprintf(stderr, "%s failed: 0x%x (line %d)\n", #call, error_, __LINE__);          \
            std::exit(2);                                                                          \
        }                                                                                          \
    } while (0)

inline VKAPI_ATTR VkBool32 VKAPI_CALL OnValidation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                            VkDebugUtilsMessageTypeFlagsEXT type,
                                            const VkDebugUtilsMessengerCallbackDataEXT* data,
                                            void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        // Loader notes (an implicit layer that does not load) are general messages, not errors.
        const bool error = severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT &&
                           (type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT);
        g_validation_errors += error ? 1 : 0;
        std::fprintf(stderr, "[validation %s] %s\n", error ? "error" : "warning", data->pMessage);
    }
    return VK_FALSE;
}

struct Gpu {
    VkInstance instance{};
    VkDebugUtilsMessengerEXT messenger{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t family{};
    VkCommandPool pool{};
    VkPhysicalDeviceMemoryProperties memory{};

    uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags flags) const {
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) {
                return i;
            }
        }
        std::fprintf(stderr, "no memory type\n");
        std::exit(2);
    }
};

inline bool HasLayer(const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (const auto& layer : layers) {
        if (std::strcmp(layer.layerName, name) == 0) {
            return true;
        }
    }
    return false;
}

inline Gpu CreateGpu(const char* name) {
    Gpu gpu;
    const char* layer = "VK_LAYER_KHRONOS_validation";
    const bool validate = !std::getenv("FFX_FG_TEST_NO_VALIDATION") && HasLayer(layer);
    const char* extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = name;
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app;
    instance_info.enabledLayerCount = validate ? 1 : 0;
    instance_info.ppEnabledLayerNames = &layer;
    instance_info.enabledExtensionCount = validate ? 1 : 0;
    instance_info.ppEnabledExtensionNames = &extension;
    CHECK_VK(vkCreateInstance(&instance_info, nullptr, &gpu.instance));
    std::printf("validation layer: %s\n", validate ? "on" : "off");
    if (validate) {
        VkDebugUtilsMessengerCreateInfoEXT messenger_info{
            VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        messenger_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messenger_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
        messenger_info.pfnUserCallback = OnValidation;
        const auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(gpu.instance, "vkCreateDebugUtilsMessengerEXT"));
        CHECK_VK(create(gpu.instance, &messenger_info, nullptr, &gpu.messenger));
    }

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(gpu.instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(gpu.instance, &count, devices.data());
    for (const auto device : devices) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(device, &properties);
        if (!gpu.physical || properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            gpu.physical = device;
        }
    }
    if (!gpu.physical) {
        std::fprintf(stderr, "no Vulkan device\n");
        std::exit(2);
    }
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(gpu.physical, &properties);
    std::printf("device: %s\n", properties.deviceName);
    vkGetPhysicalDeviceMemoryProperties(gpu.physical, &gpu.memory);

    vkGetPhysicalDeviceQueueFamilyProperties(gpu.physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu.physical, &count, families.data());
    gpu.family = UINT32_MAX;
    for (uint32_t i = 0; i < count; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            gpu.family = i;
            break;
        }
    }
    // Every core feature the device has, as Lichii enables most of them.
    VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    features12.pNext = &features13;
    VkPhysicalDeviceVulkan11Features features11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    features11.pNext = &features12;
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &features11;
    vkGetPhysicalDeviceFeatures2(gpu.physical, &features);
    features.features.robustBufferAccess = VK_FALSE;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = gpu.family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.pNext = &features;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    CHECK_VK(vkCreateDevice(gpu.physical, &device_info, nullptr, &gpu.device));
    vkGetDeviceQueue(gpu.device, gpu.family, 0, &gpu.queue);
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = gpu.family;
    CHECK_VK(vkCreateCommandPool(gpu.device, &pool_info, nullptr, &gpu.pool));
    return gpu;
}

struct Image {
    VkImage image{};
    VkDeviceMemory memory{};
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    uint32_t texel_size{};
};

inline Image CreateImage(const Gpu& gpu, VkFormat format, uint32_t texel_size, uint32_t width,
                  uint32_t height) {
    Image image;
    image.texel_size = texel_size;
    image.info.imageType = VK_IMAGE_TYPE_2D;
    image.info.format = format;
    image.info.extent = {width, height, 1};
    image.info.mipLevels = 1;
    image.info.arrayLayers = 1;
    image.info.samples = VK_SAMPLE_COUNT_1_BIT;
    image.info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image.info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    CHECK_VK(vkCreateImage(gpu.device, &image.info, nullptr, &image.image));
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(gpu.device, image.image, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex =
        gpu.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CHECK_VK(vkAllocateMemory(gpu.device, &allocate, nullptr, &image.memory));
    CHECK_VK(vkBindImageMemory(gpu.device, image.image, image.memory, 0));
    return image;
}

struct Buffer {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    void* data{};
};

inline Buffer CreateHostBuffer(const Gpu& gpu, VkDeviceSize size) {
    Buffer buffer;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    CHECK_VK(vkCreateBuffer(gpu.device, &info, nullptr, &buffer.buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(gpu.device, buffer.buffer, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = gpu.MemoryType(
        requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK_VK(vkAllocateMemory(gpu.device, &allocate, nullptr, &buffer.memory));
    CHECK_VK(vkBindBufferMemory(gpu.device, buffer.buffer, buffer.memory, 0));
    CHECK_VK(vkMapMemory(gpu.device, buffer.memory, 0, VK_WHOLE_SIZE, 0, &buffer.data));
    return buffer;
}

inline VkCommandBuffer Begin(const Gpu& gpu) {
    VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    info.commandPool = gpu.pool;
    info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    info.commandBufferCount = 1;
    VkCommandBuffer cmd;
    CHECK_VK(vkAllocateCommandBuffers(gpu.device, &info, &cmd));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CHECK_VK(vkBeginCommandBuffer(cmd, &begin));
    return cmd;
}

inline void Submit(const Gpu& gpu, VkCommandBuffer cmd) {
    CHECK_VK(vkEndCommandBuffer(cmd));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    CHECK_VK(vkQueueSubmit(gpu.queue, 1, &submit, VK_NULL_HANDLE));
    CHECK_VK(vkQueueWaitIdle(gpu.queue));
    vkFreeCommandBuffers(gpu.device, gpu.pool, 1, &cmd);
}

/// Full barrier, so the test needs no per-use tracking: every image lives in GENERAL, as Lichii's.
inline void Barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0,
                         nullptr);
}

inline void ToGeneral(VkCommandBuffer cmd, const Image& image) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);
}

inline VkBufferImageCopy WholeImage(const Image& image) {
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = image.info.extent;
    return copy;
}

inline void Upload(const Gpu& gpu, const Image& image, const void* data) {
    const VkDeviceSize size =
        VkDeviceSize{image.info.extent.width} * image.info.extent.height * image.texel_size;
    Buffer staging = CreateHostBuffer(gpu, size);
    std::memcpy(staging.data, data, size);
    VkCommandBuffer cmd = Begin(gpu);
    Barrier(cmd);
    const VkBufferImageCopy copy = WholeImage(image);
    vkCmdCopyBufferToImage(cmd, staging.buffer, image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    Barrier(cmd);
    Submit(gpu, cmd);
    vkDestroyBuffer(gpu.device, staging.buffer, nullptr);
    vkFreeMemory(gpu.device, staging.memory, nullptr);
}

inline std::vector<uint8_t> Download(const Gpu& gpu, const Image& image) {
    const VkDeviceSize size =
        VkDeviceSize{image.info.extent.width} * image.info.extent.height * image.texel_size;
    Buffer staging = CreateHostBuffer(gpu, size);
    VkCommandBuffer cmd = Begin(gpu);
    Barrier(cmd);
    const VkBufferImageCopy copy = WholeImage(image);
    vkCmdCopyImageToBuffer(cmd, image.image, VK_IMAGE_LAYOUT_GENERAL, staging.buffer, 1, &copy);
    Barrier(cmd);
    Submit(gpu, cmd);
    std::vector<uint8_t> pixels(size);
    std::memcpy(pixels.data(), staging.data, size);
    vkDestroyBuffer(gpu.device, staging.buffer, nullptr);
    vkFreeMemory(gpu.device, staging.memory, nullptr);
    return pixels;
}

inline FfxResource External(const Image& image, const wchar_t* name) {
    return ffxGetResourceVK(reinterpret_cast<void*>(image.image),
                            ffxGetImageResourceDescriptionVK(image.image, image.info,
                                                             FFX_RESOURCE_USAGE_READ_ONLY),
                            name, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
}

} // namespace ffx_fg_gpu
