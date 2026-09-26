#include "mem.h"
#include "log.h"

#include <stdlib.h>

static void oom(size_t size)
{
    log_fatal("out of memory allocating %zu bytes", size);
}

void *mem_alloc(size_t size)
{
    void *p = mi_malloc(size);
    if (!p && size) oom(size);
    return p;
}

void *mem_calloc(size_t count, size_t size)
{
    void *p = mi_calloc(count, size);
    if (!p && count && size) oom(mem_array_size(count, size));
    return p;
}

void *mem_realloc(void *ptr, size_t size)
{
    void *p = mi_realloc(ptr, size);
    if (!p && size) oom(size);
    return p;
}

void *mem_alloc_aligned(size_t size, size_t align)
{
    void *p = mi_malloc_aligned(size, align);
    if (!p && size) oom(size);
    return p;
}

size_t mem_array_size(size_t count, size_t size)
{
    size_t total;
    if (__builtin_mul_overflow(count, size, &total))
        log_fatal("allocation size overflow (%zu * %zu)", count, size);
    return total;
}

void mem_init(void)
{
    /* Low-end devices are usually memory constrained: return freed pages to
     * the OS reasonably quickly instead of holding on to them. */
    mi_option_set(mi_option_purge_delay, 100);
}

void mem_print_stats(void)
{
    mi_stats_print_out(NULL, NULL);
}

#ifdef MC_VK_MIMALLOC
#include <vulkan/vulkan.h>

static VKAPI_ATTR void *VKAPI_CALL vk_alloc(void *user, size_t size, size_t align,
                                            VkSystemAllocationScope scope)
{
    (void)user; (void)scope;
    if (size == 0) return NULL;
    return mi_malloc_aligned(size, align);
}

static VKAPI_ATTR void *VKAPI_CALL vk_realloc(void *user, void *orig, size_t size,
                                              size_t align, VkSystemAllocationScope scope)
{
    (void)user; (void)scope;
    if (size == 0) {
        mi_free(orig);
        return NULL;
    }
    return mi_realloc_aligned(orig, size, align);
}

static VKAPI_ATTR void VKAPI_CALL vk_free(void *user, void *mem)
{
    (void)user;
    mi_free(mem);
}

static const VkAllocationCallbacks g_vk_callbacks = {
    .pUserData = NULL,
    .pfnAllocation = vk_alloc,
    .pfnReallocation = vk_realloc,
    .pfnFree = vk_free,
};

const void *mem_vk_callbacks(void) { return &g_vk_callbacks; }
#else
const void *mem_vk_callbacks(void) { return NULL; }
#endif
