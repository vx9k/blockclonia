/* Choosing Vulkan features from what the GPU offers (gpucaps.c). */
#include "gpucaps.h"
#include "test_util.h"

#include <string.h>

/* A 1.4 desktop device offering everything the renderer can use. */
static gpu_offer full_offer(void)
{
    gpu_offer o;
    memset(&o, 0, sizeof o);
    o.instance_api = GPU_API(1, 4);
    o.device_api = GPU_API(1, 4) | 363u; /* patch bits, as drivers report */
    o.multi_draw_indirect = o.draw_indirect_first_instance = 1;
    o.max_draw_indirect_count = UINT32_MAX;
    o.sampler_anisotropy = 1;
    o.max_sampler_anisotropy = 16.0f;
    o.timestamp_compute_and_graphics = 1;
    o.timestamp_valid_bits = 36;
    o.timestamp_period = 52.08f;
    o.timeline_semaphore = o.host_query_reset = 1;
    o.dynamic_rendering = o.synchronization2 = 1;
    o.khr_dynamic_rendering = o.khr_synchronization2 = 1;
    o.host_image_copy = 1;
    o.ext_memory_budget = 1;
    o.present_relaxed = 1;
    o.pipeline_cache = 1;
    return o;
}

static void test_versions(void)
{
    /* The minimum of loader, headers, 1.4 and the cap; patch dropped. */
    CHECK(gpucaps_instance_api(GPU_API(1, 4) | 309u, GPU_API(1, 4) | 357u, 0) == GPU_API(1, 4));
    CHECK(gpucaps_instance_api(GPU_API(1, 4), GPU_API(1, 3) | 239u, 0) == GPU_API(1, 3)); /* bookworm headers */
    CHECK(gpucaps_instance_api(GPU_API(1, 2), GPU_API(1, 4), 0) == GPU_API(1, 2));        /* old loader */
    CHECK(gpucaps_instance_api(GPU_API(1, 5), GPU_API(1, 5), 0) == GPU_API_MAX);          /* newer than we know */
    CHECK(gpucaps_instance_api(GPU_API(1, 4), GPU_API(1, 4), GPU_API(1, 1)) == GPU_API(1, 1));
    CHECK(gpucaps_instance_api(GPU_API(1, 2), GPU_API(1, 4), GPU_API(1, 3)) == GPU_API(1, 2)); /* cap only lowers */
    CHECK(gpucaps_instance_api(0, GPU_API(1, 4), 0) == GPU_API(1, 0)); /* never below 1.0 */
    /* Device: min(device, instance). */
    CHECK(gpucaps_device_api(GPU_API(1, 4) | 363u, GPU_API(1, 2)) == GPU_API(1, 2));
    CHECK(gpucaps_device_api(GPU_API(1, 2) | 255u, GPU_API(1, 4)) == GPU_API(1, 2)); /* Raspberry Pi 4 */
    CHECK(GPU_API_MAJOR(GPU_API(1, 3) | 275u) == 1 && GPU_API_MINOR(GPU_API(1, 3) | 275u) == 3);
}

/* The renderer's sequence: instance from the loader, the headers and
 * MC_VK_API, then an offer holding only the two versions (no optional
 * feature is implemented yet). The renderer used to request 1.0 and show
 * "Vulkan 1.0" whatever the system offered. */
static uint32_t renderer_api(uint32_t loader, uint32_t headers, const char *env, uint32_t device, char *s, size_t n)
{
    uint32_t cap = 0;
    (void)gpucaps_parse_api(env, &cap);
    gpu_offer o;
    memset(&o, 0, sizeof o);
    o.instance_api = gpucaps_instance_api(loader, headers, cap);
    o.device_api = device;
    gpu_choice c;
    gpucaps_choose(&o, 0, &c);
    CHECK(c.enabled == 0 && c.instance_api == o.instance_api);
    gpucaps_summary(&c, s, n);
    return c.device_api;
}

static void test_renderer_versions(void)
{
    char s[64];
    uint32_t v14 = GPU_API(1, 4) | 357u, v13 = GPU_API(1, 3) | 275u, pi = GPU_API(1, 3) | 239u;
    /* Loader and headers 1.4.357, Mesa 26 at 1.4.363. */
    CHECK(renderer_api(v14, v14, NULL, GPU_API(1, 4) | 363u, s, sizeof s) == GPU_API(1, 4));
    CHECK(strcmp(s, "Vulkan 1.4") == 0);
    CHECK(renderer_api(v13, v13, NULL, GPU_API(1, 4) | 363u, s, sizeof s) == GPU_API(1, 3)); /* Ubuntu 24.04 */
    CHECK(renderer_api(pi, pi, NULL, GPU_API(1, 2) | 255u, s, sizeof s) == GPU_API(1, 2)); /* Pi 4, bookworm */
    CHECK(strcmp(s, "Vulkan 1.2") == 0);
    /* A 1.0 loader (no vkEnumerateInstanceVersion) and MC_VK_API=1.0. */
    CHECK(renderer_api(0, v14, NULL, GPU_API(1, 4), s, sizeof s) == GPU_API(1, 0));
    CHECK(renderer_api(v14, v14, "1.0", GPU_API(1, 4), s, sizeof s) == GPU_API(1, 0));
    CHECK(strcmp(s, "Vulkan 1.0") == 0);
    /* A malformed MC_VK_API caps nothing. */
    CHECK(renderer_api(v14, v14, "1.5", GPU_API(1, 4), s, sizeof s) == GPU_API(1, 4));
    CHECK(renderer_api(v14, v14, "", GPU_API(1, 4), s, sizeof s) == GPU_API(1, 4));
}

static void test_parse_api(void)
{
    uint32_t v = 7;
    CHECK(gpucaps_parse_api("1.0", &v) && v == GPU_API(1, 0));
    CHECK(gpucaps_parse_api(" 1.3 ", &v) && v == GPU_API(1, 3));
    CHECK(gpucaps_parse_api("1.4", &v) && v == GPU_API(1, 4));
    v = 7;
    CHECK(!gpucaps_parse_api("1.5", &v) && v == 7);
    CHECK(!gpucaps_parse_api("2.0", &v));
    CHECK(!gpucaps_parse_api("1.", &v));
    CHECK(!gpucaps_parse_api("1.2x", &v));
    CHECK(!gpucaps_parse_api("", &v));
    CHECK(!gpucaps_parse_api(NULL, &v) && v == 7);
}

static void test_parse_disable(void)
{
    char unk[64];
    CHECK(gpucaps_parse_disable(NULL, unk, sizeof unk) == 0 && unk[0] == '\0');
    CHECK(gpucaps_parse_disable("", unk, sizeof unk) == 0);
    CHECK(gpucaps_parse_disable("mdi", unk, sizeof unk) == GPUCAP_MDI);
    CHECK(gpucaps_parse_disable(" MDI , DynRend,aniso ", unk, sizeof unk) ==
          (GPUCAP_MDI | GPUCAP_DYNREND | GPUCAP_ANISO));
    CHECK(unk[0] == '\0');
    CHECK(gpucaps_parse_disable("sync2,timeline,hostcopy,budget,timestamps,pcache", unk, sizeof unk) ==
          (GPUCAP_SYNC2 | GPUCAP_TIMELINE | GPUCAP_HOSTCOPY | GPUCAP_BUDGET | GPUCAP_TIMESTAMPS | GPUCAP_PCACHE));
    CHECK(gpucaps_parse_disable("all", unk, sizeof unk) == GPUCAP_ALL);
    CHECK(gpucaps_parse_disable("mdi,bogus,,warp", unk, sizeof unk) == GPUCAP_MDI);
    CHECK(strcmp(unk, "bogus,warp") == 0);
    CHECK(gpucaps_parse_disable("bogus", NULL, 0) == 0); /* no buffer is fine */
    /* Every name round-trips. */
    int names = 0;
    for (int i = 0; i < GPUCAP_COUNT; i++) {
        const char *n = gpucaps_name(1u << i);
        names += n && gpucaps_parse_disable(n, NULL, 0) == (1u << i);
    }
    CHECK(names == GPUCAP_COUNT);
    CHECK(gpucaps_name(1u << GPUCAP_COUNT) == NULL);
}

static void test_choose_full(void)
{
    gpu_offer o = full_offer();
    gpu_choice c;
    gpucaps_choose(&o, 0, &c);
    CHECK(c.device_api == GPU_API(1, 4));
    CHECK(c.enabled == GPUCAP_ALL);
    CHECK(c.disabled == 0);
    CHECK(!c.dynrend_khr && !c.sync2_khr); /* core on 1.3+ */
    CHECK(c.anisotropy == 8.0f);           /* 16x offered, 8x used */
    CHECK(c.max_draw_indirect_count == UINT32_MAX);
    CHECK(c.timestamp_mask == (1ull << 36) - 1u);
    CHECK(c.timestamp_ms_per_tick > 5.2e-5 && c.timestamp_ms_per_tick < 5.21e-5);

    o.timestamp_valid_bits = 64;
    gpucaps_choose(&o, 0, &c);
    CHECK(c.timestamp_mask == UINT64_MAX);
    o.max_sampler_anisotropy = 4.0f;
    gpucaps_choose(&o, 0, &c);
    CHECK(c.anisotropy == 4.0f);
}

/* Each version step turns on exactly the features that are core there. */
static void test_choose_versions(void)
{
    gpu_offer o = full_offer();
    gpu_choice c;
    uint32_t always = GPUCAP_MDI | GPUCAP_ANISO | GPUCAP_TIMESTAMPS | GPUCAP_PCACHE | GPUCAP_RELAXED;

    o.instance_api = GPU_API(1, 0); /* MC_VK_API=1.0 on a 1.4 device */
    gpucaps_choose(&o, 0, &c);
    CHECK(c.device_api == GPU_API(1, 0));
    CHECK(c.enabled == always);

    o.instance_api = GPU_API(1, 1);
    gpucaps_choose(&o, 0, &c);
    CHECK(c.enabled == (always | GPUCAP_BUDGET));

    /* 1.2: timeline and host reset core; sync2 and dynamic rendering
     * through the KHR extensions. */
    o.instance_api = GPU_API(1, 2);
    gpucaps_choose(&o, 0, &c);
    CHECK(c.enabled == (always | GPUCAP_BUDGET | GPUCAP_TIMELINE | GPUCAP_HOSTRESET | GPUCAP_SYNC2 | GPUCAP_DYNREND));
    CHECK(c.dynrend_khr && c.sync2_khr);
    o.khr_dynamic_rendering = o.khr_synchronization2 = 0;
    gpucaps_choose(&o, 0, &c);
    CHECK(!(c.enabled & (GPUCAP_SYNC2 | GPUCAP_DYNREND)) && !c.dynrend_khr && !c.sync2_khr);
    o.khr_dynamic_rendering = o.khr_synchronization2 = 1;

    o.instance_api = GPU_API(1, 3);
    gpucaps_choose(&o, 0, &c);
    CHECK(c.enabled == (GPUCAP_ALL & ~GPUCAP_HOSTCOPY));
    CHECK(!c.dynrend_khr && !c.sync2_khr);

    o.instance_api = GPU_API(1, 4);
    gpucaps_choose(&o, 0, &c);
    CHECK(c.enabled == GPUCAP_ALL);
}

/* A Vulkan 1.0 device with no optional features gets the plain path. */
static void test_choose_minimal(void)
{
    gpu_offer o;
    memset(&o, 0, sizeof o);
    o.instance_api = GPU_API(1, 4);
    o.device_api = GPU_API(1, 0);
    o.timestamp_valid_bits = 64; /* but timestampComputeAndGraphics is false */
    o.timestamp_period = 1.0f;
    o.host_image_copy = o.timeline_semaphore = o.dynamic_rendering = o.synchronization2 = 1; /* not at 1.0 */
    o.ext_memory_budget = 1;
    gpu_choice c;
    gpucaps_choose(&o, 0, &c);
    CHECK(c.enabled == 0);
    CHECK(c.anisotropy == 1.0f);
    CHECK(c.max_draw_indirect_count == 1);
    CHECK(c.timestamp_mask == 0);
    char s[160];
    gpucaps_summary(&c, s, sizeof s);
    CHECK(strcmp(s, "Vulkan 1.0") == 0);
}

static void test_dependencies(void)
{
    gpu_offer o = full_offer();
    gpu_choice c;
    /* The section origin rides on firstInstance: MDI without it is useless. */
    o.draw_indirect_first_instance = 0;
    gpucaps_choose(&o, 0, &c);
    CHECK(!(c.enabled & GPUCAP_MDI) && c.max_draw_indirect_count == 1);
    o = full_offer();
    o.multi_draw_indirect = 0;
    gpucaps_choose(&o, 0, &c);
    CHECK(!(c.enabled & GPUCAP_MDI));
    o = full_offer();
    o.max_draw_indirect_count = 1; /* the 1.0 minimum without the feature */
    gpucaps_choose(&o, 0, &c);
    CHECK(!(c.enabled & GPUCAP_MDI));

    /* Dynamic rendering transitions images with barrier2. */
    o = full_offer();
    gpucaps_choose(&o, GPUCAP_SYNC2, &c);
    CHECK(!(c.enabled & (GPUCAP_SYNC2 | GPUCAP_DYNREND)));
    CHECK(c.disabled == GPUCAP_SYNC2);
    o.synchronization2 = 0;
    gpucaps_choose(&o, 0, &c);
    CHECK(!(c.enabled & GPUCAP_DYNREND));
    /* ...but synchronization2 alone is still used. */
    o = full_offer();
    gpucaps_choose(&o, GPUCAP_DYNREND, &c);
    CHECK((c.enabled & GPUCAP_SYNC2) && !(c.enabled & GPUCAP_DYNREND));

    /* Host query reset only matters with timestamps. */
    o = full_offer();
    o.timestamp_compute_and_graphics = 0;
    gpucaps_choose(&o, 0, &c);
    CHECK(!(c.enabled & (GPUCAP_TIMESTAMPS | GPUCAP_HOSTRESET)));
    o = full_offer();
    o.timestamp_valid_bits = 0; /* the queue cannot time */
    gpucaps_choose(&o, 0, &c);
    CHECK(!(c.enabled & GPUCAP_TIMESTAMPS));
    o = full_offer();
    gpucaps_choose(&o, GPUCAP_TIMESTAMPS, &c);
    CHECK(!(c.enabled & GPUCAP_HOSTRESET));

    /* Anisotropy of 1 is no anisotropy. */
    o = full_offer();
    o.max_sampler_anisotropy = 1.0f;
    gpucaps_choose(&o, 0, &c);
    CHECK(!(c.enabled & GPUCAP_ANISO) && c.anisotropy == 1.0f);

    /* No pipeline cache without a path; no budget without the extension. */
    o = full_offer();
    o.pipeline_cache = 0;
    o.ext_memory_budget = 0;
    gpucaps_choose(&o, 0, &c);
    CHECK(!(c.enabled & (GPUCAP_PCACHE | GPUCAP_BUDGET)));
}

static void test_overrides(void)
{
    gpu_offer o = full_offer();
    gpu_choice c;
    uint32_t off = gpucaps_parse_disable("mdi,dynrend,sync2,timeline,aniso,hostcopy,budget,timestamps,pcache", NULL, 0);
    gpucaps_choose(&o, off, &c);
    CHECK(c.enabled == GPUCAP_RELAXED); /* hostreset went with timestamps */
    CHECK(c.disabled == off);
    gpucaps_choose(&o, GPUCAP_ALL, &c);
    CHECK(c.enabled == 0);
    /* Disabling something the device lacks is not reported as disabled. */
    o.host_image_copy = 0;
    gpucaps_choose(&o, GPUCAP_HOSTCOPY, &c);
    CHECK(!(c.disabled & GPUCAP_HOSTCOPY));
    /* MC_VK_API=1.1 as the renderer applies it. */
    uint32_t cap = 0;
    CHECK(gpucaps_parse_api("1.1", &cap));
    o = full_offer();
    o.instance_api = gpucaps_instance_api(GPU_API(1, 4), GPU_API(1, 4), cap);
    gpucaps_choose(&o, 0, &c);
    CHECK(c.device_api == GPU_API(1, 1) && !(c.enabled & (GPUCAP_TIMELINE | GPUCAP_DYNREND | GPUCAP_HOSTCOPY)));
}

static void test_summary(void)
{
    gpu_offer o = full_offer();
    o.present_relaxed = 0;
    gpu_choice c;
    char s[256];
    gpucaps_choose(&o, 0, &c);
    gpucaps_summary(&c, s, sizeof s);
    CHECK(strcmp(s, "Vulkan 1.4: multi-draw indirect, dynamic rendering, synchronization2, timeline semaphores, "
                    "pipeline cache, 8x anisotropic, host image copy, memory budget, GPU timestamps") == 0);
    o.instance_api = GPU_API(1, 2);
    gpucaps_choose(&o, GPUCAP_MDI | GPUCAP_PCACHE, &c);
    gpucaps_summary(&c, s, sizeof s);
    CHECK(strcmp(s, "Vulkan 1.2: dynamic rendering (KHR), synchronization2 (KHR), timeline semaphores, "
                    "8x anisotropic, memory budget, GPU timestamps") == 0);
    /* Truncation keeps a terminated prefix. */
    char small[12];
    memset(small, 'x', sizeof small);
    gpucaps_summary(&c, small, sizeof small);
    CHECK(small[sizeof small - 1] == '\0' && strncmp(small, "Vulkan 1.2:", 11) == 0);
}

static void put_u32(uint8_t *p, uint32_t v) { memcpy(p, &v, sizeof v); }

static void test_pipeline_cache_header(void)
{
    uint8_t uuid[16], other[16];
    for (int i = 0; i < 16; i++) {
        uuid[i] = (uint8_t)(i * 7 + 1);
        other[i] = uuid[i];
    }
    other[15] ^= 1u;
    uint8_t blob[64];
    memset(blob, 0xAB, sizeof blob);
    put_u32(blob, 32);
    put_u32(blob + 4, 1);
    put_u32(blob + 8, 0x8086);
    put_u32(blob + 12, 0x46d0);
    memcpy(blob + 16, uuid, 16);
    CHECK(gpucaps_pipeline_cache_ok(blob, sizeof blob, 1u << 20, 0x8086, 0x46d0, uuid));
    CHECK(gpucaps_pipeline_cache_ok(blob, 32, 1u << 20, 0x8086, 0x46d0, uuid)); /* header only */
    CHECK(!gpucaps_pipeline_cache_ok(blob, sizeof blob, 32, 0x8086, 0x46d0, uuid)); /* over the cap */
    CHECK(!gpucaps_pipeline_cache_ok(blob, 31, 1u << 20, 0x8086, 0x46d0, uuid));    /* torn */
    CHECK(!gpucaps_pipeline_cache_ok(NULL, 64, 1u << 20, 0x8086, 0x46d0, uuid));
    CHECK(!gpucaps_pipeline_cache_ok(blob, sizeof blob, 1u << 20, 0x10005, 0x46d0, uuid)); /* other vendor */
    CHECK(!gpucaps_pipeline_cache_ok(blob, sizeof blob, 1u << 20, 0x8086, 0x46d1, uuid));  /* other device */
    CHECK(!gpucaps_pipeline_cache_ok(blob, sizeof blob, 1u << 20, 0x8086, 0x46d0, other)); /* other driver */
    put_u32(blob + 4, 2);
    CHECK(!gpucaps_pipeline_cache_ok(blob, sizeof blob, 1u << 20, 0x8086, 0x46d0, uuid)); /* unknown version */
    put_u32(blob + 4, 1);
    put_u32(blob, 16);
    CHECK(!gpucaps_pipeline_cache_ok(blob, sizeof blob, 1u << 20, 0x8086, 0x46d0, uuid)); /* bad header size */
    put_u32(blob, 65);
    CHECK(!gpucaps_pipeline_cache_ok(blob, sizeof blob, 1u << 20, 0x8086, 0x46d0, uuid)); /* header past the end */
}

void test_gpucaps_all(void)
{
    test_versions();
    test_renderer_versions();
    test_parse_api();
    test_parse_disable();
    test_choose_full();
    test_choose_versions();
    test_choose_minimal();
    test_dependencies();
    test_overrides();
    test_summary();
    test_pipeline_cache_header();
}
