#include "renderer.h"
#include "gpupool.h"
#include "log.h"
#include "mem.h"
#include "mesher.h"
#include "texgen.h"

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define FRAMES 2
#define MAX_SWAP_IMAGES 8
#define STAGING_SIZE (4u << 20)
#define POOL_GRANULE 64u      /* vertices */
#define MAX_POOL_BLOCKS 8
#define POOL_GROW_MB 16u
#define QUADS_PER_DRAW 16384u /* 16-bit indices address 65536 vertices */
#define UI_MAX_QUADS QUADS_PER_DRAW /* the overlay is always a single draw */
#define MAX_VIS (65 * 65 * SECTIONS) /* every section at the largest radius (32) */
#define DYN_SIZE (1u << 20)
#define DYN_LINES_BYTES 1024u
#define ZNEAR 0.05f

static const uint32_t SPV_BLOCK_VERT[] =
#include "block.vert.inc"
;
static const uint32_t SPV_BLOCK_FRAG[] =
#include "block.frag.inc"
;
static const uint32_t SPV_ENTITY_VERT[] =
#include "entity.vert.inc"
;
static const uint32_t SPV_LINE_VERT[] =
#include "line.vert.inc"
;
static const uint32_t SPV_LINE_FRAG[] =
#include "line.frag.inc"
;
static const uint32_t SPV_UI_VERT[] =
#include "ui.vert.inc"
    ;
static const uint32_t SPV_UI_FRAG[] =
#include "ui.frag.inc"
    ;

#define VK_CHECK(x)                                                              \
    do {                                                                         \
        VkResult r_ = (x);                                                       \
        if (r_ != VK_SUCCESS) log_fatal("%s failed (VkResult %d)", #x, (int)r_); \
    } while (0)

typedef struct {
    VkBuffer buf;
    VkDeviceMemory mem;
    void *map;
    VkDeviceSize size;
} gbuf;

typedef struct {
    VkBufferCopy *v;
    int count, cap;
} copy_list;

typedef struct {
    VkCommandBuffer cmd;
    VkFence fence;
    VkSemaphore image_ready;
    uint64_t serial;              /* number of the frame last submitted from this slot */
    gbuf staging;
    VkDeviceSize staging_used;
    copy_list copies[MAX_POOL_BLOCKS]; /* staging -> pool copies, per pool block */
    gbuf dyn;                     /* per-frame section origins, body instances, lines */
    gbuf ui;                      /* overlay quads, written in place by the game */
} frame;

/* Chunk meshes live in a few big vertex buffers ("pool blocks"). The first
 * is sized for the render radius; more are added only when it fills. */
typedef struct {
    gbuf buf;
    gpupool alloc;
} pool_block;

/* A pool range waiting for the GPU to stop reading it. */
typedef struct {
    uint32_t start, len;
    uint8_t block;
    uint64_t serial;              /* frames submitted before the free */
} pool_free;

_Static_assert(sizeof(entity_instance) == 48, "entity_instance is the instance vertex layout");
_Static_assert(T_FIRE0 == 32 && T_WATER == 14 && T_LEAVES == 8, "update the layer constants in push_vert.glsl");

typedef struct {
    const section_mesh *m;
    uint32_t faces;               /* opaque face groups that can face the camera */
} visible;

/* Push constants, split by stage so the vertex-only range is all a draw
 * ever updates. 112 bytes: inside the 128 every implementation offers. */
typedef struct {
    float view_proj[16];
    float origin[4];
    float fog[4];
} push_vert;

typedef struct {
    float color[4];
} push_frag;

typedef struct {
    float scale[2], offset[2];
    float linear_out, pad[3];
} push_ui;

#define PUSH_FRAG_OFFSET 96u
_Static_assert(sizeof(push_vert) == PUSH_FRAG_OFFSET, "push_vert must end where push_frag starts");
_Static_assert(MAX_VIS * 16u + (MAX_BODIES + RENDER_MAX_ENTS + 16) * sizeof(entity_instance) + DYN_LINES_BYTES + 64 <=
                   DYN_SIZE,
               "per-frame dynamic buffer too small");

struct renderer {
    GLFWwindow *win;
    const VkAllocationCallbacks *ac;
    VkInstance inst;
    VkDebugUtilsMessengerEXT dbg;
    VkSurfaceKHR surf;
    VkPhysicalDevice pd;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties memprops;
    VkDevice dev;
    VkQueue queue;
    uint32_t qfam;

    VkSwapchainKHR swap;
    VkFormat swap_fmt;
    VkColorSpaceKHR swap_cs;
    VkExtent2D extent;
    uint32_t image_count;
    VkImage images[MAX_SWAP_IMAGES];
    VkImageView views[MAX_SWAP_IMAGES];
    VkFramebuffer fbs[MAX_SWAP_IMAGES];
    VkSemaphore render_done[MAX_SWAP_IMAGES];
    int swap_srgb;
    /* Swapchain images get TRANSFER_SRC only once a screenshot is wanted:
     * on some drivers the extra usage turns off framebuffer compression. */
    int shot_usage, shot_supported, swap_has_src;

    VkFormat depth_fmt;
    VkImage depth;
    VkDeviceMemory depth_mem;
    VkImageView depth_view;

    VkRenderPass pass;
    VkDescriptorSetLayout dsl;
    VkDescriptorPool dpool;
    VkDescriptorSet dset;
    VkPipelineLayout layout;
    VkPipeline p_opaque, p_trans, p_entity, p_entity_trans, p_line_world, p_line_screen;
    /* Overlay: its own layout (a 2D font atlas, 2D push constants). */
    VkDescriptorSetLayout ui_dsl;
    VkDescriptorPool ui_dpool;
    VkDescriptorSet ui_dset;
    VkPipelineLayout ui_layout;
    VkPipeline p_ui;
    VkImage font;
    VkDeviceMemory font_mem;
    VkImageView font_view;
    VkCommandPool cmdpool;

    VkImage tex;
    VkDeviceMemory tex_mem;
    VkImageView tex_view;
    VkSampler sampler;

    gbuf index, cube;
    pool_block pool[MAX_POOL_BLOCKS];
    int pool_blocks, pool_host_visible;
    pool_free *frees;             /* oldest first */
    int free_count, free_cap;
    uint64_t submitted, completed;

    frame frames[FRAMES];
    uint32_t frame_index, image_index;
    int frame_active;
    int resized, vsync, radius;

    char shot_path[512];
    int shot_pending;

    visible *vis;
    int vis_cap;
    render_stats stats;
    int pool_full_warned;
};

/* ------------------------------------------------------------- utilities */

static uint32_t find_memtype(const renderer *r, uint32_t bits, VkMemoryPropertyFlags need,
                             VkMemoryPropertyFlags want)
{
    for (int pass = 0; pass < 2; pass++) {
        VkMemoryPropertyFlags flags = pass == 0 ? (need | want) : need;
        for (uint32_t i = 0; i < r->memprops.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (r->memprops.memoryTypes[i].propertyFlags & flags) == flags) return i;
    }
    log_fatal("no suitable Vulkan memory type (bits %#x, flags %#x)", bits, need);
}

static void buffer_destroy(renderer *r, gbuf *b)
{
    if (b->map) vkUnmapMemory(r->dev, b->mem);
    if (b->buf) vkDestroyBuffer(r->dev, b->buf, r->ac);
    if (b->mem) vkFreeMemory(r->dev, b->mem, r->ac);
    memset(b, 0, sizeof *b);
}

/* Returns 0 (and leaves nothing behind) if the driver is out of memory. */
static int buffer_try_create(renderer *r, gbuf *b, VkDeviceSize size, VkBufferUsageFlags usage,
                             VkMemoryPropertyFlags need, VkMemoryPropertyFlags want)
{
    memset(b, 0, sizeof *b);
    b->size = size;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (vkCreateBuffer(r->dev, &bi, r->ac, &b->buf) != VK_SUCCESS) {
        b->buf = VK_NULL_HANDLE;
        return 0;
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(r->dev, b->buf, &req);
    uint32_t type = find_memtype(r, req.memoryTypeBits, need, want);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = req.size, .memoryTypeIndex = type};
    int ok = vkAllocateMemory(r->dev, &ai, r->ac, &b->mem) == VK_SUCCESS;
    if (!ok) b->mem = VK_NULL_HANDLE;
    ok = ok && vkBindBufferMemory(r->dev, b->buf, b->mem, 0) == VK_SUCCESS;
    if (ok && (r->memprops.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
        ok = vkMapMemory(r->dev, b->mem, 0, VK_WHOLE_SIZE, 0, &b->map) == VK_SUCCESS;
        if (!ok) b->map = NULL;
    }
    if (!ok) buffer_destroy(r, b);
    return ok;
}

static void buffer_create(renderer *r, gbuf *b, VkDeviceSize size, VkBufferUsageFlags usage,
                          VkMemoryPropertyFlags need, VkMemoryPropertyFlags want)
{
    if (!buffer_try_create(r, b, size, usage, need, want))
        log_fatal("out of memory creating a %llu KB Vulkan buffer", (unsigned long long)(size >> 10));
}

static VkCommandBuffer one_shot_begin(renderer *r)
{
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .commandPool = r->cmdpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                      .commandBufferCount = 1};
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(r->dev, &ai, &cmd));
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    return cmd;
}

static void one_shot_end(renderer *r, VkCommandBuffer cmd)
{
    VK_CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
    VK_CHECK(vkQueueSubmit(r->queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(r->queue));
    vkFreeCommandBuffers(r->dev, r->cmdpool, 1, &cmd);
}

static void image_barrier(VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect, uint32_t mips,
                          uint32_t layers, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
                          VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
                          VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access, .dstAccessMask = dst_access,
        .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = {aspect, 0, mips, 0, layers},
    };
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

static float srgb_to_linear(float c)
{
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

/* --------------------------------------------------------------- instance */

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
                                               VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT *data, void *user)
{
    (void)type; (void)user;
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) log_error("vulkan: %s", data->pMessage);
    else if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) log_warn("vulkan: %s", data->pMessage);
    return VK_FALSE;
}

static int has_layer(const char *name)
{
    uint32_t n = 0;
    vkEnumerateInstanceLayerProperties(&n, NULL);
    VkLayerProperties *props = mem_alloc(mem_array_size(n ? n : 1, sizeof *props));
    vkEnumerateInstanceLayerProperties(&n, props);
    int found = 0;
    for (uint32_t i = 0; i < n && !found; i++) found = strcmp(props[i].layerName, name) == 0;
    mem_free(props);
    return found;
}

static void create_instance(renderer *r, int validate)
{
    uint32_t glfw_count = 0;
    const char **glfw_ext = glfwGetRequiredInstanceExtensions(&glfw_count);
    if (!glfw_ext) log_fatal("GLFW could not find Vulkan surface extensions");

    const char *exts[16];
    uint32_t n = 0;
    for (uint32_t i = 0; i < glfw_count && n < 15; i++) exts[n++] = glfw_ext[i];
    const char *layers[1];
    uint32_t nl = 0;
    if (validate) {
        if (has_layer("VK_LAYER_KHRONOS_validation")) {
            layers[nl++] = "VK_LAYER_KHRONOS_validation";
            exts[n++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        } else {
            log_warn("validation requested but VK_LAYER_KHRONOS_validation is not installed");
            validate = 0;
        }
    }

    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "blockclonia",
                             .applicationVersion = 1, .pEngineName = "blockclonia", .engineVersion = 1,
                             .apiVersion = VK_API_VERSION_1_0};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                               .enabledExtensionCount = n, .ppEnabledExtensionNames = exts,
                               .enabledLayerCount = nl, .ppEnabledLayerNames = layers};
    VK_CHECK(vkCreateInstance(&ci, r->ac, &r->inst));

    if (validate) {
        PFN_vkCreateDebugUtilsMessengerEXT fn =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(r->inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT di = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debug_cb};
        if (fn) VK_CHECK(fn(r->inst, &di, r->ac, &r->dbg));
    }
}

static int device_score(VkPhysicalDeviceType t)
{
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 4;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return 1;
    default: return 0;
    }
}

static int find_queue(renderer *r, VkPhysicalDevice pd, uint32_t *fam)
{
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, NULL);
    VkQueueFamilyProperties *q = mem_alloc(mem_array_size(n ? n : 1, sizeof *q));
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, q);
    int found = 0;
    for (uint32_t i = 0; i < n && !found; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, r->surf, &present);
        if ((q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
            *fam = i;
            found = 1;
        }
    }
    mem_free(q);
    return found;
}

static int has_swapchain_ext(VkPhysicalDevice pd)
{
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
    VkExtensionProperties *e = mem_alloc(mem_array_size(n ? n : 1, sizeof *e));
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, e);
    int found = 0;
    for (uint32_t i = 0; i < n && !found; i++) found = strcmp(e[i].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
    mem_free(e);
    return found;
}

static void pick_device(renderer *r, int want_index)
{
    uint32_t n = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(r->inst, &n, NULL));
    if (!n) log_fatal("no Vulkan devices found");
    VkPhysicalDevice *pds = mem_alloc(mem_array_size(n, sizeof *pds));
    VK_CHECK(vkEnumeratePhysicalDevices(r->inst, &n, pds));
    int best = -1, best_score = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pds[i], &p);
        uint32_t fam;
        if (!find_queue(r, pds[i], &fam) || !has_swapchain_ext(pds[i])) continue;
        int score = device_score(p.deviceType);
        if (want_index >= 0) score = (int)i == want_index ? 100 : -1;
        if (score > best_score) { best = (int)i; best_score = score; }
    }
    if (best < 0) log_fatal("no Vulkan device can present to this window");
    r->pd = pds[best];
    mem_free(pds);
    find_queue(r, r->pd, &r->qfam);
    vkGetPhysicalDeviceProperties(r->pd, &r->props);
    vkGetPhysicalDeviceMemoryProperties(r->pd, &r->memprops);
    log_info("GPU: %s (Vulkan %u.%u)", r->props.deviceName, VK_VERSION_MAJOR(r->props.apiVersion),
             VK_VERSION_MINOR(r->props.apiVersion));
}

static void create_device(renderer *r)
{
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = r->qfam,
                                  .queueCount = 1, .pQueuePriorities = &prio};
    const char *ext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkPhysicalDeviceFeatures feats = {0}; /* nothing optional: runs everywhere */
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qi, .enabledExtensionCount = 1,
                             .ppEnabledExtensionNames = &ext, .pEnabledFeatures = &feats};
    VK_CHECK(vkCreateDevice(r->pd, &di, r->ac, &r->dev));
    vkGetDeviceQueue(r->dev, r->qfam, 0, &r->queue);
}

/* -------------------------------------------------------------- swapchain */

static VkFormat pick_depth_format(renderer *r)
{
    const VkFormat cands[] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT,
                              VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D16_UNORM};
    for (size_t i = 0; i < sizeof cands / sizeof cands[0]; i++) {
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(r->pd, cands[i], &p);
        if (p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) return cands[i];
    }
    log_fatal("no depth format supported");
}

static void destroy_swap_resources(renderer *r)
{
    for (uint32_t i = 0; i < r->image_count; i++) {
        vkDestroyFramebuffer(r->dev, r->fbs[i], r->ac);
        vkDestroyImageView(r->dev, r->views[i], r->ac);
        vkDestroySemaphore(r->dev, r->render_done[i], r->ac);
        r->fbs[i] = VK_NULL_HANDLE;
        r->views[i] = VK_NULL_HANDLE;
        r->render_done[i] = VK_NULL_HANDLE;
    }
    vkDestroyImageView(r->dev, r->depth_view, r->ac);
    vkDestroyImage(r->dev, r->depth, r->ac);
    vkFreeMemory(r->dev, r->depth_mem, r->ac);
    r->depth_view = VK_NULL_HANDLE;
    r->depth = VK_NULL_HANDLE;
    r->depth_mem = VK_NULL_HANDLE;
}

static void choose_surface_format(renderer *r)
{
    uint32_t n = 0;
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(r->pd, r->surf, &n, NULL));
    if (!n) log_fatal("surface reports no formats");
    VkSurfaceFormatKHR *f = mem_alloc(mem_array_size(n, sizeof *f));
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(r->pd, r->surf, &n, f));
    /* Best first: 8-bit sRGB, then 8-bit UNORM (the shader encodes sRGB),
     * then whatever the surface lists first. */
    VkSurfaceFormatKHR chosen = f[0];
    if (chosen.format == VK_FORMAT_UNDEFINED)
        chosen = (VkSurfaceFormatKHR){VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    int best = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (f[i].colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) continue;
        int rank = f[i].format == VK_FORMAT_B8G8R8A8_SRGB || f[i].format == VK_FORMAT_R8G8B8A8_SRGB   ? 2
                   : f[i].format == VK_FORMAT_B8G8R8A8_UNORM || f[i].format == VK_FORMAT_R8G8B8A8_UNORM ? 1
                                                                                                        : 0;
        if (rank > best) {
            best = rank;
            chosen = f[i];
        }
    }
    mem_free(f);
    r->swap_fmt = chosen.format;
    r->swap_cs = chosen.colorSpace;
    r->swap_srgb = chosen.format == VK_FORMAT_B8G8R8A8_SRGB || chosen.format == VK_FORMAT_R8G8B8A8_SRGB;
}

/* Screenshots read back 4 bytes per pixel as 8-bit RGBA or BGRA. */
static int format_is_rgba8(VkFormat f)
{
    return f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB ||
           f == VK_FORMAT_R8G8B8A8_UNORM;
}

static VkPresentModeKHR choose_present_mode(renderer *r)
{
    if (r->vsync) return VK_PRESENT_MODE_FIFO_KHR; /* always available */
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(r->pd, r->surf, &n, NULL);
    VkPresentModeKHR *m = mem_alloc(mem_array_size(n ? n : 1, sizeof *m));
    vkGetPhysicalDeviceSurfacePresentModesKHR(r->pd, r->surf, &n, m);
    VkPresentModeKHR best = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < n; i++) {
        if (m[i] == VK_PRESENT_MODE_MAILBOX_KHR ||
            (m[i] == VK_PRESENT_MODE_IMMEDIATE_KHR && best == VK_PRESENT_MODE_FIFO_KHR))
            best = m[i];
    }
    mem_free(m);
    return best;
}

static void create_depth(renderer *r)
{
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = r->depth_fmt, .extent = {r->extent.width, r->extent.height, 1},
                            .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                     VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VK_CHECK(vkCreateImage(r->dev, &ii, r->ac, &r->depth));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(r->dev, r->depth, &req);
    /* Depth is cleared and never stored, so on tile-based GPUs it can live
     * on chip: lazily allocated memory, where offered, is never backed. */
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                               .memoryTypeIndex = find_memtype(r, req.memoryTypeBits,
                                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                                               VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)};
    VK_CHECK(vkAllocateMemory(r->dev, &ai, r->ac, &r->depth_mem));
    VK_CHECK(vkBindImageMemory(r->dev, r->depth, r->depth_mem, 0));
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = r->depth,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = r->depth_fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}};
    VK_CHECK(vkCreateImageView(r->dev, &vi, r->ac, &r->depth_view));
}

static int create_swapchain(renderer *r)
{
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r->pd, r->surf, &caps));
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == UINT32_MAX) {
        int w, h;
        glfwGetFramebufferSize(r->win, &w, &h);
        ext.width = (uint32_t)(w > 0 ? w : 1);
        ext.height = (uint32_t)(h > 0 ? h : 1);
        if (ext.width < caps.minImageExtent.width) ext.width = caps.minImageExtent.width;
        if (ext.height < caps.minImageExtent.height) ext.height = caps.minImageExtent.height;
        if (ext.width > caps.maxImageExtent.width) ext.width = caps.maxImageExtent.width;
        if (ext.height > caps.maxImageExtent.height) ext.height = caps.maxImageExtent.height;
    }
    if (ext.width == 0 || ext.height == 0) return 0; /* minimised: keep what we have */
    if (r->swap) destroy_swap_resources(r);

    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > MAX_SWAP_IMAGES) count = MAX_SWAP_IMAGES;

    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    r->shot_supported =
        (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0 && format_is_rgba8(r->swap_fmt);
    r->swap_has_src = r->shot_supported && r->shot_usage;
    if (r->swap_has_src) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    VkSwapchainKHR old = r->swap;
    VkSwapchainCreateInfoKHR si = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = r->surf, .minImageCount = count,
        .imageFormat = r->swap_fmt, .imageColorSpace = r->swap_cs, .imageExtent = ext,
        .imageArrayLayers = 1, .imageUsage = usage, .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform, .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = choose_present_mode(r), .clipped = VK_TRUE, .oldSwapchain = old};
    if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR))
        si.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    VK_CHECK(vkCreateSwapchainKHR(r->dev, &si, r->ac, &r->swap));
    if (old) vkDestroySwapchainKHR(r->dev, old, r->ac);
    r->extent = ext;

    VK_CHECK(vkGetSwapchainImagesKHR(r->dev, r->swap, &r->image_count, NULL));
    if (r->image_count > MAX_SWAP_IMAGES) log_fatal("driver returned %u swapchain images", r->image_count);
    VK_CHECK(vkGetSwapchainImagesKHR(r->dev, r->swap, &r->image_count, r->images));

    create_depth(r);
    for (uint32_t i = 0; i < r->image_count; i++) {
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = r->images[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = r->swap_fmt,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VK_CHECK(vkCreateImageView(r->dev, &vi, r->ac, &r->views[i]));
        VkImageView att[2] = {r->views[i], r->depth_view};
        VkFramebufferCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = r->pass,
                                      .attachmentCount = 2, .pAttachments = att, .width = ext.width,
                                      .height = ext.height, .layers = 1};
        VK_CHECK(vkCreateFramebuffer(r->dev, &fi, r->ac, &r->fbs[i]));
        VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(r->dev, &sci, r->ac, &r->render_done[i]));
    }
    return 1;
}

static int recreate_swapchain(renderer *r)
{
    int w = 0, h = 0;
    glfwGetFramebufferSize(r->win, &w, &h);
    if (w == 0 || h == 0) return 0;
    vkDeviceWaitIdle(r->dev);
    /* Surface caps can still say 0x0 (a minimise race): then nothing is
     * destroyed and `resized` stays set so the next frame tries again. */
    if (!create_swapchain(r)) return 0;
    r->resized = 0;
    return 1;
}

/* ------------------------------------------------------------ pipelines */

static void create_render_pass(renderer *r)
{
    VkAttachmentDescription att[2] = {
        {.format = r->swap_fmt, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR},
        /* Depth is never stored: saves a full-screen write-back on tilers. */
        {.format = r->depth_fmt, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL},
    };
    VkAttachmentReference cref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference dref = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1,
                                .pColorAttachments = &cref, .pDepthStencilAttachment = &dref};
    VkSubpassDependency dep = {
        .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    VkRenderPassCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 2,
                                 .pAttachments = att, .subpassCount = 1, .pSubpasses = &sub,
                                 .dependencyCount = 1, .pDependencies = &dep};
    VK_CHECK(vkCreateRenderPass(r->dev, &ci, r->ac, &r->pass));
}

static VkShaderModule make_module(renderer *r, const uint32_t *code, size_t bytes)
{
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes,
                                   .pCode = code};
    VkShaderModule m;
    VK_CHECK(vkCreateShaderModule(r->dev, &ci, r->ac, &m));
    return m;
}

typedef struct {
    VkShaderModule vs, fs;
    const VkVertexInputBindingDescription *bindings;
    uint32_t binding_count;
    const VkVertexInputAttributeDescription *attrs;
    uint32_t attr_count;
    VkPrimitiveTopology topology;
    int depth_test, depth_write, blend;
    VkCullModeFlags cull;
    VkPipelineLayout layout;      /* 0: the world layout */
} pipe_desc;

static VkPipeline make_pipeline(renderer *r, const pipe_desc *d)
{
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = d->vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = d->fs, .pName = "main"},
    };
    VkPipelineVertexInputStateCreateInfo vin = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                                .vertexBindingDescriptionCount = d->binding_count,
                                                .pVertexBindingDescriptions = d->bindings,
                                                .vertexAttributeDescriptionCount = d->attr_count,
                                                .pVertexAttributeDescriptions = d->attrs};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = d->topology};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                            .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = d->cull,
                                                 .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    /* Reversed Z: nearer fragments have greater depth. */
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                .depthTestEnable = (VkBool32)d->depth_test,
                                                .depthWriteEnable = (VkBool32)d->depth_write,
                                                .depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL};
    VkPipelineColorBlendAttachmentState cba = {
        .blendEnable = (VkBool32)d->blend, .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                          VK_COLOR_COMPONENT_A_BIT};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = 1, .pAttachments = &cba};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkGraphicsPipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                       .stageCount = 2,
                                       .pStages = stages,
                                       .pVertexInputState = &vin,
                                       .pInputAssemblyState = &ia,
                                       .pViewportState = &vp,
                                       .pRasterizationState = &rs,
                                       .pMultisampleState = &ms,
                                       .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb,
                                       .pDynamicState = &dy,
                                       .layout = d->layout ? d->layout : r->layout,
                                       .renderPass = r->pass,
                                       .subpass = 0};
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(r->dev, VK_NULL_HANDLE, 1, &ci, r->ac, &p));
    return p;
}

static void create_pipelines(renderer *r)
{
    VkDescriptorSetLayoutBinding b = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                      .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo dl = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .bindingCount = 1, .pBindings = &b};
    VK_CHECK(vkCreateDescriptorSetLayout(r->dev, &dl, r->ac, &r->dsl));
    VkPushConstantRange pcr[2] = {{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push_vert)},
                                  {VK_SHADER_STAGE_FRAGMENT_BIT, PUSH_FRAG_OFFSET, sizeof(push_frag)}};
    VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &r->dsl, .pushConstantRangeCount = 2,
                                     .pPushConstantRanges = pcr};
    VK_CHECK(vkCreatePipelineLayout(r->dev, &pl, r->ac, &r->layout));

    VkShaderModule bvs = make_module(r, SPV_BLOCK_VERT, sizeof SPV_BLOCK_VERT);
    VkShaderModule bfs = make_module(r, SPV_BLOCK_FRAG, sizeof SPV_BLOCK_FRAG);
    VkShaderModule evs = make_module(r, SPV_ENTITY_VERT, sizeof SPV_ENTITY_VERT);
    VkShaderModule lvs = make_module(r, SPV_LINE_VERT, sizeof SPV_LINE_VERT);
    VkShaderModule lfs = make_module(r, SPV_LINE_FRAG, sizeof SPV_LINE_FRAG);

    /* Sections: packed vertices, plus the section origin as a per-instance
     * attribute picked by firstInstance, so a draw is a single call. */
    VkVertexInputBindingDescription block_bind[2] = {{0, sizeof(uint32_t), VK_VERTEX_INPUT_RATE_VERTEX},
                                                     {1, sizeof(float) * 4, VK_VERTEX_INPUT_RATE_INSTANCE}};
    VkVertexInputAttributeDescription block_attr[2] = {{0, 0, VK_FORMAT_R32_UINT, 0},
                                                       {1, 1, VK_FORMAT_R32G32B32_SFLOAT, 0}};
    /* Entities: a static unit cube instanced per body, item or particle. */
    VkVertexInputBindingDescription ent_bind[2] = {{0, sizeof(uint32_t), VK_VERTEX_INPUT_RATE_VERTEX},
                                                   {1, sizeof(entity_instance), VK_VERTEX_INPUT_RATE_INSTANCE}};
    VkVertexInputAttributeDescription ent_attr[5] = {
        {0, 0, VK_FORMAT_R32_UINT, 0},
        {1, 1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(entity_instance, pos)},
        {2, 1, VK_FORMAT_R32_UINT, offsetof(entity_instance, tex)},
        {3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(entity_instance, rot)},
        {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(entity_instance, scale)}};
    VkVertexInputBindingDescription line_bind = {0, sizeof(float) * 3, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription line_attr = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};

    pipe_desc d = {bvs,
                   bfs,
                   block_bind,
                   2,
                   block_attr,
                   2,
                   VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                   1,
                   1,
                   0,
                   VK_CULL_MODE_BACK_BIT,
                   VK_NULL_HANDLE};
    r->p_opaque = make_pipeline(r, &d);
    d.depth_write = 0;
    d.blend = 1;
    r->p_trans = make_pipeline(r, &d);
    pipe_desc e = {evs,
                   bfs,
                   ent_bind,
                   2,
                   ent_attr,
                   5,
                   VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                   1,
                   1,
                   0,
                   VK_CULL_MODE_BACK_BIT,
                   VK_NULL_HANDLE};
    r->p_entity = make_pipeline(r, &e);
    e.depth_write = 0;
    e.blend = 1;
    r->p_entity_trans = make_pipeline(r, &e);
    pipe_desc l = {
        lvs,           lfs, &line_bind, 1, &line_attr, 1, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, 1, 0, 0, VK_CULL_MODE_NONE,
        VK_NULL_HANDLE};
    r->p_line_world = make_pipeline(r, &l);
    l.depth_test = 0;
    r->p_line_screen = make_pipeline(r, &l);

    vkDestroyShaderModule(r->dev, bvs, r->ac);
    vkDestroyShaderModule(r->dev, bfs, r->ac);
    vkDestroyShaderModule(r->dev, evs, r->ac);
    vkDestroyShaderModule(r->dev, lvs, r->ac);
    vkDestroyShaderModule(r->dev, lfs, r->ac);
}

static void create_ui_pipeline(renderer *r)
{
    VkDescriptorSetLayoutBinding b = {.binding = 0,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                      .descriptorCount = 1,
                                      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo dl = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .bindingCount = 1,
                                          .pBindings = &b};
    VK_CHECK(vkCreateDescriptorSetLayout(r->dev, &dl, r->ac, &r->ui_dsl));
    VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push_ui)};
    VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = &r->ui_dsl,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &pcr};
    VK_CHECK(vkCreatePipelineLayout(r->dev, &pl, r->ac, &r->ui_layout));

    VkShaderModule vs = make_module(r, SPV_UI_VERT, sizeof SPV_UI_VERT);
    VkShaderModule fs = make_module(r, SPV_UI_FRAG, sizeof SPV_UI_FRAG);
    VkVertexInputBindingDescription bind = {0, sizeof(ui_vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attr[3] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ui_vertex, x)},
                                                 {1, 0, VK_FORMAT_R16G16_UNORM, offsetof(ui_vertex, u)},
                                                 {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(ui_vertex, rgba)}};
    pipe_desc d = {vs,          fs, &bind, 1, attr, 3, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, 0, 1, VK_CULL_MODE_NONE,
                   r->ui_layout};
    r->p_ui = make_pipeline(r, &d);
    vkDestroyShaderModule(r->dev, vs, r->ac);
    vkDestroyShaderModule(r->dev, fs, r->ac);
}

/* ------------------------------------------------------------- resources */

/* The overlay font: a small R8 atlas built on the CPU at start-up. */
static void create_font(renderer *r)
{
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8_UNORM,
                            .extent = {UI_ATLAS_W, UI_ATLAS_H, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VK_CHECK(vkCreateImage(r->dev, &ii, r->ac, &r->font));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(r->dev, r->font, &req);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = req.size,
                               .memoryTypeIndex = find_memtype(r, req.memoryTypeBits,
                                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0)};
    VK_CHECK(vkAllocateMemory(r->dev, &ai, r->ac, &r->font_mem));
    VK_CHECK(vkBindImageMemory(r->dev, r->font, r->font_mem, 0));

    gbuf st;
    buffer_create(r, &st, UI_ATLAS_W * UI_ATLAS_H, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    ui_font_build(st.map);
    VkCommandBuffer cmd = one_shot_begin(r);
    image_barrier(cmd, r->font, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                .imageExtent = {UI_ATLAS_W, UI_ATLAS_H, 1}};
    vkCmdCopyBufferToImage(cmd, st.buf, r->font, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    image_barrier(cmd, r->font, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    one_shot_end(r, cmd);
    buffer_destroy(r, &st);

    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = r->font,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = VK_FORMAT_R8_UNORM,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VK_CHECK(vkCreateImageView(r->dev, &vi, r->ac, &r->font_view));

    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                     .maxSets = 1,
                                     .poolSizeCount = 1,
                                     .pPoolSizes = &ps};
    VK_CHECK(vkCreateDescriptorPool(r->dev, &pi, r->ac, &r->ui_dpool));
    VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                       .descriptorPool = r->ui_dpool,
                                       .descriptorSetCount = 1,
                                       .pSetLayouts = &r->ui_dsl};
    VK_CHECK(vkAllocateDescriptorSets(r->dev, &dai, &r->ui_dset));
    /* The world sampler is nearest-filtered, which is what pixel text wants. */
    VkDescriptorImageInfo dii = {r->sampler, r->font_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                              .dstSet = r->ui_dset,
                              .dstBinding = 0,
                              .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                              .pImageInfo = &dii};
    vkUpdateDescriptorSets(r->dev, 1, &w, 0, NULL);
}

static void create_texture(renderer *r)
{
    VkFormat fmt = r->swap_srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = fmt, .extent = {TEX_SIZE, TEX_SIZE, 1}, .mipLevels = TEX_MIPS,
                            .arrayLayers = T_COUNT, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VK_CHECK(vkCreateImage(r->dev, &ii, r->ac, &r->tex));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(r->dev, r->tex, &req);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                               .memoryTypeIndex = find_memtype(r, req.memoryTypeBits,
                                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0)};
    VK_CHECK(vkAllocateMemory(r->dev, &ai, r->ac, &r->tex_mem));
    VK_CHECK(vkBindImageMemory(r->dev, r->tex, r->tex_mem, 0));

    gbuf st;
    uint32_t bytes = texgen_total_bytes();
    buffer_create(r, &st, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    texgen_build(st.map);

    VkCommandBuffer cmd = one_shot_begin(r);
    image_barrier(cmd, r->tex, VK_IMAGE_ASPECT_COLOR_BIT, TEX_MIPS, T_COUNT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy regions[TEX_MIPS];
    for (int m = 0; m < TEX_MIPS; m++) {
        uint32_t s = TEX_SIZE >> m;
        regions[m] = (VkBufferImageCopy){.bufferOffset = texgen_offset(0, m),
                                         .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)m, 0, T_COUNT},
                                         .imageExtent = {s, s, 1}};
    }
    vkCmdCopyBufferToImage(cmd, st.buf, r->tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, TEX_MIPS, regions);
    image_barrier(cmd, r->tex, VK_IMAGE_ASPECT_COLOR_BIT, TEX_MIPS, T_COUNT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    one_shot_end(r, cmd);
    buffer_destroy(r, &st);

    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = r->tex,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY, .format = fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, TEX_MIPS, 0, T_COUNT}};
    VK_CHECK(vkCreateImageView(r->dev, &vi, r->ac, &r->tex_view));
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                              .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT, .maxLod = (float)TEX_MIPS};
    VK_CHECK(vkCreateSampler(r->dev, &si, r->ac, &r->sampler));

    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
                                     .poolSizeCount = 1, .pPoolSizes = &ps};
    VK_CHECK(vkCreateDescriptorPool(r->dev, &pi, r->ac, &r->dpool));
    VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                       .descriptorPool = r->dpool, .descriptorSetCount = 1, .pSetLayouts = &r->dsl};
    VK_CHECK(vkAllocateDescriptorSets(r->dev, &dai, &r->dset));
    VkDescriptorImageInfo dii = {r->sampler, r->tex_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = r->dset,
                              .dstBinding = 0, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii};
    vkUpdateDescriptorSets(r->dev, 1, &w, 0, NULL);
}

static uint32_t pack_cube_vertex(const int p[3], int face)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 5 | (uint32_t)p[2] << 10 | (uint32_t)face << 15 | 3u << 18;
}

/* The shared quad index buffer, plus the unit cube falling bodies are
 * instanced from, uploaded once into device-local memory. */
static void create_static_buffers(renderer *r)
{
    /* 16-bit indices: a draw covers at most QUADS_PER_DRAW quads and the
     * vertex offset carries the base, so larger sections split in two. */
    VkDeviceSize ibytes = (VkDeviceSize)QUADS_PER_DRAW * 6 * sizeof(uint16_t);
    VkDeviceSize cbytes = 24 * sizeof(uint32_t);
    gbuf st;
    buffer_create(r, &st, ibytes + cbytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    uint16_t *idx = st.map;
    for (uint32_t q = 0; q < QUADS_PER_DRAW; q++) {
        idx[q * 6 + 0] = (uint16_t)(q * 4 + 0);
        idx[q * 6 + 1] = (uint16_t)(q * 4 + 1);
        idx[q * 6 + 2] = (uint16_t)(q * 4 + 2);
        idx[q * 6 + 3] = (uint16_t)(q * 4 + 2);
        idx[q * 6 + 4] = (uint16_t)(q * 4 + 3);
        idx[q * 6 + 5] = (uint16_t)(q * 4 + 0);
    }
    /* Same corner order and packing as the mesher, so entity.vert decodes
     * faces and texture coordinates exactly like block.vert. */
    uint32_t *cube = (uint32_t *)((uint8_t *)st.map + ibytes);
    static const int CU[4] = {0, 1, 1, 0}, CV[4] = {0, 0, 1, 1};
    for (int f = 0, n = 0; f < 6; f++) {
        int d = f / 2, sgn = (f & 1) ? -1 : 1, u = (d + 1) % 3, w = (d + 2) % 3;
        int order[4] = {0, 1, 2, 3};
        if (sgn < 0) { order[1] = 3; order[3] = 1; }
        for (int k = 0; k < 4; k++) {
            int p[3];
            p[d] = sgn > 0;
            p[u] = CU[order[k]];
            p[w] = CV[order[k]];
            cube[n++] = pack_cube_vertex(p, f);
        }
    }
    buffer_create(r, &r->index, ibytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    buffer_create(r, &r->cube, cbytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    VkCommandBuffer cmd = one_shot_begin(r);
    VkBufferCopy ci = {0, 0, ibytes}, cc = {ibytes, 0, cbytes};
    vkCmdCopyBuffer(cmd, st.buf, r->index.buf, 1, &ci);
    vkCmdCopyBuffer(cmd, st.buf, r->cube.buf, 1, &cc);
    one_shot_end(r, cmd);
    buffer_destroy(r, &st);
}

/* ------------------------------------------------------------ vertex pool */

static int pool_add_block(renderer *r, VkDeviceSize bytes)
{
    if (r->pool_blocks == MAX_POOL_BLOCKS) return 0;
    pool_block *b = &r->pool[r->pool_blocks];
    /* Integrated GPUs share RAM with the CPU: meshes are written straight
     * into the pool and the staging copy is skipped. */
    int ok = r->pool_host_visible
                 ? buffer_try_create(r, &b->buf, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
                 : buffer_try_create(r, &b->buf, bytes,
                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (!ok) return 0;
    gpupool_init(&b->alloc, (uint32_t)(bytes / sizeof(uint32_t)), POOL_GRANULE);
    r->pool_blocks++;
    if (r->pool_blocks > 1) log_info("vertex pool grew to %d blocks", r->pool_blocks);
    return 1;
}

/* Returns the pool block that took the allocation, or -1. */
static int pool_alloc(renderer *r, uint32_t nverts, uint32_t *start, uint32_t *got)
{
    for (int i = 0; i < r->pool_blocks; i++) {
        *start = gpupool_alloc(&r->pool[i].alloc, nverts, got);
        if (*start != GPUPOOL_FAIL) return i;
    }
    if (!pool_add_block(r, (VkDeviceSize)POOL_GROW_MB << 20)) return -1;
    int i = r->pool_blocks - 1;
    *start = gpupool_alloc(&r->pool[i].alloc, nverts, got);
    return *start == GPUPOOL_FAIL ? -1 : i;
}

/* A freed range may still be read by every frame submitted so far; it
 * returns to the pool once the newest of those has finished. */
static void defer_free(renderer *r, int block, uint32_t start, uint32_t len)
{
    if (r->free_count == r->free_cap) {
        r->free_cap = r->free_cap ? r->free_cap * 2 : 64;
        r->frees = mem_realloc(r->frees, mem_array_size((size_t)r->free_cap, sizeof *r->frees));
    }
    r->frees[r->free_count++] = (pool_free){start, len, (uint8_t)block, r->submitted};
}

static void release_frees(renderer *r)
{
    for (int i = 0; i < FRAMES; i++) {
        frame *f = &r->frames[i];
        if (f->serial > r->completed && vkGetFenceStatus(r->dev, f->fence) == VK_SUCCESS) r->completed = f->serial;
    }
    int n = 0;
    while (n < r->free_count && r->frees[n].serial <= r->completed) {
        const pool_free *pf = &r->frees[n++];
        gpupool_free(&r->pool[pf->block].alloc, pf->start, pf->len);
    }
    if (!n) return;
    r->free_count -= n;
    memmove(r->frees, r->frees + n, sizeof *r->frees * (size_t)r->free_count);
}

static void create_frames(renderer *r, uint32_t pool_mb)
{
    VkCommandPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                  .queueFamilyIndex = r->qfam};
    VK_CHECK(vkCreateCommandPool(r->dev, &ci, r->ac, &r->cmdpool));

    r->pool_host_visible = r->props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ||
                           r->props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    /* Developer switch: exercise the discrete-GPU staging path anywhere. */
    const char *staging = getenv("MC_POOL_STAGING");
    if (staging && staging[0] == '1') r->pool_host_visible = 0;
    if (!pool_add_block(r, (VkDeviceSize)pool_mb << 20)) log_fatal("cannot allocate a %u MB vertex pool", pool_mb);

    for (int i = 0; i < FRAMES; i++) {
        frame *f = &r->frames[i];
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                          .commandPool = r->cmdpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                          .commandBufferCount = 1};
        VK_CHECK(vkAllocateCommandBuffers(r->dev, &ai, &f->cmd));
        VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
        VK_CHECK(vkCreateFence(r->dev, &fi, r->ac, &f->fence));
        VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(r->dev, &si, r->ac, &f->image_ready));
        if (!r->pool_host_visible)
            buffer_create(r, &f->staging, STAGING_SIZE, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
        buffer_create(r, &f->dyn, DYN_SIZE, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
        buffer_create(r, &f->ui, (VkDeviceSize)UI_MAX_QUADS * 4 * sizeof(ui_vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    }
}

/* ------------------------------------------------------------ mesh hooks */

static void on_mesh_free(void *user, section_mesh *m)
{
    renderer *r = user;
    if (m->vtx_capacity) defer_free(r, m->block, m->vtx_offset, m->vtx_capacity);
    memset(m, 0, sizeof *m);
}

static int on_mesh_ready(void *user, column *c, int sy, const uint32_t *verts, const mesh_counts *mc)
{
    renderer *r = user;
    section_mesh *m = &c->mesh[sy];
    uint32_t nverts = (mc->opaque + mc->trans) * 4u;
    VkDeviceSize bytes = (VkDeviceSize)nverts * sizeof(uint32_t);
    frame *f = &r->frames[r->frame_index];

    if (nverts && !r->pool_host_visible && (!r->frame_active || f->staging_used + bytes > STAGING_SIZE))
        return 0; /* no staging room this frame: the world keeps it and retries */

    uint32_t start = 0, got = 0;
    int blk = 0;
    if (nverts) {
        blk = pool_alloc(r, nverts, &start, &got);
        if (blk < 0) {
            /* Every block is full and the driver refused another: keep the
             * old mesh rather than queueing meshes that cannot fit. */
            if (!r->pool_full_warned) log_warn("vertex pool is full and cannot grow; some terrain will be stale");
            r->pool_full_warned = 1;
            return 1;
        }
    }
    on_mesh_free(r, m);
    if (!nverts) return 1;

    if (r->pool_host_visible) {
        memcpy((uint8_t *)r->pool[blk].buf.map + (size_t)start * sizeof(uint32_t), verts, (size_t)bytes);
    } else {
        memcpy((uint8_t *)f->staging.map + f->staging_used, verts, (size_t)bytes);
        copy_list *cl = &f->copies[blk];
        if (cl->count == cl->cap) {
            cl->cap = cl->cap ? cl->cap * 2 : 64;
            cl->v = mem_realloc(cl->v, mem_array_size((size_t)cl->cap, sizeof *cl->v));
        }
        cl->v[cl->count++] = (VkBufferCopy){f->staging_used, (VkDeviceSize)start * sizeof(uint32_t), bytes};
        f->staging_used += bytes;
    }
    m->vtx_offset = start;
    m->vtx_capacity = got;
    m->opaque_quads = mc->opaque;
    m->trans_quads = mc->trans;
    memcpy(m->face_end, mc->face_end, sizeof m->face_end);
    m->block = (uint8_t)blk;
    return 1;
}

void renderer_bind_world(renderer *r, world *w)
{
    w->render_user = r;
    w->on_mesh_ready = on_mesh_ready;
    w->on_mesh_free = on_mesh_free;
    r->radius = w->radius;
}

/* ----------------------------------------------------------------- public */

renderer *renderer_create(GLFWwindow *win, const render_opts *o)
{
    renderer *r = mem_calloc(1, sizeof *r);
    r->win = win;
    r->ac = mem_vk_callbacks();
    r->vsync = o->vsync;
    r->radius = o->render_radius < 32 ? o->render_radius : 32;
    r->shot_usage = o->screenshots;

    create_instance(r, o->validate);
    VK_CHECK(glfwCreateWindowSurface(r->inst, win, r->ac, &r->surf));
    pick_device(r, o->gpu_index);
    create_device(r);
    choose_surface_format(r);
    r->depth_fmt = pick_depth_format(r);
    create_render_pass(r);
    if (!create_swapchain(r)) log_fatal("window has zero size at startup");
    create_pipelines(r);
    create_ui_pipeline(r);

    uint32_t pool_mb = o->pool_mb;
    if (!pool_mb) {
        /* Terrain measures 2.3-4.1 KB of vertices per column; 12 KB is 3x
         * the worst case, and the pool grows in blocks if that is not
         * enough. */
        uint32_t cols = (uint32_t)(3.2 * (r->radius + 1) * (r->radius + 1));
        pool_mb = (cols * 12u + 1023u) / 1024u;
        if (pool_mb < 8) pool_mb = 8;
    }
    if (pool_mb > 512) pool_mb = 512;
    create_frames(r, pool_mb);
    create_texture(r);
    create_font(r);
    create_static_buffers(r);
    log_info("vertex pool: %u MB (%s)", pool_mb, r->pool_host_visible ? "shared memory" : "device local + staging");
    return r;
}

void renderer_destroy(renderer *r)
{
    if (!r) return;
    vkDeviceWaitIdle(r->dev);
    for (int i = 0; i < FRAMES; i++) {
        frame *f = &r->frames[i];
        buffer_destroy(r, &f->staging);
        buffer_destroy(r, &f->dyn);
        buffer_destroy(r, &f->ui);
        vkDestroyFence(r->dev, f->fence, r->ac);
        vkDestroySemaphore(r->dev, f->image_ready, r->ac);
        for (int b = 0; b < MAX_POOL_BLOCKS; b++) mem_free(f->copies[b].v);
    }
    for (int b = 0; b < r->pool_blocks; b++) {
        buffer_destroy(r, &r->pool[b].buf);
        gpupool_destroy(&r->pool[b].alloc);
    }
    mem_free(r->frees);
    buffer_destroy(r, &r->index);
    buffer_destroy(r, &r->cube);
    vkDestroySampler(r->dev, r->sampler, r->ac);
    vkDestroyImageView(r->dev, r->tex_view, r->ac);
    vkDestroyImage(r->dev, r->tex, r->ac);
    vkFreeMemory(r->dev, r->tex_mem, r->ac);
    vkDestroyDescriptorPool(r->dev, r->dpool, r->ac);
    vkDestroyImageView(r->dev, r->font_view, r->ac);
    vkDestroyImage(r->dev, r->font, r->ac);
    vkFreeMemory(r->dev, r->font_mem, r->ac);
    vkDestroyDescriptorPool(r->dev, r->ui_dpool, r->ac);
    vkDestroyPipeline(r->dev, r->p_ui, r->ac);
    vkDestroyPipelineLayout(r->dev, r->ui_layout, r->ac);
    vkDestroyDescriptorSetLayout(r->dev, r->ui_dsl, r->ac);
    vkDestroyPipeline(r->dev, r->p_opaque, r->ac);
    vkDestroyPipeline(r->dev, r->p_trans, r->ac);
    vkDestroyPipeline(r->dev, r->p_entity, r->ac);
    vkDestroyPipeline(r->dev, r->p_entity_trans, r->ac);
    vkDestroyPipeline(r->dev, r->p_line_world, r->ac);
    vkDestroyPipeline(r->dev, r->p_line_screen, r->ac);
    vkDestroyPipelineLayout(r->dev, r->layout, r->ac);
    vkDestroyDescriptorSetLayout(r->dev, r->dsl, r->ac);
    vkDestroyCommandPool(r->dev, r->cmdpool, r->ac);
    destroy_swap_resources(r);
    vkDestroySwapchainKHR(r->dev, r->swap, r->ac);
    vkDestroyRenderPass(r->dev, r->pass, r->ac);
    vkDestroyDevice(r->dev, r->ac);
    vkDestroySurfaceKHR(r->inst, r->surf, r->ac);
    if (r->dbg) {
        PFN_vkDestroyDebugUtilsMessengerEXT fn =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(r->inst, "vkDestroyDebugUtilsMessengerEXT");
        if (fn) fn(r->inst, r->dbg, r->ac);
    }
    vkDestroyInstance(r->inst, r->ac);
    mem_free(r->vis);
    mem_free(r);
}

void renderer_on_resize(renderer *r) { r->resized = 1; }

void renderer_set_vsync(renderer *r, int on)
{
    if (!!on == !!r->vsync) return;
    r->vsync = !!on;
    r->resized = 1; /* the present mode is fixed per swapchain */
}

const char *renderer_device_name(const renderer *r) { return r->props.deviceName; }
render_stats renderer_stats(const renderer *r) { return r->stats; }

void renderer_request_screenshot(renderer *r, const char *path)
{
    size_t n = strlen(path);
    if (n >= sizeof r->shot_path) {
        log_error("screenshot path too long");
        return;
    }
    memcpy(r->shot_path, path, n + 1);
    r->shot_pending = 1;
    if (!r->shot_usage) {
        /* Rebuild the swapchain with TRANSFER_SRC; the shot is taken on
         * the first frame that has it. */
        r->shot_usage = 1;
        r->resized = 1;
    }
}

int renderer_begin_frame(renderer *r)
{
    r->frame_active = 0;
    release_frees(r);
    int w = 0, h = 0;
    glfwGetFramebufferSize(r->win, &w, &h);
    if (w == 0 || h == 0) return 0;
    if (r->resized && !recreate_swapchain(r)) return 0;

    frame *f = &r->frames[r->frame_index];
    VK_CHECK(vkWaitForFences(r->dev, 1, &f->fence, VK_TRUE, UINT64_MAX));
    release_frees(r);
    f->staging_used = 0;
    for (int b = 0; b < MAX_POOL_BLOCKS; b++) f->copies[b].count = 0;

    VkResult res = vkAcquireNextImageKHR(r->dev, r->swap, UINT64_MAX, f->image_ready, VK_NULL_HANDLE,
                                         &r->image_index);
    if (res == VK_ERROR_OUT_OF_DATE_KHR) {
        r->resized = 1;
        recreate_swapchain(r);
        return 0;
    }
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) log_fatal("vkAcquireNextImageKHR failed (%d)", (int)res);
    VK_CHECK(vkResetFences(r->dev, 1, &f->fence));
    r->frame_active = 1;
    return 1;
}

static void write_screenshot(renderer *r, const gbuf *b)
{
    FILE *fp = fopen(r->shot_path, "wb");
    if (!fp) {
        log_error("cannot write screenshot %s", r->shot_path);
        return;
    }
    uint32_t w = r->extent.width, h = r->extent.height;
    int ok = fprintf(fp, "P6\n%u %u\n255\n", w, h) > 0;
    const uint8_t *px = b->map;
    int bgr = r->swap_fmt == VK_FORMAT_B8G8R8A8_SRGB || r->swap_fmt == VK_FORMAT_B8G8R8A8_UNORM;
    uint8_t *row = mem_alloc((size_t)w * 3);
    for (uint32_t y = 0; y < h && ok; y++) {
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *p = px + ((size_t)y * w + x) * 4;
            row[x * 3 + 0] = bgr ? p[2] : p[0];
            row[x * 3 + 1] = p[1];
            row[x * 3 + 2] = bgr ? p[0] : p[2];
        }
        ok = fwrite(row, 1, (size_t)w * 3, fp) == (size_t)w * 3;
    }
    mem_free(row);
    ok &= fclose(fp) == 0;
    if (ok) log_info("screenshot saved to %s", r->shot_path);
    else log_error("writing screenshot %s failed", r->shot_path);
}

/* Issues one section range, split where 16-bit indices run out. */
/* Texture layers with transparent texels, judged by an entity's side face
 * (the item sprites, glass, ice, water, cracks and flames). */
static int layer_has_alpha(uint32_t tex)
{
    uint32_t l = tex & 255u;
    return l == T_GLASS || l == T_ICE || l == T_WATER || (l >= T_ITEM_STICK && l <= T_ITEM_WATER_BUCKET) ||
           (l >= T_CRACK0 && l <= T_FIRE3);
}

static void draw_quads(VkCommandBuffer cmd, uint32_t first_vertex, uint32_t quads, uint32_t instance,
                       render_stats *st)
{
    while (quads) {
        uint32_t n = quads < QUADS_PER_DRAW ? quads : QUADS_PER_DRAW;
        vkCmdDrawIndexed(cmd, n * 6, 1, 0, (int32_t)first_vertex, instance);
        first_vertex += n * 4;
        quads -= n;
        st->draw_calls++;
        st->quads += n;
    }
}

static void bind_pool(VkCommandBuffer cmd, const renderer *r, int block, int *bound)
{
    if (*bound == block) return;
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &r->pool[block].buf.buf, &zero);
    *bound = block;
}

/* Bit f set: face group f (+X, -X, +Y, -Y, +Z, -Z) of a section whose min
 * corner is at camera-relative o can face the camera. */
static uint32_t facing_groups(vec3 o)
{
    return (uint32_t)(o.x < 0) | (uint32_t)(o.x + 16 > 0) << 1 | (uint32_t)(o.y < 0) << 2 |
           (uint32_t)(o.y + 16 > 0) << 3 | (uint32_t)(o.z < 0) << 4 | (uint32_t)(o.z + 16 > 0) << 5;
}

void renderer_end_frame(renderer *r, const world *w, const physics *ph, const render_view *v, double alpha)
{
    if (!r->frame_active) return;
    frame *f = &r->frames[r->frame_index];
    VkCommandBuffer cmd = f->cmd;
    VK_CHECK(vkResetCommandBuffer(cmd, 0));
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));

    int copied = 0;
    for (int b = 0; b < r->pool_blocks; b++) {
        const copy_list *cl = &f->copies[b];
        if (!cl->count) continue;
        vkCmdCopyBuffer(cmd, f->staging.buf, r->pool[b].buf.buf, (uint32_t)cl->count, cl->v);
        copied = 1;
    }
    if (copied) {
        VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                              .dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &mb, 0,
                             NULL, 0, NULL);
    }

    /* Camera and fog. */
    float aspect = (float)r->extent.width / (float)r->extent.height;
    mat4 vp = m4_mul(m4_perspective_revz(v->fov, aspect, ZNEAR), m4_view_rot_roll(v->yaw, v->pitch, v->roll));
    frustum fr = frustum_from(vp);
    float sky[3] = {0.53f, 0.74f, 1.0f}, water_fog[3] = {0.10f, 0.25f, 0.55f};
    if (v->sky[0] > 0.0f || v->sky[1] > 0.0f || v->sky[2] > 0.0f) memcpy(sky, v->sky, sizeof sky);
    float daylight = v->daylight > 0.0f ? v->daylight : 1.0f;
    if (v->underwater)
        for (int i = 0; i < 3; i++) water_fog[i] *= 0.25f + 0.75f * daylight;
    const float *fogc = v->underwater ? water_fog : sky;
    float fog_end = v->underwater ? 24.0f : (float)(r->radius * CHUNK_W) - 8.0f;
    float fog_start = v->underwater ? 0.0f : fog_end * 0.55f;
    push_vert pv;
    memcpy(pv.view_proj, vp.m, sizeof pv.view_proj);
    /* Blocks: the camera position (wrapped to a 256 m tile, a multiple of
     * every wave length) so animations are anchored to the world. */
    pv.origin[0] = (float)fmod(v->eye.x, 256.0);
    pv.origin[1] = (float)v->eye.y;
    pv.origin[2] = (float)fmod(v->eye.z, 256.0);
    pv.origin[3] = 0.0f;
    pv.fog[0] = fog_start;
    pv.fog[1] = 1.0f / (fog_end - fog_start);
    pv.fog[2] = (float)fmod((double)v->time, 3600.0);
    pv.fog[3] = daylight;
    push_frag pf;
    for (int i = 0; i < 3; i++) pf.color[i] = r->swap_srgb ? srgb_to_linear(fogc[i]) : fogc[i];
    pf.color[3] = 1.0f;

    VkClearValue clears[2];
    clears[0].color = (VkClearColorValue){{pf.color[0], pf.color[1], pf.color[2], 1.0f}};
    clears[1].depthStencil = (VkClearDepthStencilValue){0.0f, 0}; /* reversed Z */
    VkRenderPassBeginInfo rp = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = r->pass,
                                .framebuffer = r->fbs[r->image_index], .renderArea = {{0, 0}, r->extent},
                                .clearValueCount = 2, .pClearValues = clears};
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport = {0, 0, (float)r->extent.width, (float)r->extent.height, 0.0f, 1.0f};
    VkRect2D scissor = {{0, 0}, r->extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    /* Collect visible sections. The world's spiral is sorted by distance and
     * sections go nearest the eye height first, so the list is roughly
     * front to back without sorting. Each origin is written to the dynamic
     * buffer as that draw's instance attribute. */
    uint8_t *dyn = f->dyn.map;
    float (*origins)[4] = (float (*)[4])dyn;
    int nvis = 0;
    int ccx = chunk_of((int)floor(v->eye.x)), ccz = chunk_of((int)floor(v->eye.z));
    int ey = (int)floor(v->eye.y) / SECTION_H;
    if (ey < 0) ey = 0;
    if (ey >= SECTIONS) ey = SECTIONS - 1;
    int rad = r->radius;
    if (r->vis_cap < w->spiral_count * SECTIONS) {
        mem_free(r->vis);
        r->vis_cap = w->spiral_count * SECTIONS;
        r->vis = mem_alloc(mem_array_size((size_t)r->vis_cap, sizeof(visible)));
    }
    for (int i = 0; i < w->spiral_count; i++) {
        int dx = w->spiral[i][0], dz = w->spiral[i][1];
        if (dx * dx + dz * dz > rad * rad) break;
        const column *c = world_column(w, ccx + dx, ccz + dz);
        if (!c) continue;
        float ox = (float)((double)(c->cx * CHUNK_W) - v->eye.x), oz = (float)((double)(c->cz * CHUNK_W) - v->eye.z);
        if (!frustum_box(&fr, v3(ox, (float)-v->eye.y, oz), v3(CHUNK_W, WORLD_H, CHUNK_W))) continue;
        for (int k = 0; k < 2 * SECTIONS; k++) {
            int sy = ey + ((k & 1) ? (k + 1) / 2 : -(k / 2));
            if (sy < 0 || sy >= SECTIONS) continue;
            const section_mesh *m = &c->mesh[sy];
            if (!m->vtx_capacity) continue;
            vec3 o = v3(ox, (float)((double)(sy * SECTION_H) - v->eye.y), oz);
            if (!frustum_box(&fr, o, v3(16, 16, 16))) continue;
            if (nvis == r->vis_cap || nvis == MAX_VIS) break;
            origins[nvis][0] = o.x;
            origins[nvis][1] = o.y;
            origins[nvis][2] = o.z;
            origins[nvis][3] = 0.0f;
            r->vis[nvis++] = (visible){m, facing_groups(o)};
        }
    }
    VkDeviceSize dyn_used = (VkDeviceSize)nvis * sizeof origins[0];

    render_stats st = {0};
    VkDeviceSize zero = 0;
    int bound = -1;

    /* Opaque, front to back for early-Z. Face groups that point away from
     * the camera are skipped; adjacent groups merge into one draw. */
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->p_opaque);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->layout, 0, 1, &r->dset, 0, NULL);
    vkCmdBindVertexBuffers(cmd, 1, 1, &f->dyn.buf, &zero);
    vkCmdBindIndexBuffer(cmd, r->index.buf, 0, VK_INDEX_TYPE_UINT16);
    vkCmdPushConstants(cmd, r->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof pv, &pv);
    vkCmdPushConstants(cmd, r->layout, VK_SHADER_STAGE_FRAGMENT_BIT, PUSH_FRAG_OFFSET, sizeof pf, &pf);
    for (int i = 0; i < nvis; i++) {
        const section_mesh *m = r->vis[i].m;
        if (!m->opaque_quads) continue;
        bind_pool(cmd, r, m->block, &bound);
        uint32_t faces = r->vis[i].faces;
        for (int g = 0; g < 6;) {
            if (!(faces >> g & 1)) { g++; continue; }
            int e = g;
            while (e + 1 < 6 && (faces >> (e + 1) & 1)) e++;
            uint32_t q0 = g ? m->face_end[g - 1] : 0, q1 = m->face_end[e];
            if (q1 > q0) draw_quads(cmd, m->vtx_offset + q0 * 4, q1 - q0, (uint32_t)i, &st);
            g = e + 1;
        }
    }

    /* Entities: falling bodies plus the game's items, particles and
     * overlays, one static cube instanced per entity. Opaque instances are
     * packed from the front and translucent ones (glass, ice, item cards,
     * cracks) from the back, so each group is one draw. */
    int n_opaque = 0, n_trans = 0;
    VkDeviceSize ent_off = dyn_used;
    int nb = ph ? ph->body_count : 0;
    int ng_o = v->ents ? clampi(v->ent_opaque, 0, RENDER_MAX_ENTS) : 0;
    int ng_t = v->ents ? clampi(v->ent_trans, 0, RENDER_MAX_ENTS - ng_o) : 0;
    int total = nb + ng_o + ng_t;
    if (total) {
        entity_instance *insts = (entity_instance *)(dyn + ent_off);
        for (int i = 0; i < nb; i++) {
            const body *b = &ph->bodies[i];
            dvec3 p = dv3_lerp(b->prev_pos, b->pos, alpha);
            const block_def *bd = block_get(b->block);
            int trans = (bd->flags & BF_TRANSLUCENT) != 0;
            int slot = trans ? total - 1 - n_trans++ : n_opaque++;
            insts[slot] = (entity_instance){{(float)(p.x + 0.5 - v->eye.x), (float)(p.y + 0.5 - v->eye.y),
                                             (float)(p.z + 0.5 - v->eye.z)},
                                            (uint32_t)bd->tex[0] | (uint32_t)bd->tex[2] << 8 |
                                                (uint32_t)bd->tex[3] << 16,
                                            {b->rot[0], b->rot[1], b->rot[2], b->rot[3]},
                                            {1.0f, 1.0f, 1.0f},
                                            1.0f};
        }
        for (int i = 0; i < ng_o; i++) insts[n_opaque++] = v->ents[i];
        for (int i = 0; i < ng_t; i++) insts[total - 1 - n_trans++] = v->ents[ng_o + i];
        dyn_used += (VkDeviceSize)total * sizeof(entity_instance);
        if (n_opaque) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->p_entity);
            VkBuffer bufs[2] = {r->cube.buf, f->dyn.buf};
            VkDeviceSize offs[2] = {0, ent_off};
            vkCmdBindVertexBuffers(cmd, 0, 2, bufs, offs);
            bound = -1;
            vkCmdDrawIndexed(cmd, 36, (uint32_t)n_opaque, 0, 0, 0);
            st.draw_calls++;
        }
    }

    /* Translucent, back to front. */
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->p_trans);
    vkCmdBindVertexBuffers(cmd, 1, 1, &f->dyn.buf, &zero);
    for (int i = nvis - 1; i >= 0; i--) {
        const section_mesh *m = r->vis[i].m;
        if (!m->trans_quads) continue;
        bind_pool(cmd, r, m->block, &bound);
        draw_quads(cmd, m->vtx_offset + m->opaque_quads * 4, m->trans_quads, (uint32_t)i, &st);
    }

    if (n_trans) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->p_entity_trans);
        VkBuffer bufs[2] = {r->cube.buf, f->dyn.buf};
        VkDeviceSize offs[2] = {0, ent_off};
        vkCmdBindVertexBuffers(cmd, 0, 2, bufs, offs);
        vkCmdDrawIndexed(cmd, 36, (uint32_t)n_trans, 0, 0, (uint32_t)(total - n_trans));
        st.draw_calls++;
    }

    /* Selection outline and crosshair. */
    dyn_used = (dyn_used + 15) & ~(VkDeviceSize)15;
    float *lines = (float *)(dyn + dyn_used);
    int nl = 0;
    if (v->has_selection) {
        const float e = 0.002f;
        static const int E[12][2][3] = {
            {{0, 0, 0}, {1, 0, 0}}, {{0, 0, 1}, {1, 0, 1}}, {{0, 1, 0}, {1, 1, 0}}, {{0, 1, 1}, {1, 1, 1}},
            {{0, 0, 0}, {0, 1, 0}}, {{1, 0, 0}, {1, 1, 0}}, {{0, 0, 1}, {0, 1, 1}}, {{1, 0, 1}, {1, 1, 1}},
            {{0, 0, 0}, {0, 0, 1}}, {{1, 0, 0}, {1, 0, 1}}, {{0, 1, 0}, {0, 1, 1}}, {{1, 1, 0}, {1, 1, 1}}};
        for (int i = 0; i < 12; i++)
            for (int k = 0; k < 2; k++)
                for (int a = 0; a < 3; a++) lines[nl++] = E[i][k][a] ? 1.0f + e : -e;
    }
    int sel_verts = nl / 3;
    float cs = 0.025f;
    float cross[12] = {-cs / aspect, 0, 0, cs / aspect, 0, 0, 0, -cs, 0, 0, cs, 0};
    memcpy(lines + nl, cross, sizeof cross);
    VkDeviceSize line_off = dyn_used;
    vkCmdBindVertexBuffers(cmd, 0, 1, &f->dyn.buf, &line_off);
    if (sel_verts) {
        push_vert lp = pv;
        lp.origin[0] = (float)(v->selection.x - v->eye.x);
        lp.origin[1] = (float)(v->selection.y - v->eye.y);
        lp.origin[2] = (float)(v->selection.z - v->eye.z);
        push_frag lc = {{0.02f, 0.02f, 0.02f, 1.0f}};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->p_line_world);
        vkCmdPushConstants(cmd, r->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof lp, &lp);
        vkCmdPushConstants(cmd, r->layout, VK_SHADER_STAGE_FRAGMENT_BIT, PUSH_FRAG_OFFSET, sizeof lc, &lc);
        vkCmdDraw(cmd, (uint32_t)sel_verts, 1, 0, 0);
        st.draw_calls++;
    }
    /* The view model: its own projection (a fixed 70 degree view, so a wide
     * FOV setting does not stretch the arm) over a cleared depth buffer, so
     * it never sinks into walls. */
    int nvm = v->view_model ? clampi(v->view_model_count, 0, 16) : 0;
    if (nvm) {
        VkDeviceSize vm_off = (line_off + DYN_LINES_BYTES + 63) & ~(VkDeviceSize)63;
        /* Solid parts first, then anything with transparent texels (item
         * cards, glass, a held campfire), which needs blending. */
        entity_instance *vm = (entity_instance *)(dyn + vm_off);
        int vm_solid = 0, vm_cards = 0;
        for (int i = 0; i < nvm; i++)
            if (!layer_has_alpha(v->view_model[i].tex)) vm[vm_solid++] = v->view_model[i];
        for (int i = 0; i < nvm; i++)
            if (layer_has_alpha(v->view_model[i].tex)) vm[vm_solid + vm_cards++] = v->view_model[i];
        VkClearAttachment ca = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .clearValue.depthStencil = {0.0f, 0}};
        VkClearRect cr = {.rect = {{0, 0}, r->extent}, .baseArrayLayer = 0, .layerCount = 1};
        vkCmdClearAttachments(cmd, 1, &ca, 1, &cr);
        push_vert mp = pv;
        mat4 proj = m4_perspective_revz(70.0f * (float)(MC_PI / 180.0), aspect, 0.02f);
        memcpy(mp.view_proj, proj.m, sizeof mp.view_proj);
        mp.fog[0] = 1e6f; /* no fog on the hands; fog.y = 0 also tells entity.vert */
        mp.fog[1] = 0.0f;
        mp.fog[3] = 1.0f;
        VkBuffer bufs[2] = {r->cube.buf, f->dyn.buf};
        VkDeviceSize offs[2] = {0, vm_off};
        if (vm_solid) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->p_entity);
            vkCmdPushConstants(cmd, r->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof mp, &mp);
            vkCmdPushConstants(cmd, r->layout, VK_SHADER_STAGE_FRAGMENT_BIT, PUSH_FRAG_OFFSET, sizeof pf, &pf);
            vkCmdBindVertexBuffers(cmd, 0, 2, bufs, offs);
            vkCmdDrawIndexed(cmd, 36, (uint32_t)vm_solid, 0, 0, 0);
            st.draw_calls++;
        }
        if (vm_cards) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->p_entity_trans);
            vkCmdPushConstants(cmd, r->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof mp, &mp);
            vkCmdPushConstants(cmd, r->layout, VK_SHADER_STAGE_FRAGMENT_BIT, PUSH_FRAG_OFFSET, sizeof pf, &pf);
            vkCmdBindVertexBuffers(cmd, 0, 2, bufs, offs);
            vkCmdDrawIndexed(cmd, 36, (uint32_t)vm_cards, 0, 0, (uint32_t)vm_solid);
            st.draw_calls++;
        }
        vkCmdBindVertexBuffers(cmd, 0, 1, &f->dyn.buf, &line_off); /* the crosshair's vertices */
    }
    if (!v->hide_crosshair) {
        push_vert cp;
        memset(&cp, 0, sizeof cp);
        mat4 id = m4_identity();
        memcpy(cp.view_proj, id.m, sizeof cp.view_proj);
        push_frag cc = {{0.95f, 0.95f, 0.95f, 1.0f}};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->p_line_screen);
        vkCmdPushConstants(cmd, r->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof cp, &cp);
        vkCmdPushConstants(cmd, r->layout, VK_SHADER_STAGE_FRAGMENT_BIT, PUSH_FRAG_OFFSET, sizeof cc, &cc);
        vkCmdDraw(cmd, 4, 1, (uint32_t)sel_verts, 0);
        st.draw_calls++;
    }

    /* Overlay: one indexed draw of the quads the game wrote this frame. */
    int ui_quads = v->ui_quads < (int)UI_MAX_QUADS ? v->ui_quads : (int)UI_MAX_QUADS;
    if (ui_quads > 0) {
        push_ui up = {{2.0f / (float)r->extent.width, 2.0f / (float)r->extent.height},
                      {-1.0f, -1.0f},
                      r->swap_srgb ? 1.0f : 0.0f,
                      {0, 0, 0}};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->p_ui);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->ui_layout, 0, 1, &r->ui_dset, 0, NULL);
        vkCmdPushConstants(cmd, r->ui_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof up, &up);
        VkDeviceSize zero_off = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &f->ui.buf, &zero_off);
        vkCmdBindIndexBuffer(cmd, r->index.buf, 0, VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(cmd, (uint32_t)ui_quads * 6, 1, 0, 0, 0);
        st.draw_calls++;
    }

    vkCmdEndRenderPass(cmd);

    int shot = r->shot_pending && r->swap_has_src;
    gbuf shot_buf = {0};
    if (shot) {
        VkDeviceSize bytes = (VkDeviceSize)r->extent.width * r->extent.height * 4;
        buffer_create(r, &shot_buf, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        VkImage img = r->images[r->image_index];
        image_barrier(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    .imageExtent = {r->extent.width, r->extent.height, 1}};
        vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shot_buf.buf, 1, &region);
        image_barrier(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_READ_BIT, 0,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    } else if (r->shot_pending && !r->shot_supported) {
        log_error("this surface does not allow reading back swapchain images");
        r->shot_pending = 0;
    }

    VK_CHECK(vkEndCommandBuffer(cmd));

    f->serial = ++r->submitted;
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
                       .pWaitSemaphores = &f->image_ready, .pWaitDstStageMask = &wait_stage,
                       .commandBufferCount = 1, .pCommandBuffers = &cmd, .signalSemaphoreCount = 1,
                       .pSignalSemaphores = &r->render_done[r->image_index]};
    VK_CHECK(vkQueueSubmit(r->queue, 1, &si, f->fence));

    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                           .pWaitSemaphores = &r->render_done[r->image_index], .swapchainCount = 1,
                           .pSwapchains = &r->swap, .pImageIndices = &r->image_index};
    VkResult res = vkQueuePresentKHR(r->queue, &pi);
    if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) r->resized = 1;
    else if (res != VK_SUCCESS) log_fatal("vkQueuePresentKHR failed (%d)", (int)res);

    if (shot) {
        VK_CHECK(vkWaitForFences(r->dev, 1, &f->fence, VK_TRUE, UINT64_MAX));
        write_screenshot(r, &shot_buf);
        buffer_destroy(r, &shot_buf);
        r->shot_pending = 0;
    }

    st.sections_drawn = nvis;
    for (int b = 0; b < r->pool_blocks; b++) {
        st.pool_used_kb += (uint32_t)((uint64_t)r->pool[b].alloc.used * 4 / 1024);
        st.pool_total_kb += (uint32_t)((uint64_t)r->pool[b].alloc.total * 4 / 1024);
    }
    r->stats = st;
    r->frame_active = 0;
    r->frame_index = (r->frame_index + 1) % FRAMES;
}

ui_vertex *renderer_ui_buffer(renderer *r, int *max_quads, int *fb_w, int *fb_h)
{
    *fb_w = (int)r->extent.width;
    *fb_h = (int)r->extent.height;
    if (!r->frame_active) {
        *max_quads = 0;
        return NULL;
    }
    *max_quads = (int)UI_MAX_QUADS;
    return r->frames[r->frame_index].ui.map;
}
