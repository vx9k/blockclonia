/* Liquids besides water: their blocks and buckets, layering by density,
 * slow flow, lava setting where it meets water, buoyancy in each, and
 * drowning in any of them. */
#include "fluid.h"
#include "fx.h"
#include "interact.h"
#include "mem.h"
#include "survival.h"
#include "test_util.h"

#include <math.h>

static int settle(test_world *t, int max_ticks)
{
    int ticks = 0;
    while (fluid_pending(&t->ph.fl) && ticks < max_ticks) fluid_tick(&t->ph.fl, (uint32_t)ticks++);
    return ticks;
}

/* A one-cell shaft of stone from the ground up `h` cells, at x, z. */
static void shaft(world *w, int x, int z, int h)
{
    for (int y = GROUND; y < GROUND + h; y++) {
        world_set(w, x + 1, y, z, B_STONE, 0);
        world_set(w, x - 1, y, z, B_STONE, 0);
        world_set(w, x, y, z + 1, B_STONE, 0);
        world_set(w, x, y, z - 1, B_STONE, 0);
    }
}

static void test_liquid_defs(void)
{
    static const uint8_t LIQ[] = {B_WATER, B_LAVA, B_CRUDE_OIL, B_BRINE};
    for (int i = 0; i < 4; i++) {
        CHECK(block_fluid(LIQ[i]) && block_has_meta(LIQ[i]));
        CHECK(!item_valid(LIQ[i])); /* liquid is carried in buckets, never as a block */
    }
    CHECK(block_get(B_CRUDE_OIL)->density < block_get(B_WATER)->density);
    CHECK(block_get(B_WATER)->density < block_get(B_BRINE)->density);
    CHECK(block_get(B_BRINE)->density < block_get(B_LAVA)->density);
    CHECK(item_bucket(B_WATER) == I_WATER_BUCKET && item_bucket(B_CRUDE_OIL) == I_OIL_BUCKET);
    CHECK(item_bucket(B_BRINE) == I_BRINE_BUCKET && item_bucket(B_LAVA) == I_NONE && item_bucket(B_STONE) == I_NONE);
    CHECK(item_valid(I_OIL_BUCKET) && item_valid(I_BRINE_BUCKET) && !item_valid(I_LIQUIDS_END));
    CHECK(item_get(I_OIL_BUCKET)->block == B_CRUDE_OIL && item_get(I_OIL_BUCKET)->tex == T_ITEM_OIL_BUCKET);
    CHECK(item_get(I_BRINE_BUCKET)->block == B_BRINE && item_get(I_BRINE_BUCKET)->tex == T_ITEM_BRINE_BUCKET);
    CHECK(item_get(I_OIL_BUCKET)->stack == 1 && item_get(I_BRINE_BUCKET)->stack == 1);
}

/* Oil poured under water comes up through it; brine poured over water
 * sinks under it. Each keeps its volume. */
static void test_liquid_layers(void)
{
    test_world t = {0};
    tw_init(&t);
    shaft(&t.w, 0, 0, 4);
    world_set(&t.w, 0, GROUND, 0, B_CRUDE_OIL, 0);
    world_set(&t.w, 0, GROUND + 1, 0, B_WATER, 0);
    world_set(&t.w, 0, GROUND + 2, 0, B_WATER, 0);
    CHECK(settle(&t, 100) < 100);
    CHECK(world_get(&t.w, 0, GROUND, 0) == B_WATER && world_get(&t.w, 0, GROUND + 1, 0) == B_WATER);
    CHECK(world_get(&t.w, 0, GROUND + 2, 0) == B_CRUDE_OIL);
    CHECK(world_fluid_level(&t.w, 0, GROUND + 2, 0) == FLUID_FULL);

    shaft(&t.w, 5, 0, 4);
    world_set(&t.w, 5, GROUND, 0, B_WATER, 0);
    world_set(&t.w, 5, GROUND + 1, 0, B_WATER, 0);
    world_set(&t.w, 5, GROUND + 2, 0, B_BRINE, 0);
    CHECK(settle(&t, 100) < 100);
    CHECK(world_get(&t.w, 5, GROUND, 0) == B_BRINE && world_get(&t.w, 5, GROUND + 1, 0) == B_WATER);
    CHECK(world_get(&t.w, 5, GROUND + 2, 0) == B_WATER);
    /* Water over oil over brine: all three sort themselves. */
    shaft(&t.w, 10, 0, 4);
    world_set(&t.w, 10, GROUND, 0, B_CRUDE_OIL, 0);
    world_set(&t.w, 10, GROUND + 1, 0, B_WATER, 0);
    world_set(&t.w, 10, GROUND + 2, 0, B_BRINE, 0);
    CHECK(settle(&t, 100) < 100);
    CHECK(world_get(&t.w, 10, GROUND, 0) == B_BRINE && world_get(&t.w, 10, GROUND + 1, 0) == B_WATER);
    CHECK(world_get(&t.w, 10, GROUND + 2, 0) == B_CRUDE_OIL);
    tw_free(&t);
}

/* Lava waits six ticks between moves, oil two, water none. */
static void test_liquid_viscous(void)
{
    test_world t = {0};
    tw_init(&t);
    world_set(&t.w, 0, GROUND, 0, B_LAVA, 0);
    world_set(&t.w, 20, GROUND, 0, B_CRUDE_OIL, 0);
    world_set(&t.w, 40, GROUND, 0, B_WATER, 0);
    fluid_tick(&t.ph.fl, 1);
    CHECK(world_fluid_level(&t.w, 0, GROUND, 0) == FLUID_FULL && world_get(&t.w, 1, GROUND, 0) == B_AIR);
    CHECK(world_fluid_level(&t.w, 20, GROUND, 0) == FLUID_FULL && world_get(&t.w, 21, GROUND, 0) == B_AIR);
    CHECK(world_fluid_level(&t.w, 40, GROUND, 0) < FLUID_FULL && world_get(&t.w, 41, GROUND, 0) == B_WATER);
    CHECK(fluid_pending(&t.ph.fl)); /* the slow cells wait their turn */
    fluid_tick(&t.ph.fl, 2);
    CHECK(world_get(&t.w, 21, GROUND, 0) == B_CRUDE_OIL);
    CHECK(world_fluid_level(&t.w, 0, GROUND, 0) == FLUID_FULL);
    for (uint32_t i = 3; i <= 6; i++) fluid_tick(&t.ph.fl, i);
    CHECK(world_get(&t.w, 1, GROUND, 0) == B_LAVA && world_fluid_level(&t.w, 0, GROUND, 0) < FLUID_FULL);
    /* Slow or not, everything comes to rest with its volume. */
    CHECK(settle(&t, 3000) < 3000);
    long lava = 0;
    for (int z = -8; z <= 8; z++)
        for (int x = -8; x <= 8; x++)
            if (world_get(&t.w, x, GROUND, z) == B_LAVA) lava += world_fluid_level(&t.w, x, GROUND, z);
    CHECK(lava == FLUID_FULL);
    tw_free(&t);
}

/* Lava meeting water or brine sets: a full cell to obsidian, a thin one to
 * basalt, boiling off as many levels of the water as it had. */
static void test_liquid_quench(void)
{
    test_world t = {0};
    tw_init(&t);
    shaft(&t.w, 0, 0, 4);
    world_set(&t.w, 0, GROUND, 0, B_WATER, 0);
    world_set(&t.w, 0, GROUND + 1, 0, B_WATER, 0);
    world_set(&t.w, 0, GROUND + 2, 0, B_LAVA, 0); /* lava poured onto water */
    settle(&t, 100);
    CHECK(world_get(&t.w, 0, GROUND + 2, 0) == B_OBSIDIAN_ROCK);
    CHECK(world_get(&t.w, 0, GROUND + 1, 0) == B_AIR); /* a full cell boiled away */
    CHECK(world_get(&t.w, 0, GROUND, 0) == B_WATER && world_fluid_level(&t.w, 0, GROUND, 0) == FLUID_FULL);

    shaft(&t.w, 5, 0, 3);
    world_set(&t.w, 5, GROUND, 0, B_BRINE, 0);
    world_set(&t.w, 5, GROUND + 1, 0, B_LAVA, 3);
    settle(&t, 100);
    CHECK(world_get(&t.w, 5, GROUND + 1, 0) == B_BASALT_ROCK);
    CHECK(world_get(&t.w, 5, GROUND, 0) == B_BRINE && world_fluid_level(&t.w, 5, GROUND, 0) == FLUID_FULL - 3);
    /* Oil doesn't quench lava: the lava sinks through it. */
    shaft(&t.w, 10, 0, 3);
    world_set(&t.w, 10, GROUND, 0, B_CRUDE_OIL, 0);
    world_set(&t.w, 10, GROUND + 1, 0, B_LAVA, 0);
    settle(&t, 100);
    CHECK(world_get(&t.w, 10, GROUND, 0) == B_LAVA && world_get(&t.w, 10, GROUND + 1, 0) == B_CRUDE_OIL);
    tw_free(&t);
}

/* Where an item comes to rest when dropped onto a pool of
 * liquid 3 m deep. */
static double rest_height(uint8_t liquid, item_id id)
{
    test_world t = {0};
    tw_init(&t);
    for (int x = 20; x < 23; x++)
        for (int z = 20; z < 23; z++)
            for (int y = GROUND - 3; y < GROUND; y++) world_set(&t.w, x, y, z, liquid, 0);
    player p = {0};
    player_spawn(&p, &t.w, 0.5, 0.5);
    player_input idle = {0};
    physics_drop_item(&t.ph, dv3(21.5, GROUND + 0.5, 21.5), dv3(0, 0, 0), id, 1, 0.0f);
    run_steps(&t, &p, &idle, 1200); /* 20 s: an item a few percent denser sinks at ~0.3 m/s */
    double y = t.ph.item_count == 1 ? t.ph.items[0].pos.y : 1e9;
    tw_free(&t);
    return y;
}

static void test_liquid_buoyancy(void)
{
    /* A bucket of water (1080 kg/m^3) sinks in water and floats on brine;
     * one of oil (950) floats on water and sinks in oil. */
    CHECK(rest_height(B_WATER, I_WATER_BUCKET) < GROUND - 2.5);
    CHECK(rest_height(B_BRINE, I_WATER_BUCKET) > GROUND - 0.5);
    CHECK(rest_height(B_WATER, I_OIL_BUCKET) > GROUND - 0.5);
    CHECK(rest_height(B_CRUDE_OIL, I_OIL_BUCKET) < GROUND - 2.5);
}

/* Any liquid over the face drowns; the airway is only air on dry land. */
static void test_liquid_airway(void)
{
    static const uint8_t LIQ[] = {B_CRUDE_OIL, B_BRINE, B_LAVA};
    for (int i = 0; i < 3; i++) {
        test_world t = {0};
        tw_init(&t);
        player p = {0};
        player_spawn(&p, &t.w, 4.5, 4.5);
        survival_before b = survival_capture(&p);
        health_env e = {0};
        survival_env(&t.w, &p, &b, 0, &e);
        CHECK(e.airway == AIRWAY_AIR);
        for (int y = GROUND; y < GROUND + 3; y++) world_set(&t.w, 4, y, 4, LIQ[i], 0);
        survival_env(&t.w, &p, &b, 0, &e);
        CHECK(e.airway == AIRWAY_WATER);
        tw_free(&t);
    }
}

/* A bucket scoops oil and brine as it does water, and pours them back; it
 * refuses lava. */
static void test_liquid_buckets(void)
{
    test_world t = {0};
    tw_init(&t);
    fx_state *fx = mem_alloc(sizeof *fx);
    fx_init(fx, 5);
    player p = {0};
    player_spawn(&p, &t.w, 7.5, 0.5);
    t.ph.pls[0] = &p;
    t.ph.pl_count = 1;
    inventory inv = {0};
    interact s = {0};
    interact_init(&s, 3);
    interact_input use = {.use_click = 1, .can_act = 1, .speed = 1.0f, .dt = 1.0f / 60.0f};
    static const uint8_t LIQ[] = {B_CRUDE_OIL, B_BRINE, B_LAVA};
    for (int i = 0; i < 3; i++) {
        for (int x = 6; x <= 8; x++)
            for (int z = -4; z <= -2; z++) world_set(&t.w, x, GROUND - 1, z, LIQ[i], 0);
        physics_fluid_tick(&t.ph);
        inv_init(&inv);
        inv_add(&inv, I_BUCKET, 1);
        p.pos = dv3(7.5, GROUND, 0.5);
        dvec3 eye = dv3(p.pos.x, p.pos.y + PLAYER_EYE, p.pos.z);
        p.pitch = -0.55f;
        vec3 dir = look_dir(p.yaw, p.pitch);
        ray_hit hit = physics_raycast(&t.w, eye, dir, 5.0);
        interact_out o = interact_frame(&s, &t.w, &t.ph, NULL, &p, &inv, fx, eye, dir, hit, &use);
        int level = 0;
        for (int x = 6; x <= 8; x++)
            for (int z = -4; z <= -2; z++) level += world_fluid_level(&t.w, x, GROUND - 1, z);
        if (LIQ[i] == B_LAVA) {
            CHECK(inv_held(&inv)->id == I_BUCKET && level == 9 * FLUID_FULL && o.msg != NULL);
            continue;
        }
        CHECK(inv_held(&inv)->id == item_bucket(LIQ[i]) && level == 8 * FLUID_FULL);
        p.pitch = -1.0f; /* the dry grass in front of the pool */
        dir = look_dir(p.yaw, p.pitch);
        hit = physics_raycast(&t.w, eye, dir, 5.0);
        interact_frame(&s, &t.w, &t.ph, NULL, &p, &inv, fx, eye, dir, hit, &use);
        CHECK(inv_held(&inv)->id == I_BUCKET && hit.hit &&
              world_get(&t.w, hit.before.x, hit.before.y, hit.before.z) == LIQ[i]);
        for (int x = 6; x <= 8; x++)
            for (int z = -4; z <= -2; z++) world_set(&t.w, x, GROUND - 1, z, B_STONE, 0);
        world_set(&t.w, hit.before.x, hit.before.y, hit.before.z, B_AIR, 0);
    }
    /* The bucket's search finds oil as it finds water. */
    world_set(&t.w, 3, GROUND, 3, B_CRUDE_OIL, 0);
    ipos c = {0};
    CHECK(survival_find_liquid(&t.w, dv3(0.5, GROUND + 0.5, 3.5), (vec3){1.0f, 0.0f, 0.0f}, 5.0, &c) && c.x == 3);
    mem_free(fx);
    tw_free(&t);
}

void test_liquids_all(void)
{
    test_liquid_defs();
    test_liquid_layers();
    test_liquid_viscous();
    test_liquid_quench();
    test_liquid_buoyancy();
    test_liquid_airway();
    test_liquid_buckets();
}
