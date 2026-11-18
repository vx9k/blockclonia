/* Darkness as something lived in: what the light at the eyes is
 * (lightfx_seen), how dark that feels, creatures that see less of a body
 * in the dark and keep out of firelight, and the inner voice that notices
 * it. */
#include "combat.h"
#include "item.h"
#include "lightfx.h"
#include "mem.h"
#include "mob.h"
#include "test_util.h"
#include "thoughts.h"

#include <math.h>

#define NIGHT_LIGHT 0.2f

static void settle(world *w)
{
    for (int i = 0; i < 200; i++) {
        world_update(w, 0.5, GROUND + 2.0, 0.5);
        jobs_poll(w->jobs, -1);
        world_schedule(w);
    }
}

/* Open sky by day is full light, the same field by night only dim; a
 * sealed pit is black until a torch is carried or set in it, and only the
 * flame counts as fire. */
static void test_seen(void)
{
    test_world t = {0};
    tw_init(&t);
    settle(&t.w);
    float fire = -1.0f;
    CHECK(lightfx_seen(&t.w, 3.5, GROUND + 1.5, 3.5, 1.0f, 0, &fire) == 1.0f && fire == 0.0f);
    float night = lightfx_seen(&t.w, 3.5, GROUND + 1.5, 3.5, NIGHT_LIGHT, 0, &fire);
    CHECK(night > 0.4f && night < 0.9f);
    float moon = lightfx_seen(&t.w, 3.5, GROUND + 1.5, 3.5, 0.04f, 0, &fire);
    CHECK(moon < night && moon > 0.0f);
    /* No daylight at all is clamped, not -inf. */
    CHECK(lightfx_seen(&t.w, 3.5, GROUND + 1.5, 3.5, 0.0f, 0, &fire) >= 0.0f);

    for (int y = GROUND - 3; y < GROUND; y++) world_set(&t.w, 5, y, 5, B_AIR, 0);
    world_set(&t.w, 5, GROUND, 5, B_STONE, 0);
    settle(&t.w);
    CHECK(lightfx_seen(&t.w, 5.5, GROUND - 1.5, 5.5, 1.0f, 0, &fire) == 0.0f && fire == 0.0f);
    /* Carried: a level below the block's own, and firelight. */
    float held = lightfx_seen(&t.w, 5.5, GROUND - 1.5, 5.5, 1.0f, B_TORCH, &fire);
    CHECK(fabsf(held - 13.0f / 15.0f) < 1e-5f && fire > 0.45f);
    /* Foxfire glows but is cold light. */
    float fox = lightfx_seen(&t.w, 5.5, GROUND - 1.5, 5.5, 1.0f, B_FOXFIRE, &fire);
    CHECK(fox > 0.0f && fox < held && fire == 0.0f);
    /* A tool that carries no light changes nothing. */
    CHECK(lightfx_seen(&t.w, 5.5, GROUND - 1.5, 5.5, 1.0f, B_STONE, &fire) == 0.0f);
    /* A torch set in the pit lights the cell and warms it. */
    world_set(&t.w, 5, GROUND - 3, 5, B_TORCH, 0);
    settle(&t.w);
    float set = lightfx_seen(&t.w, 5.5, GROUND - 2.5, 5.5, 1.0f, 0, &fire);
    CHECK(set > 0.8f && fire > 0.45f);
    tw_free(&t);
}

static void test_dark_curve(void)
{
    CHECK(lightfx_dark(0.0f) == 1.0f);
    CHECK(lightfx_dark(1.0f) == 0.0f && lightfx_dark(0.4f) == 0.0f);
    CHECK(lightfx_dark(0.1f) > lightfx_dark(0.2f));
    CHECK(lightfx_dark(-1.0f) == 1.0f && lightfx_dark(2.0f) == 0.0f);
}

/* Each creature sees a body in full light from the whole range and loses
 * part of it in the dark, wolves less than boars. */
static void test_sight(void)
{
    CHECK(mobs_sight(MOB_WOLF, 20.0, 1.0f) == 20.0 && mobs_sight(MOB_BOAR, 20.0, 0.5f) == 20.0);
    double wolf = mobs_sight(MOB_WOLF, 20.0, 0.0f), boar = mobs_sight(MOB_BOAR, 20.0, 0.0f);
    CHECK(wolf < 20.0 && boar < wolf && boar > 0.0);
    CHECK(mobs_sight(MOB_WOLF, 20.0, 0.25f) > wolf && mobs_sight(MOB_WOLF, 20.0, 0.25f) < 20.0);
    CHECK(mobs_sight(-1, 20.0, 0.0f) == 20.0 && mobs_sight(MOB_KINDS, 20.0, 0.0f) == 20.0);
}

/* Steps the creatures with spawning held off; the player is seen as sense
 * says. Returns the bites. */
static int step(test_world *t, mobs *ms, combat *c, player *pl, health *h, const mob_sense *sense)
{
    ms->spawn_t = 1e9f;
    player *tp[1] = {pl};
    health *th[1] = {h};
    combat_targets tg = {tp, th, 1, sense};
    return combat_creatures(c, ms, &t->ph, &tg, NIGHT_LIGHT);
}

static double dist_xz(dvec3 a, dvec3 b)
{
    double dx = a.x - b.x;
    return hypot(dx, a.z - b.z);
}

/* By night a wolf comes from far for a body in the open, but misses one
 * standing in the dark at the same distance. */
static void test_unseen_in_dark(void)
{
    test_world t = {0};
    tw_init(&t);
    mobs ms = {0};
    mobs_init(&ms, 9);
    combat c = {0};
    combat_init(&c, 9);
    health *h = new_body(30);
    player pl = {0};
    player_spawn(&pl, &t.w, 8.5, 8.5);
    mob_sense dark = {0.0f, 0.0f}, lit = {1.0f, 0.0f};

    int w = mobs_add(&ms, MOB_WOLF, dv3(23.5, GROUND, 8.5)); /* 15 m off */
    dvec3 from = ms.m[w].body.pos;
    for (int i = 0; i < 60 * 3; i++) step(&t, &ms, &c, &pl, h, &dark);
    CHECK(dist_xz(ms.m[w].body.pos, pl.pos) > 10.0); /* it never came */
    ms.m[w].body.pos = from;
    for (int i = 0; i < 60 * 3; i++) step(&t, &ms, &c, &pl, h, &lit);
    CHECK(dist_xz(ms.m[w].body.pos, pl.pos) < 8.0); /* it charged */
    mem_free(h);
    tw_free(&t);
}

/* A hunting wolf will not come into firelight: it keeps off and never
 * bites. One that has been hurt comes anyway. */
static void test_fire_keeps_wolves(void)
{
    test_world t = {0};
    tw_init(&t);
    mobs ms = {0};
    mobs_init(&ms, 11);
    combat c = {0};
    combat_init(&c, 11);
    health *h = new_body(31);
    player pl = {0};
    player_spawn(&pl, &t.w, 8.5, 8.5);
    mob_sense torch = {0.9f, 0.8f};

    int w = mobs_add(&ms, MOB_WOLF, dv3(12.5, GROUND, 8.5));
    int bites = 0;
    double closest = 1e9;
    for (int i = 0; i < 60 * 10; i++) {
        bites += step(&t, &ms, &c, &pl, h, &torch);
        closest = fmin(closest, dist_xz(ms.m[w].body.pos, pl.pos));
    }
    CHECK(bites == 0 && h->wound_count == 0);
    CHECK(closest > 3.0);
    /* Hurt, it forgets the fire. */
    ms.m[w].angry_t = 60.0f;
    for (int i = 0; i < 60 * 10 && !bites; i++) bites += step(&t, &ms, &c, &pl, h, &torch);
    CHECK(bites > 0);
    mem_free(h);
    tw_free(&t);
}

/* The eyes take a few seconds to register the dark, and the voice speaks
 * of it by how deep it is, with relief when light comes back. */
static void test_dark_feel(void)
{
    health *h = new_body(32);
    health_env e = calm_env();
    e.dark = 1.0f;
    live(h, &e, 0.5);
    CHECK(h->dark > 0.0f && h->dark < 0.5f); /* still adjusting */
    live(h, &e, 30.0);
    CHECK(h->dark > 0.99f && thoughts_level(h, FEEL_DARK) == 3);
    h->dark = 0.9f;
    CHECK(thoughts_level(h, FEEL_DARK) == 2);
    h->dark = 0.7f;
    CHECK(thoughts_level(h, FEEL_DARK) == 1);
    h->dark = 0.3f;
    CHECK(thoughts_level(h, FEEL_DARK) == 0);
    /* Out of range is clamped. */
    e.dark = 5.0f;
    live(h, &e, 30.0);
    CHECK(h->dark <= 1.0f);

    thoughts th;
    thoughts_init(&th, 3);
    h->dark = 1.0f;
    int said = 0;
    for (int i = 0; i < 30 * 5; i++) said += thoughts_step(&th, h, 1.0f / 30.0f);
    CHECK(said >= 1 && th.now.feeling == FEEL_DARK);
    h->dark = 0.0f;
    int relief = 0;
    for (int i = 0; i < 30 * 30; i++)
        if (thoughts_step(&th, h, 1.0f / 30.0f) && th.now.feeling == FEEL_DARK && th.now.level == 0) relief = 1;
    CHECK(relief);
    mem_free(h);
}

/* The darkness the body feels travels with the rest of it. */
static void test_dark_wire(void)
{
    health *h = new_body(33), *g = new_body(34);
    h->dark = 0.75f;
    uint8_t *buf = mem_alloc(HEALTH_NET_MAX);
    size_t n = health_encode(h, buf, HEALTH_NET_MAX);
    CHECK(n > 0 && health_decode(g, buf, n) == 0 && g->dark == 0.75f);
    mem_free(buf);
    mem_free(h);
    mem_free(g);
}

void test_dark_all(void)
{
    test_seen();
    test_dark_curve();
    test_sight();
    test_unseen_in_dark();
    test_fire_keeps_wolves();
    test_dark_feel();
    test_dark_wire();
}
