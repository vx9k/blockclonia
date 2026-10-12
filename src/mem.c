#include "mem.h"
#include "log.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef MC_MEM_LIBC
/* Sanitizer builds route everything through libc so ASan and valgrind see
 * every game allocation (they cannot track mi_* calls). */
static void *raw_malloc(size_t size) { return malloc(size); }
static void *raw_calloc(size_t count, size_t size) { return calloc(count, size); }
static void *raw_realloc(void *ptr, size_t size) { return realloc(ptr, size); }
#else
static void *raw_malloc(size_t size) { return mi_malloc(size); }
static void *raw_calloc(size_t count, size_t size) { return mi_calloc(count, size); }
static void *raw_realloc(void *ptr, size_t size) { return mi_realloc(ptr, size); }
#endif

_Noreturn static void oom(size_t size) { log_fatal("out of memory allocating %zu bytes", size); }

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

size_t mem_array_size(size_t count, size_t size)
{
#if defined(__GNUC__)
    size_t total;
    if (__builtin_mul_overflow(count, size, &total))
        log_fatal("allocation size overflow (%zu * %zu)", count, size);
    return total;
#else
    if (size && count > SIZE_MAX / size) log_fatal("allocation size overflow (%zu * %zu)", count, size);
    return count * size;
#endif
}

void mem_init(int arena_mb)
{
#ifdef MC_MEM_LIBC
    (void)arena_mb;
    log_info("allocator: libc (sanitizer build)");
#else
    /* mimalloc reserves address space in 1 GiB arenas and, where the
     * kernel overcommits, commits them eagerly: that shows up as a
     * gigabyte "committed" on a game that uses ~100 MB. Reserve in
     * arena_mb steps (sys_plan: 64 MB on small machines, more on big
     * ones) and commit on demand instead. The default purge delay
     * (10 ms) already returns freed pages quickly. */
    mi_option_set(mi_option_arena_reserve, (long)(arena_mb > 0 ? arena_mb : 64) * 1024); /* KiB */
    mi_option_set(mi_option_arena_eager_commit, 0);
#endif
}

void mem_print_stats(void)
{
#ifndef MC_MEM_LIBC
    mi_stats_print_out(NULL, NULL);
#endif
}

void mem_process_info(size_t *rss, size_t *commit)
{
    *rss = *commit = 0;
#ifndef MC_MEM_LIBC
    size_t elapsed, user, sys, peak_rss, peak_commit, faults;
    mi_process_info(&elapsed, &user, &sys, rss, &peak_rss, commit, &peak_commit, &faults);
#endif
}

void mem_version_string(char *buf, size_t n)
{
#ifdef MC_MEM_LIBC
    snprintf(buf, n, "libc");
#else
    /* 2.x reports major*100 + minor*10 + patch; 3.x major*10000 + minor*100 + patch. */
    int v = mi_version();
    if (v >= 10000) snprintf(buf, n, "mimalloc %d.%d.%d", v / 10000, v / 100 % 100, v % 100);
    else snprintf(buf, n, "mimalloc %d.%d.%d", v / 100, v / 10 % 10, v % 10);
#endif
}

#ifndef MC_MEM_LIBC
void *mem_alloc_aligned(size_t size, size_t align)
{
    void *p = mi_malloc_aligned(size, align);
    if (!p && size) oom(size);
    return p;
}

void *mem_realloc_aligned(void *ptr, size_t size, size_t align)
{
    void *p = mi_realloc_aligned(ptr, size, align);
    if (!p && size) oom(size);
    return p;
}
#endif
