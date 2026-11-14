/*
 * Test for sc-probe: drives Vulkan the way Wine does.  Every device-level function is
 * fetched with vkGetDeviceProcAddr and called directly, so queues and command buffers
 * never pass through the loader's trampolines.  Presents to a headless surface: no window.
 *
 *   test_rawcalls [frames]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s -> %d\n", #x, r_); return 1; } } while (0)
#define KEY(h) (*(void **)(h))

int main(int argc, char **argv)
{
    int frames = argc > 1 ? atoi(argv[1]) : 300;
    const char *iext[] = {"VK_KHR_surface", "VK_EXT_headless_surface"};
    const char *dext[] = {"VK_KHR_swapchain"};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "test_rawcalls",
                             .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                                .enabledExtensionCount = 2, .ppEnabledExtensionNames = iext};
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, NULL, &inst));

    VkPhysicalDevice pds[8], pd = VK_NULL_HANDLE;
    uint32_t n = 8;
    vkEnumeratePhysicalDevices(inst, &n, pds);
    VkHeadlessSurfaceCreateInfoEXT hci = {.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
    VkSurfaceKHR surf;
    CHECK(vkCreateHeadlessSurfaceEXT(inst, &hci, NULL, &surf));
    /* whichever device can present to the headless surface (the layer does not care which) */
    for (uint32_t i = 0; i < n && !pd; i++) {
        VkBool32 ok = VK_FALSE;
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceSurfaceSupportKHR(pds[i], 0, surf, &ok);
        vkGetPhysicalDeviceProperties(pds[i], &p);
        if (ok) { pd = pds[i]; printf("device: %s\n", p.deviceName); }
    }
    if (!pd) { fprintf(stderr, "no device supports the headless surface\n"); return 1; }
    VkSurfaceFormatKHR fmts[64];
    uint32_t nf = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, fmts);
    if (!nf) { fprintf(stderr, "no surface formats\n"); return 1; }

    float prio = 1;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0,
                                   .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci, .enabledExtensionCount = 1, .ppEnabledExtensionNames = dext};
    VkDevice dev;
    CHECK(vkCreateDevice(pd, &dci, NULL, &dev));

#define RAW(name) PFN_vk##name name = (PFN_vk##name)vkGetDeviceProcAddr(dev, "vk" #name); \
    if (!name) { fprintf(stderr, "no vk" #name "\n"); return 1; }
    RAW(GetDeviceQueue) RAW(CreateSwapchainKHR) RAW(GetSwapchainImagesKHR) RAW(AcquireNextImageKHR)
    RAW(QueuePresentKHR) RAW(CreateCommandPool) RAW(AllocateCommandBuffers) RAW(FreeCommandBuffers)
    RAW(BeginCommandBuffer) RAW(EndCommandBuffer) RAW(CmdPipelineBarrier) RAW(QueueSubmit) RAW(QueueWaitIdle)
    RAW(CreateImage) RAW(DestroyImage) RAW(DestroySwapchainKHR) RAW(DestroyCommandPool) RAW(DestroyDevice)
    RAW(ResetCommandBuffer) RAW(CreateSemaphore) RAW(DestroySemaphore)

    VkQueue q;
    GetDeviceQueue(dev, 0, 0, &q);

    VkSwapchainCreateInfoKHR sci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surf,
        .minImageCount = 3, .imageFormat = fmts[0].format, .imageColorSpace = fmts[0].colorSpace,
        .imageExtent = {640, 360}, .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE};
    VkSwapchainKHR sc;
    CHECK(CreateSwapchainKHR(dev, &sci, NULL, &sc));
    VkImage imgs[16];
    uint32_t ni = 16;
    CHECK(GetSwapchainImagesKHR(dev, sc, &ni, imgs));

    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    VkCommandPool pool;
    CHECK(CreateCommandPool(dev, &pci, NULL, &pool));
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb;
    CHECK(AllocateCommandBuffers(dev, &cai, &cb));
    VkSemaphoreCreateInfo semi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore sem;
    CHECK(CreateSemaphore(dev, &semi, NULL, &sem));

    printf("dispatch keys: device %p  queue %p  command buffer %p  -> %s\n", KEY(dev), KEY(q), KEY(cb),
           KEY(dev) == KEY(q) && KEY(dev) == KEY(cb) ? "same (loader-style)" : "DIFFERENT (Wine-style)");

    /* an ordinary image too, so the image bookkeeping runs */
    VkImageCreateInfo imi = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R16G16_SFLOAT, .extent = {640, 360, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .usage = VK_IMAGE_USAGE_SAMPLED_BIT};
    VkImage img;
    CHECK(CreateImage(dev, &imi, NULL, &img));

    for (int f = 0; f < frames; f++) {
        uint32_t idx;
        VkResult r = AcquireNextImageKHR(dev, sc, UINT64_MAX, sem, VK_NULL_HANDLE, &idx);
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { fprintf(stderr, "acquire -> %d\n", r); return 1; }
        VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(ResetCommandBuffer(cb, 0));
        CHECK(BeginCommandBuffer(cb, &bi));
        VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = imgs[idx], .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL,
                           0, NULL, 1, &b);
        CHECK(EndCommandBuffer(cb));
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &sem,
                           .pWaitDstStageMask = &stage, .commandBufferCount = 1, .pCommandBuffers = &cb};
        CHECK(QueueSubmit(q, 1, &si, VK_NULL_HANDLE));
        CHECK(QueueWaitIdle(q));
        VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .swapchainCount = 1, .pSwapchains = &sc,
                               .pImageIndices = &idx};
        r = QueuePresentKHR(q, &pi);
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { fprintf(stderr, "present -> %d\n", r); return 1; }
    }
    DestroyImage(dev, img, NULL);
    FreeCommandBuffers(dev, pool, 1, &cb);
    DestroySemaphore(dev, sem, NULL);
    DestroyCommandPool(dev, pool, NULL);
    DestroySwapchainKHR(dev, sc, NULL);
    DestroyDevice(dev, NULL);
    vkDestroySurfaceKHR(inst, surf, NULL);
    vkDestroyInstance(inst, NULL);
    printf("ok: %d frames presented\n", frames);
    return 0;
}
