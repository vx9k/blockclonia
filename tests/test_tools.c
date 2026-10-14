/* Tools: breaking speed and what a block yields by tool, wear and wearing
 * out, wear surviving moves, drops and saves, and the older formats. */
#include "interact.h"
#include "inventory.h"
#include "item.h"
#include "mem.h"
#include "survival.h"
#include "test_util.h"

#include <string.h>

static void test_rules(void)
{
    int wood_pick = item_tool_id(TOOL_PICKAXE, TIER_WOOD), iron_pick = item_tool_id(TOOL_PICKAXE, TIER_IRON);
    int stone_pick = item_tool_id(TOOL_PICKAXE, TIER_STONE), axe = item_tool_id(TOOL_AXE, TIER_STONE);
    int kind = 0, tier = 0;
    CHECK(item_tool(iron_pick, &kind, &tier) && kind == TOOL_PICKAXE && tier == TIER_IRON);
    CHECK(!item_tool(I_APPLE, &kind, &tier) && !item_durability(I_APPLE) && !item_durability(B_STONE));
    CHECK(item_valid(iron_pick) && item_get(iron_pick)->stack == 1 &&
          !strcmp(item_get(iron_pick)->name, "Iron pickaxe"));
    CHECK(item_durability(wood_pick) < item_durability(iron_pick) && item_durability(iron_pick) <= 255);

    /* The right tool is faster, better tiers more so; bare hands on rock
     * are slower than on soil. */
    CHECK(item_dig_speed(0, B_DIRT) == 1.0f && item_dig_speed(0, B_ROCK) < 1.0f);
    CHECK(item_dig_speed(wood_pick, B_ROCK) > 1.0f &&
          item_dig_speed(iron_pick, B_ROCK) > item_dig_speed(wood_pick, B_ROCK));
    CHECK(item_dig_speed(axe, B_LOG) > 1.0f && item_dig_speed(axe, B_ROCK) < 1.0f);
    CHECK(item_dig_speed(item_tool_id(TOOL_SHOVEL, TIER_WOOD), B_SAND) > 1.0f);

    /* Rock needs a pickaxe to yield; iron ore one of stone or better. */
    CHECK(!item_harvests(0, B_ROCK) && item_harvests(wood_pick, B_ROCK) && !item_harvests(axe, B_ROCK));
    CHECK(!item_harvests(wood_pick, B_IRON_ORE) && item_harvests(stone_pick, B_IRON_ORE));
    CHECK(item_harvests(0, B_DIRT) && item_harvests(0, B_LOG));

    /* Every tool can be made. */
    int made = 0;
    for (int r = 0; r < g_recipe_count; r++) made += item_tool(g_recipes[r].out.id, &kind, &tier);
    CHECK(made == TOOL_KINDS * TIER_COUNT);
}

static void test_wear(void)
{
    int pick = item_tool_id(TOOL_PICKAXE, TIER_WOOD), uses = item_durability(pick);
    inventory inv = {0};
    inv_init(&inv);
    inv_add(&inv, pick, 1);
    CHECK(inv.slot[0].id == pick && inv.slot[0].wear == 0);
    for (int i = 0; i < uses - 1; i++) CHECK(!inv_wear_held(&inv, 1));
    CHECK(inv.slot[0].count == 1 && inv.slot[0].wear == uses - 1);
    CHECK(inv_wear_held(&inv, 1) && inv.slot[0].count == 0); /* worn out: gone */
    inv_add(&inv, I_APPLE, 3);
    CHECK(!inv_wear_held(&inv, 1) && inv.slot[0].count == 3 && inv.slot[0].wear == 0); /* apples do not wear */

    /* Moving a worn tool never mends it: pick up, put down, split, place one. */
    inv_init(&inv);
    inv_add_stack(&inv, (item_stack){(uint8_t)pick, 1, 20});
    CHECK(inv.slot[0].wear == 20);
    inv_click(&inv, 0, 0, 0);
    inv_click(&inv, 5, 0, 0);
    CHECK(inv.slot[5].id == pick && inv.slot[5].wear == 20);
    inv_click(&inv, 5, 1, 0); /* right: take half (all of one) */
    CHECK(inv.cursor.id == pick && inv.cursor.wear == 20);
    inv_click(&inv, 7, 1, 0); /* right: place one */
    CHECK(inv.slot[7].id == pick && inv.slot[7].wear == 20);
    inv_click(&inv, 7, 0, 1); /* shift: to the backpack */
    CHECK(inv_count(&inv, pick) == 1);
    for (int i = 0; i < INV_SLOTS; i++)
        if (inv.slot[i].id == pick) CHECK(inv.slot[i].wear == 20);

    /* Dropped and picked up again, it keeps its wear. */
    test_world t = {0};
    tw_init(&t);
    CHECK(
        physics_drop_stack(&t.ph, dv3(5.5, GROUND + 0.5, 5.5), dv3(0, 0, 0), (item_stack){(uint8_t)pick, 1, 33}, 0.0f));
    CHECK(t.ph.item_count == 1 && t.ph.items[0].wear == 33);
    inventory got = {0};
    inv_init(&got);
    CHECK(inv_accept(&got, (item_stack){(uint8_t)pick, 1, 33}) == 1 && got.slot[0].wear == 33);
    tw_free(&t);
}

static void test_formats(void)
{
    int pick = item_tool_id(TOOL_PICKAXE, TIER_STONE);
    inventory inv = {0};
    inv_init(&inv);
    inv_add(&inv, B_LOG, 5);
    inv_add_stack(&inv, (item_stack){(uint8_t)pick, 1, 40});
    uint8_t buf[INV_ENCODED_SIZE] = {0};
    CHECK(inv_encode(&inv, buf, sizeof buf) == INV_ENCODED_SIZE && !memcmp(buf, "INV2", 4));
    inventory back = {0};
    CHECK(inv_decode(&back, buf, sizeof buf) == 0 && back.slot[1].id == pick && back.slot[1].wear == 40);
    /* Wear on something that does not wear, or past worn out, is refused. */
    buf[8 + 2] = 1;
    CHECK(inv_decode(&back, buf, sizeof buf) == -1);
    inv_encode(&inv, buf, sizeof buf);
    buf[8 + 3 + 2] = (uint8_t)item_durability(pick);
    CHECK(inv_decode(&back, buf, sizeof buf) == -1);

    /* An INV1 inventory from an older build still loads, without wear. */
    uint8_t v1[INV1_ENCODED_SIZE] = {'I', 'N', 'V', '1', 2, 0, 0, 0};
    v1[8] = B_LOG;
    v1[9] = 5;
    v1[8 + 2 * 36] = I_APPLE; /* the cursor */
    v1[8 + 2 * 36 + 1] = 2;
    CHECK(inv_decode(&back, v1, sizeof v1) == 0 && back.selected == 2 && inv_count(&back, B_LOG) == 5 &&
          back.cursor.id == I_APPLE && back.cursor.count == 2);
    CHECK(inv_decode(&back, v1, sizeof v1 - 1) == -1);

    /* A version 1 player record (INV1 inside) still loads; version 2 is
     * what is written now. */
    player p = {0};
    p.pos = dv3(10.5, 70.0, -3.5);
    uint8_t pf[PLAYER_FILE_SIZE] = {0};
    CHECK(survival_encode_player(&p, &inv, 0.5, pf, sizeof pf) == PLAYER_FILE_SIZE && pf[4] == 2);
    uint8_t old[PLAYER_FILE_SIZE_V1] = {0};
    memcpy(old, pf, 44);
    old[4] = 1;
    memcpy(old + 44, v1, sizeof v1);
    player q = {0};
    inventory qi = {0};
    CHECK(survival_decode_player(&q, &qi, NULL, old, sizeof old) == 0 && q.pos.x == 10.5 && inv_count(&qi, B_LOG) == 5);
    old[4] = 2; /* a version that does not match its length */
    CHECK(survival_decode_player(&q, &qi, NULL, old, sizeof old) == -1);
}

/* Breaking rock: slow and fruitless by hand, faster with a pickaxe, which
 * wears by one per block. */
static void test_breaking(void)
{
    test_world t = {0};
    tw_init(&t);
    world_set(&t.w, 4, GROUND, 4, B_ROCK, 0);
    world_set(&t.w, 6, GROUND, 4, B_ROCK, 0);
    fx_state *fx = mem_calloc(1, sizeof *fx);
    fx_init(fx, 1);
    player pl = {0};
    pl.pos = dv3(4.5, GROUND + 1.0, 2.5);
    inventory inv = {0};
    inv_init(&inv);
    interact ia = {0};
    interact_init(&ia, 3);
    interact_input in = {.attack = 1, .can_act = 1, .speed = 1.0f, .dt = 1.0f / 60.0f};
    ray_hit hit = {.hit = 1, .block = {4, GROUND, 4}, .before = {4, GROUND, 3}, .id = B_ROCK};
    vec3 dir = {0.0f, 0.0f, 1.0f};
    int hand = 0;
    for (; hand < 6000 && world_get(&t.w, 4, GROUND, 4) == B_ROCK; hand++)
        interact_frame(&ia, &t.w, &t.ph, NULL, &pl, &inv, fx, pl.pos, dir, hit, &in);
    CHECK(world_get(&t.w, 4, GROUND, 4) == B_AIR && t.ph.item_count == 0); /* nothing to show for it */

    int pick = item_tool_id(TOOL_PICKAXE, TIER_IRON);
    inv_add(&inv, pick, 1);
    hit.block = (ipos){6, GROUND, 4};
    hit.before = (ipos){6, GROUND, 3};
    int tool = 0;
    for (; tool < 6000 && world_get(&t.w, 6, GROUND, 4) == B_ROCK; tool++)
        interact_frame(&ia, &t.w, &t.ph, NULL, &pl, &inv, fx, pl.pos, dir, hit, &in);
    CHECK(world_get(&t.w, 6, GROUND, 4) == B_AIR && t.ph.item_count == 1 && t.ph.items[0].id == B_STONE);
    CHECK(tool * 10 < hand); /* iron is about 20x faster than bare hands on rock */
    CHECK(inv.slot[0].wear == 1);
    mem_free(fx);
    tw_free(&t);
}

void test_tools_all(void)
{
    test_rules();
    test_wear();
    test_formats();
    test_breaking();
}
