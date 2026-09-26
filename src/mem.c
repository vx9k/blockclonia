#include "mem.h"
#include "log.h"

#include <stdlib.h>

#ifdef MC_MEM_LIBC
/* Sanitizer builds route everything through libc so ASan and valgrind see
 * every game allocation (they cannot track mi_* calls). */
static void *raw_malloc(size_t size) { return malloc(size); }
static void *raw_calloc(size_t count, size_t size) { return calloc(count, size); }
static void *raw_realloc(void *ptr, size_t size) { return realloc(ptr, size); }
static void *raw_malloc_aligned(size_t size, size_t align)
{
    void *p = NULL;
    if (align < sizeof(void *)) align = sizeof(void *);
    return posix_memalign(&p, align, size) == 0 ? p : NULL;
}
#else
static void *raw_malloc(size_t size) { return mi_malloc(size); }
static void *raw_calloc(size_t count, size_t size) { return mi_calloc(count, size); }
static void *raw_realloc(void *ptr, size_t size) { return mi_realloc(ptr, size); }
static void *raw_malloc_aligned(size_t size, size_t align) { return mi_malloc_aligned(size, align); }
#endif

static void oom(size_t size)
{
    log_fatal("out of memory allocating %zu bytes", size);
}

void *mem_alloc(size_t size)
{
    void *p = raw_malloc(size);
    if (!p && size) oom(size);
    return p;
}

void *mem_calloc(size_t count, size_t size)
{
    void *p = raw_calloc(count, size);
    if (!p && count && size) oom(mem_array_size(count, size));
    return p;
}

void *mem_realloc(void *ptr, size_t size)
{
    void *p = raw_realloc(ptr, size);
    if (!p && size) oom(size);
    return p;
}

void *mem_alloc_aligned(size_t size, size_t align)
{
    void *p = raw_malloc_aligned(size, align);
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
#ifdef MC_MEM_LIBC
    log_info("allocator: libc (sanitizer build)");
#else
    /* Sized for 1-2 GB devices. mimalloc reserves address space in 1 GiB
     * arenas and, where the kernel overcommits, commits them eagerly: that
     * shows up as a gigabyte "committed" on a game that uses ~100 MB.
     * Reserve in 64 MiB steps and commit on demand instead. The default
     * purge delay (10 ms) already returns freed pages quickly. */
    mi_option_set(mi_option_arena_reserve, 64 * 1024); /* KiB */
    mi_option_set(mi_option_arena_eager_commit, 0);
#endif
}

void mem_print_stats(void)
{
#ifndef MC_MEM_LIBC
    mi_stats_print_out(NULL, NULL);
#endif
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

/* Opt-in (MC_VK_ALLOC=1). Some drivers mishandle custom allocators: Mesa's
 * lavapipe frees a stack address through pfnFree while recording a
 * pipeline barrier. Where mimalloc is linked as the shared, overriding
 * build (the Linux default), the driver's malloc already is mimalloc, so
 * the callbacks add nothing there. */
const void *mem_vk_callbacks(void)
{
    const char *e = getenv("MC_VK_ALLOC");
    return (e && e[0] == '1') ? &g_vk_callbacks : NULL;
}
#else
const void *mem_vk_callbacks(void) { return NULL; }
#endif
