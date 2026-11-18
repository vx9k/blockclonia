/* Tools: breaking speed and what a block yields by tool, wear and wearing
 * out, wear surviving moves, drops and saves, and the older formats. */
#include "combat.h"
#include "interact.h"
#include "inventory.h"
#include "item.h"
#include "mem.h"
#include "sound_synth.h"
#include "survival.h"
#include "test_util.h"

#include <string.h>

static void test_rules(void)
{
    item_id wood_pick = item_tool_id(TOOL_PICKAXE, TIER_WOOD), iron_pick = item_tool_id(TOOL_PICKAXE, TIER_IRON);
    item_id stone_pick = item_tool_id(TOOL_PICKAXE, TIER_STONE), axe = item_tool_id(TOOL_AXE, TIER_STONE);
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
    item_id pick = item_tool_id(TOOL_PICKAXE, TIER_WOOD);
    int uses = item_durability(pick);
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
    inv_add_stack(&inv, (item_stack){pick, 1, 20});
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
    CHECK(physics_drop_stack(&t.ph, dv3(5.5, GROUND + 0.5, 5.5), dv3(0, 0, 0), (item_stack){pick, 1, 33}, 0.0f));
    CHECK(t.ph.item_count == 1 && t.ph.items[0].wear == 33);
    inventory got = {0};
    inv_init(&got);
    CHECK(inv_accept(&got, (item_stack){pick, 1, 33}) == 1 && got.slot[0].wear == 33);
    tw_free(&t);
}

static void test_formats(void)
{
    item_id pick = item_tool_id(TOOL_PICKAXE, TIER_STONE);
    inventory inv = {0};
    inv_init(&inv);
    inv_add(&inv, B_LOG, 5);
    inv_add_stack(&inv, (item_stack){pick, 1, 40});
    uint8_t buf[INV_ENCODED_SIZE] = {0};
    CHECK(inv_encode(&inv, buf, sizeof buf) == INV_ENCODED_SIZE && !memcmp(buf, "INV3", 4));
    /* Slot 0 is (u32 id little-endian, count, wear); every item id is past
     * the 255 one byte held, so slot 1 shows all four bytes in use. */
    CHECK(buf[8] == B_LOG && !buf[9] && !buf[10] && !buf[11] && buf[12] == 5 && !buf[13]);
    CHECK(pick > 255 && buf[14] == (uint8_t)pick && buf[15] == (uint8_t)(pick >> 8) && !buf[16] && !buf[17]);
    inventory back = {0};
    CHECK(inv_decode(&back, buf, sizeof buf) == 0 && back.slot[1].id == pick && back.slot[1].wear == 40);
    CHECK(inv_count(&back, B_LOG) == 5 && inv_count(&back, pick) == 1);
    /* Wear on something that does not wear, or past worn out, is refused. */
    buf[8 + 5] = 1;
    CHECK(inv_decode(&back, buf, sizeof buf) == -1);
    inv_encode(&inv, buf, sizeof buf);
    buf[14 + 5] = (uint8_t)item_durability(pick);
    CHECK(inv_decode(&back, buf, sizeof buf) == -1);
    /* An id in the high bytes that names no item is refused. */
    inv_encode(&inv, buf, sizeof buf);
    buf[14 + 3] = 1;
    CHECK(inv_decode(&back, buf, sizeof buf) == -1);
    /* Older layouts are not read: no compatibility kept before release. */
    inv_encode(&inv, buf, sizeof buf);
    buf[3] = '2';
    CHECK(inv_decode(&back, buf, sizeof buf) == -1);
    CHECK(inv_decode(&back, buf, sizeof buf - 1) == -1);

    /* The player record is version 3, with an INV3 inventory inside. */
    player p = {0};
    p.pos = dv3(10.5, 70.0, -3.5);
    uint8_t pf[PLAYER_FILE_SIZE] = {0};
    CHECK(survival_encode_player(&p, &inv, 0.5, pf, sizeof pf) == PLAYER_FILE_SIZE && pf[4] == 3);
    player q = {0};
    inventory qi = {0};
    CHECK(survival_decode_player(&q, &qi, NULL, pf, sizeof pf) == 0 && q.pos.x == 10.5 && inv_count(&qi, pick) == 1);
    pf[4] = 2;
    CHECK(survival_decode_player(&q, &qi, NULL, pf, sizeof pf) == -1);
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

    item_id pick = item_tool_id(TOOL_PICKAXE, TIER_IRON);
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

/* The lowest pickaxe tier that gets anything out of a block, -1 when bare
 * hands do. */
static int tier_needed(uint8_t b)
{
    if (item_harvests(0, b)) return -1;
    for (int t = 0; t < TIER_COUNT; t++)
        if (item_harvests(item_tool_id(TOOL_PICKAXE, t), b)) return t;
    return TIER_COUNT;
}

/* What each block is made of decides the tool, the tier, the sound and
 * how it treats a falling or sliding body; pinned for every block. */
static void test_materials(void)
{
    static const struct {
        int tool, tier;
        synth_material sound;
        float cushion, grit;
    } WANT[B_COUNT] = {
        [B_AIR] = {-1, -1, MAT_GENERIC, 1.0f, 0.0f},
        [B_BEDROCK] = {-1, -1, MAT_STONE, 1.0f, 0.4f},
        [B_STONE] = {TOOL_PICKAXE, TIER_WOOD, MAT_STONE, 1.0f, 0.4f},
        [B_DIRT] = {TOOL_SHOVEL, -1, MAT_DIRT, 0.92f, 0.55f},
        [B_GRASS] = {TOOL_SHOVEL, -1, MAT_GRASS, 0.92f, 0.55f},
        [B_SAND] = {TOOL_SHOVEL, -1, MAT_SAND, 0.85f, 0.6f},
        [B_GRAVEL] = {TOOL_SHOVEL, -1, MAT_GRAVEL, 0.85f, 0.85f},
        [B_LOG] = {TOOL_AXE, -1, MAT_WOOD, 1.0f, 0.3f},
        [B_LEAVES] = {-1, -1, MAT_LEAVES, 0.35f, 0.0f},
        [B_PLANKS] = {TOOL_AXE, -1, MAT_WOOD, 1.0f, 0.3f},
        [B_GLASS] = {-1, -1, MAT_GLASS, 1.0f, 0.0f},
        [B_BRICK] = {TOOL_PICKAXE, TIER_WOOD, MAT_STONE, 1.0f, 0.4f},
        [B_SNOW] = {TOOL_SHOVEL, -1, MAT_SNOW, 0.6f, 0.0f},
        [B_ICE] = {TOOL_PICKAXE, -1, MAT_GLASS, 1.0f, 0.0f},
        [B_WATER] = {-1, -1, MAT_WATER, 1.0f, 0.0f},
        [B_CAMPFIRE] = {TOOL_AXE, -1, MAT_WOOD, 1.0f, 0.0f},
        [B_ASH] = {TOOL_SHOVEL, -1, MAT_DIRT, 1.0f, 0.55f},
        [B_COAL_ORE] = {TOOL_PICKAXE, TIER_WOOD, MAT_STONE, 1.0f, 0.4f},
        [B_COPPER_ORE] = {TOOL_PICKAXE, TIER_STONE, MAT_STONE, 1.0f, 0.4f},
        [B_IRON_ORE] = {TOOL_PICKAXE, TIER_STONE, MAT_STONE, 1.0f, 0.4f},
        [B_FURNACE] = {TOOL_PICKAXE, TIER_WOOD, MAT_STONE, 1.0f, 0.4f},
        [B_ROCK] = {TOOL_PICKAXE, TIER_WOOD, MAT_STONE, 1.0f, 0.4f},
        [B_GRANITE] = {TOOL_PICKAXE, TIER_WOOD, MAT_STONE, 1.0f, 0.4f},
        [B_GRANITE_ROCK] = {TOOL_PICKAXE, TIER_WOOD, MAT_STONE, 1.0f, 0.4f},
        [B_BASALT] = {TOOL_PICKAXE, TIER_WOOD, MAT_STONE, 1.0f, 0.4f},
        [B_BASALT_ROCK] = {TOOL_PICKAXE, TIER_WOOD, MAT_STONE, 1.0f, 0.4f},
        [B_LIMESTONE] = {TOOL_PICKAXE, TIER_WOOD, MAT_STONE, 1.0f, 0.4f},
        [B_LIMESTONE_ROCK] = {TOOL_PICKAXE, TIER_WOOD, MAT_STONE, 1.0f, 0.4f},
        [B_SANDSTONE] = {TOOL_PICKAXE, TIER_WOOD, MAT_STONE, 1.0f, 0.6f},
        [B_SANDSTONE_ROCK] = {TOOL_PICKAXE, TIER_WOOD, MAT_STONE, 1.0f, 0.6f},
        [B_SHALE] = {TOOL_PICKAXE, TIER_WOOD, MAT_STONE, 1.0f, 0.35f},
        [B_SHALE_ROCK] = {TOOL_PICKAXE, TIER_WOOD, MAT_STONE, 1.0f, 0.35f},
        [B_MARBLE] = {TOOL_PICKAXE, TIER_WOOD, MAT_STONE, 1.0f, 0.3f},
        [B_MARBLE_ROCK] = {TOOL_PICKAXE, TIER_WOOD, MAT_STONE, 1.0f, 0.3f},
        [B_OBSIDIAN] = {TOOL_PICKAXE, TIER_WOOD, MAT_STONE, 1.0f, 0.2f},
        [B_OBSIDIAN_ROCK] = {TOOL_PICKAXE, TIER_WOOD, MAT_STONE, 1.0f, 0.2f},
        [B_CLAY] = {TOOL_SHOVEL, -1, MAT_DIRT, 0.9f, 0.3f},
        [B_MUD] = {TOOL_SHOVEL, -1, MAT_DIRT, 0.7f, 0.5f},
        [B_PEAT] = {TOOL_SHOVEL, -1, MAT_DIRT, 0.75f, 0.2f},
        [B_PODZOL] = {TOOL_SHOVEL, -1, MAT_DIRT, 0.9f, 0.45f},
        [B_TIN_ORE] = {TOOL_PICKAXE, TIER_STONE, MAT_STONE, 1.0f, 0.4f},
        [B_ZINC_ORE] = {TOOL_PICKAXE, TIER_STONE, MAT_STONE, 1.0f, 0.4f},
        [B_LEAD_ORE] = {TOOL_PICKAXE, TIER_STONE, MAT_STONE, 1.0f, 0.4f},
        [B_NICKEL_ORE] = {TOOL_PICKAXE, TIER_COPPER, MAT_STONE, 1.0f, 0.4f},
        [B_SILVER_ORE] = {TOOL_PICKAXE, TIER_STONE, MAT_STONE, 1.0f, 0.4f},
        [B_GOLD_ORE] = {TOOL_PICKAXE, TIER_STONE, MAT_STONE, 1.0f, 0.4f},
        [B_BAUXITE] = {TOOL_PICKAXE, TIER_WOOD, MAT_STONE, 1.0f, 0.5f},
        [B_TITANIUM_ORE] = {TOOL_PICKAXE, TIER_IRON, MAT_STONE, 1.0f, 0.4f},
        [B_URANIUM_ORE] = {TOOL_PICKAXE, TIER_COPPER, MAT_STONE, 1.0f, 0.4f},
        [B_LITHIUM_ORE] = {TOOL_PICKAXE, TIER_COPPER, MAT_STONE, 1.0f, 0.4f},
        [B_SULFUR_ORE] = {TOOL_PICKAXE, TIER_WOOD, MAT_STONE, 1.0f, 0.3f},
        [B_SALTPETER_ORE] = {TOOL_PICKAXE, TIER_WOOD, MAT_STONE, 1.0f, 0.3f},
        [B_ROCK_SALT] = {TOOL_PICKAXE, TIER_WOOD, MAT_STONE, 1.0f, 0.3f},
        [B_QUARTZ_ORE] = {TOOL_PICKAXE, TIER_IRON, MAT_STONE, 1.0f, 0.4f},
        [B_GRAPHITE_ORE] = {TOOL_PICKAXE, TIER_WOOD, MAT_STONE, 1.0f, 0.3f},
        [B_LAVA] = {-1, -1, MAT_WATER, 1.0f, 0.0f},
        [B_CRUDE_OIL] = {-1, -1, MAT_WATER, 1.0f, 0.0f},
        [B_BRINE] = {-1, -1, MAT_WATER, 1.0f, 0.0f},
        [B_TORCH] = {TOOL_AXE, -1, MAT_WOOD, 1.0f, 0.0f},
        [B_LAMP] = {-1, -1, MAT_GLASS, 1.0f, 0.0f},
        [B_FOXFIRE] = {TOOL_SHOVEL, -1, MAT_DIRT, 0.7f, 0.1f},
        [B_BIRCH_LOG] = {TOOL_AXE, -1, MAT_WOOD, 1.0f, 0.3f},
        [B_SPRUCE_LOG] = {TOOL_AXE, -1, MAT_WOOD, 1.0f, 0.3f},
        [B_PINE_LOG] = {TOOL_AXE, -1, MAT_WOOD, 1.0f, 0.3f},
        [B_THORN_LOG] = {TOOL_AXE, -1, MAT_WOOD, 1.0f, 0.3f},
        [B_BIRCH_LEAVES] = {-1, -1, MAT_LEAVES, 0.35f, 0.0f},
        [B_SPRUCE_LEAVES] = {-1, -1, MAT_LEAVES, 0.4f, 0.0f},
        [B_PINE_LEAVES] = {-1, -1, MAT_LEAVES, 0.4f, 0.0f},
        [B_THORN_LEAVES] = {-1, -1, MAT_LEAVES, 0.4f, 0.0f},
        [B_GRASS_TUFT] = {-1, -1, MAT_LEAVES, 1.0f, 0.0f},
        [B_TALL_GRASS] = {-1, -1, MAT_LEAVES, 1.0f, 0.0f},
        [B_FERN] = {-1, -1, MAT_LEAVES, 1.0f, 0.0f},
        [B_REEDS] = {-1, -1, MAT_LEAVES, 1.0f, 0.0f},
        [B_HEATHER] = {-1, -1, MAT_LEAVES, 1.0f, 0.0f},
        [B_DRY_SHRUB] = {-1, -1, MAT_LEAVES, 1.0f, 0.0f},
        [B_BUSH] = {-1, -1, MAT_LEAVES, 1.0f, 0.0f},
        [B_POPPY] = {-1, -1, MAT_LEAVES, 1.0f, 0.0f},
        [B_DANDELION] = {-1, -1, MAT_LEAVES, 1.0f, 0.0f},
        [B_CORNFLOWER] = {-1, -1, MAT_LEAVES, 1.0f, 0.0f},
        [B_OXEYE] = {-1, -1, MAT_LEAVES, 1.0f, 0.0f},
        [B_VINES] = {-1, -1, MAT_LEAVES, 1.0f, 0.0f},
        [B_WILLOW_LOG] = {TOOL_AXE, -1, MAT_WOOD, 1.0f, 0.3f},
        [B_MAPLE_LOG] = {TOOL_AXE, -1, MAT_WOOD, 1.0f, 0.3f},
        [B_CHERRY_LOG] = {TOOL_AXE, -1, MAT_WOOD, 1.0f, 0.3f},
        [B_REDWOOD_LOG] = {TOOL_AXE, -1, MAT_WOOD, 1.0f, 0.3f},
        [B_PALM_LOG] = {TOOL_AXE, -1, MAT_WOOD, 1.0f, 0.3f},
        [B_BAOBAB_LOG] = {TOOL_AXE, -1, MAT_WOOD, 1.0f, 0.3f},
        [B_UMBRELLA_LOG] = {TOOL_AXE, -1, MAT_WOOD, 1.0f, 0.3f},
        [B_MOUNTAIN_PINE_LOG] = {TOOL_AXE, -1, MAT_WOOD, 1.0f, 0.3f},
        [B_WILLOW_LEAVES] = {-1, -1, MAT_LEAVES, 0.35f, 0.0f},
        [B_MAPLE_LEAVES] = {-1, -1, MAT_LEAVES, 0.35f, 0.0f},
        [B_CHERRY_BLOSSOM] = {-1, -1, MAT_LEAVES, 0.35f, 0.0f},
        [B_REDWOOD_LEAVES] = {-1, -1, MAT_LEAVES, 0.4f, 0.0f},
        [B_PALM_FRONDS] = {-1, -1, MAT_LEAVES, 0.4f, 0.0f},
        [B_BAOBAB_LEAVES] = {-1, -1, MAT_LEAVES, 0.35f, 0.0f},
        [B_UMBRELLA_LEAVES] = {-1, -1, MAT_LEAVES, 0.4f, 0.0f},
        [B_MOUNTAIN_PINE_LEAVES] = {-1, -1, MAT_LEAVES, 0.4f, 0.0f},
    };
    int bad = 0;
    for (int b = 0; b < B_COUNT; b++) {
        uint8_t id = (uint8_t)b;
        int ok = block_tool(id) == WANT[b].tool && tier_needed(id) == WANT[b].tier &&
                 synth_material_of(id) == WANT[b].sound && block_get(id)->cushion == WANT[b].cushion &&
                 block_get(id)->grit == WANT[b].grit && block_get(id)->mat <= BM_WATER;
        if (!ok) fprintf(stderr, "  block %d (%s) differs\n", b, block_get(id)->name);
        bad += !ok;
    }
    CHECK(bad == 0);
}

/* One touchdown on a block at vel: what the legs lost, and the grit in the
 * dirtiest scrape (0 for none). */
static void land_on(uint8_t ground, dvec3 vel, float *leg_loss, float *grit)
{
    test_world t = {0};
    tw_init(&t);
    world_set(&t.w, 4, GROUND - 1, 4, ground, 0);
    player p = {0};
    player_spawn(&p, &t.w, 4.5, 4.5);
    p.pos.y = GROUND;
    p.on_ground = 1;
    p.submerged = 0.0;
    health *h = new_body(3);
    float before = h->part[BP_LLEG].integrity + h->part[BP_RLEG].integrity;
    survival_before b = {vel, 0, 0.0};
    survival_impacts(h, &t.w, &t.ph.contact[0], &p, &b);
    *leg_loss = before - (h->part[BP_LLEG].integrity + h->part[BP_RLEG].integrity);
    *grit = 0.0f;
    for (int i = 0; i < h->wound_count; i++)
        if (h->wounds[i].kind == WOUND_ABRASION && h->wounds[i].contamination > *grit)
            *grit = h->wounds[i].contamination;
    mem_free(h);
    tw_free(&t);
}

/* The table's cushion and grit reach the body: leaves break a fall that
 * rock does not, and a slide on gravel grinds in more than one on rock,
 * while snow takes no skin off at all. */
static void test_ground(void)
{
    float leaves = 0.0f, rock = 0.0f, grit = 0.0f;
    land_on(B_LEAVES, dv3(0.0, -14.0, 0.0), &leaves, &grit);
    land_on(B_ROCK, dv3(0.0, -14.0, 0.0), &rock, &grit);
    CHECK(rock > 0.0f && leaves < rock);
    float gravel = 0.0f, stone = 0.0f, snow = 0.0f, legs = 0.0f;
    land_on(B_GRAVEL, dv3(10.0, -3.0, 0.0), &legs, &gravel);
    land_on(B_ROCK, dv3(10.0, -3.0, 0.0), &legs, &stone);
    land_on(B_SNOW, dv3(10.0, -3.0, 0.0), &legs, &snow);
    CHECK(gravel > stone && stone > 0.0f && snow == 0.0f);
    /* Ids past the table (the unloaded sentinel) take no tool and make the
     * plain sound, rather than reading as the stone block_get falls back to. */
    CHECK(block_tool(B_UNLOADED) == -1 && synth_material_of(B_UNLOADED) == MAT_GENERIC);
}

/* An item id is all 32 bits. One that matches a real item only in its low
 * bytes is an unknown item, not that item: nothing may truncate it to a
 * byte on the way, or a corrupt save or packet would conjure real items. */
static void test_wide_ids(void)
{
    const item_id aliases[] = {ITEM_END, I_SPIRITS | 0x10000u, I_SPIRITS | 0x80000000u, B_STONE | 0x1000000u,
                               0xFFFFFFFFu};
    for (size_t i = 0; i < sizeof aliases / sizeof aliases[0]; i++) {
        item_id a = aliases[i];
        CHECK(!item_valid(a) && !strcmp(item_get(a)->name, "?") && !item_durability(a));
        CHECK(item_dig_speed(a, B_DIRT) == item_dig_speed(0, B_DIRT) && combat_blow(a) == combat_blow(0));
        inventory inv = {0};
        inv_init(&inv);
        CHECK(inv_add(&inv, a, 5) == 5 && inv_count(&inv, a) == 0);
        inv_set_held(&inv, a, 1);
        CHECK(inv_held(&inv)->count == 0);
    }
    /* Past one byte, every id up to ITEM_END is a real, named item. */
    CHECK(ITEM_END > 256 && I_SPIRITS > 255);
    for (item_id id = ITEM_FIRST; id < ITEM_END; id++) CHECK(item_valid(id) && strcmp(item_get(id)->name, "?") != 0);
}

/* Tool ids map to and from (kind, tier) one to one, and only inside the
 * tool range: an id a byte or more past it is no tool. */
static void test_tool_ids(void)
{
    int kind = -1, tier = -1, seen = 0;
    for (int k = 0; k < TOOL_KINDS; k++)
        for (int t = 0; t < TIER_COUNT; t++) {
            item_id id = item_tool_id(k, t);
            seen += item_tool(id, &kind, &tier) && kind == k && tier == t && item_valid(id);
        }
    CHECK(seen == TOOL_KINDS * TIER_COUNT);
    CHECK(!item_tool(I_TOOL_FIRST - 1, &kind, &tier) && !item_tool(I_TOOL_END, &kind, &tier));
    CHECK(!item_tool(I_TOOL_FIRST + 0x100u, &kind, &tier) && !item_tool(I_TOOL_FIRST | 0x80000000u, &kind, &tier));
}

/* INV3 keeps the id as four little-endian bytes, and a flipped bit in any
 * byte above the lowest refuses the whole inventory, leaving the old one. */
static void test_inv3_ids(void)
{
    inventory inv = {0}, out = {0};
    inv_init(&inv);
    inv_set_held(&inv, I_SPIRITS, 2);
    uint8_t buf[INV_ENCODED_SIZE] = {0};
    CHECK(inv_encode(&inv, buf, sizeof buf) == INV_ENCODED_SIZE && !memcmp(buf, "INV3", 4));
    const uint8_t *slot = buf + 8 + 6 * inv.selected;
    CHECK(slot[0] == (uint8_t)I_SPIRITS && slot[1] == (uint8_t)(I_SPIRITS >> 8) && !slot[2] && !slot[3]);
    CHECK(slot[4] == 2 && slot[5] == 0);
    CHECK(inv_decode(&out, buf, sizeof buf) == 0 && inv_held(&out)->id == I_SPIRITS && inv_held(&out)->count == 2);

    for (int k = 1; k <= 4; k++) {
        uint8_t bad[INV_ENCODED_SIZE] = {0};
        memcpy(bad, buf, sizeof bad);
        if (k < 4) bad[8 + 6 * inv.selected + k] ^= 0x40;
        else memcpy(bad, "INV2", 4);
        inv_init(&out);
        inv_add(&out, I_APPLE, 1);
        CHECK(inv_decode(&out, bad, sizeof bad) < 0 && inv_count(&out, I_APPLE) == 1);
    }
}

/* Plants: nothing collides with them, light passes, the leaves' sound,
 * and broken by hand, faster than anything solid. Grass tears into fibre
 * now and then, reeds always; shrubs give sticks; a flower is itself. */
static void test_plant_blocks(void)
{
    for (int b = B_GRASS_TUFT; b <= B_VINES; b++) {
        uint8_t id = (uint8_t)b;
        CHECK(block_plant(id) && !block_solid(id) && !block_opaque(id) && !block_leaves(id) && !block_log(id));
        CHECK(item_harvests(0, id) && block_get(id)->hardness > 0.0f && block_get(id)->hardness < 0.5f);
        CHECK(block_get(id)->mat == BM_LEAVES && synth_material_of(id) == MAT_LEAVES);
    }
    CHECK(!block_plant(B_THORN_LEAVES) && !block_plant(B_GRASS) && !block_plant(B_COUNT) && !block_plant(B_AIR));
    uint32_t rng = 5;
    int fibre[3] = {0}, sticks = 0, flowers = 0;
    for (int k = 0; k < 300; k++) {
        static const uint8_t TRY[5] = {B_TALL_GRASS, B_REEDS, B_BUSH, B_CORNFLOWER, B_GRASS_TUFT};
        item_stack out[3] = {{0}};
        int n = item_drops(TRY[k % 5], &rng, out);
        for (int j = 0; j < n; j++) {
            if (out[j].id == I_FIBRE) fibre[k % 5 == 1 ? 1 : (k % 5 == 2 ? 2 : 0)]++;
            sticks += out[j].id == I_STICK;
            flowers += out[j].id == B_CORNFLOWER;
        }
    }
    CHECK(fibre[0] > 10 && fibre[0] < 120 && fibre[1] == 60 && sticks > 15 && flowers == 60);
}

/* Breaking the soil under a plant uproots it, and its drops come with
 * it; a block aimed at a plant replaces it; and a flower takes root in
 * soil but not on stone. */
static void test_plants_by_hand(void)
{
    test_world t = {0};
    tw_init(&t);
    world_set(&t.w, 4, GROUND, 4, B_REEDS, 0);
    world_set(&t.w, 4, GROUND + 1, 4, B_REEDS, 0);
    world_set(&t.w, 6, GROUND, 4, B_TALL_GRASS, 0);
    fx_state *fx = mem_calloc(1, sizeof *fx);
    fx_init(fx, 1);
    player pl = {0};
    pl.pos = dv3(4.5, GROUND + 1.0, 1.5);
    inventory inv = {0};
    inv_init(&inv);
    interact ia = {0};
    interact_init(&ia, 3);
    interact_input dig = {.attack = 1, .can_act = 1, .speed = 1.0f, .dt = 1.0f / 60.0f};
    ray_hit hit = {.hit = 1, .block = {4, GROUND - 1, 4}, .before = {4, GROUND - 1, 3}, .id = B_GRASS};
    vec3 dir = {0.0f, 0.0f, 1.0f};
    for (int f = 0; f < 6000 && world_get(&t.w, 4, GROUND - 1, 4) == B_GRASS; f++)
        interact_frame(&ia, &t.w, &t.ph, NULL, &pl, &inv, fx, pl.pos, dir, hit, &dig);
    CHECK(world_get(&t.w, 4, GROUND - 1, 4) == B_AIR);
    CHECK(world_get(&t.w, 4, GROUND, 4) == B_AIR && world_get(&t.w, 4, GROUND + 1, 4) == B_AIR);
    int fibre = 0;
    for (int i = 0; i < t.ph.item_count; i++) fibre += t.ph.items[i].id == I_FIBRE;
    CHECK(fibre == 2); /* both reeds, and the dirt besides */
    CHECK(world_get(&t.w, 6, GROUND, 4) == B_TALL_GRASS);

    /* Planks aimed at the grass go where it stood. */
    interact_input use = {.use_click = 1, .can_act = 1, .speed = 1.0f, .dt = 1.0f / 60.0f};
    inv_add(&inv, B_PLANKS, 1);
    hit = (ray_hit){1, {6, GROUND, 4}, {6, GROUND, 3}, B_TALL_GRASS};
    interact_frame(&ia, &t.w, &t.ph, NULL, &pl, &inv, fx, pl.pos, dir, hit, &use);
    CHECK(world_get(&t.w, 6, GROUND, 4) == B_PLANKS && world_get(&t.w, 6, GROUND, 3) == B_AIR);

    /* A poppy: not on the planks, but on the grass beside them. */
    inv_set_held(&inv, B_POPPY, 2);
    hit = (ray_hit){1, {6, GROUND, 4}, {6, GROUND + 1, 4}, B_PLANKS};
    interact_out o = interact_frame(&ia, &t.w, &t.ph, NULL, &pl, &inv, fx, pl.pos, dir, hit, &use);
    CHECK(world_get(&t.w, 6, GROUND + 1, 4) == B_AIR && o.msg != NULL && inv_held(&inv)->count == 2);
    hit = (ray_hit){1, {8, GROUND - 1, 4}, {8, GROUND, 4}, B_GRASS};
    interact_frame(&ia, &t.w, &t.ph, NULL, &pl, &inv, fx, pl.pos, dir, hit, &use);
    CHECK(world_get(&t.w, 8, GROUND, 4) == B_POPPY && inv_held(&inv)->count == 1);
    mem_free(fx);
    tw_free(&t);
}

void test_tools_all(void)
{
    test_plant_blocks();
    test_plants_by_hand();
    test_ground();
    test_materials();
    test_rules();
    test_wear();
    test_formats();
    test_breaking();
    test_wide_ids();
    test_tool_ids();
    test_inv3_ids();
}
