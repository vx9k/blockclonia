/* Lights that move (lamps.c): which columns relight when one appears,
 * moves or goes out, what the light jobs are handed, that the flood fill
 * really lights a pit with one, and where the lamps come from: carried
 * and lying light-giving items, muzzle flashes and burning creatures. */
#include "combat.h"
#include "item.h"
#include "lamps.h"
#include "lightfx.h"
#include "mem.h"
#include "mob.h"
#include "test_util.h"

#include <math.h>

static void settle(world *w)
{
    for (int i = 0; i < 200; i++) {
        world_update(w, 0.5, GROUND + 2.0, 0.5);
        jobs_poll(w->jobs, -1);
        world_schedule(w);
    }
}

/* Clears every loaded column's light_dirty flag. */
static void clean(const world *w)
{
    for (int cz = -2; cz <= 2; cz++)
        for (int cx = -2; cx <= 2; cx++) {
            column *c = world_column(w, cx, cz);
            if (c) c->light_dirty = 0;
        }
}

static int dirty(const world *w, int cx, int cz)
{
    const column *c = world_column(w, cx, cz);
    return c && c->light_dirty;
}

/* What changes relights, and only that. */
static void test_lamps_changes(void)
{
    test_world t = {0};
    tw_init(&t);
    settle(&t.w);
    lamps l;
    lamps_init(&l, NULL, NULL);
    uint16_t torch = lightfx_carried(B_TORCH);

    lamps_add(&l, dv3(8.5, GROUND + 1.5, 8.5), torch);
    clean(&t.w);
    CHECK(lamps_end(&l, &t.w) == 1 && dirty(&t.w, 0, 0));
    /* The same lamp next frame, or moved within its cell: nothing. */
    lamps_add(&l, dv3(8.9, GROUND + 1.1, 8.2), torch);
    clean(&t.w);
    CHECK(lamps_end(&l, &t.w) == 0 && !dirty(&t.w, 0, 0));
    /* Into the next cell: where it was and where it is. */
    lamps_add(&l, dv3(9.5, GROUND + 1.5, 8.5), torch);
    clean(&t.w);
    CHECK(lamps_end(&l, &t.w) == 2 && dirty(&t.w, 0, 0));
    /* Far into another column: the ones by the old cell and the new. */
    lamps_add(&l, dv3(-24.5, GROUND + 1.5, 8.5), torch);
    clean(&t.w);
    CHECK(lamps_end(&l, &t.w) == 2 && dirty(&t.w, 0, 0) && dirty(&t.w, -2, 0));
    /* Gone. */
    clean(&t.w);
    CHECK(lamps_end(&l, &t.w) == 1 && dirty(&t.w, -2, 0) && !dirty(&t.w, 1, 0));
    /* No light, no lamp; nor outside the world; nor past the cap. */
    lamps_add(&l, dv3(1.5, GROUND + 1.5, 1.5), 0);
    lamps_add(&l, dv3(1.5, GROUND + 1.5, 1.5), LIGHT_PACK(15, 0, 0, 0)); /* sky light only */
    lamps_add(&l, dv3(1.5, -1.0, 1.5), torch);
    lamps_add(&l, dv3(1.5, WORLD_H + 0.5, 1.5), torch);
    lamps_add(&l, dv3((double)NAN, 10.0, 1.5), torch);
    CHECK(l.n == 0);
    for (int i = 0; i < LAMPS_MAX + 5; i++) lamps_add(&l, dv3(1.5 + i, GROUND + 1.5, 1.5), torch);
    CHECK(l.n == LAMPS_MAX);
    tw_free(&t);
}

static int fake_glow(const void *user, int x0, int z0, int top, struct light_emitter *out, int cap)
{
    (void)x0;
    (void)z0;
    (void)top;
    const int *n = user;
    for (int i = 0; i < *n && i < cap; i++) out[i] = (light_emitter){1, 2, 3, LIGHT_PACK(0, 9, 0, 0)};
    return *n < cap ? *n : cap;
}

/* The light job is handed the chained hook's emitters, then the lamps in
 * its box, in the box's coordinates. */
static void test_lamps_glow(void)
{
    test_world t = {0};
    tw_init(&t);
    int hot = 2;
    lamps l;
    lamps_init(&l, fake_glow, &hot);
    lamps_add(&l, dv3(5.5, 20.5, 6.5), LIGHT_PACK(0, 13, 12, 9));
    lamps_add(&l, dv3(100.5, 20.5, 6.5), LIGHT_PACK(0, 13, 12, 9)); /* outside the box */
    lamps_add(&l, dv3(5.5, 70.5, 6.5), LIGHT_PACK(0, 13, 12, 9));   /* above top */
    lamps_end(&l, &t.w);
    light_emitter out[8] = {{0}};
    int n = lamps_glow(&l, -16, -16, 64, out, 8);
    CHECK(n == 3);
    CHECK(out[0].light == LIGHT_PACK(0, 9, 0, 0) && out[1].x == 1);
    CHECK(out[2].x == 21 && out[2].y == 20 && out[2].z == 22 && out[2].light == LIGHT_PACK(0, 13, 12, 9));
    /* Room for the hook's alone. */
    CHECK(lamps_glow(&l, -16, -16, 64, out, 2) == 2);
    /* Without a hook to chain to. */
    lamps_init(&l, NULL, NULL);
    lamps_add(&l, dv3(5.5, 20.5, 6.5), LIGHT_PACK(0, 13, 12, 9));
    lamps_end(&l, &t.w);
    CHECK(lamps_glow(&l, -16, -16, 64, out, 8) == 1 && out[0].x == 21);
    tw_free(&t);
}

/* A lamp lights a sealed pit through the light jobs, coloured like its
 * source, and the pit goes dark again when it goes. */
static void test_lamps_light_pit(void)
{
    test_world t = {0};
    tw_init(&t);
    lamps l;
    lamps_init(&l, NULL, NULL);
    t.w.glow = lamps_glow;
    t.w.glow_user = &l;
    for (int y = GROUND - 3; y < GROUND; y++) world_set(&t.w, 5, y, 5, B_AIR, 0);
    world_set(&t.w, 5, GROUND, 5, B_STONE, 0);
    settle(&t.w);
    CHECK(world_light(&t.w, 5, GROUND - 2, 5) == 0);
    lamps_add(&l, dv3(5.5, GROUND - 2.5, 5.5), lightfx_carried(B_TORCH));
    lamps_end(&l, &t.w);
    settle(&t.w);
    uint16_t lit = world_light(&t.w, 5, GROUND - 2, 5);
    CHECK(LIGHT_R(lit) >= 12 && LIGHT_R(lit) > LIGHT_B(lit));
    lamps_end(&l, &t.w);
    settle(&t.w);
    CHECK(world_light(&t.w, 5, GROUND - 2, 5) == 0);
    tw_free(&t);
}

static void test_carried(void)
{
    uint16_t em = light_emission(B_TORCH), c = lightfx_carried(B_TORCH);
    CHECK(LIGHT_R(c) == LIGHT_R(em) - 1 && LIGHT_G(c) == LIGHT_G(em) - 1 && LIGHT_SKY(c) == 0);
    CHECK(lightfx_carried(0) == 0 && lightfx_carried(B_STONE) == 0);
    CHECK(lightfx_carried(item_tool_id(TOOL_PICKAXE, TIER_IRON)) == 0);
    CHECK(lightfx_carried(B_LAMP) != 0 && lightfx_carried(B_FOXFIRE) != 0);
}

/* A shot leaves a flash where it went off for a moment, which others see
 * and the one who fired does not need (block.frag lights theirs). */
static void test_flashes(void)
{
    test_world t = {0};
    tw_init(&t);
    mobs ms = {0};
    mobs_init(&ms, 3);
    ms.spawn_t = 1e9f;
    combat c = {0};
    combat_init(&c, 3);
    health *h = new_body(40), *h2 = new_body(41);
    player pl = {0}, them = {0};
    player_spawn(&pl, &t.w, 8.5, 8.5);
    player_spawn(&them, &t.w, 8.5, 30.5);
    player *tp[2] = {&pl, &them};
    health *th[2] = {h, h2};
    combat_targets tg = {tp, th, 2, NULL};
    inventory inv = {0};
    inv_init(&inv);
    inv_add(&inv, I_RIFLE, 1);
    inv_add(&inv, I_CARTRIDGE, 2);
    dvec3 out[COMBAT_FLASHES] = {{0}};
    CHECK(combat_flashes(&c, -1, 0.1, out, COMBAT_FLASHES) == 0);
    dvec3 eye = dv3(8.5, GROUND + 1.6, 8.5);
    combat_out o = combat_frame(&c, &ms, &t.ph, &tg, 1, &inv, eye, v3(1.0f, 0.0f, 0.0f), 5.0, 1, 1.0f, 1.0f / 60.0f);
    CHECK(o.fired);
    CHECK(combat_flashes(&c, -1, 0.1, out, COMBAT_FLASHES) == 1 &&
          dv3_len(dv3_add(out[0], dv3_scale(o.at, -1.0))) < 1e-9);
    CHECK(combat_flashes(&c, 1, 0.1, out, COMBAT_FLASHES) == 0); /* the shooter's own */
    CHECK(combat_flashes(&c, 0, 0.1, out, 0) == 0);
    /* Over in a tenth of a second of simulated time. */
    for (int i = 0; i < 8; i++) combat_creatures(&c, &ms, &t.ph, &tg, 1.0f);
    CHECK(combat_flashes(&c, -1, 0.1, out, COMBAT_FLASHES) == 0);
    mem_free(h);
    mem_free(h2);
    tw_free(&t);
}

/* A creature that walks into a fire catches; it burns on a while after,
 * shows flames, and burns to death if it stays; water puts it out. */
static void test_burning(void)
{
    test_world t = {0};
    tw_init(&t);
    mobs ms = {0};
    mobs_init(&ms, 5);
    ms.spawn_t = 1e9f;
    player pl = {0};
    player_spawn(&pl, &t.w, 8.5, 8.5);
    player *tp[1] = {&pl};
    mob_events ev = {0};
    world_set(&t.w, 20, GROUND, 8, B_CAMPFIRE, 15);
    int b = mobs_add(&ms, MOB_BOAR, dv3(20.5, GROUND, 8.5));
    mobs_step(&ms, &t.ph, tp, NULL, 1, 1.0f, &ev);
    CHECK(ms.count == 1 && ms.m[b].burn_t > MOB_BURN_S - 0.1f);
    float hp = ms.m[b].hp;
    entity_instance e[MOB_FIGURE] = {0};
    const float feet[3] = {0.0f, 0.0f, 0.0f};
    CHECK(mob_figure(&ms.m[b], feet, 0.5f, 0.0f, e, MOB_FIGURE) == MOB_FIGURE);
    CHECK((e[MOB_BODY].tint & ENTITY_GLOWS) && e[MOB_BODY].light == 1.0f && !(e[0].tint & ENTITY_GLOWS));
    /* Out of the fire, it burns on, then goes out. */
    world_set(&t.w, 20, GROUND, 8, B_AIR, 0);
    for (int i = 0; i < 60; i++) mobs_step(&ms, &t.ph, tp, NULL, 1, 1.0f, &ev);
    CHECK(ms.count == 1 && ms.m[b].burn_t > 0.0f && ms.m[b].hp < hp - 2.0f);
    for (int i = 0; i < 60 * 5 && ms.m[b].burn_t > 0.0f; i++) mobs_step(&ms, &t.ph, tp, NULL, 1, 1.0f, &ev);
    CHECK(ms.count == 1 && ms.m[b].burn_t <= 0.0f && ms.m[b].hp > 0.0f);
    CHECK(mob_figure(&ms.m[b], feet, 0.5f, 0.0f, e, MOB_FIGURE) == MOB_BODY);
    /* In water it goes out at once. */
    ms.m[b].burn_t = 3.0f;
    dvec3 at = ms.m[b].body.pos;
    world_set(&t.w, (int)floor(at.x), (int)floor(at.y), (int)floor(at.z), B_WATER, 0);
    mobs_step(&ms, &t.ph, tp, NULL, 1, 1.0f, &ev);
    CHECK(ms.m[b].burn_t == 0.0f);
    world_set(&t.w, (int)floor(at.x), (int)floor(at.y), (int)floor(at.z), B_AIR, 0);
    /* Held in lava's heat it dies, cooked. */
    t.ph.item_count = 0;
    ms.m[b].hp = 1.0f;
    ms.m[b].burn_t = 1.0f;
    for (int i = 0; i < 60 && ms.count; i++) mobs_step(&ms, &t.ph, tp, NULL, 1, 1.0f, &ev);
    CHECK(ms.count == 0);
    int cooked = 0;
    for (int i = 0; i < t.ph.item_count; i++) cooked |= t.ph.items[i].id == I_COOKED_MEAT;
    CHECK(cooked);
    tw_free(&t);
}

void test_lamps_all(void)
{
    test_lamps_changes();
    test_lamps_glow();
    test_lamps_light_pit();
    test_carried();
    test_flashes();
    test_burning();
}
