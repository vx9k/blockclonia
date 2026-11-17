/* The fluid module on its own: flow, volume, the wake queue, the budget
 * and displacement, in the flat test world. */
#include "fluid.h"
#include "noise.h"
#include "test_util.h"

#include <stdlib.h>

/* Liquid volume of one kind in a box around the origin, in levels. */
static long volume(const world *w, uint8_t id, int r, int y0, int y1)
{
    long v = 0;
    for (int y = y0; y < y1; y++)
        for (int z = -r; z <= r; z++)
            for (int x = -r; x <= r; x++)
                if (world_get(w, x, y, z) == id) v += world_fluid_level(w, x, y, z);
    return v;
}

/* Every woken cell has had its turn. */
static int settle(test_world *t, int max_ticks)
{
    int ticks = 0;
    while (fluid_pending(&t->ph.fl) && ticks < max_ticks) fluid_tick(&t->ph.fl, (uint32_t)ticks++);
    return ticks;
}

/* A stone-walled basin of inner size n x n on the ground at the origin. */
static void basin(world *w, int n, int wall)
{
    for (int y = GROUND; y < GROUND + wall; y++)
        for (int i = -1; i <= n; i++) {
            world_set(w, i, y, -1, B_STONE, 0);
            world_set(w, i, y, n, B_STONE, 0);
            world_set(w, -1, y, i, B_STONE, 0);
            world_set(w, n, y, i, B_STONE, 0);
        }
}

static void test_fluid_blocks(void)
{
    CHECK(block_fluid(B_WATER));
    CHECK(!block_fluid(B_AIR) && !block_fluid(B_STONE) && !block_fluid(B_ICE) && !block_fluid(B_UNLOADED));
    CHECK(block_has_meta(B_WATER) && block_has_meta(B_CAMPFIRE) && !block_has_meta(B_STONE));

    test_world t = {0};
    tw_init(&t);
    world_set(&t.w, 2, GROUND, 2, B_WATER, 3);
    world_set(&t.w, 3, GROUND, 2, B_WATER, 0);
    CHECK(world_fluid_level(&t.w, 2, GROUND, 2) == 3);
    CHECK(world_fluid_level(&t.w, 3, GROUND, 2) == FLUID_FULL); /* meta 0 is a full cell */
    CHECK(world_water_level(&t.w, 2, GROUND, 2) == 3);
    CHECK(world_fluid_level(&t.w, 4, GROUND, 2) == 0);
    CHECK(world_fluid_level(&t.w, 2, GROUND - 1, 2) == 0); /* ground */
    CHECK(world_fluid_level(&t.w, 2, -1, 2) == 0 && world_fluid_level(&t.w, 2, WORLD_H, 2) == 0);
    CHECK(world_fluid_level(&t.w, 100000, GROUND, 0) == 0); /* not loaded */
    /* A level past full is stored as full. */
    world_set(&t.w, 5, GROUND, 2, B_WATER, FLUID_FULL + 3);
    CHECK(world_fluid_level(&t.w, 5, GROUND, 2) == FLUID_FULL);
    /* Liquid counts as the surface (where things land and spawn). */
    CHECK(world_surface_y(&t.w, 3, 2) == GROUND + 1);
    tw_free(&t);
}

static void test_fluid_set(void)
{
    test_world t = {0};
    tw_init(&t);
    fluid_set(&t.w, 0, GROUND, 0, B_WATER, 5);
    CHECK(world_get(&t.w, 0, GROUND, 0) == B_WATER && world_fluid_level(&t.w, 0, GROUND, 0) == 5);
    fluid_set(&t.w, 0, GROUND, 0, B_WATER, FLUID_FULL);
    CHECK(world_get_meta(&t.w, 0, GROUND, 0) == 0); /* full is stored as 0 */
    fluid_set(&t.w, 0, GROUND, 0, B_WATER, FLUID_FULL + 5);
    CHECK(world_fluid_level(&t.w, 0, GROUND, 0) == FLUID_FULL);
    fluid_set(&t.w, 0, GROUND, 0, B_WATER, 0);
    CHECK(world_get(&t.w, 0, GROUND, 0) == B_AIR);
    fluid_set(&t.w, 0, GROUND, 0, B_WATER, -3);
    CHECK(world_get(&t.w, 0, GROUND, 0) == B_AIR);
    tw_free(&t);
}

static void test_fluid_wake(void)
{
    test_world t = {0};
    tw_init(&t);
    fluid_forget(&t.ph.fl);
    CHECK(fluid_pending(&t.ph.fl) == 0);
    /* Waking dry ground queues nothing: only liquid cells take a turn. */
    fluid_wake(&t.ph.fl, 0, GROUND, 0);
    CHECK(fluid_pending(&t.ph.fl) == 0);

    /* The cell and its six neighbours, each once however often woken. */
    for (int k = 0; k < 7; k++) {
        static const int N[7][3] = {{0, 0, 0}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        world_set(&t.w, 10 + N[k][0], GROUND + 2 + N[k][1], 10 + N[k][2], B_WATER, 0);
    }
    fluid_forget(&t.ph.fl);
    fluid_wake(&t.ph.fl, 10, GROUND + 2, 10);
    CHECK(fluid_pending(&t.ph.fl) == 7);
    fluid_wake(&t.ph.fl, 10, GROUND + 2, 10);
    fluid_wake(&t.ph.fl, 11, GROUND + 2, 10);
    CHECK(fluid_pending(&t.ph.fl) == 7);
    fluid_forget(&t.ph.fl);
    CHECK(fluid_pending(&t.ph.fl) == 0);

    /* A change through the world wakes the liquid next to it. */
    world_set(&t.w, 11, GROUND + 2, 10, B_AIR, 0);
    CHECK(fluid_pending(&t.ph.fl) > 0);
    tw_free(&t);
}

/* A cell in the air falls straight down, whole, one cell a tick. */
static void test_fluid_fall(void)
{
    test_world t = {0};
    tw_init(&t);
    world_set(&t.w, 0, GROUND + 10, 0, B_WATER, 4);
    fluid_tick(&t.ph.fl, 0);
    CHECK(world_get(&t.w, 0, GROUND + 10, 0) == B_AIR);
    CHECK(world_fluid_level(&t.w, 0, GROUND + 9, 0) == 4);
    for (int i = 1; i < 9; i++) fluid_tick(&t.ph.fl, (uint32_t)i);
    CHECK(world_fluid_level(&t.w, 0, GROUND + 1, 0) == 4);
    fluid_tick(&t.ph.fl, 9);
    CHECK(world_fluid_level(&t.w, 0, GROUND, 0) > 0); /* landed, and starts to spread */
    CHECK(volume(&t.w, B_WATER, 4, GROUND, GROUND + 12) == 4);
    tw_free(&t);
}

/* Liquid pours into the cell below until it is full. */
static void test_fluid_pour_down(void)
{
    test_world t = {0};
    tw_init(&t);
    basin(&t.w, 1, 3); /* a 1x1 shaft: nowhere to spread */
    world_set(&t.w, 0, GROUND, 0, B_WATER, 5);
    world_set(&t.w, 0, GROUND + 1, 0, B_WATER, 6);
    settle(&t, 50);
    CHECK(world_fluid_level(&t.w, 0, GROUND, 0) == FLUID_FULL);
    CHECK(world_fluid_level(&t.w, 0, GROUND + 1, 0) == 3);
    CHECK(world_get(&t.w, 0, GROUND + 2, 0) == B_AIR);
    tw_free(&t);
}

/* A column poured into a basin comes to rest as a level pool. */
static void test_fluid_level_pool(void)
{
    test_world t = {0};
    tw_init(&t);
    const int n = 6;
    basin(&t.w, n, 4);
    for (int y = GROUND; y < GROUND + 4; y++) world_set(&t.w, 2, y, 3, B_WATER, 0);
    long before = volume(&t.w, B_WATER, n + 2, GROUND, GROUND + 6);
    CHECK(before == 4 * FLUID_FULL);
    int ticks = settle(&t, 2000);
    CHECK(ticks < 2000);
    CHECK(volume(&t.w, B_WATER, n + 2, GROUND, GROUND + 6) == before);
    /* One layer, each cell within a level of its neighbours. */
    int steep = 0, above = 0;
    for (int z = 0; z < n; z++)
        for (int x = 0; x < n; x++) {
            int l = world_fluid_level(&t.w, x, GROUND, z);
            if (x + 1 < n) steep += abs(l - world_fluid_level(&t.w, x + 1, GROUND, z)) > 1;
            if (z + 1 < n) steep += abs(l - world_fluid_level(&t.w, x, GROUND, z + 1)) > 1;
            above += world_get(&t.w, x, GROUND + 1, z) != B_AIR;
        }
    CHECK(steep == 0 && above == 0);
    tw_free(&t);
}

/* Random spills over open ground: volume is exact after every tick. */
static void test_fluid_conserves(void)
{
    test_world t = {0};
    tw_init(&t);
    uint32_t rng = 99;
    for (int i = 0; i < 40; i++) {
        rng = hash_u32(rng + 1u);
        int x = (int)(rng % 21u) - 10, z = (int)((rng >> 8) % 21u) - 10, y = GROUND + (int)((rng >> 16) % 6u);
        world_set(&t.w, x, y, z, B_WATER, (uint8_t)(1 + (rng >> 24) % FLUID_FULL));
    }
    /* A few walls for the water to find its way round. */
    for (int x = -4; x <= 4; x++) world_set(&t.w, x, GROUND, 3, B_STONE, 0);
    long before = volume(&t.w, B_WATER, 40, GROUND, GROUND + 8);
    int same = 1;
    for (int tick = 0; tick < 300 && fluid_pending(&t.ph.fl); tick++) {
        fluid_tick(&t.ph.fl, (uint32_t)tick);
        same &= volume(&t.w, B_WATER, 40, GROUND, GROUND + 8) == before;
    }
    CHECK(same);
    CHECK(before > 0);
    tw_free(&t);
}

/* A tick run in slices, as the game spreads it over physics steps, does
 * exactly what a whole tick does. */
static uint64_t world_digest(const world *w, int r)
{
    uint64_t h = 1469598103934665603ull;
    for (int y = GROUND; y < GROUND + 6; y++)
        for (int z = -r; z <= r; z++)
            for (int x = -r; x <= r; x++) {
                h = (h ^ world_get(w, x, y, z)) * 1099511628211ull;
                h = (h ^ (uint64_t)world_fluid_level(w, x, y, z)) * 1099511628211ull;
            }
    return h;
}

static uint64_t spill(int sliced)
{
    test_world t = {0};
    tw_init(&t);
    for (int y = GROUND; y < GROUND + 5; y++) world_set(&t.w, 0, y, 0, B_WATER, 0);
    for (int tick = 0; tick < 40; tick++) {
        if (!sliced) {
            fluid_tick(&t.ph.fl, (uint32_t)tick);
            continue;
        }
        fluid_begin_tick(&t.ph.fl, (uint32_t)tick);
        for (int s = 0; s < FLUID_TICK_EVERY; s++) fluid_run(&t.ph.fl, t.ph.fl.per_step);
    }
    uint64_t h = world_digest(&t.w, 12);
    tw_free(&t);
    return h;
}

static void test_fluid_sliced(void)
{
    uint64_t whole = spill(0);
    CHECK(whole == spill(0)); /* deterministic */
    CHECK(whole == spill(1));
}

/* More cells than a tick's budget: the tick takes the budget and carries
 * the rest, which still get their turn. */
static void test_fluid_budget(void)
{
    test_world t = {0};
    tw_init(&t);
    /* 80 x 80 cells of shallow water, each with room to fall. */
    for (int z = -40; z < 40; z++)
        for (int x = -40; x < 40; x++) world_set(&t.w, x, GROUND + 3, z, B_WATER, 2);
    CHECK(fluid_pending(&t.ph.fl) >= 6400);
    fluid_begin_tick(&t.ph.fl, 0);
    CHECK(t.ph.fl.updates < 6400 && t.ph.fl.updates > 1000);
    CHECK(t.ph.fl.per_step * FLUID_TICK_EVERY >= t.ph.fl.updates);
    fluid_run(&t.ph.fl, 1 << 20);
    /* Every cell gets its turn: none is still where it started after a
     * few ticks. */
    int left = 1, ticks = 1;
    for (; left && ticks < 40; ticks++) {
        fluid_tick(&t.ph.fl, (uint32_t)ticks);
        left = 0;
        for (int z = -40; z < 40; z++)
            for (int x = -40; x < 40; x++) left += world_get(&t.w, x, GROUND + 3, z) == B_WATER;
    }
    CHECK(left == 0);
    CHECK(ticks == 2); /* the cells left over went first in the next tick */
    tw_free(&t);
}

/* A lone full cell on a floor shares the same amount with all four
 * neighbours, rather than halving into each in turn. */
static void test_fluid_spread_even(void)
{
    test_world t = {0};
    tw_init(&t);
    world_set(&t.w, 4, GROUND, 4, B_WATER, 0);
    fluid_tick(&t.ph.fl, 0);
    int e = world_fluid_level(&t.w, 5, GROUND, 4);
    CHECK(e > 0);
    CHECK(world_fluid_level(&t.w, 3, GROUND, 4) == e);
    CHECK(world_fluid_level(&t.w, 4, GROUND, 5) == e);
    CHECK(world_fluid_level(&t.w, 4, GROUND, 3) == e);
    CHECK(world_fluid_level(&t.w, 4, GROUND, 4) + 4 * e == FLUID_FULL);
    tw_free(&t);
}

/* The thinnest film runs off a ledge instead of waiting at its edge. */
static void test_fluid_ledge(void)
{
    test_world t = {0};
    tw_init(&t);
    world_set(&t.w, 0, GROUND, 0, B_STONE, 0);
    world_set(&t.w, 0, GROUND + 1, 0, B_WATER, 1);
    settle(&t, 50);
    CHECK(world_get(&t.w, 0, GROUND + 1, 0) == B_AIR);
    CHECK(volume(&t.w, B_WATER, 3, GROUND, GROUND + 1) == 1);
    tw_free(&t);
}

/* Digging out the ground beside a film gives it a drop: the film wakes and
 * runs into the hole, though the cell next to it stayed air. */
static void test_fluid_wake_drop(void)
{
    test_world t = {0};
    tw_init(&t);
    world_set(&t.w, 0, GROUND, 0, B_WATER, 1);
    settle(&t, 50);
    CHECK(world_fluid_level(&t.w, 0, GROUND, 0) == 1); /* a film on flat ground stays */
    world_set(&t.w, 1, GROUND - 1, 0, B_AIR, 0);
    CHECK(fluid_pending(&t.ph.fl) > 0);
    settle(&t, 50);
    CHECK(world_get(&t.w, 0, GROUND, 0) == B_AIR);
    CHECK(world_fluid_level(&t.w, 1, GROUND - 1, 0) == 1);
    tw_free(&t);
}

/* A cell between two drops splits evenly between them. */
static void test_fluid_drop_split(void)
{
    test_world t = {0};
    tw_init(&t);
    /* A pillar with the cell on top walled in on two sides. */
    world_set(&t.w, 0, GROUND, 0, B_STONE, 0);
    world_set(&t.w, 0, GROUND + 1, 1, B_STONE, 0);
    world_set(&t.w, 0, GROUND + 1, -1, B_STONE, 0);
    world_set(&t.w, 0, GROUND + 1, 0, B_WATER, 0);
    fluid_forget(&t.ph.fl);
    fluid_wake(&t.ph.fl, 0, GROUND + 1, 0);
    fluid_tick(&t.ph.fl, 0);
    CHECK(world_get(&t.w, 0, GROUND + 1, 0) == B_AIR);
    CHECK(world_fluid_level(&t.w, 1, GROUND + 1, 0) == FLUID_FULL / 2);
    CHECK(world_fluid_level(&t.w, -1, GROUND + 1, 0) == FLUID_FULL / 2);
    tw_free(&t);
}

/* A drop can be liquid with room: a film on a step drains into the
 * half-full pool below it. */
static void test_fluid_drop_into_pool(void)
{
    test_world t = {0};
    tw_init(&t);
    basin(&t.w, 2, 1);
    world_set(&t.w, 0, GROUND, 0, B_WATER, 3);
    world_set(&t.w, 1, GROUND, 0, B_WATER, 3);
    world_set(&t.w, 0, GROUND, 1, B_WATER, 3);
    world_set(&t.w, 1, GROUND, 1, B_WATER, 3);
    world_set(&t.w, -2, GROUND + 1, 0, B_STONE, 0);
    world_set(&t.w, -1, GROUND + 1, 0, B_WATER, 1); /* on the basin wall */
    settle(&t, 50);
    CHECK(world_get(&t.w, -1, GROUND + 1, 0) == B_AIR);
    CHECK(volume(&t.w, B_WATER, 1, GROUND, GROUND + 1) == 13);
    CHECK(volume(&t.w, B_WATER, 3, GROUND + 1, GROUND + 2) == 0);
    tw_free(&t);
}

/* Random puddles on flat ground come to rest: the queue empties, no cell
 * stands more than a level above a neighbour, and nothing is lost. */
static void test_fluid_puddles_rest(void)
{
    test_world t = {0};
    tw_init(&t);
    uint32_t s = 7;
    long before = 0;
    for (int i = 0; i < 40; i++) {
        int x = (int)(hash_u32(s++) % 12), z = (int)(hash_u32(s++) % 12);
        int l = 1 + (int)(hash_u32(s++) % FLUID_FULL);
        before += l - world_fluid_level(&t.w, x, GROUND, z);
        fluid_set(&t.w, x, GROUND, z, B_WATER, l);
    }
    CHECK(settle(&t, 500) < 500);
    CHECK(volume(&t.w, B_WATER, 30, GROUND, GROUND + 1) == before);
    int steep = 0;
    for (int z = -20; z < 30; z++)
        for (int x = -20; x < 30; x++) {
            int l = world_fluid_level(&t.w, x, GROUND, z);
            steep += abs(l - world_fluid_level(&t.w, x + 1, GROUND, z)) > 1;
            steep += abs(l - world_fluid_level(&t.w, x, GROUND, z + 1)) > 1;
        }
    CHECK(steep == 0);
    tw_free(&t);
}

/* A reservoir on a platform with one side open empties over the edge:
 * every level ends on the ground. */
static void test_fluid_waterfall(void)
{
    test_world t = {0};
    tw_init(&t);
    const int h = 4;
    for (int z = -1; z <= 3; z++)
        for (int x = -1; x < 3; x++) {
            for (int y = GROUND; y < GROUND + h; y++) world_set(&t.w, x, y, z, B_STONE, 0);
            int wall = x == -1 || z == -1 || z == 3;
            world_set(&t.w, x, GROUND + h, z, wall ? B_STONE : B_WATER, 0);
        }
    long before = volume(&t.w, B_WATER, 4, GROUND + h, GROUND + h + 1);
    CHECK(before == 9 * FLUID_FULL);
    CHECK(settle(&t, 1000) < 1000);
    CHECK(volume(&t.w, B_WATER, 4, GROUND + h, GROUND + h + 1) == 0);
    CHECK(volume(&t.w, B_WATER, 30, GROUND, GROUND + 1) == before);
    tw_free(&t);
}

/* A film a few cells from a ledge walks to it; one further away stays. */
static void test_fluid_seek(void)
{
    test_world t = {0};
    tw_init(&t);
    world_set(&t.w, 3, GROUND - 1, 0, B_AIR, 0); /* a hole three cells east */
    world_set(&t.w, 0, GROUND, 0, B_WATER, 1);
    world_set(&t.w, -10, GROUND, 10, B_WATER, 1); /* far from any ledge */
    settle(&t, 50);
    CHECK(world_get(&t.w, 0, GROUND, 0) == B_AIR);
    CHECK(world_fluid_level(&t.w, 3, GROUND - 1, 0) == 1);
    CHECK(world_fluid_level(&t.w, -10, GROUND, 10) == 1);
    tw_free(&t);
}

/* Between two ledges a film walks to the nearer, whichever way the tick
 * turns the order it looks in. */
static void test_fluid_seek_nearest(void)
{
    for (uint32_t order = 0; order < 4; order++) {
        test_world t = {0};
        tw_init(&t);
        world_set(&t.w, 2, GROUND - 1, 0, B_AIR, 0);  /* two cells east */
        world_set(&t.w, -4, GROUND - 1, 0, B_AIR, 0); /* four cells west */
        world_set(&t.w, 0, GROUND, 0, B_WATER, 1);
        for (uint32_t i = 0; i < 20; i++) fluid_tick(&t.ph.fl, order + i);
        CHECK(world_fluid_level(&t.w, 2, GROUND - 1, 0) == 1);
        CHECK(world_get(&t.w, -4, GROUND - 1, 0) == B_AIR);
        CHECK(fluid_pending(&t.ph.fl) == 0);
        tw_free(&t);
    }
}

/* Only a cell with room wakes the liquid diagonally above it. */
static void test_fluid_wake_diagonal(void)
{
    test_world t = {0};
    tw_init(&t);
    world_set(&t.w, 0, GROUND, 0, B_WATER, 1);
    fluid_forget(&t.ph.fl);
    fluid_wake(&t.ph.fl, 1, GROUND - 1, 0); /* the ground: no room */
    CHECK(fluid_pending(&t.ph.fl) == 0);
    world_set(&t.w, 1, GROUND - 1, 0, B_AIR, 0);
    CHECK(fluid_pending(&t.ph.fl) == 1);
    fluid_tick(&t.ph.fl, 0);
    CHECK(world_fluid_level(&t.w, 1, GROUND, 0) == 1);
    tw_free(&t);
}

/* The --bench dam break in small: a walled reservoir loses a wall and
 * floods the plain. It settles, loses nothing and leaves no step. */
static void test_fluid_dam_break(void)
{
    test_world t = {0};
    tw_init(&t);
    const int n = 6, depth = 3;
    basin(&t.w, n, depth);
    for (int y = GROUND; y < GROUND + depth; y++)
        for (int z = 0; z < n; z++)
            for (int x = 0; x < n; x++) world_set(&t.w, x, y, z, B_WATER, 0);
    settle(&t, 100);
    long before = volume(&t.w, B_WATER, n, GROUND, GROUND + depth);
    CHECK(before == n * n * depth * FLUID_FULL);
    for (int y = GROUND; y < GROUND + depth; y++)
        for (int z = 0; z < n; z++) world_set(&t.w, n, y, z, B_AIR, 0);
    CHECK(settle(&t, 3000) < 3000);
    CHECK(volume(&t.w, B_WATER, 40, GROUND, GROUND + depth) == before);
    CHECK(volume(&t.w, B_WATER, 40, GROUND + 1, GROUND + depth) < before / 2);
    tw_free(&t);
}

/* Water flows on across a column border. */
static void test_fluid_border(void)
{
    test_world t = {0};
    tw_init(&t);
    for (int y = GROUND; y < GROUND + 3; y++) world_set(&t.w, CHUNK_W - 1, y, 5, B_WATER, 0);
    settle(&t, 500);
    CHECK(world_fluid_level(&t.w, CHUNK_W, GROUND, 5) > 0);
    tw_free(&t);
}

static void test_fluid_displace(void)
{
    test_world t = {0};
    tw_init(&t);
    /* Into air above. */
    fluid_displace(&t.w, 0, GROUND, 0, B_WATER, 5, 0);
    CHECK(world_fluid_level(&t.w, 0, GROUND + 1, 0) == 5);
    /* Onto the same liquid, capped at a full cell. */
    fluid_displace(&t.w, 0, GROUND, 0, B_WATER, 6, 1);
    CHECK(world_fluid_level(&t.w, 0, GROUND + 1, 0) == FLUID_FULL);
    /* Not into a solid, and nothing for no liquid or a solid "liquid". */
    world_set(&t.w, 2, GROUND + 1, 0, B_STONE, 0);
    fluid_displace(&t.w, 2, GROUND, 0, B_WATER, 4, 0);
    CHECK(world_get(&t.w, 2, GROUND + 1, 0) == B_STONE);
    fluid_displace(&t.w, 4, GROUND, 0, B_WATER, 0, 0);
    fluid_displace(&t.w, 4, GROUND, 0, B_STONE, 4, 0);
    CHECK(world_get(&t.w, 4, GROUND + 1, 0) == B_AIR);
    /* Not past the top of the world. */
    fluid_displace(&t.w, 6, WORLD_H - 1, 0, B_WATER, 4, 0);
    CHECK(world_get(&t.w, 6, WORLD_H - 1, 0) == B_AIR);
    tw_free(&t);
}

/* A block that lands in water pushes it up instead of deleting it. */
static void test_fluid_body_lands(void)
{
    test_world t = {0};
    tw_init(&t);
    basin(&t.w, 1, 4);
    world_set(&t.w, 0, GROUND, 0, B_WATER, 0);
    fluid_forget(&t.ph.fl);
    world_set(&t.w, 0, GROUND + 6, 0, B_STONE, 0); /* unsupported: falls */
    physics_check_structure(&t.ph, 0, GROUND + 6, 0);
    player pl = {0};
    pl.pos = dv3(20.5, GROUND, 20.5);
    player_input in = {0};
    run_steps(&t, &pl, &in, 240);
    CHECK(world_get(&t.w, 0, GROUND, 0) == B_STONE);
    CHECK(world_water_level(&t.w, 0, GROUND + 1, 0) == FLUID_FULL);
    tw_free(&t);
}

/* Liquid is no target: the player reaches through it to the bed, and a
 * stone dropped into a pool sinks to the bottom rather than resting on
 * the surface. */
static void test_fluid_not_solid(void)
{
    test_world t = {0};
    tw_init(&t);
    basin(&t.w, 3, 4);
    for (int y = GROUND; y < GROUND + 3; y++)
        for (int z = 0; z < 3; z++)
            for (int x = 0; x < 3; x++) world_set(&t.w, x, y, z, B_WATER, 0);
    fluid_forget(&t.ph.fl);
    ray_hit r = physics_raycast(&t.w, dv3(1.5, GROUND + 3.5, 1.5), v3(0.0f, -1.0f, 0.0f), 8.0);
    CHECK(r.hit && r.id == B_GRASS && r.block.y == GROUND - 1 && r.before.y == GROUND);

    world_set(&t.w, 1, GROUND + 6, 1, B_STONE, 0);
    physics_check_structure(&t.ph, 1, GROUND + 6, 1);
    player pl = {0};
    pl.pos = dv3(20.5, GROUND, 20.5);
    player_input in = {0};
    run_steps(&t, &pl, &in, 300);
    CHECK(world_get(&t.w, 1, GROUND, 1) == B_STONE);
    CHECK(world_get(&t.w, 1, GROUND + 3, 1) != B_STONE);
    tw_free(&t);
}

void test_fluid_all(void)
{
    test_fluid_blocks();
    test_fluid_set();
    test_fluid_wake();
    test_fluid_fall();
    test_fluid_pour_down();
    test_fluid_level_pool();
    test_fluid_conserves();
    test_fluid_sliced();
    test_fluid_budget();
    test_fluid_spread_even();
    test_fluid_ledge();
    test_fluid_wake_drop();
    test_fluid_drop_split();
    test_fluid_drop_into_pool();
    test_fluid_puddles_rest();
    test_fluid_waterfall();
    test_fluid_seek();
    test_fluid_seek_nearest();
    test_fluid_wake_diagonal();
    test_fluid_dam_break();
    test_fluid_border();
    test_fluid_displace();
    test_fluid_body_lands();
    test_fluid_not_solid();
}
