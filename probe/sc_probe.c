/*
 * sc-probe: a passive, host-side Vulkan layer.
 *
 * It changes nothing: every call is forwarded untouched.  It only writes a log of
 *   - the device extensions the application enables,
 *   - swapchains (format, size, present mode) and the presented frame rate,
 *   - the image views handed to the NVIDIA driver through VK_NVX_image_view_handle
 *     and the kernels launched through VK_NVX_binary_import.  That is how DLSS runs
 *     on Linux, so these views are DLSS's inputs and output: colour, depth, motion
 *     vectors.
 *
 * The question it answers: can a host-side layer see the game's motion vectors and
 * depth without any Windows-side component?
 *
 * Active only when SC_PROBE=1 is in the environment (see the manifest).
 * Log: $SC_PROBE_DIR/probe-<process>-<pid>.log   (default directory: /tmp)
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#define EXPORT __attribute__((visibility("default")))
#define KEY(h) (*(void **)(h))

/* ---------------------------------------------------------------- logging */

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *log_file;
static struct timespec t0;

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9;
}

static void plog(const char *fmt, ...)
{
    va_list ap;
    pthread_mutex_lock(&log_lock);
    if (!log_file) {
        char comm[64] = "unknown", path[512];
        const char *dir = getenv("SC_PROBE_DIR");
        FILE *c = fopen("/proc/self/comm", "r");
        if (c) {
            if (fgets(comm, sizeof comm, c)) comm[strcspn(comm, "\n")] = 0;
            fclose(c);
        }
        for (char *p = comm; *p; p++)
            if (*p == '/' || *p == ' ') *p = '_';
        snprintf(path, sizeof path, "%s/probe-%s-%d.log", dir && *dir ? dir : "/tmp", comm, getpid());
        log_file = fopen(path, "a");
        clock_gettime(CLOCK_MONOTONIC, &t0);
        if (log_file) setvbuf(log_file, NULL, _IOLBF, 0);
    }
    if (log_file) {
        fprintf(log_file, "%10.3f ", now_s());
        va_start(ap, fmt);
        vfprintf(log_file, fmt, ap);
        va_end(ap);
        fputc('\n', log_file);
    }
    pthread_mutex_unlock(&log_lock);
}

/* ------------------------------------------------- image / view bookkeeping */

struct rec {
    uint64_t handle;
    struct rec *next;
    /* image */
    VkFormat format;
    uint32_t w, h, mips, samples;
    VkImageUsageFlags usage;
    /* view */
    uint64_t image;
    VkFormat view_format;
    VkImageAspectFlags aspect;
};

#define BUCKETS 16384
static struct rec *images[BUCKETS], *views[BUCKETS];
static pthread_mutex_t map_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned bucket(uint64_t h) { return (unsigned)((h * 0x9E3779B97F4A7C15ull) >> 50) % BUCKETS; }

static struct rec *map_find(struct rec **map, uint64_t h)
{
    for (struct rec *r = map[bucket(h)]; r; r = r->next)
        if (r->handle == h) return r;
    return NULL;
}

static void map_put(struct rec **map, const struct rec *src)
{
    struct rec *r = map_find(map, src->handle);
    if (!r) {
        unsigned b = bucket(src->handle);
        r = malloc(sizeof *r);
        if (!r) return;
        *r = *src;
        r->next = map[b];
        map[b] = r;
    } else {
        struct rec *next = r->next;
        *r = *src;
        r->next = next;
    }
}

static void map_del(struct rec **map, uint64_t h)
{
    for (struct rec **p = &map[bucket(h)]; *p; p = &(*p)->next)
        if ((*p)->handle == h) {
            struct rec *dead = *p;
            *p = dead->next;
            free(dead);
            return;
        }
}

/* --------------------------------------------------------- dispatch tables */

struct instance {
    void *key;
    VkInstance handle;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkDestroyInstance DestroyInstance;
};

struct device {
    void *key;
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkDestroyDevice DestroyDevice;
    PFN_vkCreateImage CreateImage;
    PFN_vkDestroyImage DestroyImage;
    PFN_vkCreateImageView CreateImageView;
    PFN_vkDestroyImageView DestroyImageView;
    PFN_vkGetDeviceQueue GetDeviceQueue;
    PFN_vkGetDeviceQueue2 GetDeviceQueue2;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkFreeCommandBuffers FreeCommandBuffers;
    PFN_vkCreateSwapchainKHR CreateSwapchainKHR;
    PFN_vkQueuePresentKHR QueuePresentKHR;
    PFN_vkGetImageViewHandleNVX GetImageViewHandleNVX;
    PFN_vkGetImageViewHandle64NVX GetImageViewHandle64NVX;
    PFN_vkGetImageViewAddressNVX GetImageViewAddressNVX;
    PFN_vkCreateCuModuleNVX CreateCuModuleNVX;
    PFN_vkCreateCuFunctionNVX CreateCuFunctionNVX;
    PFN_vkCmdCuLaunchKernelNVX CmdCuLaunchKernelNVX;
    /* capture mode only */
    VkDevice handle;
    VkPhysicalDeviceMemoryProperties mem;
    PFN_vkCreateBuffer CreateBuffer;
    PFN_vkDestroyBuffer DestroyBuffer;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkBindBufferMemory BindBufferMemory;
    PFN_vkMapMemory MapMemory;
    PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
};

#define MAX_OBJ 16
static struct instance instances[MAX_OBJ];
static struct device devices[MAX_OBJ];
static pthread_mutex_t tab_lock = PTHREAD_MUTEX_INITIALIZER;

static struct instance *get_instance(void *key)
{
    for (int i = 0; i < MAX_OBJ; i++)
        if (instances[i].key == key) return &instances[i];
    return NULL;
}

static struct device *get_device(void *key)
{
    for (int i = 0; i < MAX_OBJ; i++)
        if (devices[i].key == key) return &devices[i];
    return NULL;
}

/*
 * Queues and command buffers.  An application that calls through the loader gets
 * queues and command buffers whose dispatch key equals the device's.  Wine does not:
 * it fetches vkGetDeviceQueue / vkAllocateCommandBuffers with vkGetDeviceProcAddr and
 * so bypasses the loader code that sets that key.  These objects are therefore
 * tracked explicitly, in owners[]: handle -> device slot.
 */
static struct rec *owners[BUCKETS];

static void set_owner(void *handle, struct device *d)
{
    struct rec rec = {.handle = (uint64_t)(uintptr_t)handle, .image = (uint64_t)(uintptr_t)d};
    pthread_mutex_lock(&map_lock);
    map_put(owners, &rec);
    pthread_mutex_unlock(&map_lock);
}

static void drop_owner(void *handle)
{
    pthread_mutex_lock(&map_lock);
    map_del(owners, (uint64_t)(uintptr_t)handle);
    pthread_mutex_unlock(&map_lock);
}

static struct device *device_of(void *handle)
{
    struct device *d = NULL;
    pthread_mutex_lock(&map_lock);
    struct rec *r = map_find(owners, (uint64_t)(uintptr_t)handle);
    if (r) d = (struct device *)(uintptr_t)r->image;
    pthread_mutex_unlock(&map_lock);
    return d ? d : get_device(KEY(handle));
}

/* ----------------------------------------------------------------- counters */

static uint64_t n_present, n_handle, n_launch;
static uint64_t last_present, last_handle, last_launch;
static double last_report;
/* full detail for the first DETAIL_FRAMES frames that contain NVX handle calls */
#define DETAIL_FRAMES 4
static int detail_frames_left = DETAIL_FRAMES;
static int frame_had_handles;

/* signatures of views already described once in full */
#define MAX_SIG 256
static struct { VkFormat f; uint32_t w, h; VkDescriptorType d; uint64_t count; } sigs[MAX_SIG];
static int n_sigs;

/* ------------------------------------------------------------ capture mode
 *
 * SC_PROBE_CAPTURE=<n>   (1..4) frames per burst; unset or 0 = capture off (the default)
 * SC_PROBE_CAPTURE_EVERY=<seconds between bursts>, default 20
 * SC_PROBE_CAPTURE_BURSTS=<number of bursts>, default 6
 * SC_PROBE_CAPTURE_DELAY=<seconds after start before the first burst>, default 0
 * SC_PROBE_CAPTURE_TRIGGER=1: nothing is captured until the file $SC_PROBE_DIR/GO appears
 *     (looked for once a second).  The file is removed and one series of bursts runs;
 *     creating it again starts another series.
 *
 * In a burst, for n consecutive frames, the three DLSS inputs (colour, motion vectors,
 * depth) are copied into buffers owned by this layer.  The copy commands go into the
 * game's own command buffer immediately before DLSS's first kernel launch, so they see
 * exactly what DLSS sees.  No image layout or other state of the game is changed.
 *
 * "Frame" here means one DLSS evaluation, not one present: the game records the next
 * frame's commands on another thread while the previous frame is being presented, so
 * presents cannot be used to tell where an evaluation starts.  An evaluation is a run of
 * 32-bit view handle requests, the input views, kernel launches (with more view requests in
 * between) and one closing 32-bit request; so the first 32-bit handle request after any
 * kernel launch starts the next evaluation.  (The depth view cannot mark it: the transformer
 * model asks for it twice per evaluation.)
 * A few presents later the buffers are written to
 *     $SC_PROBE_DIR/cap-<pid>-b<burst>-f<frame>.bin
 * = 256-byte text header, then colour (w*h*8 bytes, RGBA16F), motion vectors
 * (w*h*4, RG16F), depth (w*h*4, float32).
 */
#define CAP_MAX_FRAMES 4
#define CAP_CANDIDATES 8

struct cap_buf { VkBuffer buf; VkDeviceMemory mem; void *map; VkDeviceSize size; };
struct cap_slot {
    struct cap_buf b[3];
    int pending, burst, idx, series;
    uint32_t w, h;
    VkFormat depth_fmt;
    uint64_t present, eval;
    double time;
};
struct cap_cand { uint64_t image; uint32_t w, h; };

static pthread_mutex_t cap_lock = PTHREAD_MUTEX_INITIALIZER;
static int cap_frames = -1, cap_every = 20, cap_bursts = 6, cap_selftest;
static struct cap_slot cap_slots[CAP_MAX_FRAMES];
static struct device *cap_dev;
static int cap_pos, cap_done, cap_fresh, cap_warned, cap_delay;
static uint64_t cap_eval;
static int cap_launched; /* a kernel was launched since the current evaluation began */
static int cap_trigger, cap_armed = 1, cap_series;
static double cap_last_look = -10.0;
static double cap_next;
/* the most recent inputs: the depth image, and per image size the last sampled RGBA16F
 * and RG16F image (colour and vectors are the ones whose size equals the depth image's) */
static struct { uint64_t depth; uint32_t w, h; VkFormat depth_fmt;
                struct cap_cand color[CAP_CANDIDATES], mv[CAP_CANDIDATES]; int n_color, n_mv; } cap_cur;

static void cap_remember(struct cap_cand *list, int *n, struct cap_cand c)
{
    for (int i = 0; i < *n; i++)
        if (list[i].w == c.w && list[i].h == c.h) { list[i] = c; return; }
    if (*n < CAP_CANDIDATES) list[(*n)++] = c;
}

static void cap_init(void)
{
    const char *e;
    if (cap_frames >= 0) return;
    cap_frames = (e = getenv("SC_PROBE_CAPTURE")) ? atoi(e) : 0;
    if (cap_frames < 0) cap_frames = 0;
    if (cap_frames > CAP_MAX_FRAMES) cap_frames = CAP_MAX_FRAMES;
    if ((e = getenv("SC_PROBE_CAPTURE_EVERY")) && atoi(e) > 0) cap_every = atoi(e);
    if ((e = getenv("SC_PROBE_CAPTURE_BURSTS")) && atoi(e) > 0) cap_bursts = atoi(e);
    if ((e = getenv("SC_PROBE_CAPTURE_DELAY")) && atoi(e) > 0) cap_delay = atoi(e);
    cap_selftest = (e = getenv("SC_PROBE_SELFTEST")) && atoi(e);
    if ((e = getenv("SC_PROBE_CAPTURE_TRIGGER")) && atoi(e)) { cap_trigger = 1; cap_armed = 0; }
    if (cap_frames)
        plog("CAPTURE on: %d frame(s) per burst, every %d s, %d burst(s), first after %d s%s", cap_frames, cap_every,
             cap_bursts, cap_delay, cap_selftest ? " [selftest]" : "");
    cap_next = now_s() + cap_delay;
}

/* called for every view handed to the driver */
static void cap_note(const struct rec *view, const struct rec *img, VkDescriptorType dtype, int handle32)
{
    if (cap_frames <= 0) return;
    pthread_mutex_lock(&cap_lock);
    if ((handle32 && cap_launched) || (cap_selftest && (view->aspect & VK_IMAGE_ASPECT_DEPTH_BIT))) {
        cap_fresh = 1; /* a new evaluation */
        cap_eval++;
        cap_launched = 0;
    }
    if (view->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) {
        cap_cur.depth = img->handle;
        cap_cur.w = img->w;
        cap_cur.h = img->h;
        cap_cur.depth_fmt = img->format;
    } else if (dtype == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
        struct cap_cand c = {img->handle, img->w, img->h};
        if (view->view_format == VK_FORMAT_R16G16B16A16_SFLOAT) cap_remember(cap_cur.color, &cap_cur.n_color, c);
        else if (view->view_format == VK_FORMAT_R16G16_SFLOAT) cap_remember(cap_cur.mv, &cap_cur.n_mv, c);
    }
    pthread_mutex_unlock(&cap_lock);
}

static int cap_alloc(struct device *d, struct cap_buf *b, VkDeviceSize size)
{
    if (b->buf && b->size >= size) return 1;
    if (b->buf) return 0; /* resolution grew: keep it simple, no reallocation */
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkMemoryRequirements mr;
    if (d->CreateBuffer(d->handle, &bi, NULL, &b->buf) != VK_SUCCESS) { b->buf = VK_NULL_HANDLE; return 0; }
    d->GetBufferMemoryRequirements(d->handle, b->buf, &mr);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    int type = -1;
    for (int pass = 0; pass < 2 && type < 0; pass++)
        for (uint32_t i = 0; i < d->mem.memoryTypeCount; i++) {
            VkMemoryPropertyFlags f = d->mem.memoryTypes[i].propertyFlags;
            if (!(mr.memoryTypeBits & (1u << i)) || (f & want) != want) continue;
            if (pass == 0 && !(f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) continue;
            type = (int)i;
            break;
        }
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                               .memoryTypeIndex = (uint32_t)type};
    if (type < 0 || d->AllocateMemory(d->handle, &ai, NULL, &b->mem) != VK_SUCCESS ||
        d->BindBufferMemory(d->handle, b->buf, b->mem, 0) != VK_SUCCESS ||
        d->MapMemory(d->handle, b->mem, 0, VK_WHOLE_SIZE, 0, &b->map) != VK_SUCCESS) {
        if (b->mem) d->FreeMemory(d->handle, b->mem, NULL);
        d->DestroyBuffer(d->handle, b->buf, NULL);
        memset(b, 0, sizeof *b);
        return 0;
    }
    b->size = size;
    return 1;
}

/* called where DLSS is about to run: cb is the game's command buffer */
static void cap_inject(struct device *d, VkCommandBuffer cb)
{
    uint64_t color = 0, mv = 0;
    if (cap_frames <= 0 || !d->CmdCopyImageToBuffer) return;
    pthread_mutex_lock(&cap_lock);
    if (cap_trigger && !cap_armed && now_s() - cap_last_look >= 1.0) {
        char go[512];
        const char *dir = getenv("SC_PROBE_DIR");
        snprintf(go, sizeof go, "%s/GO", dir && *dir ? dir : "/tmp");
        cap_last_look = now_s();
        if (!access(go, F_OK)) {
            unlink(go);
            cap_armed = 1;
            cap_series++;
            cap_done = cap_pos = 0;
            cap_next = 0;
            plog("CAPTURE triggered (series %d)", cap_series);
        }
    }
    if (!cap_armed) goto out;
    if (cap_done >= cap_bursts) { /* series finished */
        if (cap_trigger) { cap_armed = 0; plog("CAPTURE series %d finished", cap_series); }
        goto out;
    }
    if (!cap_fresh || (cap_pos == 0 && now_s() < cap_next)) goto out;
    cap_fresh = 0; /* one capture per evaluation, whether or not it succeeds */
    if (cap_dev && cap_dev != d) goto out;
    for (int i = 0; i < cap_cur.n_color && !color; i++)
        if (cap_cur.color[i].w == cap_cur.w && cap_cur.color[i].h == cap_cur.h) color = cap_cur.color[i].image;
    for (int i = 0; i < cap_cur.n_mv && !mv; i++)
        if (cap_cur.mv[i].w == cap_cur.w && cap_cur.mv[i].h == cap_cur.h) mv = cap_cur.mv[i].image;
    if (!cap_cur.depth || !color || !mv ||
        (cap_cur.depth_fmt != VK_FORMAT_D32_SFLOAT && cap_cur.depth_fmt != VK_FORMAT_D32_SFLOAT_S8_UINT)) {
        if (!cap_warned++)
            plog("CAPTURE skipped: inputs not recognised (depth=%d fmt=%d, colour=%d of %d, vectors=%d of %d)",
                 cap_cur.depth != 0, cap_cur.depth_fmt, color != 0, cap_cur.n_color, mv != 0, cap_cur.n_mv);
        goto out;
    }
    struct cap_slot *s = &cap_slots[cap_pos];
    VkDeviceSize px = (VkDeviceSize)cap_cur.w * cap_cur.h;
    if (s->pending || !cap_alloc(d, &s->b[0], px * 8) || !cap_alloc(d, &s->b[1], px * 4) ||
        !cap_alloc(d, &s->b[2], px * 4)) {
        if (!cap_warned++) plog("CAPTURE skipped: no buffer");
        goto out;
    }
    cap_dev = d;
    const uint64_t src[3] = {color, mv, cap_cur.depth};
    for (int i = 0; i < 3; i++) {
        VkBufferImageCopy region = {
            .imageSubresource = {i == 2 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {cap_cur.w, cap_cur.h, 1}};
        /* The images are being sampled by DLSS; their layout is left alone.  The NVIDIA
         * driver does not act on image layouts, so naming the transfer layout here is
         * enough and nothing has to be transitioned and restored. */
        d->CmdCopyImageToBuffer(cb, (VkImage)src[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->b[i].buf, 1, &region);
    }
    s->pending = 1;
    s->burst = cap_done;
    s->series = cap_series;
    s->idx = cap_pos;
    s->w = cap_cur.w;
    s->h = cap_cur.h;
    s->depth_fmt = cap_cur.depth_fmt;
    s->present = n_present;
    s->eval = cap_eval;
    s->time = now_s();
    if (++cap_pos >= cap_frames) {
        cap_pos = 0;
        cap_done++;
        cap_next = now_s() + cap_every;
    }
out:
    pthread_mutex_unlock(&cap_lock);
}

/* once per present (force = at device destruction): write finished captures, start a new frame */
static void cap_frame_end(int force)
{
    if (cap_frames <= 0) return;
    pthread_mutex_lock(&cap_lock);
    for (int i = 0; i < CAP_MAX_FRAMES; i++) {
        struct cap_slot *s = &cap_slots[i];
        if (!s->pending || (!force && n_present - s->present < 4)) continue;
        char path[512], header[256];
        const char *dir = getenv("SC_PROBE_DIR");
        VkDeviceSize px = (VkDeviceSize)s->w * s->h;
        snprintf(path, sizeof path, "%s/cap-%d-s%d-b%02d-f%d.bin", dir && *dir ? dir : "/tmp", getpid(), s->series,
                 s->burst, s->idx);
        memset(header, ' ', sizeof header);
        int n = snprintf(header, sizeof header, "SCCAP1 w=%u h=%u color=RGBA16F mv=RG16F depth=F32 depth_format=%d "
                         "present=%llu time=%.3f burst=%d frame=%d eval=%llu", s->w, s->h, s->depth_fmt,
                         (unsigned long long)s->present, s->time, s->burst, s->idx, (unsigned long long)s->eval);
        header[n] = ' ';
        header[255] = '\n';
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(header, 1, sizeof header, f);
            fwrite(s->b[0].map, 1, px * 8, f);
            fwrite(s->b[1].map, 1, px * 4, f);
            fwrite(s->b[2].map, 1, px * 4, f);
            fclose(f);
        }
        plog("CAPTURE wrote %s (%ux%u, present #%llu)%s", path, s->w, s->h, (unsigned long long)s->present,
             f ? "" : " FAILED");
        s->pending = 0;
    }
    pthread_mutex_unlock(&cap_lock);
}

static void cap_destroy(struct device *d)
{
    if (cap_frames <= 0 || cap_dev != d) return;
    cap_frame_end(1);
    pthread_mutex_lock(&cap_lock);
    for (int i = 0; i < CAP_MAX_FRAMES; i++)
        for (int j = 0; j < 3; j++) {
            struct cap_buf *b = &cap_slots[i].b[j];
            if (!b->buf) continue;
            d->DestroyBuffer(d->handle, b->buf, NULL);
            d->FreeMemory(d->handle, b->mem, NULL);
            memset(b, 0, sizeof *b);
        }
    cap_dev = NULL;
    pthread_mutex_unlock(&cap_lock);
}

static void note_view(const char *call, uint64_t view, VkDescriptorType dtype)
{
    struct rec v = {0}, img = {0};
    int known_view = 0, known_img = 0, i, first = 0;

    pthread_mutex_lock(&map_lock);
    struct rec *rv = map_find(views, view);
    if (rv) {
        v = *rv;
        known_view = 1;
        struct rec *ri = map_find(images, rv->image);
        if (ri) {
            img = *ri;
            known_img = 1;
        }
    }
    n_handle++;
    frame_had_handles = 1;
    for (i = 0; i < n_sigs; i++)
        if (sigs[i].f == v.view_format && sigs[i].w == img.w && sigs[i].h == img.h && sigs[i].d == dtype) break;
    if (i == n_sigs && n_sigs < MAX_SIG) {
        sigs[i].f = v.view_format;
        sigs[i].w = img.w;
        sigs[i].h = img.h;
        sigs[i].d = dtype;
        n_sigs++;
        first = 1;
    }
    if (i < MAX_SIG) sigs[i].count++;
    int detail = detail_frames_left > 0;
    pthread_mutex_unlock(&map_lock);
    if (known_view && known_img) cap_note(&v, &img, dtype, !strcmp(call, "handle32"));

    if (first || detail)
        plog("NVX %s view=%#llx known=%d/%d view_format=%d image_format=%d size=%ux%u mips=%u samples=%u "
             "usage=%#x aspect=%#x descriptor=%d%s",
             call, (unsigned long long)view, known_view, known_img, v.view_format, img.format, img.w, img.h,
             img.mips, img.samples, img.usage, v.aspect, dtype, first ? " [new signature]" : "");
}

/* ------------------------------------------------------- intercepted calls */

static VKAPI_ATTR VkResult VKAPI_CALL p_CreateImage(VkDevice dev, const VkImageCreateInfo *ci,
                                                    const VkAllocationCallbacks *a, VkImage *out)
{
    struct device *d = get_device(KEY(dev));
    VkResult r = d->CreateImage(dev, ci, a, out);
    if (r == VK_SUCCESS && ci->imageType == VK_IMAGE_TYPE_2D) {
        struct rec rec = {.handle = (uint64_t)*out, .format = ci->format, .w = ci->extent.width,
                          .h = ci->extent.height, .mips = ci->mipLevels, .samples = ci->samples,
                          .usage = ci->usage};
        pthread_mutex_lock(&map_lock);
        map_put(images, &rec);
        pthread_mutex_unlock(&map_lock);
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL p_DestroyImage(VkDevice dev, VkImage image, const VkAllocationCallbacks *a)
{
    struct device *d = get_device(KEY(dev));
    pthread_mutex_lock(&map_lock);
    map_del(images, (uint64_t)image);
    pthread_mutex_unlock(&map_lock);
    d->DestroyImage(dev, image, a);
}

static VKAPI_ATTR VkResult VKAPI_CALL p_CreateImageView(VkDevice dev, const VkImageViewCreateInfo *ci,
                                                        const VkAllocationCallbacks *a, VkImageView *out)
{
    struct device *d = get_device(KEY(dev));
    VkResult r = d->CreateImageView(dev, ci, a, out);
    if (r == VK_SUCCESS) {
        struct rec rec = {.handle = (uint64_t)*out, .image = (uint64_t)ci->image, .view_format = ci->format,
                          .aspect = ci->subresourceRange.aspectMask};
        pthread_mutex_lock(&map_lock);
        map_put(views, &rec);
        pthread_mutex_unlock(&map_lock);
        if (cap_selftest) note_view("selftest", (uint64_t)*out, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    }
    return r;
}

/* Self-test only (SC_PROBE_SELFTEST=1): an empty vkCmdPipelineBarrier stands in for DLSS's
 * kernel launch, and every image view created counts as a view handed to the driver. */
static VKAPI_ATTR void VKAPI_CALL p_CmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags a, VkPipelineStageFlags b,
                                                       VkDependencyFlags f, uint32_t nm, const VkMemoryBarrier *m,
                                                       uint32_t nb, const VkBufferMemoryBarrier *bb, uint32_t ni,
                                                       const VkImageMemoryBarrier *ib)
{
    struct device *d = device_of(cb);
    if (!nm && !nb && !ni) cap_inject(d, cb);
    d->CmdPipelineBarrier(cb, a, b, f, nm, m, nb, bb, ni, ib);
}

static VKAPI_ATTR void VKAPI_CALL p_DestroyImageView(VkDevice dev, VkImageView view, const VkAllocationCallbacks *a)
{
    struct device *d = get_device(KEY(dev));
    pthread_mutex_lock(&map_lock);
    map_del(views, (uint64_t)view);
    pthread_mutex_unlock(&map_lock);
    d->DestroyImageView(dev, view, a);
}

static VKAPI_ATTR void VKAPI_CALL p_GetDeviceQueue(VkDevice dev, uint32_t family, uint32_t index, VkQueue *out)
{
    struct device *d = get_device(KEY(dev));
    d->GetDeviceQueue(dev, family, index, out);
    if (*out) set_owner(*out, d);
}

static VKAPI_ATTR void VKAPI_CALL p_GetDeviceQueue2(VkDevice dev, const VkDeviceQueueInfo2 *info, VkQueue *out)
{
    struct device *d = get_device(KEY(dev));
    d->GetDeviceQueue2(dev, info, out);
    if (*out) set_owner(*out, d);
}

static VKAPI_ATTR VkResult VKAPI_CALL p_AllocateCommandBuffers(VkDevice dev, const VkCommandBufferAllocateInfo *ai,
                                                               VkCommandBuffer *out)
{
    struct device *d = get_device(KEY(dev));
    VkResult r = d->AllocateCommandBuffers(dev, ai, out);
    if (r == VK_SUCCESS)
        for (uint32_t i = 0; i < ai->commandBufferCount; i++) set_owner(out[i], d);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL p_FreeCommandBuffers(VkDevice dev, VkCommandPool pool, uint32_t count,
                                                       const VkCommandBuffer *cbs)
{
    struct device *d = get_device(KEY(dev));
    if (cbs)
        for (uint32_t i = 0; i < count; i++)
            if (cbs[i]) drop_owner(cbs[i]);
    d->FreeCommandBuffers(dev, pool, count, cbs);
}

static VKAPI_ATTR VkResult VKAPI_CALL p_CreateSwapchainKHR(VkDevice dev, const VkSwapchainCreateInfoKHR *ci,
                                                           const VkAllocationCallbacks *a, VkSwapchainKHR *out)
{
    struct device *d = get_device(KEY(dev));
    VkResult r = d->CreateSwapchainKHR(dev, ci, a, out);
    plog("SWAPCHAIN result=%d format=%d colorspace=%d size=%ux%u min_images=%u present_mode=%d usage=%#x",
         r, ci->imageFormat, ci->imageColorSpace, ci->imageExtent.width, ci->imageExtent.height,
         ci->minImageCount, ci->presentMode, ci->imageUsage);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL p_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pi)
{
    struct device *d = device_of(queue);
    double t;
    uint64_t dp = 0, dh = 0, dl = 0;
    double dt = 0;
    int end_detail = 0;

    pthread_mutex_lock(&map_lock);
    n_present++;
    if (frame_had_handles && detail_frames_left > 0) {
        detail_frames_left--;
        end_detail = 1;
    }
    frame_had_handles = 0;
    t = now_s();
    if (t - last_report >= 10.0) {
        dp = n_present - last_present;
        dh = n_handle - last_handle;
        dl = n_launch - last_launch;
        dt = t - last_report;
        last_present = n_present;
        last_handle = n_handle;
        last_launch = n_launch;
        last_report = t;
    }
    pthread_mutex_unlock(&map_lock);

    if (end_detail) plog("FRAME end of a frame with NVX handle calls (present #%llu)", (unsigned long long)n_present);
    if (dt > 0)
        plog("STATS %.1f presents/s, %.2f NVX handle calls/frame, %.2f NVX kernel launches/frame",
             dp / dt, dp ? (double)dh / dp : 0.0, dp ? (double)dl / dp : 0.0);
    cap_frame_end(0);
    return d->QueuePresentKHR(queue, pi);   /* d is never NULL: see device_of() and p_GetDeviceQueue */
}

static VKAPI_ATTR uint32_t VKAPI_CALL p_GetImageViewHandleNVX(VkDevice dev, const VkImageViewHandleInfoNVX *info)
{
    struct device *d = get_device(KEY(dev));
    note_view("handle32", (uint64_t)info->imageView, info->descriptorType);
    return d->GetImageViewHandleNVX(dev, info);
}

static VKAPI_ATTR uint64_t VKAPI_CALL p_GetImageViewHandle64NVX(VkDevice dev, const VkImageViewHandleInfoNVX *info)
{
    struct device *d = get_device(KEY(dev));
    note_view("handle64", (uint64_t)info->imageView, info->descriptorType);
    return d->GetImageViewHandle64NVX(dev, info);
}

static VKAPI_ATTR VkResult VKAPI_CALL p_GetImageViewAddressNVX(VkDevice dev, VkImageView view,
                                                               VkImageViewAddressPropertiesNVX *props)
{
    struct device *d = get_device(KEY(dev));
    note_view("address", (uint64_t)view, (VkDescriptorType)-1);
    return d->GetImageViewAddressNVX(dev, view, props);
}

static VKAPI_ATTR VkResult VKAPI_CALL p_CreateCuModuleNVX(VkDevice dev, const VkCuModuleCreateInfoNVX *ci,
                                                          const VkAllocationCallbacks *a, VkCuModuleNVX *out)
{
    struct device *d = get_device(KEY(dev));
    VkResult r = d->CreateCuModuleNVX(dev, ci, a, out);
    plog("NVX module result=%d size=%zu", r, ci->dataSize);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL p_CreateCuFunctionNVX(VkDevice dev, const VkCuFunctionCreateInfoNVX *ci,
                                                            const VkAllocationCallbacks *a, VkCuFunctionNVX *out)
{
    struct device *d = get_device(KEY(dev));
    VkResult r = d->CreateCuFunctionNVX(dev, ci, a, out);
    plog("NVX function result=%d name=%s", r, ci->pName ? ci->pName : "(null)");
    return r;
}

static VKAPI_ATTR void VKAPI_CALL p_CmdCuLaunchKernelNVX(VkCommandBuffer cb, const VkCuLaunchInfoNVX *li)
{
    struct device *d = device_of(cb);
    int detail;
    pthread_mutex_lock(&map_lock);
    n_launch++;
    detail = detail_frames_left > 0;
    pthread_mutex_unlock(&map_lock);
    if (detail)
        plog("NVX launch function=%#llx grid=%ux%ux%u block=%ux%ux%u params=%zu extras=%zu",
             (unsigned long long)li->function, li->gridDimX, li->gridDimY, li->gridDimZ, li->blockDimX,
             li->blockDimY, li->blockDimZ, li->paramCount, li->extraCount);
    cap_inject(d, cb);
    cap_launched = 1;
    d->CmdCuLaunchKernelNVX(cb, li);
}

/* ------------------------------------------------ instance / device set-up */

static VKAPI_ATTR VkResult VKAPI_CALL p_CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a,
                                                       VkInstance *out)
{
    VkLayerInstanceCreateInfo *link = (VkLayerInstanceCreateInfo *)ci->pNext;
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
        link = (VkLayerInstanceCreateInfo *)link->pNext;
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = create(ci, a, out);
    if (r != VK_SUCCESS) return r;

    pthread_mutex_lock(&tab_lock);
    struct instance *in = get_instance(NULL);
    if (in) {
        in->handle = *out;
        in->gipa = gipa;
        in->DestroyInstance = (PFN_vkDestroyInstance)gipa(*out, "vkDestroyInstance");
        in->key = KEY(*out);
    }
    pthread_mutex_unlock(&tab_lock);
    plog("INSTANCE app=%s engine=%s", ci->pApplicationInfo && ci->pApplicationInfo->pApplicationName
             ? ci->pApplicationInfo->pApplicationName : "?",
         ci->pApplicationInfo && ci->pApplicationInfo->pEngineName ? ci->pApplicationInfo->pEngineName : "?");
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL p_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *a)
{
    if (!instance) return;
    pthread_mutex_lock(&tab_lock);
    struct instance *in = get_instance(KEY(instance));
    PFN_vkDestroyInstance destroy = in ? in->DestroyInstance : NULL;
    if (in) memset(in, 0, sizeof *in);
    pthread_mutex_unlock(&tab_lock);
    if (destroy) destroy(instance, a);
}

static VKAPI_ATTR VkResult VKAPI_CALL p_CreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci,
                                                     const VkAllocationCallbacks *a, VkDevice *out)
{
    VkLayerDeviceCreateInfo *link = (VkLayerDeviceCreateInfo *)ci->pNext;
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
        link = (VkLayerDeviceCreateInfo *)link->pNext;
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    pthread_mutex_lock(&tab_lock);
    struct instance *in = get_instance(KEY(phys));
    VkInstance inst = in ? in->handle : VK_NULL_HANDLE;
    pthread_mutex_unlock(&tab_lock);

    PFN_vkCreateDevice create = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
    if (!create) create = (PFN_vkCreateDevice)gipa(VK_NULL_HANDLE, "vkCreateDevice");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = create(phys, ci, a, out);
    if (r != VK_SUCCESS) return r;

    pthread_mutex_lock(&tab_lock);
    struct device *d = get_device(NULL);
    if (d) {
#define LOAD(name) d->name = (PFN_vk##name)gdpa(*out, "vk" #name)
        d->gdpa = gdpa;
        LOAD(DestroyDevice);
        LOAD(CreateImage);
        LOAD(DestroyImage);
        LOAD(CreateImageView);
        LOAD(DestroyImageView);
        LOAD(GetDeviceQueue);
        LOAD(GetDeviceQueue2);
        LOAD(AllocateCommandBuffers);
        LOAD(FreeCommandBuffers);
        LOAD(CreateSwapchainKHR);
        LOAD(QueuePresentKHR);
        LOAD(GetImageViewHandleNVX);
        LOAD(GetImageViewHandle64NVX);
        LOAD(GetImageViewAddressNVX);
        LOAD(CreateCuModuleNVX);
        LOAD(CreateCuFunctionNVX);
        LOAD(CmdCuLaunchKernelNVX);
        LOAD(CreateBuffer);
        LOAD(DestroyBuffer);
        LOAD(GetBufferMemoryRequirements);
        LOAD(AllocateMemory);
        LOAD(FreeMemory);
        LOAD(BindBufferMemory);
        LOAD(MapMemory);
        LOAD(CmdCopyImageToBuffer);
        LOAD(CmdPipelineBarrier);
#undef LOAD
        d->handle = *out;
        PFN_vkGetPhysicalDeviceMemoryProperties memprops =
            (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
        if (memprops) memprops(phys, &d->mem);
        d->key = KEY(*out);
    }
    pthread_mutex_unlock(&tab_lock);

    plog("DEVICE created, %u extensions:", ci->enabledExtensionCount);
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) plog("  ext %s", ci->ppEnabledExtensionNames[i]);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL p_DestroyDevice(VkDevice dev, const VkAllocationCallbacks *a)
{
    if (!dev) return;
    pthread_mutex_lock(&tab_lock);
    struct device *d = get_device(KEY(dev));
    PFN_vkDestroyDevice destroy = d ? d->DestroyDevice : NULL;
    if (d) cap_destroy(d);
    if (d) memset(d, 0, sizeof *d);
    pthread_mutex_unlock(&tab_lock);
    plog("DEVICE destroyed after %llu presents", (unsigned long long)n_present);
    if (destroy) destroy(dev, a);
}

/* ----------------------------------------------------------- entry points */

static const struct { const char *name; void *fn; int needs_next; } device_hooks[] = {
    {"vkDestroyDevice", p_DestroyDevice, 0},
    {"vkCreateImage", p_CreateImage, 0},
    {"vkDestroyImage", p_DestroyImage, 0},
    {"vkCreateImageView", p_CreateImageView, 0},
    {"vkDestroyImageView", p_DestroyImageView, 0},
    {"vkGetDeviceQueue", p_GetDeviceQueue, 0},
    {"vkGetDeviceQueue2", p_GetDeviceQueue2, 1},
    {"vkAllocateCommandBuffers", p_AllocateCommandBuffers, 0},
    {"vkFreeCommandBuffers", p_FreeCommandBuffers, 0},
    {"vkCreateSwapchainKHR", p_CreateSwapchainKHR, 1},
    {"vkQueuePresentKHR", p_QueuePresentKHR, 1},
    {"vkGetImageViewHandleNVX", p_GetImageViewHandleNVX, 1},
    {"vkGetImageViewHandle64NVX", p_GetImageViewHandle64NVX, 1},
    {"vkGetImageViewAddressNVX", p_GetImageViewAddressNVX, 1},
    {"vkCreateCuModuleNVX", p_CreateCuModuleNVX, 1},
    {"vkCreateCuFunctionNVX", p_CreateCuFunctionNVX, 1},
    {"vkCmdCuLaunchKernelNVX", p_CmdCuLaunchKernelNVX, 1},
    {"vkCmdPipelineBarrier", p_CmdPipelineBarrier, 2},   /* 2 = hooked in self-test only */
};
#define N_HOOKS (sizeof device_hooks / sizeof device_hooks[0])

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL p_GetDeviceProcAddr(VkDevice dev, const char *name)
{
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)p_GetDeviceProcAddr;
    struct device *d = dev ? get_device(KEY(dev)) : NULL;
    if (!d) return NULL;
    PFN_vkVoidFunction next = d->gdpa(dev, name);
    for (size_t i = 0; i < N_HOOKS; i++)
        if (!strcmp(name, device_hooks[i].name))
            /* only stand in for functions the driver actually provides */
            return (device_hooks[i].needs_next == 2 && !cap_selftest) || (device_hooks[i].needs_next && !next)
                       ? next : (PFN_vkVoidFunction)device_hooks[i].fn;
    return next;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL p_GetInstanceProcAddr(VkInstance instance, const char *name)
{
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)p_GetInstanceProcAddr;
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)p_CreateInstance;
    if (!strcmp(name, "vkDestroyInstance")) return (PFN_vkVoidFunction)p_DestroyInstance;
    if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)p_CreateDevice;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)p_GetDeviceProcAddr;

    struct instance *in = instance ? get_instance(KEY(instance)) : NULL;
    PFN_vkVoidFunction next = in ? in->gipa(instance, name) : NULL;
    for (size_t i = 0; i < N_HOOKS; i++)
        if (!strcmp(name, device_hooks[i].name))
            return (device_hooks[i].needs_next == 2 && !cap_selftest) || (device_hooks[i].needs_next && !next)
                       ? next : (PFN_vkVoidFunction)device_hooks[i].fn;
    return next;
}

/* The layer stays out of every process whose command line does not contain
 * $SC_PROBE_MATCH (default "StarCitizen.exe"): the launcher, its helpers, anything else. */
static int process_matches(void)
{
    char buf[4096];
    const char *want = getenv("SC_PROBE_MATCH");
    if (!want || !*want) want = "StarCitizen.exe";
    FILE *f = fopen("/proc/self/cmdline", "r");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    for (size_t i = 0; i < n; i++)
        if (!buf[i]) buf[i] = ' ';
    buf[n] = 0;
    return strcasestr(buf, want) != NULL;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v)
{
    if (!v || v->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) return VK_ERROR_INITIALIZATION_FAILED;
    if (!process_matches()) return VK_ERROR_INITIALIZATION_FAILED; /* the loader then skips this layer */
    cap_init();
    if (v->loaderLayerInterfaceVersion > 2) v->loaderLayerInterfaceVersion = 2;
    v->pfnGetInstanceProcAddr = p_GetInstanceProcAddr;
    v->pfnGetDeviceProcAddr = p_GetDeviceProcAddr;
    v->pfnGetPhysicalDeviceProcAddr = NULL;
    return VK_SUCCESS;
}
