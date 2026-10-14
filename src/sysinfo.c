#include "sysinfo.h"
#include "log.h"
#include "os.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __APPLE__
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

#define MIB (1024ull * 1024ull)
#define GIB (1024ull * MIB)

#ifdef __APPLE__
static uint64_t sysctl_u64(const char *name)
{
    uint64_t v = 0;
    size_t len = sizeof v;
    /* The keys are 32- or 64-bit; a short read leaves the high bytes 0 on
     * the little-endian Macs this runs on. */
    if (sysctlbyname(name, &v, &len, NULL, 0) != 0 || len > sizeof v) return 0;
    return v;
}
#else
/* Reads a small text file into buf (NUL-terminated); 0 on success. These
 * are kernel pseudo-files, not world data, so a fixed buffer and plain
 * open/read are enough: nothing is allocated, as detection runs before
 * the allocator is configured. */
static int read_small(const char *path, char *buf, size_t cap)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    return 0;
}

/* "48K", "1M" or a plain byte count, as /sys reports cache sizes. */
static uint32_t parse_size(const char *s)
{
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (end == s) return 0;
    if (*end == 'K') v *= 1024ull;
    else if (*end == 'M') v *= MIB;
    return v > UINT32_MAX ? 0 : (uint32_t)v;
}
#endif

void sys_detect(sys_info *si)
{
    memset(si, 0, sizeof *si);
    si->cores = os_cpu_count();
#ifdef __APPLE__
    si->ram = sysctl_u64("hw.memsize");
    si->l1d = (uint32_t)sysctl_u64("hw.l1dcachesize");
    /* Apple silicon reports its performance cores' caches per level. */
    uint64_t l2 = sysctl_u64("hw.perflevel0.l2cachesize");
    si->l2 = (uint32_t)(l2 ? l2 : sysctl_u64("hw.l2cachesize"));
    si->l3 = (uint32_t)sysctl_u64("hw.l3cachesize");
#else
    long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGESIZE);
    if (pages > 0 && page > 0) si->ram = (uint64_t)pages * (uint64_t)page;
    char buf[4096] = {0};
    if (read_small("/proc/meminfo", buf, sizeof buf) == 0) {
        const char *p = strstr(buf, "MemAvailable:");
        if (p) si->ram_avail = strtoull(p + 13, NULL, 10) * 1024ull;
    }
    for (int i = 0; i < 8; i++) {
        char path[96] = {0}, level[16] = {0}, type[32] = {0}, size[32] = {0};
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu0/cache/index%d/level", i);
        if (read_small(path, level, sizeof level) != 0) break;
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu0/cache/index%d/type", i);
        if (read_small(path, type, sizeof type) != 0 || strncmp(type, "Instruction", 11) == 0) continue;
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu0/cache/index%d/size", i);
        if (read_small(path, size, sizeof size) != 0) continue;
        uint32_t bytes = parse_size(size);
        if (level[0] == '1') si->l1d = bytes;
        else if (level[0] == '2') si->l2 = bytes;
        else if (level[0] == '3') si->l3 = bytes;
    }
#endif
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void sys_plan(const sys_info *si, sys_budget *b)
{
    /* What the game may plan to use: what the OS says is available, or,
     * where it does not say, half of RAM so the desktop and its other
     * programs keep the rest; 1 GiB when nothing is known. */
    uint64_t usable = si->ram_avail ? si->ram_avail : si->ram / 2;
    if (!usable) usable = GIB;
    int cores = si->cores > 0 ? si->cores : 1;

    /* One core stays with the main thread (the frame and physics). A
     * worker's generation and meshing buffers come to a few MB, so the
     * 128 MB each is allowed is generous; it only bites on small boards. */
    b->workers = clampi(cores - 1, 1, 64);
    b->workers = clampi(b->workers, 1, (int)(usable / (128 * MIB) > 64 ? 64 : usable / (128 * MIB)));

    /* Columns are 32 KB of blocks plus meshes and metadata, ~100 KB in
     * all; radius 16 keeps 1089 of them. The radius also costs GPU memory
     * and frame time, which this cannot see, so it stays moderate; the
     * player can raise it to 32 in the settings. */
    if (usable >= 6 * GIB && cores >= 8) b->render_distance = 16;
    else if (usable >= 3 * GIB && cores >= 6) b->render_distance = 12;
    else if (usable >= 768 * MIB) b->render_distance = 8;
    else b->render_distance = 6;

    /* SQLite's page cache keeps recently streamed columns off the disk:
     * 1/512 of what the game may use, between 2 and 64 MB. */
    uint64_t kb = usable / 512 / 1024;
    b->sqlite_cache_kb = (int)(kb < 2048 ? 2048 : (kb > 65536 ? 65536 : kb));

    /* mimalloc reserves address space in this step; larger steps mean
     * fewer mappings, and only committed pages cost memory. */
    b->arena_mb = si->ram >= 8 * GIB ? 256 : 64;
}

void sys_log(const sys_info *si, const sys_budget *b)
{
    log_info("machine: %d cores, %.1f GB RAM (%.1f GB available), caches L1d %u KB, L2 %u KB, L3 %u KB; "
             "planning %d workers, render distance %d by default, %d MB SQLite cache",
             si->cores, (double)si->ram / (double)GIB, (double)si->ram_avail / (double)GIB, si->l1d / 1024,
             si->l2 / 1024, si->l3 / 1024, b->workers, b->render_distance, b->sqlite_cache_kb / 1024);
}
