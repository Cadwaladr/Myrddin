/*
 * Test for sc-fg's frame chain: a stand-in for how Star Citizen presents under Wine.
 *
 * Device A renders: per frame one command buffer with the scene (a background and a bar that
 * moves every frame), a pass into a "HUD layer", the copy of the frame that the layer takes as its
 * cue, the copy into an image shared with device B, and an empty pipeline barrier that stands in
 * for DLSS (SC_FG_TEST_INPUTS=1). The same thread then records, on device B, the copy out of the
 * shared image (the hand-over) and queues it for the "queue thread", which submits it, blits to
 * the swapchain and presents, like vkd3d-proton's. At most three frames may be queued. A timeline
 * semaphore shared by the two devices keeps a frame from being drawn into the shared image before
 * the one before it has been copied out (--unprotected leaves that away).
 *
 *   test_chain [seconds] [options]
 *     --game-ms N      time the game thread spends per frame (default 4)
 *     --jitter MS      ... give or take up to MS milliseconds, a different amount every frame
 *     --spike N MS     ... and MS milliseconds more in every Nth frame
 *     --sim-ms N       a stage in front of that thread (the game's main thread): it prepares a frame in
 *                      N milliseconds, one ahead at most, and the game thread takes it when it begins
 *                      its own. What the game can do is then set by the slower of the two, and a game
 *                      thread that is held back does not show it (the multiplier's choice must cope)
 *     --sim-jitter MS  ... give or take up to MS milliseconds
 *     --sim-change S N after S seconds the stage takes N milliseconds (the scene changes)
 *     --sim-dip S L MS every S seconds, for L milliseconds, its frames take MS milliseconds more
 *     --worker-ms N    extra time the queue thread spends per frame (default 0)
 *     --nohud N        every Nth frame comes without the HUD pass and the frame copy
 *     --gap N          every Nth frame has its HUD like the others, in the layer and over the frame, but
 *                      no HUD pass and no frame copy that the layer could take as its cue (what the game does
 *                      now and then: the layer has to keep scene and HUD apart without a capture)
 *     --split-hud N    every Nth frame records its HUD pass into a command buffer of its own, submitted
 *                      before the frame's; the frame copy stays in the frame's (a capture all the same)
 *     --stale-hud N    every Nth frame draws its HUD pass into a layer of its own and comes without the
 *                      frame copy; that layer is destroyed, and the next frame recorded into the same
 *                      command buffer brings a frame copy without a pass (what the game does when the HUD
 *                      goes away between two frames: the copy must not be taken for that old pass's)
 *     --dup N          every Nth frame is presented twice
 *     --early          the hand-over is recorded before the frame's submit (a chain that is one off)
 *     --fifo           FIFO present mode (default: mailbox when there is one)
 *     --unprotected    nothing keeps the shared image from being overwritten before it is copied out
 *     --engine NAME    the engine's name the instance is made with ("StarEngine": the game's own renderer,
 *                      whose swapchain the layer must leave as it is)
 *     --stall N MS     the queue thread stops for MS milliseconds at frame N (a hitch)
 *     --gpu-load N     N scaled 4K blits in every frame: a game whose frame rate the GPU sets
 *     --wild           one of the background's stripes, which stands still, comes with vectors that say
 *                      it moves by (40, -30) pixels a frame (the layer's vector check must put that right)
 *     --replay PREFIX  the pictures and inputs of a dump the layer wrote (PREFIX-previous.ppm, -real.ppm,
 *                      -inputs-WxH.bin) instead of the drawn scene: the two pictures in turn, at their own
 *                      size, each with the dump's vectors and depth
 *     --marker VX VY   a HUD that is not empty: a marker (a frame with a block and four bars in it, magenta)
 *                      that moves by (VX, VY) pixels a frame and turns round at the edges, and things that
 *                      stay where they are (two lines and a block, cyan, which the marker's way crosses).
 *                      In the HUD layer the marker is opaque and the rest half transparent; all of it goes
 *                      over the frame after the copy of it. The scene's top and bottom 56 rows, which the
 *                      marker never reaches, hold every grey from black to white (see --hud-files)
 *     --hud-files PREFIX N COUNT   the HUD layer is, in turn, each of COUNT captures of the game's HUD layer
 *                      (PREFIX-NNN-hud-WxH.bin from NNN = N on, RGBA16F, as "sc-turbo fg-dump COUNT" writes
 *                      them in a row), at their own size; the scene is one plain grey and the frame that grey
 *                      with the capture's HUD over it in white, so that the HUD of a generated frame can be
 *                      read off its picture (tools/hud-motion-check.py). 96 rows of the scene, those
 *                      where the captures have the least HUD, hold every grey from black to white
 *                      instead: the layer measures the game's tone curve on the scene and takes the HUD
 *                      layer only once it has one
 */
#define _GNU_SOURCE
#include <glob.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>
#include "xdg-shell-client-protocol.h"

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s -> %d\n", #x, r_); exit(1); } } while (0)
static int W = 1280, H = 720, RW = 640, RH = 360; /* the window and the size "DLSS" renders at */
#define RING 8
#define LATENCY 3

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
static void sleep_ms(double ms)
{
    if (ms <= 0) return;
    struct timespec t = {(time_t)(ms / 1000), (long)((ms - 1000.0 * (long)(ms / 1000)) * 1e6)};
    nanosleep(&t, NULL);
}

/* ---- wayland ---- */
static struct wl_compositor *compositor;
static struct xdg_wm_base *wm_base;
static void wm_ping(void *data, struct xdg_wm_base *base, uint32_t serial) { xdg_wm_base_pong(base, serial); }
static const struct xdg_wm_base_listener wm_listener = {wm_ping};
static void reg_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version)
{
    if (!strcmp(iface, "wl_compositor")) compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, "xdg_wm_base")) {
        wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
    }
}
static void reg_remove(void *data, struct wl_registry *reg, uint32_t name) {}
static const struct wl_registry_listener reg_listener = {reg_global, reg_remove};
static void surf_configure(void *data, struct xdg_surface *s, uint32_t serial) { xdg_surface_ack_configure(s, serial); }
static const struct xdg_surface_listener surf_listener = {surf_configure};
static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *s) {}
static void top_close(void *d, struct xdg_toplevel *t) {}
static const struct xdg_toplevel_listener top_listener = {top_configure, top_close};

/* ---- vulkan helpers ---- */
static VkPhysicalDevice pd;
static VkPhysicalDeviceMemoryProperties mp;

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t t = 0; t < mp.memoryTypeCount; t++)
        if ((bits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & want) == want) return t;
    fprintf(stderr, "no memory type\n");
    exit(1);
}

struct image { VkImage img; VkDeviceMemory mem; VkDeviceSize size; };

/* share: 0 plain, 1 exportable, 2 import from fd */
static struct image make_image(VkDevice dev, VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags usage, int share, int fd,
                               VkDeviceSize import_size)
{
    struct image im = {0};
    VkExternalMemoryImageCreateInfo ext = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = share ? &ext : NULL, .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .usage = usage};
    CHECK(vkCreateImage(dev, &ci, NULL, &im.img));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, im.img, &mr);
    VkExportMemoryAllocateInfo exp = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    VkImportMemoryFdInfoKHR imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, .fd = fd};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = share == 1 ? (void *)&exp : share == 2 ? (void *)&imp : NULL,
        .allocationSize = share == 2 ? import_size : mr.size, .memoryTypeIndex = mem_type(mr.memoryTypeBits, 0)};
    CHECK(vkAllocateMemory(dev, &ai, NULL, &im.mem));
    CHECK(vkBindImageMemory(dev, im.img, im.mem, 0));
    im.size = mr.size;
    return im;
}

struct buffer { VkBuffer buf; VkDeviceMemory mem; void *data; };

/* a buffer the program can write and images can be filled from */
static struct buffer make_buffer(VkDevice dev, VkDeviceSize size)
{
    struct buffer b = {0};
    VkBufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    CHECK(vkCreateBuffer(dev, &ci, NULL, &b.buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, b.buf, &mr);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    CHECK(vkAllocateMemory(dev, &ai, NULL, &b.mem));
    CHECK(vkBindBufferMemory(dev, b.buf, b.mem, 0));
    CHECK(vkMapMemory(dev, b.mem, 0, size, 0, &b.data));
    return b;
}

/* the size of a P6 picture */
static void ppm_size(const char *path, int *w, int *h)
{
    FILE *f = fopen(path, "rb");
    int max = 0;
    if (!f || fscanf(f, "P6 %d %d %d", w, h, &max) != 3 || max != 255) { fprintf(stderr, "%s: not a P6 picture\n", path); exit(1); }
    fclose(f);
}

/* a P6 picture of w x h as RGBA bytes */
static struct buffer load_ppm(VkDevice dev, const char *path, int w, int h)
{
    FILE *f = fopen(path, "rb");
    int pw = 0, ph = 0, max = 0;
    if (!f || fscanf(f, "P6 %d %d %d", &pw, &ph, &max) != 3 || pw != w || ph != h) { fprintf(stderr, "%s: not a %dx%d P6 picture\n", path, w, h); exit(1); }
    fgetc(f); /* the one white space behind the header */
    struct buffer b = make_buffer(dev, (VkDeviceSize)w * h * 4);
    unsigned char *row = malloc((size_t)w * 3), *out = b.data;
    for (int y = 0; y < h; y++) {
        if (fread(row, 3, (size_t)w, f) != (size_t)w) { fprintf(stderr, "%s: too short\n", path); exit(1); }
        for (int x = 0; x < w; x++, out += 4) { out[0] = row[x * 3]; out[1] = row[x * 3 + 1]; out[2] = row[x * 3 + 2]; out[3] = 255; }
    }
    free(row);
    fclose(f);
    return b;
}

/* a half-precision number */
static float half_value(unsigned short v)
{
    const int exponent = (v >> 10) & 31, mantissa = v & 1023;
    float value;
    if (exponent == 0) value = mantissa / 1024.0f / 16384.0f;
    else if (exponent == 31) value = mantissa ? 0.0f : 65504.0f; /* (not a number: nothing; without end: the largest) */
    else { value = 1.0f + mantissa / 1024.0f; for (int e = exponent; e > 15; e--) value *= 2; for (int e = exponent; e < 15; e++) value *= 0.5f; }
    return v & 0x8000 ? -value : value;
}

static void fill(VkCommandBuffer cb, VkBuffer from, VkImage to, VkImageAspectFlags aspect, int w, int h)
{
    VkBufferImageCopy r = {.imageSubresource = {aspect, 0, 0, 1}, .imageExtent = {(uint32_t)w, (uint32_t)h, 1}};
    vkCmdCopyBufferToImage(cb, from, to, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkImageAspectFlags aspect)
{
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
        .subresourceRange = {aspect, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

static void copy(VkCommandBuffer cb, VkImage from, VkImage to, int32_t sx, int32_t dx, uint32_t w, uint32_t h)
{
    VkImageCopy r = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffset = {sx, 0, 0},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffset = {dx, 0, 0}, .extent = {w, h, 1}};
    vkCmdCopyImage(cb, from, VK_IMAGE_LAYOUT_GENERAL, to, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
}

/* ---- the queue thread ---- */
struct job { VkCommandBuffer cb; int bb; VkFence frame_done; int twice; uint64_t number; };
static struct job jobs[16];
static int job_head, job_tail, in_flight, quit;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_ready = PTHREAD_COND_INITIALIZER, job_done = PTHREAD_COND_INITIALIZER;

static VkDevice dev_a, dev_b;
static VkQueue queue_b;
static VkSwapchainKHR swapchain;
static VkImage swap_images[16];
static uint32_t swap_count;
static struct image backbuffers[RING];
static VkCommandBuffer blit_cbs[RING];
static VkSemaphore acquired[RING], rendered[RING];
static double worker_ms, stall_ms;
static unsigned presents, stall_frame;
/* the stage in front of the game thread (--sim-ms) */
static double sim_ms, sim_jitter_ms, sim_change_s, sim_change_ms, sim_dip_s, sim_dip_length_ms, sim_dip_ms, sim_start;
static int sim_ready; /* frames prepared and not taken yet: 0 or 1 */
static pthread_cond_t sim_cond = PTHREAD_COND_INITIALIZER;
static VkSemaphore copied_b; /* timeline: the count of frames copied out of the shared image */
static int unprotected;

static void present_once(int bb)
{
    static unsigned n;
    const unsigned k = n++ % RING;
    uint32_t index = 0;
    CHECK(vkAcquireNextImageKHR(dev_b, swapchain, UINT64_MAX, acquired[k], VK_NULL_HANDLE, &index));
    VkCommandBuffer cb = blit_cbs[k];
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkBeginCommandBuffer(cb, &bi));
    barrier(cb, swap_images[index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageBlit r = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {W, H, 1}},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {W, H, 1}}};
    vkCmdBlitImage(cb, backbuffers[bb].img, VK_IMAGE_LAYOUT_GENERAL, swap_images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r, VK_FILTER_NEAREST);
    barrier(cb, swap_images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_ASPECT_COLOR_BIT);
    CHECK(vkEndCommandBuffer(cb));
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &acquired[k], .pWaitDstStageMask = &stage,
        .commandBufferCount = 1, .pCommandBuffers = &cb, .signalSemaphoreCount = 1, .pSignalSemaphores = &rendered[k]};
    CHECK(vkQueueSubmit(queue_b, 1, &si, VK_NULL_HANDLE));
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &rendered[k],
        .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &index};
    VkResult r_ = vkQueuePresentKHR(queue_b, &pi);
    if (r_ != VK_SUCCESS && r_ != VK_SUBOPTIMAL_KHR) { fprintf(stderr, "present -> %d\n", r_); exit(1); }
    presents++;
}

static void *simulation(void *unused)
{
    unsigned state = 777;
    for (;;) {
        const double t = now_s() - sim_start;
        state = state * 1664525u + 1013904223u;
        double ms = (sim_change_s > 0 && t >= sim_change_s ? sim_change_ms : sim_ms) + sim_jitter_ms * ((state >> 8) / 8388608.0 - 1.0);
        if (sim_dip_s > 0 && t >= sim_dip_s && t - sim_dip_s * (long)(t / sim_dip_s) < sim_dip_length_ms / 1000.0) ms += sim_dip_ms;
        sleep_ms(ms);
        pthread_mutex_lock(&lock);
        sim_ready = 1;
        pthread_cond_broadcast(&sim_cond);
        while (sim_ready && !quit) pthread_cond_wait(&sim_cond, &lock); /* until the game thread has taken it */
        const int done = quit;
        pthread_mutex_unlock(&lock);
        if (done) return NULL;
    }
}

static void *worker(void *unused)
{
    for (;;) {
        pthread_mutex_lock(&lock);
        while (job_head == job_tail && !quit) pthread_cond_wait(&job_ready, &lock);
        if (job_head == job_tail) { pthread_mutex_unlock(&lock); return NULL; }
        struct job j = jobs[job_head++ % 16];
        pthread_mutex_unlock(&lock);
        /* like D3D12's Wait on a fence shared with the renderer: the frame must be finished */
        CHECK(vkWaitForFences(dev_a, 1, &j.frame_done, VK_TRUE, UINT64_MAX));
        VkSubmitInfo empty = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
        CHECK(vkQueueSubmit(queue_b, 1, &empty, VK_NULL_HANDLE));
        VkTimelineSemaphoreSubmitInfo ts = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, .signalSemaphoreValueCount = 1,
            .pSignalSemaphoreValues = &j.number};
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = unprotected ? NULL : &ts, .commandBufferCount = 1, .pCommandBuffers = &j.cb,
            .signalSemaphoreCount = unprotected ? 0 : 1, .pSignalSemaphores = &copied_b};
        CHECK(vkQueueSubmit(queue_b, 1, &si, VK_NULL_HANDLE));
        sleep_ms(worker_ms);
        if (stall_ms > 0 && j.number == stall_frame) sleep_ms(stall_ms);
        present_once(j.bb);
        if (j.twice) present_once(j.bb);
        pthread_mutex_lock(&lock);
        in_flight--;
        pthread_cond_signal(&job_done);
        pthread_mutex_unlock(&lock);
    }
}

/* --marker: the rectangles of the marker (in its box of 72 x 48) and of what stays in place: x, y, w, h */
static const int marker_rects[][4] = {{0, 0, 72, 3}, {0, 45, 72, 3}, {0, 3, 3, 42}, {69, 3, 3, 42}, {12, 12, 20, 24},
    {40, 14, 3, 20}, {46, 14, 3, 20}, {52, 14, 3, 20}, {58, 14, 3, 20}};
static const int static_rects[][4] = {{40, 40, 200, 3}, {40, 40, 3, 100}, {300, 200, 120, 60}};

/* v, kept between 0 and range by turning round at the ends */
static int bounce(long v, int range)
{
    long m = v % (2L * range);
    if (m < 0) m += 2L * range;
    return (int)(m < range ? m : 2L * range - m);
}

/* rectangles of one colour: from the corner of `from` (as large as the largest of them) into `to` at (ox, oy) */
static void copy_rects(VkCommandBuffer cb, VkImage from, VkImage to, const int rects[][4], int count, int ox, int oy)
{
    VkImageCopy r[16];
    for (int i = 0; i < count; i++)
        r[i] = (VkImageCopy){.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .dstOffset = {ox + rects[i][0], oy + rects[i][1], 0}, .extent = {(uint32_t)rects[i][2], (uint32_t)rects[i][3], 1}};
    vkCmdCopyImage(cb, from, VK_IMAGE_LAYOUT_GENERAL, to, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)count, r);
}

int main(int argc, char **argv)
{
    double seconds = 12, game_ms = 4, jitter_ms = 0, spike_ms = 0;
    unsigned spike = 0, random_state = 12345;
    int stale_hud = 0;
    int gap = 0, split_hud = 0;
    int nohud = 0, dup = 0, early = 0, fifo = 0, gpu_load = 0, wild = 0, marker = 0, marker_vx = 0, marker_vy = 0;
    const char *replay = NULL, *hud_files = NULL, *engine = NULL;
    int hud_first = 0, hud_count = 0;
    char replay_inputs[600] = "";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game-ms") && i + 1 < argc) game_ms = atof(argv[++i]);
        else if (!strcmp(argv[i], "--worker-ms") && i + 1 < argc) worker_ms = atof(argv[++i]);
        else if (!strcmp(argv[i], "--jitter") && i + 1 < argc) jitter_ms = atof(argv[++i]);
        else if (!strcmp(argv[i], "--spike") && i + 2 < argc) { spike = (unsigned)atoi(argv[++i]); spike_ms = atof(argv[++i]); }
        else if (!strcmp(argv[i], "--sim-ms") && i + 1 < argc) sim_ms = atof(argv[++i]);
        else if (!strcmp(argv[i], "--sim-jitter") && i + 1 < argc) sim_jitter_ms = atof(argv[++i]);
        else if (!strcmp(argv[i], "--sim-change") && i + 2 < argc) { sim_change_s = atof(argv[++i]); sim_change_ms = atof(argv[++i]); }
        else if (!strcmp(argv[i], "--sim-dip") && i + 3 < argc) { sim_dip_s = atof(argv[++i]); sim_dip_length_ms = atof(argv[++i]); sim_dip_ms = atof(argv[++i]); }
        else if (!strcmp(argv[i], "--nohud") && i + 1 < argc) nohud = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--gap") && i + 1 < argc) gap = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--split-hud") && i + 1 < argc) split_hud = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--stale-hud") && i + 1 < argc) stale_hud = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dup") && i + 1 < argc) dup = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--early")) early = 1;
        else if (!strcmp(argv[i], "--fifo")) fifo = 1;
        else if (!strcmp(argv[i], "--gpu-load") && i + 1 < argc) gpu_load = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--unprotected")) unprotected = 1;
        else if (!strcmp(argv[i], "--engine") && i + 1 < argc) engine = argv[++i];
        else if (!strcmp(argv[i], "--wild")) wild = 1;
        else if (!strcmp(argv[i], "--replay") && i + 1 < argc) replay = argv[++i];
        else if (!strcmp(argv[i], "--marker") && i + 2 < argc) { marker = 1; marker_vx = atoi(argv[++i]); marker_vy = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--hud-files") && i + 3 < argc) { hud_files = argv[++i]; hud_first = atoi(argv[++i]); hud_count = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--stall") && i + 2 < argc) { stall_frame = (unsigned)atoi(argv[++i]); stall_ms = atof(argv[++i]); }
        else seconds = atof(argv[i]);
    }

    if (replay) { /* the sizes are the dump's */
        char path[600];
        snprintf(path, sizeof path, "%s-real.ppm", replay);
        ppm_size(path, &W, &H);
        snprintf(path, sizeof path, "%s-inputs-*.bin", replay);
        glob_t found;
        const char *size;
        if (glob(path, 0, NULL, &found) || found.gl_pathc != 1 || !(size = strstr(found.gl_pathv[0], "-inputs-")) ||
            sscanf(size, "-inputs-%dx%d.bin", &RW, &RH) != 2) { fprintf(stderr, "%s: no inputs file\n", path); return 1; }
        snprintf(replay_inputs, sizeof replay_inputs, "%s", found.gl_pathv[0]);
        globfree(&found);
        printf("replay: %s, %dx%d, inputs %dx%d\n", replay, W, H, RW, RH);
    }

    char hud_paths[16][600];
    if (hud_files) { /* the size is the captures' */
        if (hud_count < 2 || hud_count > 16) { fprintf(stderr, "--hud-files: 2 to 16 captures\n"); return 1; }
        for (int k = 0; k < hud_count; k++) {
            char path[600];
            snprintf(path, sizeof path, "%s-%03d-hud-*.bin", hud_files, hud_first + k);
            glob_t found;
            const char *size;
            int w = 0, h = 0;
            if (glob(path, 0, NULL, &found) || found.gl_pathc != 1 || !(size = strstr(found.gl_pathv[0], "-hud-")) ||
                sscanf(size, "-hud-%dx%d.bin", &w, &h) != 2 || (k && (w != W || h != H))) { fprintf(stderr, "%s: no capture (of the first one's size)\n", path); return 1; }
            W = w; H = h; RW = W / 2; RH = H / 2;
            snprintf(hud_paths[k], sizeof hud_paths[k], "%s", found.gl_pathv[0]);
            globfree(&found);
        }
        printf("hud-files: %d captures from %s, %dx%d\n", hud_count, hud_paths[0], W, H);
    }

    struct wl_display *display = wl_display_connect(NULL);
    if (!display) { fprintf(stderr, "no wayland display\n"); return 1; }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &reg_listener, NULL);
    wl_display_roundtrip(display);
    if (!compositor || !wm_base) { fprintf(stderr, "no compositor / xdg_wm_base\n"); return 1; }
    struct wl_surface *surface = wl_compositor_create_surface(compositor);
    struct xdg_surface *xsurface = xdg_wm_base_get_xdg_surface(wm_base, surface);
    xdg_surface_add_listener(xsurface, &surf_listener, NULL);
    struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xsurface);
    xdg_toplevel_add_listener(toplevel, &top_listener, NULL);
    xdg_toplevel_set_title(toplevel, "test_chain");
    wl_surface_commit(surface);
    wl_display_roundtrip(display);

    const char *iexts[] = {"VK_KHR_surface", "VK_KHR_wayland_surface"};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pEngineName = engine, .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app, .enabledExtensionCount = 2, .ppEnabledExtensionNames = iexts};
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, NULL, &inst));
    VkPhysicalDevice pds[8];
    uint32_t n = 8;
    vkEnumeratePhysicalDevices(inst, &n, pds);
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pds[i], &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { pd = pds[i]; printf("device: %s\n", p.deviceName); }
    }
    if (!pd) { fprintf(stderr, "no discrete GPU (weston must run with --renderer=gl)\n"); return 1; }
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkWaylandSurfaceCreateInfoKHR wci = {.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR, .display = display, .surface = surface};
    VkSurfaceKHR vsurface;
    CHECK(vkCreateWaylandSurfaceKHR(inst, &wci, NULL, &vsurface));

    float prio = 1;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
    const char *exts_a[] = {"VK_KHR_external_memory_fd", "VK_KHR_external_semaphore_fd"};
    const char *exts_b[] = {"VK_KHR_external_memory_fd", "VK_KHR_external_semaphore_fd", "VK_KHR_swapchain"};
    VkPhysicalDeviceTimelineSemaphoreFeatures tsf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, .timelineSemaphore = VK_TRUE};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &tsf, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
        .enabledExtensionCount = 2, .ppEnabledExtensionNames = exts_a};
    CHECK(vkCreateDevice(pd, &dci, NULL, &dev_a));
    dci.enabledExtensionCount = 3;
    dci.ppEnabledExtensionNames = exts_b;
    CHECK(vkCreateDevice(pd, &dci, NULL, &dev_b));
    VkQueue queue_a;
    vkGetDeviceQueue(dev_a, 0, 0, &queue_a);
    vkGetDeviceQueue(dev_b, 0, 0, &queue_b);

    /* swapchain on B */
    VkSurfaceCapabilitiesKHR caps;
    CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, vsurface, &caps));
    VkSurfaceFormatKHR formats[64];
    uint32_t nf = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, vsurface, &nf, formats);
    VkSurfaceFormatKHR format = formats[0];
    for (uint32_t i = 0; i < nf; i++)
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) { format = formats[i]; break; }
    VkPresentModeKHR modes[16], mode = VK_PRESENT_MODE_FIFO_KHR;
    uint32_t nm = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, vsurface, &nm, modes);
    for (uint32_t i = 0; i < nm && !fifo; i++)
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = modes[i];
    VkSwapchainCreateInfoKHR sci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = vsurface,
        .minImageCount = caps.minImageCount < 3 ? 3 : caps.minImageCount, .imageFormat = format.format, .imageColorSpace = format.colorSpace,
        .imageExtent = {W, H}, .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = mode, .clipped = VK_TRUE};
    CHECK(vkCreateSwapchainKHR(dev_b, &sci, NULL, &swapchain));
    swap_count = 16;
    vkGetSwapchainImagesKHR(dev_b, swapchain, &swap_count, swap_images);
    printf("swapchain: format %d, mode %d, %u images\n", format.format, mode, swap_count);

    /* images on A */
    const VkImageUsageFlags xfer = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    struct image vec = make_image(dev_a, VK_FORMAT_R16G16_SFLOAT, RW, RH, xfer | VK_IMAGE_USAGE_SAMPLED_BIT, 0, -1, 0);
    struct image depth = make_image(dev_a, VK_FORMAT_D32_SFLOAT_S8_UINT, RW, RH, xfer | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0, -1, 0);
    struct image bg = make_image(dev_a, VK_FORMAT_R8G8B8A8_UNORM, W, H, xfer, 0, -1, 0);
    struct image bar = make_image(dev_a, VK_FORMAT_R8G8B8A8_UNORM, 64, H, xfer, 0, -1, 0);
    struct image f0 = make_image(dev_a, VK_FORMAT_R8G8B8A8_UNORM, W, H, xfer, 0, -1, 0);
    struct image f1 = make_image(dev_a, VK_FORMAT_R8G8B8A8_UNORM, W, H, xfer, 0, -1, 0);
    struct image hud = make_image(dev_a, VK_FORMAT_R16G16B16A16_SFLOAT, W, H, xfer | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, -1, 0);
    struct image shared_a = make_image(dev_a, VK_FORMAT_R8G8B8A8_UNORM, W, H, xfer, 1, -1, 0);
    struct image load0 = make_image(dev_a, VK_FORMAT_R8G8B8A8_UNORM, 3840, 2160, xfer, 0, -1, 0);
    struct image load1 = make_image(dev_a, VK_FORMAT_R8G8B8A8_UNORM, 3840, 2160, xfer, 0, -1, 0);
    struct image wild_vec = make_image(dev_a, VK_FORMAT_R16G16_SFLOAT, 12, RH, xfer, 0, -1, 0);
    /* --marker: what is copied into the HUD layer (opaque), and the two colours that go over the frame */
    struct image hud_solid = make_image(dev_a, VK_FORMAT_R16G16B16A16_SFLOAT, 256, 128, xfer, 0, -1, 0);
    struct image hud_half = make_image(dev_a, VK_FORMAT_R16G16B16A16_SFLOAT, 256, 128, xfer, 0, -1, 0);
    struct image marker_colour = make_image(dev_a, VK_FORMAT_R8G8B8A8_UNORM, 256, 128, xfer, 0, -1, 0);
    struct image static_colour = make_image(dev_a, VK_FORMAT_R8G8B8A8_UNORM, 256, 128, xfer, 0, -1, 0);
    struct image pictures[2] = {{0}};
    struct buffer picture_data[2] = {{0}}, vector_data = {0}, depth_data = {0};
    if (replay) {
        char path[600];
        for (int i = 0; i < 2; i++) {
            snprintf(path, sizeof path, "%s-%s.ppm", replay, i ? "real" : "previous");
            picture_data[i] = load_ppm(dev_a, path, W, H);
            pictures[i] = make_image(dev_a, VK_FORMAT_R8G8B8A8_UNORM, W, H, xfer, 0, -1, 0);
        }
        const size_t bytes = (size_t)RW * RH * 4;
        vector_data = make_buffer(dev_a, bytes);
        depth_data = make_buffer(dev_a, bytes);
        FILE *f = fopen(replay_inputs, "rb");
        if (!f || fread(vector_data.data, 1, bytes, f) != bytes || fread(depth_data.data, 1, bytes, f) != bytes) {
            fprintf(stderr, "%s: too short\n", replay_inputs);
            return 1;
        }
        fclose(f);
    }
    /* --hud-files: the captures, and the frames that show them (white over the scene's grey) */
    struct buffer hud_data[16] = {{0}}, hud_frames[16] = {{0}}, plain_data = {0};
    static const unsigned char plain[3] = {64, 77, 89};
    if (hud_files) {
        /* (worked out in ordinary memory and copied into the buffers: reading a mapped buffer back is slow) */
        const size_t pixels = (size_t)W * H;
        unsigned short *captures[16] = {0};
        unsigned char *scene = malloc(pixels * 4), *shown = malloc(pixels * 4);
        for (int k = 0; k < hud_count; k++) {
            captures[k] = malloc(pixels * 8);
            FILE *f = fopen(hud_paths[k], "rb");
            if (!f || fread(captures[k], 8, pixels, f) != pixels) { fprintf(stderr, "%s: too short\n", hud_paths[k]); return 1; }
            fclose(f);
            hud_data[k] = make_buffer(dev_a, pixels * 8);
            memcpy(hud_data[k].data, captures[k], pixels * 8);
        }
        /* every grey goes into the 96 rows where the captures have the least HUD */
        int ramp_at = 0;
        unsigned long least = ~0ul;
        unsigned *in_row = calloc((size_t)H, sizeof *in_row);
        for (int k = 0; k < hud_count; k++) {
            for (size_t i = 0; i < pixels; i++) in_row[i / (size_t)W] += (captures[k][i * 4 + 3] & 0x7fff) != 0;
        }
        for (int y = 0; y + 96 <= H; y += 16) {
            unsigned long sum = 0;
            for (int j = 0; j < 96; j++) sum += in_row[y + j];
            if (sum < least) { least = sum; ramp_at = y; }
        }
        free(in_row);
        printf("hud-files: every grey in rows %d to %d (%lu HUD pixels there, all captures)\n", ramp_at, ramp_at + 95, least);
        for (size_t i = 0; i < pixels; i++) {
            unsigned char *out = scene + i * 4;
            const int y = (int)(i / (size_t)W);
            if (y >= ramp_at && y < ramp_at + 96) out[0] = out[1] = out[2] = (unsigned char)((i % (size_t)W) * 256 / (size_t)W);
            else { out[0] = plain[0]; out[1] = plain[1]; out[2] = plain[2]; }
            out[3] = 255;
        }
        plain_data = make_buffer(dev_a, pixels * 4);
        memcpy(plain_data.data, scene, pixels * 4);
        for (int k = 0; k < hud_count; k++) {
            const unsigned short *in = captures[k];
            const unsigned char *under = scene;
            unsigned char *out = shown;
            for (size_t i = 0; i < pixels; i++, in += 4, out += 4, under += 4) {
                float alpha = half_value(in[3]);
                alpha = alpha < 0 ? 0 : alpha > 1 ? 1 : alpha;
                for (int c = 0; c < 3; c++) out[c] = (unsigned char)(under[c] * (1 - alpha) + 255 * alpha + 0.5f);
                out[3] = 255;
            }
            hud_frames[k] = make_buffer(dev_a, pixels * 4);
            memcpy(hud_frames[k].data, shown, pixels * 4);
            free(captures[k]);
        }
        free(scene);
        free(shown);
    }
    if (marker) { /* --marker's scene: the grey with its darker bars, and every grey in the rows the marker never reaches */
        const size_t pixels = (size_t)W * H;
        unsigned char *scene = malloc(pixels * 4);
        for (size_t i = 0; i < pixels; i++) {
            unsigned char *out = scene + i * 4;
            const int x = (int)(i % (size_t)W), y = (int)(i / (size_t)W);
            const int bar_at = x >= 40 ? x - (x - 40) % 160 : -1; /* the bars: 24 wide, every 160 pixels from 40 on */
            if (y < 56 || y >= H - 56) out[0] = out[1] = out[2] = (unsigned char)((long)x * 256 / W);
            else if (bar_at >= 0 && x < bar_at + 24 && bar_at + 24 < W) { out[0] = 20; out[1] = 128; out[2] = 31; }
            else { out[0] = plain[0]; out[1] = plain[1]; out[2] = plain[2]; }
            out[3] = 255;
        }
        plain_data = make_buffer(dev_a, pixels * 4);
        memcpy(plain_data.data, scene, pixels * 4);
        free(scene);
    }
    VkMemoryGetFdInfoKHR gfi = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = shared_a.mem, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    int fd = -1;
    PFN_vkGetMemoryFdKHR get_fd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(dev_a, "vkGetMemoryFdKHR");
    CHECK(get_fd(dev_a, &gfi, &fd));
    struct image shared_b = make_image(dev_b, VK_FORMAT_R8G8B8A8_UNORM, W, H, xfer, 2, fd, shared_a.size);
    for (int i = 0; i < RING; i++) backbuffers[i] = make_image(dev_b, VK_FORMAT_R8G8B8A8_UNORM, W, H, xfer, 0, -1, 0);

    /* the semaphore both devices share: B counts the copies out of the shared image, A waits for them */
    VkSemaphore copied_a;
    {
        VkSemaphoreTypeCreateInfo type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
        VkExportSemaphoreCreateInfo export = {.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO, .pNext = &type,
            .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT};
        VkSemaphoreCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &export};
        CHECK(vkCreateSemaphore(dev_b, &info, NULL, &copied_b));
        VkSemaphoreGetFdInfoKHR get = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR, .semaphore = copied_b,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT};
        int sfd = -1;
        PFN_vkGetSemaphoreFdKHR get_sfd = (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(dev_b, "vkGetSemaphoreFdKHR");
        CHECK(get_sfd(dev_b, &get, &sfd));
        info.pNext = &type;
        CHECK(vkCreateSemaphore(dev_a, &info, NULL, &copied_a));
        VkImportSemaphoreFdInfoKHR import = {.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR, .semaphore = copied_a,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT, .fd = sfd};
        PFN_vkImportSemaphoreFdKHR import_sfd = (PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(dev_a, "vkImportSemaphoreFdKHR");
        CHECK(import_sfd(dev_a, &import));
    }

    /* the views DLSS would be handed: vectors (sampled RG16F) once, the depth every frame */
    VkImageViewCreateInfo vvi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = vec.img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R16G16_SFLOAT, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageViewCreateInfo dvi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = depth.img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_D32_SFLOAT_S8_UINT, .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}};
    VkImageView vec_view, hud_view;
    VkImageViewCreateInfo hvi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = hud.img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    CHECK(vkCreateImageView(dev_a, &hvi, NULL, &hud_view));

    /* the HUD layer's pass: one colour attachment, loaded */
    VkAttachmentDescription ad = {.format = VK_FORMAT_R16G16B16A16_SFLOAT, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL};
    VkAttachmentReference ar = {0, VK_IMAGE_LAYOUT_GENERAL};
    VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ar};
    VkRenderPassCreateInfo rci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ad, .subpassCount = 1, .pSubpasses = &sp};
    VkRenderPass rp;
    CHECK(vkCreateRenderPass(dev_a, &rci, NULL, &rp));
    VkFramebufferCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1, .pAttachments = &hud_view, .width = W, .height = H, .layers = 1};
    VkFramebuffer fb;
    CHECK(vkCreateFramebuffer(dev_a, &fci, NULL, &fb));

    /* command buffers, fences, semaphores */
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    VkCommandPool pool_a, pool_b, pool_w;
    CHECK(vkCreateCommandPool(dev_a, &pci, NULL, &pool_a));
    CHECK(vkCreateCommandPool(dev_b, &pci, NULL, &pool_b));
    CHECK(vkCreateCommandPool(dev_b, &pci, NULL, &pool_w));
    VkCommandBuffer cbs_a[RING + 1], cbs_b[RING], cbs_split[RING];
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool_a, .commandBufferCount = RING + 1};
    CHECK(vkAllocateCommandBuffers(dev_a, &cai, cbs_a));
    cai.commandBufferCount = RING;
    CHECK(vkAllocateCommandBuffers(dev_a, &cai, cbs_split));
    cai.commandPool = pool_b;
    cai.commandBufferCount = RING;
    CHECK(vkAllocateCommandBuffers(dev_b, &cai, cbs_b));
    cai.commandPool = pool_w;
    CHECK(vkAllocateCommandBuffers(dev_b, &cai, blit_cbs));
    VkFence fences[RING];
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    VkSemaphoreCreateInfo semi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (int i = 0; i < RING; i++) {
        CHECK(vkCreateFence(dev_a, &fi, NULL, &fences[i]));
        CHECK(vkCreateSemaphore(dev_b, &semi, NULL, &acquired[i]));
        CHECK(vkCreateSemaphore(dev_b, &semi, NULL, &rendered[i]));
    }

    /* setup on A: a background with structure, a white bar, cleared inputs and HUD layer */
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VkCommandBuffer cb = cbs_a[RING];
    CHECK(vkBeginCommandBuffer(cb, &bi));
    const VkImageSubresourceRange colour = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const VkImageSubresourceRange ds = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
    VkImage colours[] = {vec.img, bg.img, bar.img, f0.img, f1.img, hud.img, shared_a.img, load0.img, load1.img, wild_vec.img,
        hud_solid.img, hud_half.img, marker_colour.img, static_colour.img};
    for (unsigned i = 0; i < sizeof colours / sizeof *colours; i++)
        barrier(cb, colours[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);
    barrier(cb, depth.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    VkClearColorValue zero = {{0, 0, 0, 0}}, white = {{1, 1, 1, 1}}, grey = {{0.25f, 0.3f, 0.35f, 1}};
    VkClearDepthStencilValue half = {0.5f, 0};
    vkCmdClearColorImage(cb, vec.img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &colour);
    vkCmdClearColorImage(cb, hud.img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &colour);
    vkCmdClearColorImage(cb, bar.img, VK_IMAGE_LAYOUT_GENERAL, &white, 1, &colour);
    vkCmdClearColorImage(cb, bg.img, VK_IMAGE_LAYOUT_GENERAL, &grey, 1, &colour);
    vkCmdClearColorImage(cb, shared_a.img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &colour);
    vkCmdClearColorImage(cb, load0.img, VK_IMAGE_LAYOUT_GENERAL, &grey, 1, &colour);
    vkCmdClearColorImage(cb, load1.img, VK_IMAGE_LAYOUT_GENERAL, &grey, 1, &colour);
    VkClearColorValue solid = {{0.5f, 0.5f, 0.5f, 1}}, half_solid = {{0.25f, 0.25f, 0.25f, 0.5f}}, magenta = {{1, 0.2f, 0.85f, 1}},
        cyan = {{0.1f, 0.85f, 0.9f, 1}};
    vkCmdClearColorImage(cb, hud_solid.img, VK_IMAGE_LAYOUT_GENERAL, &solid, 1, &colour);
    vkCmdClearColorImage(cb, hud_half.img, VK_IMAGE_LAYOUT_GENERAL, &half_solid, 1, &colour);
    vkCmdClearColorImage(cb, marker_colour.img, VK_IMAGE_LAYOUT_GENERAL, &magenta, 1, &colour);
    vkCmdClearColorImage(cb, static_colour.img, VK_IMAGE_LAYOUT_GENERAL, &cyan, 1, &colour);
    vkCmdClearDepthStencilImage(cb, depth.img, VK_IMAGE_LAYOUT_GENERAL, &half, 1, &ds);
    if (wild) { /* the second stripe (200..224 of the window, 100..112 of the vectors) */
        VkClearColorValue moving = {{40, -30, 0, 0}};
        vkCmdClearColorImage(cb, wild_vec.img, VK_IMAGE_LAYOUT_GENERAL, &moving, 1, &colour);
        copy(cb, wild_vec.img, vec.img, 0, 100, 12, RH);
    }
    if (hud_files || marker) fill(cb, plain_data.buf, bg.img, VK_IMAGE_ASPECT_COLOR_BIT, W, H);
    if (replay) {
        for (int i = 0; i < 2; i++) {
            barrier(cb, pictures[i].img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);
            fill(cb, picture_data[i].buf, pictures[i].img, VK_IMAGE_ASPECT_COLOR_BIT, W, H);
        }
        fill(cb, vector_data.buf, vec.img, VK_IMAGE_ASPECT_COLOR_BIT, RW, RH);
        fill(cb, depth_data.buf, depth.img, VK_IMAGE_ASPECT_DEPTH_BIT, RW, RH);
    }
    CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    CHECK(vkQueueSubmit(queue_a, 1, &si, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(queue_a));
    /* structure in the background: stripes of other greys, from the bar image scaled down is not possible with
       copies, so: darker bars every 160 pixels, from a cleared strip of the frame image */
    CHECK(vkBeginCommandBuffer(cb, &bi));
    VkClearColorValue dark = {{0.08f, 0.5f, 0.12f, 1}};
    vkCmdClearColorImage(cb, f1.img, VK_IMAGE_LAYOUT_GENERAL, &dark, 1, &colour);
    for (int x = 40; x + 24 < W && !hud_files && !marker; x += 160) copy(cb, f1.img, bg.img, 0, x, 24, H);
    CHECK(vkEndCommandBuffer(cb));
    CHECK(vkQueueSubmit(queue_a, 1, &si, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(queue_a));
    CHECK(vkCreateImageView(dev_a, &vvi, NULL, &vec_view));
    /* setup on B */
    {
        VkCommandBuffer cb2 = cbs_b[0];
        CHECK(vkBeginCommandBuffer(cb2, &bi));
        barrier(cb2, shared_b.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);
        for (int i = 0; i < RING; i++) barrier(cb2, backbuffers[i].img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);
        CHECK(vkEndCommandBuffer(cb2));
        VkSubmitInfo s2 = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb2};
        CHECK(vkQueueSubmit(queue_b, 1, &s2, VK_NULL_HANDLE));
        CHECK(vkQueueWaitIdle(queue_b));
    }

    pthread_t thread, sim_thread;
    pthread_create(&thread, NULL, worker, NULL);
    const double start = now_s();
    sim_start = start;
    if (sim_ms > 0) pthread_create(&sim_thread, NULL, simulation, NULL);
    unsigned frame = 0;
    struct image stale_img = {0}; /* --stale-hud: the layer of a pass that was left without its frame copy */
    VkImageView stale_view = VK_NULL_HANDLE;
    VkFramebuffer stale_fb = VK_NULL_HANDLE;
    int stale_slot = -1;
    unsigned stale_count = 0;
    double last_report = start;
    unsigned last_frames = 0;
    while (now_s() - start < seconds) {
        const unsigned slot = frame % RING;
        const double frame_start = now_s();
        if (sim_ms > 0) { /* the frame the stage in front has prepared */
            pthread_mutex_lock(&lock);
            while (!sim_ready) pthread_cond_wait(&sim_cond, &lock);
            sim_ready = 0;
            pthread_cond_broadcast(&sim_cond);
            pthread_mutex_unlock(&lock);
        }
        { /* the game's own work */
            random_state = random_state * 1664525u + 1013904223u;
            double own = game_ms + jitter_ms * ((random_state >> 8) / 8388608.0 - 1.0);
            if (spike && frame % spike == spike - 1) own += spike_ms;
            if (own > 0) sleep_ms(own);
        }
        /* --- device A: the frame --- */
        CHECK(vkWaitForFences(dev_a, 1, &fences[slot], VK_TRUE, UINT64_MAX));
        CHECK(vkResetFences(dev_a, 1, &fences[slot]));
        /* --stale-hud: this command buffer's pass of RING frames ago has no layer any more */
        const int stale_use = stale_slot == (int)slot;
        const int stale_now = !stale_use && stale_hud && stale_slot < 0 && frame % stale_hud == (unsigned)stale_hud - 1;
        if (stale_use) {
            vkDestroyFramebuffer(dev_a, stale_fb, NULL);
            vkDestroyImageView(dev_a, stale_view, NULL);
            vkDestroyImage(dev_a, stale_img.img, NULL);
            vkFreeMemory(dev_a, stale_img.mem, NULL);
            stale_slot = -1;
            stale_count++;
        }
        VkImageView depth_view; /* a new evaluation begins */
        CHECK(vkCreateImageView(dev_a, &dvi, NULL, &depth_view));
        cb = cbs_a[slot];
        CHECK(vkBeginCommandBuffer(cb, &bi));
        for (int i = 0; i < gpu_load; i++) { /* the GPU's work for the frame */
            VkImageBlit lb = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {1920, 1080, 1}},
                .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {3840, 2160, 1}}};
            vkCmdBlitImage(cb, (i & 1 ? load1 : load0).img, VK_IMAGE_LAYOUT_GENERAL, (i & 1 ? load0 : load1).img, VK_IMAGE_LAYOUT_GENERAL, 1, &lb, VK_FILTER_LINEAR);
        }
        if (replay) {
            copy(cb, pictures[frame & 1].img, f0.img, 0, 0, W, H);
        } else {
            copy(cb, bg.img, f0.img, 0, 0, W, H);
            if (!hud_files) copy(cb, bar.img, f0.img, 0, (int32_t)((frame * 12) % (W - 64)), 64, H);
        }
        const int with_hud = !(nohud && frame % nohud == (unsigned)nohud - 1);
        const int gap_now = gap && frame % gap == (unsigned)gap - 1;
        const int split_now = split_hud && frame % split_hud == (unsigned)split_hud - 1;
        /* --marker: where the marker is in this frame */
        const int marker_x = 60 + bounce((long)frame * marker_vx, W - 72 - 120), marker_y = 60 + bounce((long)frame * marker_vy, H - 48 - 120);
        const int marker_count = (int)(sizeof marker_rects / sizeof *marker_rects), static_count = (int)(sizeof static_rects / sizeof *static_rects);
        if (stale_now) { /* the HUD pass into a layer of its own, and no frame copy after it */
            stale_img = make_image(dev_a, VK_FORMAT_R16G16B16A16_SFLOAT, W, H, xfer | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0, -1, 0);
            hvi.image = stale_img.img;
            CHECK(vkCreateImageView(dev_a, &hvi, NULL, &stale_view));
            hvi.image = hud.img;
            fci.pAttachments = &stale_view;
            CHECK(vkCreateFramebuffer(dev_a, &fci, NULL, &stale_fb));
            fci.pAttachments = &hud_view;
            VkImageMemoryBarrier sb = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = stale_img.img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &sb);
            VkRenderPassBeginInfo rb = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = stale_fb, .renderArea = {{0, 0}, {W, H}}};
            vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdEndRenderPass(cb);
            stale_slot = (int)slot;
        } else if (stale_use) {
            copy(cb, f0.img, f1.img, 0, 0, W, H); /* a frame copy with no pass before it in this recording */
        } else if (with_hud) {
            if (hud_files) fill(cb, hud_data[frame % (unsigned)hud_count].buf, hud.img, VK_IMAGE_ASPECT_COLOR_BIT, W, H);
            if (marker) { /* the HUD layer as the game would have drawn it */
                vkCmdClearColorImage(cb, hud.img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &colour);
                copy_rects(cb, hud_half.img, hud.img, static_rects, static_count, 0, 0);
                copy_rects(cb, hud_solid.img, hud.img, marker_rects, marker_count, marker_x, marker_y);
            }
            VkRenderPassBeginInfo rb = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb, .renderArea = {{0, 0}, {W, H}}};
            if (split_now) {
                CHECK(vkBeginCommandBuffer(cbs_split[slot], &bi));
                vkCmdBeginRenderPass(cbs_split[slot], &rb, VK_SUBPASS_CONTENTS_INLINE);
                vkCmdEndRenderPass(cbs_split[slot]);
                CHECK(vkEndCommandBuffer(cbs_split[slot]));
            } else if (!gap_now) {
                vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
                vkCmdEndRenderPass(cb);
            }
            if (!gap_now) copy(cb, f0.img, f1.img, 0, 0, W, H); /* the frame set aside before the HUD goes over it */
            if (hud_files) fill(cb, hud_frames[frame % (unsigned)hud_count].buf, f0.img, VK_IMAGE_ASPECT_COLOR_BIT, W, H);
            if (marker) { /* ... which it does here */
                copy_rects(cb, static_colour.img, f0.img, static_rects, static_count, 0, 0);
                copy_rects(cb, marker_colour.img, f0.img, marker_rects, marker_count, marker_x, marker_y);
            }
        }
        copy(cb, f0.img, shared_a.img, 0, 0, W, H);
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 0, NULL); /* "DLSS" */
        CHECK(vkEndCommandBuffer(cb));
        /* the frame waits, on the GPU's side, until the frame before it has been copied out of the shared image */
        const uint64_t copied_before = frame;
        const VkPipelineStageFlags all_stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkTimelineSemaphoreSubmitInfo fts = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, .waitSemaphoreValueCount = 1,
            .pWaitSemaphoreValues = &copied_before};
        VkSubmitInfo fs = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = unprotected ? NULL : &fts, .waitSemaphoreCount = unprotected ? 0 : 1,
            .pWaitSemaphores = &copied_a, .pWaitDstStageMask = &all_stages, .commandBufferCount = 1, .pCommandBuffers = &cb};
        VkCommandBuffer both[2] = {cbs_split[slot], cb};
        if (split_now && with_hud && !stale_now && !stale_use) {
            fs.commandBufferCount = 2;
            fs.pCommandBuffers = both;
        }
        VkCommandBuffer cb2 = cbs_b[slot];
        if (!early) CHECK(vkQueueSubmit(queue_a, 1, &fs, fences[slot]));
        /* --- device B, same thread: the hand-over --- */
        CHECK(vkBeginCommandBuffer(cb2, &bi));
        copy(cb2, shared_b.img, backbuffers[slot].img, 0, 0, W, H);
        CHECK(vkEndCommandBuffer(cb2));
        if (early) CHECK(vkQueueSubmit(queue_a, 1, &fs, fences[slot]));
        vkDestroyImageView(dev_a, depth_view, NULL);
        /* --- to the queue thread; at most LATENCY frames on their way --- */
        pthread_mutex_lock(&lock);
        jobs[job_tail++ % 16] = (struct job){cb2, (int)slot, fences[slot], dup && frame % dup == (unsigned)dup - 1, frame + 1};
        in_flight++;
        pthread_cond_signal(&job_ready);
        while (in_flight >= LATENCY) pthread_cond_wait(&job_done, &lock);
        pthread_mutex_unlock(&lock);
        frame++;
        wl_display_dispatch_pending(display);
        wl_display_flush(display);
        const double t = now_s();
        if (t - last_report >= 2.0) {
            printf("%5.1f s: %.1f frames/s, %u presents so far, frame time %.1f ms\n", t - start, (frame - last_frames) / (t - last_report), presents, (t - frame_start) * 1000);
            fflush(stdout);
            last_report = t;
            last_frames = frame;
        }
    }
    pthread_mutex_lock(&lock);
    quit = 1;
    pthread_cond_signal(&job_ready);
    pthread_cond_broadcast(&sim_cond);
    pthread_mutex_unlock(&lock);
    pthread_join(thread, NULL);
    if (sim_ms > 0) pthread_join(sim_thread, NULL);
    vkDeviceWaitIdle(dev_a);
    vkDeviceWaitIdle(dev_b);
    if (stale_hud) printf("stale-hud: %u passes left without their frame copy, their layers destroyed\n", stale_count);
    printf("%u frames, %u presents\n", frame, presents);
    vkDestroySwapchainKHR(dev_b, swapchain, NULL);
    vkDestroyDevice(dev_b, NULL);
    vkDestroyDevice(dev_a, NULL);
    vkDestroySurfaceKHR(inst, vsurface, NULL);
    vkDestroyInstance(inst, NULL);
    return 0;
}
