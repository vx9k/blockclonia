/* Choosing which Vulkan features the renderer turns on.
 *
 * The renderer needs Vulkan 1.2 (GPU_API_MIN) and uses what newer
 * versions and extensions offer, up to 1.4 desktop drivers. Each optional
 * feature saves CPU time, memory or stutter, and each has a fallback on a
 * bare 1.2 device. The decision what to use is made here, from plain
 * numbers and booleans the renderer read from the instance and the device,
 * so it can be unit tested without a GPU (tests/test_gpucaps.c) and the
 * rules live in one place:
 *
 *   - API version: instance = min(loader, headers, 1.4, MC_VK_API);
 *     device = min(device, instance). Either below 1.2 is refused
 *     (gpucaps_usable). A feature is only used when its core version (or,
 *     for dynamic rendering and synchronization2 on 1.2, its extension) is
 *     available at the negotiated version.
 *   - Dependencies: multi-draw indirect needs drawIndirectFirstInstance too
 *     (the section origin rides on firstInstance); dynamic rendering needs
 *     synchronization2 (its layout transitions use vkCmdPipelineBarrier2).
 *   - Required, so not optional bits: 1.2's timeline semaphores (frame
 *     pacing) and host query reset (timestamps), which every 1.2 device
 *     supports.
 *   - Overrides for testing the fallbacks on a capable machine:
 *     MC_VK_API=1.2|1.3|1.4 caps the version and MC_VK_DISABLE is a
 *     comma list of feature names (see gpucaps_name) or "all".
 *
 * No Vulkan headers here: versions use Vulkan's own packing
 * (major << 22 | minor << 12 | patch), so they compare directly with
 * VkPhysicalDeviceProperties::apiVersion. */
#ifndef MC_GPUCAPS_H
#define MC_GPUCAPS_H

#include <stddef.h>
#include <stdint.h>

/* The same packing as VK_MAKE_API_VERSION(0, major, minor, 0). */
#define GPU_API(major, minor) ((uint32_t)(major) << 22 | (uint32_t)(minor) << 12)
#define GPU_API_MAJOR(v) ((unsigned)((v) >> 22) & 0x7Fu)
#define GPU_API_MINOR(v) ((unsigned)((v) >> 12) & 0x3FFu)
/* The newest version the renderer knows how to use, and the oldest it
 * runs on: 1.2 brings timeline semaphores and host query reset as
 * required features, and every desktop driver and lavapipe offer it. */
#define GPU_API_MAX GPU_API(1, 4)
#define GPU_API_MIN GPU_API(1, 2)

/* Optional features, as bits of gpu_choice.enabled and of the disable mask. */
/* clang-format off */
#define GPUCAP_MDI         (1u << 0) /* multi-draw indirect for terrain sections (1.0 optional) */
#define GPUCAP_DYNREND     (1u << 1) /* dynamic rendering instead of a VkRenderPass (1.3, KHR on 1.2) */
#define GPUCAP_SYNC2       (1u << 2) /* vkCmdPipelineBarrier2 / vkQueueSubmit2 (1.3, KHR on 1.2) */
#define GPUCAP_ANISO       (1u << 3) /* anisotropic minification (optional samplerAnisotropy) */
#define GPUCAP_HOSTCOPY    (1u << 4) /* textures uploaded without a staging buffer (1.4) */
#define GPUCAP_BUDGET      (1u << 5) /* VK_EXT_memory_budget: VRAM use in the stats */
#define GPUCAP_TIMESTAMPS  (1u << 6) /* GPU frame time from timestamp queries */
#define GPUCAP_PCACHE      (1u << 7) /* pipeline cache kept on disk between runs */
#define GPUCAP_RELAXED     (1u << 8) /* FIFO_RELAXED present mode with vsync on */
#define GPUCAP_COUNT 9
/* clang-format on */
#define GPUCAP_ALL ((1u << GPUCAP_COUNT) - 1u)

/* What the instance and device report. All flags 0/1; zero-initialise and
 * fill in what was queried. */
typedef struct {
    uint32_t instance_api; /* negotiated instance version (gpucaps_instance_api) */
    uint32_t device_api;   /* VkPhysicalDeviceProperties::apiVersion */
    /* Vulkan 1.0 features and limits */
    int multi_draw_indirect, draw_indirect_first_instance;
    uint32_t max_draw_indirect_count;
    int sampler_anisotropy;
    float max_sampler_anisotropy;
    int timestamp_compute_and_graphics;
    uint32_t timestamp_valid_bits; /* of the queue family the renderer uses */
    float timestamp_period;        /* ns per tick */
    /* 1.3 features, or on a 1.2 device the features of the KHR extensions */
    int dynamic_rendering, synchronization2;
    int khr_dynamic_rendering, khr_synchronization2; /* the extensions are listed */
    /* 1.4 */
    int host_image_copy;
    /* extensions and surface */
    int ext_memory_budget;
    int portability_subset; /* VK_KHR_portability_subset is listed: MoltenVK */
    int present_relaxed; /* the surface offers VK_PRESENT_MODE_FIFO_RELAXED_KHR */
    int pipeline_cache;  /* a cache file path is configured */
} gpu_offer;

typedef struct {
    uint32_t instance_api, device_api; /* device_api = min(device, instance) */
    uint32_t enabled;                  /* GPUCAP_* bits in use */
    uint32_t disabled;                 /* bits the overrides turned off (for the log) */
    int dynrend_khr, sync2_khr;        /* enabled through the KHR extension (1.2), not core */
    float anisotropy;                  /* max anisotropy for the filtered sampler: 1 when off */
    uint32_t max_draw_indirect_count;  /* draws per vkCmdDrawIndexedIndirect */
    double timestamp_ms_per_tick;      /* timestampPeriod in ms */
    uint64_t timestamp_mask;           /* timestampValidBits as a mask */
} gpu_choice;

/* instance = min(loader, headers, GPU_API_MAX, cap); cap 0 means none.
 * A loader without vkEnumerateInstanceVersion is 1.0. Patch bits are
 * dropped: 1.3.275 negotiates as 1.3. */
uint32_t gpucaps_instance_api(uint32_t loader, uint32_t headers, uint32_t cap);
/* min(device, instance), patch bits dropped. */
uint32_t gpucaps_device_api(uint32_t device, uint32_t instance);
/* 1 when a device reporting `device` can run under an instance created at
 * `instance`: both at GPU_API_MIN or newer. */
int gpucaps_usable(uint32_t device, uint32_t instance);

/* "1.2" -> GPU_API(1, 2). Returns 0 (and leaves *out) unless the string is
 * one of 1.2 .. 1.4; NULL or "" also return 0. */
int gpucaps_parse_api(const char *s, uint32_t *out);
/* "mdi, dynrend,aniso" -> GPUCAP_MDI | GPUCAP_DYNREND | GPUCAP_ANISO. Case
 * and blanks are ignored, "all" disables everything. Unknown names are
 * copied, comma separated, to `unknown` (may be NULL) so they can be logged. */
uint32_t gpucaps_parse_disable(const char *s, char *unknown, size_t unknown_len);
/* Short name of one GPUCAP_* bit ("mdi"), as MC_VK_DISABLE spells it; NULL
 * for anything else. */
const char *gpucaps_name(uint32_t bit);

/* Decides what to enable: what the device offers at the negotiated
 * version, minus `disable`, with dependencies resolved. */
void gpucaps_choose(const gpu_offer *o, uint32_t disable, gpu_choice *c);

/* "Vulkan 1.4: multi-draw indirect, dynamic rendering, ...", or just
 * "Vulkan 1.2" when nothing optional is on. Always NUL-terminated. */
void gpucaps_summary(const gpu_choice *c, char *buf, size_t n);

/* A saved pipeline cache is only handed to the driver when its header
 * (VkPipelineCacheHeaderVersionOne: header size, version 1, vendor ID,
 * device ID, pipelineCacheUUID) matches this device and it is no larger
 * than `max_size`. Drivers must reject stale data themselves, but some
 * crash on garbage, so a torn or foreign file is dropped here first. */
#define GPUCACHE_HEADER_SIZE 32u
int gpucaps_pipeline_cache_ok(const void *data, size_t size, size_t max_size, uint32_t vendor, uint32_t device,
                              const uint8_t uuid[16]);
/* Reads a saved pipeline cache through save_read_file (a regular file, no
 * symlink followed, at most max_size bytes) and returns it in a mem_alloc
 * buffer with its length in *size, or NULL (*size 0) when the file is
 * missing or unreadable or gpucaps_pipeline_cache_ok rejects it. */
void *gpucaps_pipeline_cache_load(const char *path, size_t max_size, uint32_t vendor, uint32_t device,
                                  const uint8_t uuid[16], size_t *size);

#endif
