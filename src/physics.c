#include "physics.h"
#include "mem.h"
#include "log.h"
#include "noise.h"

#include <math.h>
#include <string.h>
#include <time.h>

#define EPS 1e-7
#define REGION_W (2 * STRUCT_RADIUS + 1)
#define REGION_CELLS (REGION_W * REGION_W * REGION_W)
#define FLUID_QUEUE_CAP 16384   /* initial; grows on demand */
#define FLUID_QUEUE_MAX (1 << 18)
#define FLUID_BUDGET 4096
#define GROUNDED (STRUCT_MAX_SPAN + 1)
#define STRUCT_BUDGET_S 0.0004 /* per step, after the first check */

enum { SWEEP_BODIES = 1, SWEEP_PLAYER = 2 };

static inline double getc3(dvec3 v, int a) { return a == 0 ? v.x : (a == 1 ? v.y : v.z); }
static inline void addc3(dvec3 *v, int a, double d)
{
    if (a == 0) v->x += d; else if (a == 1) v->y += d; else v->z += d;
}
static inline void setc3(dvec3 *v, int a, double d)
{
    if (a == 0) v->x = d; else if (a == 1) v->y = d; else v->z = d;
}

static int is_structural(uint8_t id)
{
    return id == B_UNLOADED || (id < B_COUNT && (g_blocks[id].flags & BF_SOLID));
}

/* Lighter than water: floats, and a floating block counts as supported. */
static int is_buoyant(uint8_t id)
{
    return id < B_COUNT && (g_blocks[id].flags & BF_SOLID) && g_blocks[id].density < 1000.0f;
}

static int is_free(uint8_t id) { return id == B_AIR || id == B_WATER; }

/* Would a block placed at (x, y, z) be supported right away? */
static int supported_below(const world *w, int x, int y, int z, uint8_t id)
{
    if (y <= 0) return 1;
    uint8_t below = world_get(w, x, y - 1, z);
    return is_structural(below) || (below == B_WATER && is_buoyant(id));
}

static int span_of(uint8_t id)
{
    if (id == B_UNLOADED) return STRUCT_MAX_SPAN;
    int s = block_get(id)->span;
    return s > STRUCT_MAX_SPAN ? STRUCT_MAX_SPAN : s;
}

static double mono_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int boxes_overlap(aabb a, aabb b)
{
    return a.min.x < b.max.x - EPS && a.max.x > b.min.x + EPS &&
           a.min.y < b.max.y - EPS && a.max.y > b.min.y + EPS &&
           a.min.z < b.max.z - EPS && a.max.z > b.min.z + EPS;
}

/* ----------------------------------------------------------- position set */

static uint32_t hash_pos(ipos p)
{
    return hash_u32((uint32_t)p.x * 73856093U ^ (uint32_t)p.y * 19349663U ^ (uint32_t)p.z * 83492791U);
}

static void pq_init(pos_queue *q, int cap)
{
    q->cap = cap;
    q->count = 0;
    q->items = mem_alloc(mem_array_size((size_t)cap, sizeof(ipos)));
    q->set_cap = 1;
    while (q->set_cap < (uint32_t)cap * 2) q->set_cap <<= 1;
    q->set = mem_calloc(q->set_cap, sizeof(uint32_t));
}

static void pq_free(pos_queue *q)
{
    mem_free(q->items);
    mem_free(q->set);
    memset(q, 0, sizeof *q);
}

static void pq_grow(pos_queue *q)
{
    q->cap *= 2;
    q->items = mem_realloc(q->items, mem_array_size((size_t)q->cap, sizeof(ipos)));
    q->set_cap *= 2;
    mem_free(q->set);
    q->set = mem_calloc(q->set_cap, sizeof(uint32_t));
    uint32_t mask = q->set_cap - 1;
    for (int i = 0; i < q->count; i++) {
        uint32_t h = hash_pos(q->items[i]) & mask;
        while (q->set[h]) h = (h + 1) & mask;
        q->set[h] = (uint32_t)i + 1;
    }
}

static int pq_push(pos_queue *q, ipos p)
{
    uint32_t mask = q->set_cap - 1, h = hash_pos(p) & mask;
    while (q->set[h]) {
        ipos o = q->items[q->set[h] - 1];
        if (o.x == p.x && o.y == p.y && o.z == p.z) return 1;
        h = (h + 1) & mask;
    }
    if (q->count >= q->cap) {
        if (q->cap >= FLUID_QUEUE_MAX) {
            /* A dropped wake leaves water frozen until something nearby
             * changes; say so once rather than silently. */
            static int warned;
            if (!warned) log_warn("fluid queue full (%d cells); some water will pause", q->cap);
            warned = 1;
            return 0;
        }
        pq_grow(q);
        return pq_push(q, p);
    }
    q->items[q->count++] = p;
    q->set[h] = (uint32_t)q->count;
    return 1;
}

/* Cost follows the item count, not the table size: a tick that touched a
 * few cells no longer wipes 128 KB. */
static void pq_clear(pos_queue *q)
{
    if (!q->count) return;
    if ((uint32_t)q->count > q->set_cap / 8) {
        memset(q->set, 0, q->set_cap * sizeof(uint32_t));
    } else {
        uint32_t mask = q->set_cap - 1;
        for (int i = 0; i < q->count; i++) {
            /* Every item is in the table, so its probe always finds it;
             * slots already cleared are simply stepped over. */
            uint32_t h = hash_pos(q->items[i]) & mask;
            while (q->set[h] != (uint32_t)i + 1) h = (h + 1) & mask;
            q->set[h] = 0;
        }
    }
    q->count = 0;
}

/* ------------------------------------------------------------------ init */

void physics_init(physics *ph, world *w)
{
    memset(ph, 0, sizeof *ph);
    ph->w = w;
    ph->bodies = mem_alloc(sizeof(body) * MAX_BODIES);
    pq_init(&ph->fluid_now, FLUID_QUEUE_CAP);
    pq_init(&ph->fluid_next, FLUID_QUEUE_CAP);
    ph->st_s = mem_alloc(REGION_CELLS);
    ph->st_id = mem_alloc(REGION_CELLS);
    for (int i = 0; i <= GROUNDED; i++)
        ph->st_bucket[i] = mem_alloc(sizeof(int32_t) * REGION_CELLS);
}

void physics_destroy(physics *ph)
{
    mem_free(ph->bodies);
    pq_free(&ph->fluid_now);
    pq_free(&ph->fluid_next);
    mem_free(ph->st_s);
    mem_free(ph->st_id);
    for (int i = 0; i <= GROUNDED; i++) mem_free(ph->st_bucket[i]);
    memset(ph, 0, sizeof *ph);
}

/* ------------------------------------------------------------- collision */

aabb player_box(const player *p)
{
    aabb b;
    b.min = dv3(p->pos.x - PLAYER_HALF_W, p->pos.y, p->pos.z - PLAYER_HALF_W);
    b.max = dv3(p->pos.x + PLAYER_HALF_W, p->pos.y + PLAYER_HEIGHT, p->pos.z + PLAYER_HALF_W);
    return b;
}

/* Clamp a move of `delta` along `axis` against an obstacle box. */
static double clip_against(aabb box, aabb ob, int axis, double delta)
{
    for (int a = 0; a < 3; a++) {
        if (a == axis) continue;
        if (getc3(box.min, a) >= getc3(ob.max, a) - EPS || getc3(box.max, a) <= getc3(ob.min, a) + EPS)
            return delta;
    }
    if (delta > 0) {
        double gap = getc3(ob.min, axis) - getc3(box.max, axis);
        if (gap >= -EPS && gap < delta) delta = gap > 0 ? gap : 0;
    } else {
        double gap = getc3(ob.max, axis) - getc3(box.min, axis);
        if (gap <= EPS && gap > delta) delta = gap < 0 ? gap : 0;
    }
    return delta;
}

static double sweep(const physics *ph, aabb box, int axis, double delta, int flags, int skip_body)
{
    if (delta == 0.0) return 0.0;
    /* A single step never needs to move further than this; clamping keeps
     * the block scan bounded even if a velocity explodes. */
    delta = clampd(delta, -8.0, 8.0);

    int lo[3], hi[3];
    for (int a = 0; a < 3; a++) {
        if (a == axis) continue;
        lo[a] = (int)floor(getc3(box.min, a) + EPS);
        hi[a] = (int)floor(getc3(box.max, a) - EPS);
    }
    if (delta > 0) {
        lo[axis] = (int)ceil(getc3(box.max, axis) - EPS);
        hi[axis] = (int)floor(getc3(box.max, axis) + delta - EPS);
    } else {
        lo[axis] = (int)floor(getc3(box.min, axis) + delta + EPS);
        hi[axis] = (int)floor(getc3(box.min, axis) + EPS) - 1;
    }

    const world *w = ph->w;
    if (lo[axis] <= hi[axis]) {
        /* Walk cells nearest-first along the axis so we can stop early. */
        int n = hi[axis] - lo[axis] + 1;
        for (int t = 0; t < n; t++) {
            int c = delta > 0 ? lo[axis] + t : hi[axis] - t;
            int found = 0;
            int p[3];
            p[axis] = c;
            int a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
            for (p[a1] = lo[a1]; p[a1] <= hi[a1] && !found; p[a1]++)
                for (p[a2] = lo[a2]; p[a2] <= hi[a2]; p[a2]++)
                    if (block_solid(world_get(w, p[0], p[1], p[2]))) { found = 1; break; }
            if (found) {
                double lim = delta > 0 ? c - getc3(box.max, axis) : (c + 1) - getc3(box.min, axis);
                if (delta > 0) delta = lim > 0 ? (lim < delta ? lim : delta) : 0;
                else delta = lim < 0 ? (lim > delta ? lim : delta) : 0;
                break;
            }
        }
    }

    if ((flags & SWEEP_BODIES)) {
        /* Only bodies overlapping the swept box can shorten the move;
         * rejecting the rest is a few compares instead of a clip. */
        aabb sw = box;
        if (delta > 0) addc3(&sw.max, axis, delta);
        else addc3(&sw.min, axis, delta);
        for (int i = 0; i < ph->body_count; i++) {
            const body *b = &ph->bodies[i];
            if (b->pos.x >= sw.max.x || b->pos.x + 1 <= sw.min.x || b->pos.y >= sw.max.y ||
                b->pos.y + 1 <= sw.min.y || b->pos.z >= sw.max.z || b->pos.z + 1 <= sw.min.z || i == skip_body)
                continue;
            aabb ob = {b->pos, dv3(b->pos.x + 1, b->pos.y + 1, b->pos.z + 1)};
            delta = clip_against(box, ob, axis, delta);
        }
    }
    if ((flags & SWEEP_PLAYER) && ph->pl && !ph->pl->flying)
        delta = clip_against(box, player_box(ph->pl), axis, delta);
    return delta;
}

double physics_sweep(const physics *ph, aabb box, int axis, double delta, int skip_body,
                     int include_player)
{
    return sweep(ph, box, axis, delta, SWEEP_BODIES | (include_player ? SWEEP_PLAYER : 0), skip_body);
}

/* Fraction of the box's height that is under water, sampled on the box's
 * central column (good enough for bodies up to one block wide). */
static double water_fraction(const world *w, aabb box)
{
    int x = (int)floor((box.min.x + box.max.x) * 0.5);
    int z = (int)floor((box.min.z + box.max.z) * 0.5);
    double h = box.max.y - box.min.y, wet = 0.0;
    for (int y = (int)floor(box.min.y); y <= (int)floor(box.max.y - EPS); y++) {
        int level = world_water_level(w, x, y, z);
        if (!level) continue;
        double top = y + level / (double)WATER_FULL;
        double lo = box.min.y > y ? box.min.y : y;
        double hi = box.max.y < top ? box.max.y : top;
        if (hi > lo) wet += hi - lo;
    }
    return h > 0 ? clampd(wet / h, 0.0, 1.0) : 0.0;
}

/* --------------------------------------------------------------- player */

void player_spawn(player *p, const world *w, double x, double z)
{
    memset(p, 0, sizeof *p);
    int sy = world_surface_y(w, (int)floor(x), (int)floor(z));
    p->pos = dv3(floor(x) + 0.5, sy < 0 ? WORLD_H : sy, floor(z) + 0.5);
    p->prev_pos = p->pos;
}

static double ground_friction(const physics *ph, const player *p)
{
    int y = (int)floor(p->pos.y - 0.05);
    double best = 0.0;
    const double off[5][2] = {{0, 0}, {-PLAYER_HALF_W, -PLAYER_HALF_W}, {PLAYER_HALF_W, -PLAYER_HALF_W},
                              {-PLAYER_HALF_W, PLAYER_HALF_W}, {PLAYER_HALF_W, PLAYER_HALF_W}};
    for (int i = 0; i < 5; i++) {
        uint8_t id = world_get(ph->w, (int)floor(p->pos.x + off[i][0] * 0.99), y,
                               (int)floor(p->pos.z + off[i][1] * 0.99));
        if (!block_solid(id)) continue;
        double mu = id == B_UNLOADED ? 0.6 : block_get(id)->friction;
        if (i == 0) return mu; /* standing squarely on one block */
        if (mu > best) best = mu;
    }
    return best > 0 ? best : 0.6; /* standing on a falling body */
}

static void player_move(physics *ph, player *p, double dt)
{
    aabb box = player_box(p);
    double dy = p->vel.y * dt;
    double my = sweep(ph, box, 1, dy, SWEEP_BODIES, -1);
    box.min.y += my; box.max.y += my;
    p->on_ground = dy < 0 && my > dy + EPS;
    if (my != dy) p->vel.y = 0;

    double dx = p->vel.x * dt;
    double mx = sweep(ph, box, 0, dx, SWEEP_BODIES, -1);
    box.min.x += mx; box.max.x += mx;
    if (mx != dx) p->vel.x = 0;

    double dz = p->vel.z * dt;
    double mz = sweep(ph, box, 2, dz, SWEEP_BODIES, -1);
    box.min.z += mz; box.max.z += mz;
    if (mz != dz) p->vel.z = 0;

    p->pos = dv3((box.min.x + box.max.x) * 0.5, box.min.y, (box.min.z + box.max.z) * 0.5);
    p->pos.x = clampd(p->pos.x, -WORLD_LIMIT, WORLD_LIMIT);
    p->pos.z = clampd(p->pos.z, -WORLD_LIMIT, WORLD_LIMIT);
}

static void player_step(physics *ph, player *p, const player_input *in)
{
    const double dt = PHYS_DT;
    p->prev_pos = p->pos;

    double sy = sin(p->yaw), cy = cos(p->yaw);
    double fx = in->forward * sy + in->right * cy;
    double fz = -in->forward * cy + in->right * sy;
    double len = hypot(fx, fz);
    if (len > 1.0) { fx /= len; fz /= len; len = 1.0; }

    if (p->flying) {
        double speed = in->sprint ? 24.0 : 10.0;
        p->vel = dv3(fx * speed, (in->jump - in->descend) * speed, fz * speed);
        player_move(ph, p, dt);
        return;
    }

    double sub = water_fraction(ph->w, player_box(p));
    p->submerged = sub;

    /* Gravity and Archimedes: a human is slightly less dense than water. */
    p->vel.y -= GRAVITY * dt;
    p->vel.y += GRAVITY * (1000.0 / PLAYER_DENSITY) * sub * dt;

    /* Quadratic drag, 0.5 * rho * Cd*A * v^2 / m, integrated implicitly so
     * it stays stable in dense water. */
    const double k_air = 0.5 * 1.225 * 0.5 / PLAYER_MASS;
    const double k_water = 0.5 * 1000.0 * 0.25 / PLAYER_MASS;
    double k = k_air * (1.0 - sub) + k_water * sub;
    double speed = dv3_len(p->vel);
    p->vel = dv3_scale(p->vel, 1.0 / (1.0 + k * speed * dt));

    /* Locomotion: feet can only push as hard as friction allows. */
    double target = in->sprint ? 6.5 : 4.0;
    double amax;
    if (sub > 0.5) {
        target = in->sprint ? 1.8 : 1.2;
        amax = 3.0;
    } else if (p->on_ground) {
        amax = ground_friction(ph, p) * GRAVITY;
    } else {
        amax = 0.6; /* a little mid-air body English, nothing more */
    }
    double dvx = fx * target - p->vel.x, dvz = fz * target - p->vel.z;
    double dv = hypot(dvx, dvz), maxdv = amax * dt;
    if (dv > maxdv) { dvx *= maxdv / dv; dvz *= maxdv / dv; }
    if (!p->on_ground && sub <= 0.5 && len < 1e-3) dvx = dvz = 0; /* keep momentum */
    p->vel.x += dvx;
    p->vel.z += dvz;

    if (in->jump) {
        if (sub > 0.3) {
            p->vel.y += 14.0 * dt; /* swim stroke */
            if (p->vel.y > 2.0) p->vel.y = 2.0;
        } else if (p->on_ground) {
            p->vel.y = sqrt(2.0 * GRAVITY * 1.15); /* just clears one block */
            p->on_ground = 0;
        }
    }

    player_move(ph, p, dt);
}

/* --------------------------------------------------------------- bodies */

static void spawn_body(physics *ph, int x, int y, int z, uint8_t id)
{
    body *b = &ph->bodies[ph->body_count++];
    memset(b, 0, sizeof *b);
    b->pos = dv3(x, y, z);
    b->prev_pos = b->pos;
    b->block = id;
}

static void set_water(world *w, int x, int y, int z, int level)
{
    if (level <= 0) world_set(w, x, y, z, B_AIR, 0);
    else world_set(w, x, y, z, B_WATER, (uint8_t)(level >= WATER_FULL ? 0 : level));
}

/* Returns 1 if the body is finished (placed or destroyed). */
static int solidify(physics *ph, const body *b)
{
    world *w = ph->w;
    int x = (int)floor(b->pos.x + 0.5), y = (int)floor(b->pos.y + 0.5), z = (int)floor(b->pos.z + 0.5);
    for (int t = 0; t < 3; t++, y++) {
        if (y >= WORLD_H) return 1;
        uint8_t id = world_get(w, x, y, z);
        if (id != B_AIR && id != B_WATER) continue;
        aabb cell = {dv3(x, y, z), dv3(x + 1, y + 1, z + 1)};
        /* Never inside the player, flying or not; and only where it will
         * stay put, or the structural check would knock it loose again. A
         * body resting on the player just rests until they move. */
        if (ph->pl && boxes_overlap(cell, player_box(ph->pl))) return 0;
        if (!supported_below(w, x, y, z, b->block)) return 0;
        int displaced = id == B_WATER ? world_water_level(w, x, y, z) : 0;
        world_set(w, x, y, z, b->block, 0);
        if (displaced) {
            /* Push the displaced water up rather than deleting it. */
            uint8_t up = world_get(w, x, y + 1, z);
            int have = up == B_WATER ? world_water_level(w, x, y + 1, z) : 0;
            if (up == B_AIR || up == B_WATER) {
                int total = have + displaced;
                set_water(w, x, y + 1, z, total > WATER_FULL ? WATER_FULL : total);
            }
        }
        return 1;
    }
    return 1; /* no room: crushed to rubble */
}

static void bodies_step(physics *ph)
{
    const double dt = PHYS_DT;
    for (int i = 0; i < ph->body_count; i++) {
        body *b = &ph->bodies[i];
        b->prev_pos = b->pos;
        int bx = (int)floor(b->pos.x + 0.5), bz = (int)floor(b->pos.z + 0.5);
        if (!world_column(ph->w, chunk_of(bx), chunk_of(bz))) {
            /* Its terrain streamed out: nothing sensible to simulate. */
            ph->bodies[i--] = ph->bodies[--ph->body_count];
            continue;
        }
        double rho = block_get(b->block)->density;
        aabb box = {b->pos, dv3(b->pos.x + 1, b->pos.y + 1, b->pos.z + 1)};
        double sub = water_fraction(ph->w, box);

        b->vel.y -= GRAVITY * dt;
        b->vel.y += GRAVITY * (1000.0 / rho) * sub * dt;
        /* Cube face-on: Cd ~1.05, A = 1 m^2, m = rho * 1 m^3. */
        double k = 0.5 * 1.05 * (1.225 * (1.0 - sub) + 1000.0 * sub) / rho;
        double speed = dv3_len(b->vel);
        /* Quadratic drag fades at low speed; a floating block also sheds
         * energy into the waves it makes, so it settles in seconds. */
        b->vel = dv3_scale(b->vel, 1.0 / (1.0 + (k * speed + 2.0 * sub) * dt));

        int landed = 0;
        for (int axis = 0; axis < 3; axis++) {
            double d = getc3(b->vel, axis) * dt;
            if (d == 0.0) continue;
            double m = sweep(ph, box, axis, d, SWEEP_PLAYER, i);
            if (m != d) {
                /* Landing means the world stopped it, not the player. */
                if (axis == 1 && d < 0 && sweep(ph, box, axis, d, 0, i) != d) landed = 1;
                setc3(&b->vel, axis, 0.0);
            }
            addc3(&box.min, axis, m);
            addc3(&box.max, axis, m);
        }
        b->pos = box.min;

        if (dv3_len(b->vel) < 0.05) b->rest_time += (float)dt;
        else b->rest_time = 0.0f;

        if ((landed || b->rest_time > 1.0f) && solidify(ph, b)) {
            ph->bodies[i--] = ph->bodies[--ph->body_count];
        }
    }
}

/* Puts a body back into the world where it would have come to rest:
 * straight down onto support, then up to the first free cell. */
static void settle_body(physics *ph, const body *b)
{
    world *w = ph->w;
    int x = (int)floor(b->pos.x + 0.5), z = (int)floor(b->pos.z + 0.5);
    int y = (int)floor(b->pos.y + 0.5);
    y = y < 0 ? 0 : (y >= WORLD_H ? WORLD_H - 1 : y);
    while (y > 0 && is_free(world_get(w, x, y, z)) && !supported_below(w, x, y, z, b->block)) y--;
    while (y < WORLD_H && !is_free(world_get(w, x, y, z))) y++;
    if (y < WORLD_H) world_set(w, x, y, z, b->block, 0);
}

void physics_settle_bodies(physics *ph, const column *only)
{
    ph->suppress_struct++;
    for (int i = 0; i < ph->body_count; i++) {
        const body *b = &ph->bodies[i];
        int bx = (int)floor(b->pos.x + 0.5), bz = (int)floor(b->pos.z + 0.5);
        if (only && (chunk_of(bx) != only->cx || chunk_of(bz) != only->cz)) continue;
        settle_body(ph, b);
        ph->bodies[i--] = ph->bodies[--ph->body_count];
    }
    ph->suppress_struct--;
}

void physics_on_column_unload(void *user, column *c)
{
    physics_settle_bodies(user, c);
}

/* ------------------------------------------------------------ structure */

static void queue_structure(physics *ph, int x, int y, int z)
{
    if (ph->struct_count >= (int)(sizeof ph->struct_queue / sizeof ph->struct_queue[0])) return;
    int tail = (ph->struct_head + ph->struct_count) % (int)(sizeof ph->struct_queue / sizeof ph->struct_queue[0]);
    ph->struct_queue[tail] = (ipos){x, y, z};
    ph->struct_count++;
}

int physics_check_structure(physics *ph, int x, int y, int z)
{
    const int R = STRUCT_RADIUS;
    const int x0 = x - R, z0 = z - R;
    const int y0 = y - R < 0 ? 0 : y - R;
    const int y1 = y + R >= WORLD_H ? WORLD_H - 1 : y + R;
    const int ny = y1 - y0 + 1;
    world *w = ph->w;
    int8_t *s = ph->st_s;
    uint8_t *ids = ph->st_id;
    int32_t **bucket = ph->st_bucket;
    int *blen = ph->st_bucket_len;
    for (int i = 0; i <= GROUNDED; i++) blen[i] = 0;

#define RIDX(i, j, k) (((j) * REGION_W + (k)) * REGION_W + (i))
    /* Read the region once, one column lookup per (x, z); every later pass
     * works on this copy. */
    for (int k = 0; k < REGION_W; k++)
        for (int i = 0; i < REGION_W; i++) {
            int wx = x0 + i, wz = z0 + k;
            const column *c = world_column(w, chunk_of(wx), chunk_of(wz));
            const uint8_t *col = c ? &c->blocks[col_index(wx & (CHUNK_W - 1), 0, wz & (CHUNK_W - 1))] : NULL;
            for (int j = 0; j < ny; j++) ids[RIDX(i, j, k)] = col ? col[(y0 + j) * COL_AREA] : B_UNLOADED;
        }

    /* s = -2: not structural, -1: unsupported (so far), >= 0: stability. */
    for (int j = 0; j < ny; j++)
        for (int k = 0; k < REGION_W; k++)
            for (int i = 0; i < REGION_W; i++) {
                int idx = RIDX(i, j, k);
                uint8_t id = ids[idx];
                if (!is_structural(id)) { s[idx] = -2; continue; }
                int wy = y0 + j;
                /* Cells on the region boundary are assumed supported: we
                 * cannot see their load path, and a false collapse is far
                 * worse than a missed one. */
                int anchor = id == B_UNLOADED || (block_get(id)->flags & BF_ANCHOR) || wy == 0 ||
                             i == 0 || i == REGION_W - 1 || k == 0 || k == REGION_W - 1 ||
                             (j == 0 && y0 > 0) || (j == ny - 1 && y1 < WORLD_H - 1) ||
                             (j > 0 && ids[RIDX(i, j - 1, k)] == B_WATER && is_buoyant(id)); /* floating */
                if (anchor) {
                    s[idx] = GROUNDED;
                    bucket[GROUNDED][blen[GROUNDED]++] = idx;
                } else {
                    s[idx] = -1;
                }
            }

    /* Bucket queue, strongest first. GROUNDED blocks have an unbroken
     * vertical load path to the ground. A block resting on a grounded block
     * is grounded; resting on anything else it inherits that block's
     * remaining budget. Each sideways or hanging step spends one block of
     * budget, capped by the material's own span. Granular blocks accept
     * only support from directly below. */
    static const int N6[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int lvl = GROUNDED; lvl >= 0; lvl--) {
        while (blen[lvl] > 0) {
            int idx = bucket[lvl][--blen[lvl]];
            if (s[idx] != lvl) continue;
            int i = idx % REGION_W, k = (idx / REGION_W) % REGION_W, j = idx / (REGION_W * REGION_W);
            for (int d = 0; d < 6; d++) {
                int ni = i + N6[d][0], nj = j + N6[d][1], nk = k + N6[d][2];
                if (ni < 0 || ni >= REGION_W || nk < 0 || nk >= REGION_W || nj < 0 || nj >= ny) continue;
                int nidx = RIDX(ni, nj, nk);
                if (s[nidx] < -1) continue;
                uint8_t nid = ids[nidx];
                int sp = span_of(nid), cand;
                int capped = sp < lvl ? sp : lvl;
                if (d == 2) cand = lvl == GROUNDED ? GROUNDED : capped;   /* resting on us */
                else if (nid < B_COUNT && (g_blocks[nid].flags & BF_GRANULAR)) continue;
                else cand = capped - 1;                                   /* cantilever / hanging */
                if (cand > s[nidx]) {
                    s[nidx] = (int8_t)cand;
                    bucket[cand][blen[cand]++] = nidx;
                }
            }
        }
    }

    /* Everything unreached falls, lowest first so the pile stacks. */
    int fell = 0;
    ph->suppress_struct++;
    for (int j = 0; j < ny; j++)
        for (int k = 0; k < REGION_W; k++)
            for (int i = 0; i < REGION_W; i++) {
                if (s[RIDX(i, j, k)] != -1) continue;
                if (ph->body_count >= MAX_BODIES) goto done;
                int wx = x0 + i, wy = y0 + j, wz = z0 + k;
                uint8_t id = ids[RIDX(i, j, k)]; /* only this pass edits, one cell at a time */
                if (id >= B_COUNT) continue;
                spawn_body(ph, wx, wy, wz, id);
                world_set(w, wx, wy, wz, B_AIR, 0);
                fell++;
            }
done:
    ph->suppress_struct--;
#undef RIDX
    /* Out of bodies: check again once some have landed, so the rest of the
     * collapse is not left hanging. */
    if (ph->body_count >= MAX_BODIES) queue_structure(ph, x, y, z);
    if (fell) ph->last_collapse = fell;
    return fell;
}

/* 1 if every cell from (x, y, z) down to bedrock is structural. */
static int grounded_column(const world *w, int x, int y, int z)
{
    if (y < 0) return 1;
    const column *c = world_column(w, chunk_of(x), chunk_of(z));
    if (!c) return 1; /* unloaded: treated as an anchor, as in the full check */
    const uint8_t *col = &c->blocks[col_index(x & (CHUNK_W - 1), 0, z & (CHUNK_W - 1))];
    for (int yy = y; yy >= 0; yy--)
        if (!is_structural(col[yy * COL_AREA])) return 0;
    return 1;
}

/* Exact shortcut for a removed block: when nothing structural rests on it
 * and each structural neighbour stands on its own unbroken column, no load
 * path ran only through it, so the full check would find nothing to drop.
 * Skips about a third of surface digs. */
static int removal_is_safe(const world *w, int x, int y, int z)
{
    if (y + 1 < WORLD_H && is_structural(world_get(w, x, y + 1, z))) return 0;
    static const int N5[5][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}};
    for (int d = 0; d < 5; d++) {
        int nx = x + N5[d][0], ny = y + N5[d][1], nz = z + N5[d][2];
        if (ny < 0 || !is_structural(world_get(w, nx, ny, nz))) continue;
        if (!grounded_column(w, nx, ny, nz)) return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------- fluids */

static void wake_water(physics *ph, int x, int y, int z)
{
    if (world_get(ph->w, x, y, z) == B_WATER) pq_push(&ph->fluid_next, (ipos){x, y, z});
}

void physics_on_block_changed(void *user, int x, int y, int z, uint8_t old_id, uint8_t new_id)
{
    physics *ph = user;
    if (new_id == B_AIR || new_id == B_WATER || old_id == B_WATER) {
        wake_water(ph, x, y, z);
        wake_water(ph, x + 1, y, z);
        wake_water(ph, x - 1, y, z);
        wake_water(ph, x, y + 1, z);
        wake_water(ph, x, y - 1, z);
        wake_water(ph, x, y, z + 1);
        wake_water(ph, x, y, z - 1);
    }
    if (ph->suppress_struct) return;
    /* Water drained from under a floating block: it may now hang. */
    if (old_id == B_WATER && new_id != B_WATER && is_buoyant(world_get(ph->w, x, y + 1, z)))
        queue_structure(ph, x, y + 1, z);
    int was = is_structural(old_id), is = is_structural(new_id);
    if (was && !is) {
        if (!removal_is_safe(ph->w, x, y, z)) queue_structure(ph, x, y, z);
    } else if (is && !was) {
        /* A block resting on a supported block is always supported (the
         * invariant is that every existing block is), so only sideways or
         * hanging placements need a check. */
        if (!is_structural(world_get(ph->w, x, y - 1, z))) queue_structure(ph, x, y, z);
    }
}

static void fluid_cell(physics *ph, ipos p)
{
    world *w = ph->w;
    int level = world_water_level(w, p.x, p.y, p.z);
    if (!level) return;

    if (p.y > 0) {
        uint8_t below = world_get(w, p.x, p.y - 1, p.z);
        if (below == B_AIR) {
            set_water(w, p.x, p.y - 1, p.z, level);
            set_water(w, p.x, p.y, p.z, 0);
            return;
        }
        if (below == B_WATER) {
            int lb = world_water_level(w, p.x, p.y - 1, p.z);
            int t = WATER_FULL - lb < level ? WATER_FULL - lb : level;
            if (t > 0) {
                set_water(w, p.x, p.y - 1, p.z, lb + t);
                level -= t;
                set_water(w, p.x, p.y, p.z, level);
                if (!level) return;
            }
        }
    }

    static const int D[4][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};
    uint32_t start = (uint32_t)(ph->step_count / FLUID_TICK_EVERY) + hash_pos(p);
    for (int k = 0; k < 4 && level > 1; k++) {
        const int *d = D[(start + (uint32_t)k) & 3];
        int nx = p.x + d[0], nz = p.z + d[1];
        uint8_t nb = world_get(w, nx, p.y, nz);
        int ln;
        if (nb == B_AIR) ln = 0;
        else if (nb == B_WATER) ln = world_water_level(w, nx, p.y, nz);
        else continue;
        int t = (level - ln) / 2;
        if (t <= 0) continue;
        set_water(w, nx, p.y, nz, ln + t);
        level -= t;
        set_water(w, p.x, p.y, p.z, level);
    }
}

/* Moves this tick's unprocessed cells to the next tick. */
static void fluid_carry(physics *ph)
{
    pos_queue *now = &ph->fluid_now;
    for (int i = ph->fluid_read; i < now->count; i++) pq_push(&ph->fluid_next, now->items[i]);
    pq_clear(now);
    ph->fluid_read = ph->fluid_end = 0;
}

static void fluid_begin_tick(physics *ph)
{
    fluid_carry(ph);
    pos_queue tmp = ph->fluid_now;
    ph->fluid_now = ph->fluid_next;
    ph->fluid_next = tmp;
    int n = ph->fluid_now.count;
    ph->fluid_end = n < FLUID_BUDGET ? n : FLUID_BUDGET;
    ph->fluid_per_step = (ph->fluid_end + FLUID_TICK_EVERY - 1) / FLUID_TICK_EVERY;
    ph->fluid_updates = ph->fluid_end;
}

static void fluid_run(physics *ph, int n)
{
    int end = ph->fluid_read + n < ph->fluid_end ? ph->fluid_read + n : ph->fluid_end;
    while (ph->fluid_read < end) fluid_cell(ph, ph->fluid_now.items[ph->fluid_read++]);
}

void physics_fluid_tick(physics *ph)
{
    fluid_begin_tick(ph);
    fluid_run(ph, FLUID_BUDGET);
    fluid_carry(ph);
}

/* ------------------------------------------------------------------ step */

void physics_step(physics *ph, player *p, const player_input *in)
{
    ph->pl = p;
    player_step(ph, p, in);
    bodies_step(ph);

    /* At least one structural check per step, more while time allows. */
    double t0 = ph->struct_count > 1 ? mono_sec() : 0.0;
    for (int n = 0; ph->struct_count > 0 && ph->body_count < MAX_BODIES; n++) {
        if (n > 0 && mono_sec() - t0 > STRUCT_BUDGET_S) break;
        ipos q = ph->struct_queue[ph->struct_head];
        ph->struct_head = (ph->struct_head + 1) % (int)(sizeof ph->struct_queue / sizeof ph->struct_queue[0]);
        ph->struct_count--;
        physics_check_structure(ph, q.x, q.y, q.z);
    }

    /* Fluids tick at 10 Hz, but each tick's cells are spread over the
     * steps until the next one instead of landing in a single step. */
    ph->step_count++;
    if (ph->step_count % FLUID_TICK_EVERY == 0) fluid_begin_tick(ph);
    fluid_run(ph, ph->fluid_per_step);
}

/* --------------------------------------------------------------- raycast */

ray_hit physics_raycast(const world *w, dvec3 o, vec3 dir, double max_dist)
{
    ray_hit r = {0};
    int x = (int)floor(o.x), y = (int)floor(o.y), z = (int)floor(o.z);
    int px = x, py = y, pz = z;
    double dx = dir.x, dy = dir.y, dz = dir.z;
    int sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1, sz = dz > 0 ? 1 : -1;
    double tdx = dx != 0 ? fabs(1.0 / dx) : INFINITY;
    double tdy = dy != 0 ? fabs(1.0 / dy) : INFINITY;
    double tdz = dz != 0 ? fabs(1.0 / dz) : INFINITY;
    double tmx = dx != 0 ? ((dx > 0 ? (x + 1 - o.x) : (o.x - x)) * tdx) : INFINITY;
    double tmy = dy != 0 ? ((dy > 0 ? (y + 1 - o.y) : (o.y - y)) * tdy) : INFINITY;
    double tmz = dz != 0 ? ((dz > 0 ? (z + 1 - o.z) : (o.z - z)) * tdz) : INFINITY;
    double t = 0.0;
    for (int steps = 0; steps < 256 && t <= max_dist; steps++) {
        uint8_t id = world_get(w, x, y, z);
        if (id == B_UNLOADED) return r;
        if (id != B_AIR && id != B_WATER) {
            r.hit = 1;
            r.block = (ipos){x, y, z};
            r.before = (ipos){px, py, pz};
            r.id = id;
            return r;
        }
        px = x; py = y; pz = z;
        if (tmx < tmy && tmx < tmz) { x += sx; t = tmx; tmx += tdx; }
        else if (tmy < tmz) { y += sy; t = tmy; tmy += tdy; }
        else { z += sz; t = tmz; tmz += tdz; }
    }
    return r;
}
