#include "gpupool.h"
#include "mem.h"
#include "log.h"

#include <string.h>

void gpupool_init(gpupool *p, uint32_t total, uint32_t granule)
{
    memset(p, 0, sizeof *p);
    p->granule = granule ? granule : 1;
    p->total = total / p->granule * p->granule;
    p->cap = 64;
    p->free = mem_alloc(sizeof(gp_range) * (size_t)p->cap);
    p->free[0] = (gp_range){0, p->total};
    p->count = p->total ? 1 : 0;
}

void gpupool_destroy(gpupool *p)
{
    mem_free(p->free);
    memset(p, 0, sizeof *p);
}

uint32_t gpupool_alloc(gpupool *p, uint32_t len, uint32_t *got)
{
    if (len == 0 || len > p->total) return GPUPOOL_FAIL;
    uint32_t need = (len + p->granule - 1) / p->granule * p->granule;
    for (int i = 0; i < p->count; i++) {
        gp_range *r = &p->free[i];
        if (r->len < need) continue;
        uint32_t start = r->start;
        r->start += need;
        r->len -= need;
        if (r->len == 0) {
            memmove(r, r + 1, sizeof(gp_range) * (size_t)(p->count - i - 1));
            p->count--;
        }
        p->used += need;
        if (got) *got = need;
        return start;
    }
    return GPUPOOL_FAIL;
}

void gpupool_free(gpupool *p, uint32_t start, uint32_t len)
{
    if (len == 0) return;
    if (start > p->total || len > p->total - start) log_fatal("gpupool: bad free %u+%u", start, len);
    /* Find insertion point (first range starting after `start`). */
    int i = 0;
    while (i < p->count && p->free[i].start < start) i++;
    if ((i > 0 && p->free[i - 1].start + p->free[i - 1].len > start) ||
        (i < p->count && start + len > p->free[i].start))
        log_fatal("gpupool: double free or overlap at %u+%u", start, len);

    int merge_prev = i > 0 && p->free[i - 1].start + p->free[i - 1].len == start;
    int merge_next = i < p->count && start + len == p->free[i].start;
    if (merge_prev && merge_next) {
        p->free[i - 1].len += len + p->free[i].len;
        memmove(&p->free[i], &p->free[i + 1], sizeof(gp_range) * (size_t)(p->count - i - 1));
        p->count--;
    } else if (merge_prev) {
        p->free[i - 1].len += len;
    } else if (merge_next) {
        p->free[i].start = start;
        p->free[i].len += len;
    } else {
        if (p->count == p->cap) {
            p->cap *= 2;
            p->free = mem_realloc(p->free, sizeof(gp_range) * (size_t)p->cap);
        }
        memmove(&p->free[i + 1], &p->free[i], sizeof(gp_range) * (size_t)(p->count - i));
        p->free[i] = (gp_range){start, len};
        p->count++;
    }
    p->used -= len;
}
