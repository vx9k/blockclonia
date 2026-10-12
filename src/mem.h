/* Memory: every heap allocation in the game goes through mimalloc.
 *
 * Game code uses these wrappers (mi_* underneath) so allocation policy
 * lives in one place. Third-party code (GLFW, the Vulkan loader and
 * driver) gets mimalloc through the shared library's malloc override,
 * which is why mimalloc is linked first. The renderer can also hand these
 * to the driver explicitly as VkAllocationCallbacks (MC_VK_ALLOC=1), for
 * platforms without the override; this header stays free of Vulkan.
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
#ifdef MC_MEM_LIBC
/* Sanitizer builds: libc malloc, which ASan and valgrind can track. */
static inline void mem_free(void *ptr) { free(ptr); }
#else
static inline void mem_free(void *ptr) { mi_free(ptr); }
#endif

/* Overflow-checked count * size for array allocations. Aborts on overflow. */
size_t mem_array_size(size_t count, size_t size);

/* Tunes mimalloc for small-memory devices (reserve in 64 MiB steps,
 * commit on demand). Call first thing in main. */
void mem_init(void);
/* mimalloc's statistics to stderr (--mem-stats). */
void mem_print_stats(void);
/* Resident and committed bytes of the process, from mimalloc; 0 when
 * unknown (sanitizer builds). */
void mem_process_info(size_t *rss, size_t *commit);
/* "mimalloc 3.5.3" (the linked library's version), or "libc". */
void mem_version_string(char *buf, size_t n);

#ifndef MC_MEM_LIBC
/* Aligned allocations (align a power of two) for the renderer's
 * VkAllocationCallbacks; release them with mem_free. Not in sanitizer
 * builds: libc has no aligned realloc. */
void *mem_alloc_aligned(size_t size, size_t align);
void *mem_realloc_aligned(void *ptr, size_t size, size_t align);
#endif

#endif
