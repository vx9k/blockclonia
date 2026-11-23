/* The machine the game runs on: memory, CPU caches and cores, and the
 * budgets derived from them (worker threads, the default render distance,
 * the SQLite page cache, the allocator's reservation step), so a desktop
 * uses what it has instead of what the smallest supported device has.
 * Detection reads the OS (Linux: sysconf, /proc and /sys; macOS: sysctl)
 * without allocating; planning is pure and unit-tested. It does not size
 * GPU memory: the renderer asks the driver for that (gpucaps). */
#ifndef MC_SYSINFO_H
#define MC_SYSINFO_H

#include <stdint.h>

typedef struct {
    uint64_t ram;       /* bytes of physical memory; 0 unknown */
    uint64_t ram_avail; /* bytes the OS says a new program can use without swapping; 0 unknown */
    uint32_t l1d, l2, l3; /* data cache sizes in bytes, per core (L1, L2) or shared (L3); 0 unknown */
    int cores;          /* online cores, at least 1 */
} sys_info;

typedef struct {
    int workers;          /* job threads beside the main thread */
    int render_distance;  /* columns, for a first run without a settings file */
    int sqlite_cache_kb;  /* page cache per open world */
    int arena_mb;         /* mimalloc's reservation step */
} sys_budget;

/* Fills si from the running system; never fails (unknowns are 0). */
void sys_detect(sys_info *si);
/* The budgets for a machine. Pure: the same info gives the same plan. */
void sys_plan(const sys_info *si, sys_budget *b);
/* One log line describing si and b. */
void sys_log(const sys_info *si, const sys_budget *b);

#endif
