/* First-fit range allocator for the single big vertex buffer that holds all
 * chunk meshes. One VkBuffer for every section means one bind per frame and
 * no risk of hitting maxMemoryAllocationCount (4096 on many drivers). */
#ifndef MC_GPUPOOL_H
#define MC_GPUPOOL_H

#include <stdint.h>

#define GPUPOOL_FAIL UINT32_MAX

typedef struct { uint32_t start, len; } gp_range;

typedef struct {
    gp_range *free;   /* sorted by start, never adjacent (always coalesced) */
    int count, cap;
    uint32_t total, granule;
    uint32_t used;
} gpupool;

/* A pool of `total` units, allocated in multiples of `granule`. */
void gpupool_init(gpupool *p, uint32_t total, uint32_t granule);
void gpupool_destroy(gpupool *p);
/* Returns the start (in units) or GPUPOOL_FAIL. `*got` receives the rounded
 * length actually reserved. */
uint32_t gpupool_alloc(gpupool *p, uint32_t len, uint32_t *got);
void gpupool_free(gpupool *p, uint32_t start, uint32_t len);

#endif
