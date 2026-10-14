/* Creatures and fighting: mobs that stand, wander and hunt, blows and
 * bullets, what they do to mobs and players, and eating what a boar leaves. */
#include "combat.h"
#include "mem.h"
#include "mob.h"
#include "test_util.h"

#include <math.h>
#include <string.h>

/* A step of the creatures with spawning held off, so only the mobs a test
 * placed are there. */
static int step(test_world *t, mobs *ms, combat *c, player *pl, health *h, float daylight)
{
    ms->spawn_t = 1e9f;
    player *tp[1] = {pl};
    health *th[1] = {h};
    combat_targets tg = {tp, th, 1};
    return combat_creatures(c, ms, &t->ph, &tg, daylight);
}

static void test_mobs(void)
{
    test_world t = {0};
    tw_init(&t);
    mobs ms = {0};
    mobs_init(&ms, 7);
    combat c = {0};
    combat_init(&c, 7);
    health *h = new_body(3);
    player pl = {0};
    player_spawn(&pl, &t.w, 8.5, 8.5);

    /* A boar dropped in the air lands, stands on the grass and wanders. */
    int b = mobs_add(&ms, MOB_BOAR, dv3(20.5, GROUND + 3.0, 8.5));
    CHECK(b == 0 && ms.count == 1 && ms.m[0].body.height < 1.0f);
    dvec3 start = dv3(20.5, GROUND, 8.5);
    double moved = 0.0;
    for (int i = 0; i < 60 * 20; i++) {
        step(&t, &ms, &c, &pl, h, 1.0f);
        dvec3 p = ms.m[0].body.pos;
        moved = fmax(moved, sqrt((p.x - start.x) * (p.x - start.x) + (p.z - start.z) * (p.z - start.z)));
    }
    CHECK(ms.count == 1 && fabs(ms.m[0].body.pos.y - GROUND) < 0.01 && ms.m[0].body.on_ground);
    CHECK(moved > 1.0);
    CHECK(h->wound_count == 0); /* boars do not bite */

    /* A wolf by day leaves the player alone; at night it comes and bites. */
    ms.count = 0;
    mobs_add(&ms, MOB_WOLF, dv3(16.5, GROUND, 8.5));
    int bites = 0;
    for (int i = 0; i < 60 * 5; i++) bites += step(&t, &ms, &c, &pl, h, 1.0f);
    CHECK(bites == 0 && h->wound_count == 0);
    for (int i = 0; i < 60 * 10 && !h->wound_count; i++) step(&t, &ms, &c, &pl, h, 0.2f);
    CHECK(h->wound_count > 0);
    if (h->wound_count) {
        int part = h->wounds[0].part;
        CHECK(part == BP_LLEG || part == BP_RLEG || part == BP_LARM || part == BP_RARM);
        CHECK(h->wounds[0].contamination > 0.5f); /* a dirty bite */
    }

    /* The ray finds the wolf's box, not the empty air beside it. */
    const mob *w = &ms.m[0];
    dvec3 o = dv3(w->body.pos.x - 3.0, w->body.pos.y + 0.5, w->body.pos.z);
    double d = 0.0;
    CHECK(mobs_raycast(&ms, o, v3(1.0f, 0.0f, 0.0f), 10.0, &d) == 0 && d > 2.0 && d < 3.0);
    CHECK(mobs_raycast(&ms, o, v3(0.0f, 0.0f, 1.0f), 10.0, &d) == -1);

    /* Far from every player, mobs go. */
    pl.pos = dv3(500.0, GROUND, 500.0);
    step(&t, &ms, &c, &pl, h, 1.0f);
    CHECK(ms.count == 0);
    mem_free(h);
    tw_free(&t);
}

static void test_blows(void)
{
    test_world t = {0};
    tw_init(&t);
    mobs ms = {0};
    mobs_init(&ms, 9);
    combat c = {0};
    combat_init(&c, 9);
    health *h = new_body(4);
    player pl = {0};
    player_spawn(&pl, &t.w, 8.5, 8.5);
    player *tp[1] = {&pl};
    health *th[1] = {h};
    combat_targets tg = {tp, th, 1};
    inventory inv = {0};
    inv_init(&inv);
    dvec3 eye = dv3(8.5, GROUND + PLAYER_EYE, 8.5);
    vec3 east = v3(1.0f, -0.35f, 0.0f);
    float l = sqrtf(east.x * east.x + east.y * east.y);
    east = v3(east.x / l, east.y / l, 0.0f);

    /* A boar 2 m away: in reach and nearer than the ground behind it. */
    mobs_add(&ms, MOB_BOAR, dv3(10.5, GROUND, 8.5));
    CHECK(combat_aim(&ms, &tg, 0, eye, east, 5.0));
    CHECK(!combat_aim(&ms, &tg, 0, eye, east, 1.0)); /* a block in front shields it */
    combat_out o = combat_frame(&c, &ms, &t.ph, &tg, 0, &inv, eye, east, 5.0, 1, 0, 1.0f / 60.0f);
    CHECK(o.struck && ms.count == 1 && ms.m[0].hp < 20.0f && ms.m[0].flee_t > 0.0f); /* a fist hurts a little */
    o = combat_frame(&c, &ms, &t.ph, &tg, 0, &inv, eye, east, 5.0, 1, 0, 1.0f / 60.0f);
    CHECK(!o.struck); /* too soon after the last blow */

    /* An iron sword kills it in a few blows and wears; it leaves meat. */
    inv_add(&inv, item_tool_id(TOOL_SWORD, TIER_IRON), 1);
    CHECK(combat_blow(inv.slot[0].id) > 8.0f && combat_blow(0) == 1.0f);
    int blows = 0;
    for (int i = 0; i < 600 && ms.count; i++) {
        ms.m[0].body.pos = dv3(10.5, GROUND, 8.5); /* held still: knockback and fleeing are not the test */
        o = combat_frame(&c, &ms, &t.ph, &tg, 0, &inv, eye, east, 5.0, 1, 0, 1.0f / 60.0f);
        blows += o.struck;
    }
    CHECK(ms.count == 0 && blows >= 2 && blows <= 3 && o.killed);
    CHECK(inv.slot[0].wear == blows);
    int meat = 0;
    for (int i = 0; i < t.ph.item_count; i++) meat += t.ph.items[i].id == I_RAW_MEAT ? t.ph.items[i].count : 0;
    CHECK(meat >= 2);

    /* Meat is food; cooked, more so. */
    inv_add(&inv, I_COOKED_MEAT, 1);
    float before = h->stomach_kcal;
    char msg[64] = {0};
    CHECK(health_treat(h, &inv, BP_ABDOMEN, TREAT_EAT, 0, msg, sizeof msg) && h->stomach_kcal - before > 340.0f);
    CHECK(!inv_count(&inv, I_COOKED_MEAT));
    mem_free(h);
    tw_free(&t);
}

static void test_rifle(void)
{
    test_world t = {0};
    tw_init(&t);
    mobs ms = {0};
    mobs_init(&ms, 11);
    combat c = {0};
    combat_init(&c, 11);
    health *h = new_body(5), *other = new_body(6);
    player pl = {0}, them = {0};
    player_spawn(&pl, &t.w, 8.5, 8.5);
    player_spawn(&them, &t.w, 30.5, 8.5);
    player *tp[2] = {&pl, &them};
    health *th[2] = {h, other};
    combat_targets tg = {tp, th, 2};
    inventory inv = {0};
    inv_init(&inv);
    inv_add(&inv, I_RIFLE, 1);
    dvec3 eye = dv3(8.5, GROUND + 1.2, 8.5);
    vec3 east = v3(1.0f, 0.0f, 0.0f);

    combat_out o = combat_frame(&c, &ms, &t.ph, &tg, 0, &inv, eye, east, 5.0, 0, 1, 1.0f / 60.0f);
    CHECK(!o.fired && o.msg && c.count == 0); /* no cartridges */
    inv_add(&inv, I_CARTRIDGE, 2);
    o = combat_frame(&c, &ms, &t.ph, &tg, 0, &inv, eye, east, 5.0, 0, 1, 1.0f / 60.0f);
    CHECK(o.fired && c.count == 1 && inv_count(&inv, I_CARTRIDGE) == 1 && inv.slot[0].wear == 1);

    /* The bullet crosses 22 m in a few steps and wounds the player there,
     * not the one who fired. */
    int hits = 0;
    for (int i = 0; i < 30 && c.count; i++) hits += combat_step(&c, &ms, &t.ph, &tg);
    CHECK(hits == 1 && c.count == 0 && other->wound_count == 1 && h->wound_count == 0);

    /* A wall stops the next one; glass would have shattered. */
    world_set(&t.w, 14, GROUND + 1, 8, B_STONE, 0);
    c.cooldown[0] = 0.0f;
    o = combat_frame(&c, &ms, &t.ph, &tg, 0, &inv, eye, east, 5.0, 0, 1, 1.0f / 60.0f);
    CHECK(o.fired);
    for (int i = 0; i < 30 && c.count; i++) combat_step(&c, &ms, &t.ph, &tg);
    CHECK(c.count == 0 && other->wound_count == 1 && world_get(&t.w, 14, GROUND + 1, 8) == B_STONE);
    world_set(&t.w, 14, GROUND + 1, 8, B_GLASS, 0);
    inv_add(&inv, I_CARTRIDGE, 1);
    c.cooldown[0] = 0.0f;
    combat_frame(&c, &ms, &t.ph, &tg, 0, &inv, eye, east, 5.0, 0, 1, 1.0f / 60.0f);
    for (int i = 0; i < 30 && c.count; i++) combat_step(&c, &ms, &t.ph, &tg);
    CHECK(world_get(&t.w, 14, GROUND + 1, 8) == B_AIR);
    mem_free(h);
    mem_free(other);
    tw_free(&t);
}

void test_combat_all(void)
{
    test_mobs();
    test_blows();
    test_rifle();
}
