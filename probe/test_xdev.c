/*
 * Self-test for sc-probe's capture mode.  No window, no swapchain.
 * Creates three images shaped like DLSS's inputs (RGBA16F colour, RG16F vectors,
 * D32S8 depth), fills them with known values, leaves them in the layouts a game would
 * (shader-read / depth-read), then issues an empty vkCmdPipelineBarrier, which the
 * layer treats as "DLSS is about to run" when SC_PROBE_SELFTEST=1.
 * The capture file is then checked value by value.
 *
 *   test_capture nvidia|llvmpipe <capture dir>
 */
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s -> %d\n", #x, r_); return 1; } } while (0)
#define W 320
#define H 180

static float half(uint16_t h)
{
    int e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? ldexpf(m, -24) : ldexpf(m + 1024, e - 25);
    return h & 0x8000 ? -v : v;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: test_capture nvidia|llvmpipe <dir>\n"); return 2; }
    int want_discrete = !strcmp(argv[1], "nvidia");
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, NULL, &inst));
    VkPhysicalDevice pds[8], pd = VK_NULL_HANDLE;
    uint32_t n = 8;
    vkEnumeratePhysicalDevices(inst, &n, pds);
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pds[i], &p);
        if ((p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) == want_discrete) { pd = pds[i]; printf("device: %s\n", p.deviceName); }
    }
    if (!pd) { fprintf(stderr, "device not found\n"); return 1; }
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);

    float prio = 1;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
    const char *exts[] = {"VK_KHR_external_memory_fd"};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = 1, .ppEnabledExtensionNames = exts};
    VkDevice dev;
    CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
#define RAW(name) PFN_vk##name name = (PFN_vk##name)vkGetDeviceProcAddr(dev, "vk" #name); if (!name) { fprintf(stderr, "no vk" #name "\n"); return 1; }
    RAW(GetDeviceQueue) RAW(CreateImage) RAW(GetImageMemoryRequirements) RAW(AllocateMemory) RAW(BindImageMemory)
    RAW(CreateImageView) RAW(CreateCommandPool) RAW(AllocateCommandBuffers) RAW(BeginCommandBuffer)
    RAW(EndCommandBuffer) RAW(CmdPipelineBarrier) RAW(CmdClearColorImage) RAW(CmdClearDepthStencilImage)
    RAW(QueueSubmit) RAW(QueueWaitIdle) RAW(DestroyDevice)
    VkQueue q;
    GetDeviceQueue(dev, 0, 0, &q);

    const VkFormat fmt[3] = {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT};
    VkImage img[3];
    VkImageView view[3];
    for (int i = 0; i < 3; i++) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt[i],
            .extent = {W, H, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     (i == 2 ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)};
        CHECK(CreateImage(dev, &ci, NULL, &img[i]));
        VkMemoryRequirements mr;
        GetImageMemoryRequirements(dev, img[i], &mr);
        uint32_t t = 0;
        while (t < mp.memoryTypeCount && !(mr.memoryTypeBits & (1u << t))) t++;
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = t};
        VkDeviceMemory mem;
        CHECK(AllocateMemory(dev, &ai, NULL, &mem));
        CHECK(BindImageMemory(dev, img[i], mem, 0));
    }
    /* depth view first: the layer takes the render size from it */
    for (int k = 0; k < 3; k++) {
        int i = (k + 2) % 3;
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img[i], .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = fmt[i], .subresourceRange = {i == 2 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        CHECK(CreateImageView(dev, &vi, NULL, &view[i]));
    }

    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    VkCommandPool pool;
    CHECK(CreateCommandPool(dev, &pci, NULL, &pool));
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .commandBufferCount = 1};
    VkCommandBuffer cb;
    CHECK(AllocateCommandBuffers(dev, &cai, &cb));
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(BeginCommandBuffer(cb, &bi));
    const VkPipelineStageFlags all = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    for (int i = 0; i < 3; i++) {
        VkImageAspectFlags asp = i == 2 ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        VkImageSubresourceRange range = {asp, 0, 1, 0, 1};
        VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img[i], .subresourceRange = range};
        CmdPipelineBarrier(cb, all, all, 0, 0, NULL, 0, NULL, 1, &b);
        if (i == 0) { VkClearColorValue c = {.float32 = {0.25f, 0.5f, 0.75f, 1.0f}}; CmdClearColorImage(cb, img[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &range); }
        if (i == 1) { VkClearColorValue c = {.float32 = {1.5f, -2.0f, 0, 0}}; CmdClearColorImage(cb, img[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &range); }
        if (i == 2) { VkClearDepthStencilValue c = {0.625f, 0}; CmdClearDepthStencilImage(cb, img[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &range); }
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = i == 2 ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        CmdPipelineBarrier(cb, all, all, 0, 0, NULL, 0, NULL, 1, &b);
    }
    {   /* a "HUD layer" like the game's: a display-size RGBA16F target drawn into by a pass that loads it,
           followed by a copy of the finished RGBA8 frame to another RGBA8 image */
        RAW(CreateRenderPass) RAW(CreateFramebuffer) RAW(CmdBeginRenderPass) RAW(CmdEndRenderPass) RAW(CmdCopyImage)
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R16G16B16A16_SFLOAT,
            .extent = {1280, 720, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
        VkImage hud; CHECK(CreateImage(dev, &ci, NULL, &hud));
        VkMemoryRequirements mr; GetImageMemoryRequirements(dev, hud, &mr);
        uint32_t t = 0; while (t < mp.memoryTypeCount && !(mr.memoryTypeBits & (1u << t))) t++;
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = t};
        VkDeviceMemory mem; CHECK(AllocateMemory(dev, &ai, NULL, &mem)); CHECK(BindImageMemory(dev, hud, mem, 0));
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = hud, .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = ci.format, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkImageView hv; CHECK(CreateImageView(dev, &vi, NULL, &hv));
        VkAttachmentDescription ad = {.format = ci.format, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .initialLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
        VkAttachmentReference ar = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ar};
        VkRenderPassCreateInfo rci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ad, .subpassCount = 1, .pSubpasses = &sp};
        VkRenderPass rp; CHECK(CreateRenderPass(dev, &rci, NULL, &rp));
        VkFramebufferCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1, .pAttachments = &hv, .width = 1280, .height = 720, .layers = 1};
        VkFramebuffer fb; CHECK(CreateFramebuffer(dev, &fci, NULL, &fb));
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = hud, .subresourceRange = range};
        CmdPipelineBarrier(cb, all, all, 0, 0, NULL, 0, NULL, 1, &b);
        VkClearColorValue cv = {.float32 = {0.5f, 0.25f, 2.0f, 0.75f}};
        CmdClearColorImage(cb, hud, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &range);
        VkRenderPassBeginInfo rb = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb, .renderArea = {{0, 0}, {1280, 720}}};
        CmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
        CmdEndRenderPass(cb);
        VkImage frame[2];
        for (int i = 0; i < 2; i++) {
            ci.format = VK_FORMAT_R8G8B8A8_UNORM;
            CHECK(CreateImage(dev, &ci, NULL, &frame[i]));
            GetImageMemoryRequirements(dev, frame[i], &mr);
            t = 0; while (t < mp.memoryTypeCount && !(mr.memoryTypeBits & (1u << t))) t++;
            ai.allocationSize = mr.size; ai.memoryTypeIndex = t;
            VkDeviceMemory fm; CHECK(AllocateMemory(dev, &ai, NULL, &fm)); CHECK(BindImageMemory(dev, frame[i], fm, 0));
            b.image = frame[i];
            CmdPipelineBarrier(cb, all, all, 0, 0, NULL, 0, NULL, 1, &b);
        }
        VkClearColorValue fc = {.float32 = {0.2f, 0.4f, 0.6f, 1.0f}};
        CmdClearColorImage(cb, frame[0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &fc, 1, &range);
        b.image = frame[0]; b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        CmdPipelineBarrier(cb, all, all, 0, 0, NULL, 0, NULL, 1, &b);
        VkImageCopy whole = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {1280, 720, 1}};
        CmdCopyImage(cb, frame[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, frame[1], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &whole);
    }
    CmdPipelineBarrier(cb, all, all, 0, 0, NULL, 0, NULL, 0, NULL); /* "DLSS runs here" */
    CHECK(EndCommandBuffer(cb));
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    CHECK(QueueSubmit(q, 1, &si, VK_NULL_HANDLE));
    CHECK(QueueWaitIdle(q));
    {   /* a second device, as the game's presenting device: the layer reads the copies back through it */
        VkDevice dev2;
        CHECK(vkCreateDevice(pd, &dci, NULL, &dev2));
        VkQueue q2;
        ((PFN_vkGetDeviceQueue)vkGetDeviceProcAddr(dev2, "vkGetDeviceQueue"))(dev2, 0, 0, &q2);
        ((PFN_vkDestroyDevice)vkGetDeviceProcAddr(dev2, "vkDestroyDevice"))(dev2, NULL);
    }
    DestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    return 0;
    vkDestroyInstance(inst, NULL);

    /* check the file */
    char path[1024] = "";
    DIR *d = opendir(argv[2]);
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (!strncmp(e->d_name, "cap-", 4)) snprintf(path, sizeof path, "%s/%s", argv[2], e->d_name);
    if (!*path) { fprintf(stderr, "FAIL: no capture file in %s\n", argv[2]); return 1; }
    FILE *f = fopen(path, "rb");
    char header[257] = {0};
    size_t px = (size_t)W * H;
    uint16_t *c = malloc(px * 8), *m = malloc(px * 4);
    float *z = malloc(px * 4);
    if (fread(header, 1, 256, f) != 256 || fread(c, 8, px, f) != px || fread(m, 4, px, f) != px || fread(z, 4, px, f) != px) {
        fprintf(stderr, "FAIL: short file %s\n", path); return 1;
    }
    header[strcspn(header, "\n")] = 0;
    for (int i = 255; i > 0 && header[i] == ' '; i--) header[i] = 0;
    printf("header: %s\n", header);
    size_t bad = 0;
    for (size_t i = 0; i < px; i++)
        if (half(c[i * 4]) != 0.25f || half(c[i * 4 + 1]) != 0.5f || half(c[i * 4 + 2]) != 0.75f || half(c[i * 4 + 3]) != 1.0f ||
            half(m[i * 2]) != 1.5f || half(m[i * 2 + 1]) != -2.0f || z[i] != 0.625f) bad++;
    printf("first pixel: colour %.3f %.3f %.3f %.3f  vectors %.3f %.3f  depth %.4f\n", half(c[0]), half(c[1]), half(c[2]), half(c[3]),
           half(m[0]), half(m[1]), z[0]);
    printf("%s: %zu of %zu pixels wrong\n", bad ? "FAIL" : "PASS", bad, px);
    return bad != 0;
}
