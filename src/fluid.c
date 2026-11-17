#include "fluid.h"
#include "log.h"
#include "mem.h"

#include <string.h>

#define QUEUE_CAP 16384 /* initial; grows on demand */
#define QUEUE_MAX (1 << 18)
#define FLUID_BUDGET 4096 /* cells per tick */
#define FLUID_SEEK 4      /* how far a film looks along a line for a ledge */

static const int D[4][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}}; /* the sides */

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
    queue_init(&f->spare, QUEUE_CAP);
}

void fluid_destroy(fluids *f)
{
    queue_free(&f->now);
    queue_free(&f->next);
    queue_free(&f->spare);
}

/* A cell's block and, for a liquid, its level: world_get and
 * world_fluid_level in one column lookup. */
typedef struct {
    uint8_t id;
    int level;
} cell;

static inline cell read_cell(const world *w, int x, int y, int z)
{
    cell c = {y < 0 ? B_BEDROCK : B_AIR, 0};
    if (y < 0 || y >= WORLD_H) return c;
    const column *col = world_column(w, chunk_of(x), chunk_of(z));
    if (!col) {
        c.id = B_UNLOADED;
        return c;
    }
    int i = col_index(x & (CHUNK_W - 1), y, z & (CHUNK_W - 1));
    c.id = col->blocks[i];
    if (!block_fluid(c.id)) return c;
    uint8_t m = col->meta ? col->meta[i] : 0;
    c.level = m ? m : FLUID_FULL;
    return c;
}

static void wake_one(fluids *f, int x, int y, int z)
{
    if (block_fluid(world_get(f->w, x, y, z))) queue_push(&f->next, (ipos){x, y, z});
}

/* A cell on open ground cleared: a film up to FLUID_SEEK cells away along
 * a line may have been waiting for the way to a ledge to open (fluid_cell). */
static void wake_lines(fluids *f, int x, int y, int z)
{
    for (int k = 0; k < 4; k++)
        for (int i = 1; i <= FLUID_SEEK; i++) {
            cell c = read_cell(f->w, x + D[k][0] * i, y, z + D[k][1] * i);
            if (c.id == B_AIR) continue;
            if (c.level == 1) queue_push(&f->next, (ipos){x + D[k][0] * i, y, z + D[k][1] * i});
            break;
        }
}

void fluid_wake(fluids *f, int x, int y, int z)
{
    cell c = read_cell(f->w, x, y, z);
    if (c.level) queue_push(&f->next, (ipos){x, y, z});
    wake_one(f, x + 1, y, z);
    wake_one(f, x - 1, y, z);
    wake_one(f, x, y + 1, z);
    wake_one(f, x, y - 1, z);
    wake_one(f, x, y, z + 1);
    wake_one(f, x, y, z - 1);
    /* A cell with room is a drop for the liquid diagonally above it, which
     * may now run off its ledge (fluid_cell). */
    if (c.id == B_AIR || (c.level && c.level < FLUID_FULL)) {
        wake_one(f, x + 1, y + 1, z);
        wake_one(f, x - 1, y + 1, z);
        wake_one(f, x, y + 1, z + 1);
        wake_one(f, x, y + 1, z - 1);
    }
    if (c.id == B_AIR && y > 0) {
        cell u = read_cell(f->w, x, y - 1, z);
        if (u.id != B_AIR && (!u.level || u.level == FLUID_FULL)) wake_lines(f, x, y, z);
    }
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

/* Whether a cell at (x, y, z) has somewhere to drain: air, or the same
 * liquid with room, below it. */
static int has_drop(const world *w, int x, int y, int z, uint8_t id)
{
    if (y <= 0) return 0;
    cell u = read_cell(w, x, y - 1, z);
    return u.id == B_AIR || (u.id == id && u.level < FLUID_FULL);
}

/* The distance along (dx, dz) over open ground to the nearest ledge, or 0
 * if there is none within FLUID_SEEK. A ledge is air or the same liquid
 * with room, with a drop under it: water already pouring over the edge
 * still marks it. */
static int seek_drop(const world *w, int x, int y, int z, int dx, int dz, uint8_t id)
{
    for (int i = 1; i <= FLUID_SEEK; i++) {
        int sx = x + dx * i, sz = z + dz * i;
        cell c = read_cell(w, sx, y, sz);
        if (c.id != B_AIR && (c.id != id || c.level == FLUID_FULL)) return 0;
        if (has_drop(w, sx, y, sz, id)) return i;
        if (c.id != B_AIR) return 0;
    }
    return 0;
}

/* A liquid moves only into air or more of itself: first down, then to the
 * side. Sideways, a neighbour with a drop under it takes everything, so
 * water runs off a ledge rather than piling at its edge, and thin films
 * drain down slopes. Without one, the cell levels with all its lower
 * neighbours at once, to their common average; a difference of one level
 * is stable, which keeps a puddle from creeping forever. A film of one
 * level that could not move looks a few cells further for a ledge and
 * steps toward the nearest, so a plateau drains rather than stays wet;
 * each step brings it closer, so it never turns back. The cell writes its
 * own level once, since every write wakes up to eleven cells. */
static void fluid_cell(fluids *f, ipos p)
{
    world *w = f->w;
    cell here = read_cell(w, p.x, p.y, p.z);
    uint8_t id = here.id;
    int level = here.level;
    if (!level) return;

    if (p.y > 0) {
        cell under = read_cell(w, p.x, p.y - 1, p.z);
        if (under.id == B_AIR) {
            fluid_set(w, p.x, p.y - 1, p.z, id, level);
            fluid_set(w, p.x, p.y, p.z, id, 0);
            return;
        }
        if (under.id == id && under.level < FLUID_FULL) {
            int t = FLUID_FULL - under.level < level ? FLUID_FULL - under.level : level;
            fluid_set(w, p.x, p.y - 1, p.z, id, under.level + t);
            level -= t;
        }
    }

    uint32_t start = f->order + ipos_hash(p);
    int nx[4] = {0}, nz[4] = {0}, nl[4] = {0}, drop[4] = {0};
    int before = level;
    int k = 0, drops = 0;
    for (int i = 0; i < 4 && level > 0; i++) {
        const int *d = D[(start + (uint32_t)i) & 3];
        int x = p.x + d[0], z = p.z + d[1];
        cell n = read_cell(w, x, p.y, z);
        if ((n.id != B_AIR && n.id != id) || n.level >= level) continue;
        nx[k] = x;
        nz[k] = z;
        nl[k] = n.level;
        drop[k] = has_drop(w, x, p.y, z, id);
        drops += drop[k];
        k++;
    }

    if (drops) {
        /* Split evenly between the drops, the odd levels to the first. */
        int share = level / drops, odd = level % drops;
        for (int i = 0; i < k; i++) {
            if (!drop[i]) continue;
            int t = share + (odd > 0);
            odd--;
            if (t > FLUID_FULL - nl[i]) t = FLUID_FULL - nl[i];
            if (t <= 0) continue;
            fluid_set(w, nx[i], p.y, nz[i], id, nl[i] + t);
            level -= t;
        }
    } else if (k) {
        /* Neighbours lowest first (ties keep the turning order), then level
         * with as many of them as each still gain from it. */
        int by[4] = {0, 1, 2, 3};
        for (int i = 1; i < k; i++)
            for (int j = i; j > 0 && nl[by[j]] < nl[by[j - 1]]; j--) {
                int t = by[j];
                by[j] = by[j - 1];
                by[j - 1] = t;
            }
        int m = 0, avg = 0, sum = level;
        for (int i = 0; i < k; i++) {
            int a = (sum + nl[by[i]]) / (i + 2);
            if (a <= nl[by[i]]) break;
            sum += nl[by[i]];
            avg = a;
            m = i + 1;
        }
        for (int i = 0; i < m; i++) {
            fluid_set(w, nx[by[i]], p.y, nz[by[i]], id, avg);
            level -= avg - nl[by[i]];
        }
    }

    if (level == 1 && before == 1) {
        int best = 0;
        const int *to = NULL;
        for (int i = 0; i < 4; i++) {
            const int *d = D[(start + (uint32_t)i) & 3];
            int dist = seek_drop(w, p.x, p.y, p.z, d[0], d[1], id);
            if (dist && (!best || dist < best)) {
                best = dist;
                to = d;
            }
        }
        if (to) {
            int tx = p.x + to[0], tz = p.z + to[1];
            fluid_set(w, tx, p.y, tz, id, read_cell(w, tx, p.y, tz).level + 1);
            level = 0;
        }
    }
    if (level != here.level) fluid_set(w, p.x, p.y, p.z, id, level);
}

/* Moves this tick's unprocessed cells to the next tick. They go ahead of
 * the cells woken since: a flood bigger than the budget would otherwise
 * keep putting its far end at the back of the line, and some cells would
 * wait many ticks for their turn. */
static void fluid_carry(fluids *f)
{
    fluid_queue *now = &f->now, *line = &f->spare;
    if (f->read < now->count) {
        for (int i = f->read; i < now->count; i++) queue_push(line, now->items[i]);
        for (int i = 0; i < f->next.count; i++) queue_push(line, f->next.items[i]);
        queue_clear(&f->next);
        fluid_queue tmp = f->next;
        f->next = *line;
        *line = tmp;
    }
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
