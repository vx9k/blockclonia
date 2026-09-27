#include "gpucaps.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* Largest anisotropy the filtered sampler asks for. 8x removes the smear on
 * ground seen at grazing angles to the render distance; 16x costs extra
 * texture fetches on weak GPUs for a difference hard to see on 16 px
 * textures. */
#define ANISO_MAX 8.0f

static const struct {
    uint32_t bit;
    const char *name;
} NAMES[GPUCAP_COUNT] = {
    {GPUCAP_MDI, "mdi"},         {GPUCAP_DYNREND, "dynrend"},  {GPUCAP_SYNC2, "sync2"},
    {GPUCAP_TIMELINE, "timeline"}, {GPUCAP_ANISO, "aniso"},      {GPUCAP_HOSTCOPY, "hostcopy"},
    {GPUCAP_BUDGET, "budget"},   {GPUCAP_TIMESTAMPS, "timestamps"}, {GPUCAP_PCACHE, "pcache"},
    {GPUCAP_HOSTRESET, "hostreset"}, {GPUCAP_RELAXED, "relaxed"},
};

static uint32_t strip_patch(uint32_t v) { return GPU_API(GPU_API_MAJOR(v), GPU_API_MINOR(v)); }

static uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }

uint32_t gpucaps_instance_api(uint32_t loader, uint32_t headers, uint32_t cap)
{
    uint32_t v = min_u32(strip_patch(loader), strip_patch(headers));
    v = min_u32(v, GPU_API_MAX);
    if (cap) v = min_u32(v, strip_patch(cap));
    return v < GPU_API(1, 0) ? GPU_API(1, 0) : v;
}

uint32_t gpucaps_device_api(uint32_t device, uint32_t instance)
{
    uint32_t v = min_u32(strip_patch(device), strip_patch(instance));
    return v < GPU_API(1, 0) ? GPU_API(1, 0) : v;
}

int gpucaps_parse_api(const char *s, uint32_t *out)
{
    if (!s) return 0;
    while (isspace((unsigned char)*s)) s++;
    if (s[0] != '1' || s[1] != '.' || s[2] < '0' || s[2] > '4') return 0;
    const char *e = s + 3;
    while (isspace((unsigned char)*e)) e++;
    if (*e) return 0;
    *out = GPU_API(1, s[2] - '0');
    return 1;
}

const char *gpucaps_name(uint32_t bit)
{
    for (int i = 0; i < GPUCAP_COUNT; i++)
        if (NAMES[i].bit == bit) return NAMES[i].name;
    return NULL;
}

/* Matches one token (already lower-cased, no blanks) to a bit; 0 if unknown. */
static uint32_t token_bit(const char *tok)
{
    if (strcmp(tok, "all") == 0) return GPUCAP_ALL;
    for (int i = 0; i < GPUCAP_COUNT; i++)
        if (strcmp(tok, NAMES[i].name) == 0) return NAMES[i].bit;
    return 0;
}

static void append_unknown(char *buf, size_t n, const char *tok)
{
    if (!buf || !n) return;
    size_t used = strlen(buf);
    if (used + 1 >= n) return;
    (void)snprintf(buf + used, n - used, "%s%s", used ? "," : "", tok);
}

uint32_t gpucaps_parse_disable(const char *s, char *unknown, size_t unknown_len)
{
    if (unknown && unknown_len) unknown[0] = '\0';
    if (!s) return 0;
    uint32_t mask = 0;
    char tok[32];
    size_t len = 0;
    for (const char *p = s;; p++) {
        if (*p == ',' || *p == '\0') {
            tok[len] = '\0';
            if (len) {
                uint32_t bit = token_bit(tok);
                if (bit) mask |= bit;
                else append_unknown(unknown, unknown_len, tok);
            }
            len = 0;
            if (*p == '\0') break;
        } else if (!isspace((unsigned char)*p) && len + 1 < sizeof tok) {
            tok[len++] = (char)tolower((unsigned char)*p);
        }
    }
    return mask;
}

/* What the device offers at the negotiated version, before overrides. */
static uint32_t offered(const gpu_offer *o, uint32_t api, int *dynrend_khr, int *sync2_khr)
{
    uint32_t m = 0;
    *dynrend_khr = *sync2_khr = 0;
    if (o->multi_draw_indirect && o->draw_indirect_first_instance && o->max_draw_indirect_count > 1) m |= GPUCAP_MDI;
    /* synchronization2 and dynamic rendering: core in 1.3; on 1.2 the KHR
     * extensions (their dependencies, create_renderpass2 and
     * depth_stencil_resolve, are core there). The feature flags come from
     * the same structures either way. */
    if (o->synchronization2 && api >= GPU_API(1, 3)) m |= GPUCAP_SYNC2;
    else if (o->synchronization2 && api >= GPU_API(1, 2) && o->khr_synchronization2) {
        m |= GPUCAP_SYNC2;
        *sync2_khr = 1;
    }
    if (o->dynamic_rendering && api >= GPU_API(1, 3)) m |= GPUCAP_DYNREND;
    else if (o->dynamic_rendering && api >= GPU_API(1, 2) && o->khr_dynamic_rendering) {
        m |= GPUCAP_DYNREND;
        *dynrend_khr = 1;
    }
    if (o->timeline_semaphore && api >= GPU_API(1, 2)) m |= GPUCAP_TIMELINE;
    if (o->sampler_anisotropy && o->max_sampler_anisotropy >= 2.0f) m |= GPUCAP_ANISO;
    if (o->host_image_copy && api >= GPU_API(1, 4)) m |= GPUCAP_HOSTCOPY;
    /* vkGetPhysicalDeviceMemoryProperties2 is 1.1 core. */
    if (o->ext_memory_budget && api >= GPU_API(1, 1)) m |= GPUCAP_BUDGET;
    if (o->timestamp_compute_and_graphics && o->timestamp_valid_bits && o->timestamp_period > 0.0f)
        m |= GPUCAP_TIMESTAMPS;
    if (o->host_query_reset && api >= GPU_API(1, 2)) m |= GPUCAP_HOSTRESET;
    if (o->pipeline_cache) m |= GPUCAP_PCACHE;
    if (o->present_relaxed) m |= GPUCAP_RELAXED;
    return m;
}

void gpucaps_choose(const gpu_offer *o, uint32_t disable, gpu_choice *c)
{
    memset(c, 0, sizeof *c);
    c->instance_api = strip_patch(o->instance_api);
    c->device_api = gpucaps_device_api(o->device_api, o->instance_api);
    int dynrend_khr, sync2_khr;
    uint32_t avail = offered(o, c->device_api, &dynrend_khr, &sync2_khr);
    uint32_t m = avail & ~disable;
    /* Dependencies, in an order where each rule sees the result of the ones
     * it depends on. */
    if (!(m & GPUCAP_SYNC2)) m &= ~GPUCAP_DYNREND;
    if (!(m & GPUCAP_TIMESTAMPS)) m &= ~GPUCAP_HOSTRESET;
    c->enabled = m;
    c->disabled = avail & disable;
    c->dynrend_khr = (m & GPUCAP_DYNREND) ? dynrend_khr : 0;
    c->sync2_khr = (m & GPUCAP_SYNC2) ? sync2_khr : 0;
    c->anisotropy = 1.0f;
    if (m & GPUCAP_ANISO) c->anisotropy = o->max_sampler_anisotropy < ANISO_MAX ? o->max_sampler_anisotropy : ANISO_MAX;
    c->max_draw_indirect_count = (m & GPUCAP_MDI) ? o->max_draw_indirect_count : 1;
    if (m & GPUCAP_TIMESTAMPS) {
        c->timestamp_ms_per_tick = (double)o->timestamp_period * 1e-6;
        c->timestamp_mask = o->timestamp_valid_bits >= 64 ? UINT64_MAX : (1ull << o->timestamp_valid_bits) - 1u;
    }
}

void gpucaps_summary(const gpu_choice *c, char *buf, size_t n)
{
    if (!buf || !n) return;
    uint32_t m = c->enabled;
    char aniso[24];
    (void)snprintf(aniso, sizeof aniso, "%dx anisotropic", (int)c->anisotropy);
    /* Order of the README: what saves the most first. */
    const struct {
        uint32_t bit;
        const char *text;
    } parts[] = {
        {GPUCAP_MDI, "multi-draw indirect"},
        {GPUCAP_DYNREND, c->dynrend_khr ? "dynamic rendering (KHR)" : "dynamic rendering"},
        {GPUCAP_SYNC2, c->sync2_khr ? "synchronization2 (KHR)" : "synchronization2"},
        {GPUCAP_TIMELINE, "timeline semaphores"},
        {GPUCAP_PCACHE, "pipeline cache"},
        {GPUCAP_ANISO, aniso},
        {GPUCAP_HOSTCOPY, "host image copy"},
        {GPUCAP_BUDGET, "memory budget"},
        {GPUCAP_TIMESTAMPS, "GPU timestamps"},
        {GPUCAP_RELAXED, "relaxed vsync"},
    };
    int len = snprintf(buf, n, "Vulkan %u.%u", GPU_API_MAJOR(c->device_api), GPU_API_MINOR(c->device_api));
    int first = 1;
    for (size_t i = 0; i < sizeof parts / sizeof parts[0]; i++) {
        if (!(m & parts[i].bit) || len < 0 || (size_t)len >= n) continue;
        len += snprintf(buf + len, n - (size_t)len, "%s%s", first ? ": " : ", ", parts[i].text);
        first = 0;
    }
}

static uint32_t read_u32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof v); /* written by the driver in host byte order */
    return v;
}

int gpucaps_pipeline_cache_ok(const void *data, size_t size, size_t max_size, uint32_t vendor, uint32_t device,
                              const uint8_t uuid[16])
{
    if (!data || size < GPUCACHE_HEADER_SIZE || size > max_size) return 0;
    const uint8_t *p = data;
    uint32_t header_size = read_u32(p);
    /* VK_PIPELINE_CACHE_HEADER_VERSION_ONE */
    if (header_size < GPUCACHE_HEADER_SIZE || header_size > size || read_u32(p + 4) != 1u) return 0;
    if (read_u32(p + 8) != vendor || read_u32(p + 12) != device) return 0;
    return memcmp(p + 16, uuid, 16) == 0;
}
