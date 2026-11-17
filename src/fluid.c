#include "fluid.h"
#include "log.h"
#include "mem.h"

#include <string.h>

#define QUEUE_CAP 16384 /* initial; grows on demand */
#define QUEUE_MAX (1 << 18)
#define FLUID_BUDGET 4096 /* cells per tick */

/* ------------------------------------------------------------ the queue */

static void queue_init(fluid_queue *q, int cap)
{
    q->cap = cap;
    q->count = 0;
    q->items = mem_alloc(mem_array_size((size_t)cap, sizeof(ipos)));
    q->set_cap = 1;
    while (q->set_cap < (uint32_t)cap * 2) q->set_cap <<= 1;
    q->set = mem_calloc(q->set_cap, sizeof(uint32_t));
}

static void queue_free(fluid_queue *q)
{
    mem_free(q->items);
    mem_free(q->set);
    memset(q, 0, sizeof *q);
}

static void queue_grow(fluid_queue *q)
{
    q->cap *= 2;
    q->items = mem_realloc(q->items, mem_array_size((size_t)q->cap, sizeof(ipos)));
    q->set_cap *= 2;
    mem_free(q->set);
    q->set = mem_calloc(q->set_cap, sizeof(uint32_t));
    uint32_t mask = q->set_cap - 1;
    for (int i = 0; i < q->count; i++) {
        uint32_t h = ipos_hash(q->items[i]) & mask;
        while (q->set[h]) h = (h + 1) & mask;
        q->set[h] = (uint32_t)i + 1;
    }
}

static int queue_push(fluid_queue *q, ipos p)
{
    uint32_t mask = q->set_cap - 1, h = ipos_hash(p) & mask;
    while (q->set[h]) {
        ipos o = q->items[q->set[h] - 1];
        if (o.x == p.x && o.y == p.y && o.z == p.z) return 1;
        h = (h + 1) & mask;
    }
    if (q->count >= q->cap) {
        if (q->cap >= QUEUE_MAX) {
            /* A dropped wake leaves water frozen until something nearby
             * changes; say so once rather than silently. */
            static int warned;
            if (!warned) log_warn("fluid queue full (%d cells); some water will pause", q->cap);
            warned = 1;
            return 0;
        }
        queue_grow(q); /* rehashes: find p's slot again */
        mask = q->set_cap - 1;
        h = ipos_hash(p) & mask;
        while (q->set[h]) h = (h + 1) & mask;
    }
    q->items[q->count++] = p;
    q->set[h] = (uint32_t)q->count;
    return 1;
}

/* Cost follows the item count, not the table size: a tick that touched a
 * few cells no longer wipes 128 KB. */
static void queue_clear(fluid_queue *q)
{
    if (!q->count) return;
    if ((uint32_t)q->count > q->set_cap / 8) {
        memset(q->set, 0, q->set_cap * sizeof(uint32_t));
    } else {
        uint32_t mask = q->set_cap - 1;
        for (int i = 0; i < q->count; i++) {
            /* Every item is in the table, so its probe always finds it;
             * slots already cleared are simply stepped over. */
            uint32_t h = ipos_hash(q->items[i]) & mask;
            while (q->set[h] != (uint32_t)i + 1) h = (h + 1) & mask;
            q->set[h] = 0;
        }
    }
    q->count = 0;
}

/* ------------------------------------------------------------- the sim */

void fluid_init(fluids *f, world *w)
{
    memset(f, 0, sizeof *f);
    f->w = w;
    queue_init(&f->now, QUEUE_CAP);
    queue_init(&f->next, QUEUE_CAP);
}

void fluid_destroy(fluids *f)
{
    queue_free(&f->now);
    queue_free(&f->next);
}

static void wake_one(fluids *f, int x, int y, int z)
{
    if (block_fluid(world_get(f->w, x, y, z))) queue_push(&f->next, (ipos){x, y, z});
}

void fluid_wake(fluids *f, int x, int y, int z)
{
    wake_one(f, x, y, z);
    wake_one(f, x + 1, y, z);
    wake_one(f, x - 1, y, z);
    wake_one(f, x, y + 1, z);
    wake_one(f, x, y - 1, z);
    wake_one(f, x, y, z + 1);
    wake_one(f, x, y, z - 1);
}

int fluid_pending(const fluids *f) { return f->next.count; }

void fluid_forget(fluids *f) { queue_clear(&f->next); }

void fluid_set(world *w, int x, int y, int z, uint8_t id, int level)
{
    if (level <= 0) world_set(w, x, y, z, B_AIR, 0);
    else world_set(w, x, y, z, id, (uint8_t)(level >= FLUID_FULL ? 0 : level));
}

void fluid_displace(world *w, int x, int y, int z, uint8_t id, int level, int player)
{
    if (level <= 0 || !block_fluid(id) || y + 1 >= WORLD_H) return;
    uint8_t up = world_get(w, x, y + 1, z);
    if (up != B_AIR && up != id) return;
    int total = level + world_fluid_level(w, x, y + 1, z);
    uint8_t meta = (uint8_t)(total >= FLUID_FULL ? 0 : total);
    if (player) world_set_player(w, x, y + 1, z, id, meta);
    else world_set(w, x, y + 1, z, id, meta);
}

/* A liquid moves only into air or more of itself. */
static void fluid_cell(fluids *f, ipos p)
{
    world *w = f->w;
    uint8_t id = world_get(w, p.x, p.y, p.z);
    int level = world_fluid_level(w, p.x, p.y, p.z);
    if (!level) return;

    if (p.y > 0) {
        uint8_t below = world_get(w, p.x, p.y - 1, p.z);
        if (below == B_AIR) {
            fluid_set(w, p.x, p.y - 1, p.z, id, level);
            fluid_set(w, p.x, p.y, p.z, id, 0);
            return;
        }
        if (below == id) {
            int lb = world_fluid_level(w, p.x, p.y - 1, p.z);
            int t = FLUID_FULL - lb < level ? FLUID_FULL - lb : level;
            if (t > 0) {
                fluid_set(w, p.x, p.y - 1, p.z, id, lb + t);
                level -= t;
                fluid_set(w, p.x, p.y, p.z, id, level);
                if (!level) return;
            }
        }
    }

    static const int D[4][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};
    uint32_t start = f->order + ipos_hash(p);
    for (int k = 0; k < 4 && level > 1; k++) {
        const int *d = D[(start + (uint32_t)k) & 3];
        int nx = p.x + d[0], nz = p.z + d[1];
        uint8_t nb = world_get(w, nx, p.y, nz);
        int ln = 0;
        if (nb == B_AIR) ln = 0;
        else if (nb == id) ln = world_fluid_level(w, nx, p.y, nz);
        else continue;
        int t = (level - ln) / 2;
        if (t <= 0) continue;
        fluid_set(w, nx, p.y, nz, id, ln + t);
        level -= t;
        fluid_set(w, p.x, p.y, p.z, id, level);
    }
}

/* Moves this tick's unprocessed cells to the next tick. */
static void fluid_carry(fluids *f)
{
    fluid_queue *now = &f->now;
    for (int i = f->read; i < now->count; i++) queue_push(&f->next, now->items[i]);
    queue_clear(now);
    f->read = f->end = 0;
}

void fluid_begin_tick(fluids *f, uint32_t order)
{
    f->order = order;
    fluid_carry(f);
    fluid_queue tmp = f->now;
    f->now = f->next;
    f->next = tmp;
    int n = f->now.count;
    f->end = n < FLUID_BUDGET ? n : FLUID_BUDGET;
    f->per_step = (f->end + FLUID_TICK_EVERY - 1) / FLUID_TICK_EVERY;
    f->updates = f->end;
}

void fluid_run(fluids *f, int n)
{
    int end = f->read + n < f->end ? f->read + n : f->end;
    while (f->read < end) fluid_cell(f, f->now.items[f->read++]);
}

void fluid_tick(fluids *f, uint32_t order)
{
    fluid_begin_tick(f, order);
    fluid_run(f, FLUID_BUDGET);
    fluid_carry(f);
}
