/* Memory: every heap allocation in the game goes through mimalloc.
 *
 * Game code uses these wrappers (mi_* underneath) so allocation policy
 * lives in one place. Third-party code (GLFW, the Vulkan loader and
 * driver) gets mimalloc through the shared library's malloc override,
 * which is why mimalloc is linked first. mem_vk_callbacks() can also hand
 * mimalloc to the driver explicitly via VkAllocationCallbacks, for
 * platforms without the override.
 */
#ifndef MC_MEM_H
#define MC_MEM_H

#include <stddef.h>
#include <stdint.h>
#ifdef MC_MEM_LIBC
#include <stdlib.h>
#else
#include <mimalloc.h>
#endif

/* Aborts on out-of-memory: the game cannot recover from a failed
 * allocation mid-frame, and a clean abort beats a NULL dereference. */
void *mem_alloc(size_t size);
void *mem_calloc(size_t count, size_t size);
void *mem_realloc(void *ptr, size_t size);
void *mem_alloc_aligned(size_t size, size_t align);
#ifdef MC_MEM_LIBC
/* Sanitizer builds: libc malloc, which ASan and valgrind can track. */
static inline void mem_free(void *ptr) { free(ptr); }
#else
static inline void mem_free(void *ptr) { mi_free(ptr); }
#endif

/* Overflow-checked count * size for array allocations. Aborts on overflow. */
size_t mem_array_size(size_t count, size_t size);

void mem_init(void);
void mem_print_stats(void);

/* Returns static VkAllocationCallbacks backed by mimalloc when built with
 * MC_VK_MIMALLOC and run with MC_VK_ALLOC=1, else NULL (driver default).
 * Declared as void* so this header does not need vulkan.h. */
const void *mem_vk_callbacks(void);

#endif
