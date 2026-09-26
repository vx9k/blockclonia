/* Memory: every heap allocation in the game goes through mimalloc.
 *
 * Use these wrappers instead of malloc/free so allocation policy lives in
 * one place. The Vulkan driver's own host allocations are routed through
 * mimalloc too, via mem_vk_callbacks().
 */
#ifndef MC_MEM_H
#define MC_MEM_H

#include <stddef.h>
#include <stdint.h>
#include <mimalloc.h>

/* Aborts on out-of-memory: the game cannot recover from a failed
 * allocation mid-frame, and a clean abort beats a NULL dereference. */
void *mem_alloc(size_t size);
void *mem_calloc(size_t count, size_t size);
void *mem_realloc(void *ptr, size_t size);
void *mem_alloc_aligned(size_t size, size_t align);
static inline void mem_free(void *ptr) { mi_free(ptr); }

/* Overflow-checked count * size for array allocations. Aborts on overflow. */
size_t mem_array_size(size_t count, size_t size);

void mem_init(void);
void mem_print_stats(void);

/* Returns a pointer to static VkAllocationCallbacks backed by mimalloc, or
 * NULL when MC_VK_MIMALLOC is disabled at build time. Declared as void* so
 * this header does not need vulkan.h. */
const void *mem_vk_callbacks(void);

#endif
