/* The underground materials: rocks in place and laid, soils, ores and the
 * minerals they give, and the furnace work that turns them into metal. */
#include "block.h"
#include "interact.h"
#include "inventory.h"
#include "invui.h"
#include "item.h"
#include "itemmodel.h"
#include "mem.h"
#include "save.h"
#include "test_util.h"
#include "texgen.h"
#include "world.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Each rock in place and the loose kind it breaks into. */
static const uint8_t ROCKS[][2] = {
    {B_ROCK, B_STONE},
    {B_GRANITE_ROCK, B_GRANITE},
    {B_BASALT_ROCK, B_BASALT},
    {B_LIMESTONE_ROCK, B_LIMESTONE},
    {B_SANDSTONE_ROCK, B_SANDSTONE},
    {B_SHALE_ROCK, B_SHALE},
    {B_MARBLE_ROCK, B_MARBLE},
    {B_OBSIDIAN_ROCK, B_OBSIDIAN},
};

/* Each ore and what it yields, pinned here rather than read back from item.c. */
static const struct {
    uint8_t block;
    item_id item;
    int lo, hi; /* pieces per block */
} ORES[] = {
    {B_COAL_ORE, I_COAL, 1, 2},         {B_COPPER_ORE, I_RAW_COPPER, 1, 1}, {B_IRON_ORE, I_RAW_IRON, 1, 1},
    {B_TIN_ORE, I_RAW_TIN, 1, 1},       {B_ZINC_ORE, I_RAW_ZINC, 1, 1},     {B_LEAD_ORE, I_RAW_LEAD, 1, 1},
    {B_NICKEL_ORE, I_RAW_NICKEL, 1, 1}, {B_SILVER_ORE, I_RAW_SILVER, 1, 1}, {B_GOLD_ORE, I_RAW_GOLD, 1, 1},
    {B_BAUXITE, I_BAUXITE, 2, 2},       {B_TITANIUM_ORE, I_ILMENITE, 1, 1}, {B_URANIUM_ORE, I_URANINITE, 1, 1},
    {B_LITHIUM_ORE, I_SPODUMENE, 1, 1}, {B_SULFUR_ORE, I_SULFUR, 1, 2},     {B_SALTPETER_ORE, I_SALTPETER, 1, 2},
    {B_ROCK_SALT, I_SALT, 2, 2},        {B_QUARTZ_ORE, I_QUARTZ, 1, 1},     {B_GRAPHITE_ORE, I_GRAPHITE, 1, 2},
};
#define NORES ((int)(sizeof ORES / sizeof ORES[0]))

/* Rock in place is one mass with the ground: it spans like bedrock-backed
 * rock, never goes into an inventory, and breaks into the laid kind, which
 * looks and sounds the same but spans less and is lighter for its joints. */
static void test_rocks(void)
{
    for (int i = 0; i < (int)(sizeof ROCKS / sizeof ROCKS[0]); i++) {
        const block_def *in = block_get(ROCKS[i][0]), *laid = block_get(ROCKS[i][1]);
        CHECK(in->loose == ROCKS[i][1] && laid->loose == 0);
        CHECK(!item_valid(ROCKS[i][0]) && item_valid(ROCKS[i][1]));
        CHECK(in->span == 15 && laid->span < in->span && laid->density < in->density);
        CHECK(!strcmp(in->name, laid->name) && in->tex[0] == laid->tex[0] && in->mat == laid->mat);
        CHECK(in->conductivity == laid->conductivity && in->heat_capacity == laid->heat_capacity);
        uint32_t rng = 7;
        item_stack out[3] = {{0}};
        CHECK(item_drops(ROCKS[i][0], &rng, out) == 1 && out[0].id == ROCKS[i][1] && out[0].count == 1);
        CHECK(item_drops(ROCKS[i][1], &rng, out) == 1 && out[0].id == ROCKS[i][1]);
    }
    /* Glass breaks as glass: laid obsidian shatters, the flow it came from
     * does not fall at all. Mud slumps like sand. */
    CHECK((block_get(B_OBSIDIAN)->flags & BF_BRITTLE) && !(block_get(B_OBSIDIAN_ROCK)->flags & BF_BRITTLE));
    CHECK((block_get(B_MUD)->flags & BF_GRANULAR) && !(block_get(B_CLAY)->flags & BF_GRANULAR));
    CHECK(block_get(B_SHALE)->span < block_get(B_GRANITE)->span);
}

/* Every ore stays in place and breaks into its mineral, only for a
 * pickaxe of its tier or better; nothing else in the table is an ore. */
static void test_ores(void)
{
    for (int i = 0; i < NORES; i++) {
        uint8_t b = ORES[i].block;
        CHECK(!item_valid(b) && item_valid(ORES[i].item) && block_get(b)->span == 15);
        int lo = 99, hi = 0, other = 0;
        uint32_t rng = 12345u + (uint32_t)i;
        for (int k = 0; k < 400; k++) {
            item_stack out[3] = {{0}};
            int n = item_drops(b, &rng, out);
            other += n != 1 || out[0].id != ORES[i].item;
            if (out[0].count < lo) lo = out[0].count;
            if (out[0].count > hi) hi = out[0].count;
        }
        CHECK(other == 0 && lo == ORES[i].lo && hi == ORES[i].hi);
        int tier = block_get(b)->tier;
        CHECK(tier < TIER_COUNT && !item_harvests(0, b));
        CHECK(item_harvests(item_tool_id(TOOL_PICKAXE, tier), b));
        if (tier > 0) CHECK(!item_harvests(item_tool_id(TOOL_PICKAXE, tier - 1), b));
        CHECK(!item_harvests(item_tool_id(TOOL_SHOVEL, TIER_IRON), b));
    }
    for (int b = 0; b < B_COUNT; b++) {
        int ore = 0;
        for (int i = 0; i < NORES; i++) ore |= ORES[i].block == b;
        CHECK(ore || block_get((uint8_t)b)->tier == 0);
    }
}

/* Minerals and metals: named, a model of their own, a full stack, and
 * dense enough to sink in water except porous charcoal. */
static void test_minerals(void)
{
    for (item_id id = I_RAW_TIN; id < I_MINERALS_END; id++) {
        const item_def *d = item_get(id);
        CHECK(item_valid(id) && d->name[0] && d->desc[0] && d->stack == 64 && d->block == B_AIR);
        item_model m = {0};
        CHECK(itemmodel_build(id, &m) && m.hold == IM_HOLD_PALM);
        CHECK(id == I_CHARCOAL ? d->density < 1000.0f : d->density > 2000.0f);
    }
    /* Ingots weigh what the metal does: lead heavier than iron, gold the
     * heaviest, tin and zinc lighter than copper. */
    CHECK(item_get(I_LEAD_INGOT)->density > item_get(I_IRON_INGOT)->density);
    CHECK(item_get(I_GOLD_INGOT)->density > item_get(I_LEAD_INGOT)->density);
    CHECK(item_get(I_TIN_INGOT)->density < item_get(I_COPPER_INGOT)->density);
    CHECK(item_get(I_ZINC_INGOT)->density < item_get(I_COPPER_INGOT)->density);
    /* Heat moves through graphite and rock salt far better than through
     * granite, and hardly at all through sulfur or peat. */
    CHECK(block_get(B_GRAPHITE_ORE)->conductivity > block_get(B_ROCK_SALT)->conductivity);
    CHECK(block_get(B_ROCK_SALT)->conductivity > block_get(B_GRANITE_ROCK)->conductivity);
    CHECK(block_get(B_SULFUR_ORE)->conductivity < 0.5f && block_get(B_PEAT)->conductivity < 0.5f);
}

/* --give finds items by name, case-insensitively: no two items may share
 * one, or the second could never be given. */
static void test_unique_names(void)
{
    int clash = 0;
    for (item_id a = 1; a < ITEM_END; a++) {
        if (!item_valid(a)) continue;
        for (item_id b = a + 1; b < ITEM_END; b++) {
            if (!item_valid(b)) continue;
            const char *x = item_get(a)->name, *y = item_get(b)->name;
            size_t k = 0;
            while (x[k] && tolower((unsigned char)x[k]) == tolower((unsigned char)y[k])) k++;
            if (!x[k] && !y[k]) {
                fprintf(stderr, "  items %u and %u are both '%s'\n", a, b, x);
                clash++;
            }
        }
    }
    CHECK(clash == 0);
}

/* The index of the recipe that makes out from first, -1 if none. */
static int find_recipe(item_id out, item_id first)
{
    for (int r = 0; r < g_recipe_count; r++)
        if (g_recipes[r].out.id == out && g_recipes[r].in[0].id == first) return r;
    return -1;
}

/* A recipe at its station takes exactly its inputs and gives its output;
 * away from the station it does nothing. */
static void check_recipe(item_id out, int made, const item_count *in, int n_in, int station)
{
    int r = find_recipe(out, in[0].id);
    CHECK(r >= 0);
    if (r < 0) return;
    inventory inv = {0};
    inv_init(&inv);
    for (int k = 0; k < n_in; k++) inv_add(&inv, in[k].id, in[k].count);
    if (station) CHECK(!inv_craft(&inv, r, 0));
    CHECK(inv_craft(&inv, r, station));
    CHECK(inv_count(&inv, out) == made);
    for (int k = 0; k < n_in; k++) CHECK(inv_count(&inv, in[k].id) == 0);
    CHECK(!inv_craft(&inv, r, station));
}

static void test_smelting(void)
{
    static const struct {
        item_id ingot, raw;
        int coal;
    } SMELT[] = {
        {I_TIN_INGOT, I_RAW_TIN, 1},       {I_ZINC_INGOT, I_RAW_ZINC, 2},     {I_LEAD_INGOT, I_RAW_LEAD, 1},
        {I_NICKEL_INGOT, I_RAW_NICKEL, 2}, {I_SILVER_INGOT, I_RAW_SILVER, 1}, {I_GOLD_INGOT, I_RAW_GOLD, 1},
    };
    for (int i = 0; i < (int)(sizeof SMELT / sizeof SMELT[0]); i++) {
        item_count in[2] = {{SMELT[i].raw, 1}, {I_COAL, (uint8_t)SMELT[i].coal}};
        check_recipe(SMELT[i].ingot, 1, in, 2, STATION_FURNACE);
        CHECK(invui_recipe_category(find_recipe(SMELT[i].ingot, SMELT[i].raw)) == RCAT_SMELTING);
    }
    item_count bronze[2] = {{I_COPPER_INGOT, 3}, {I_TIN_INGOT, 1}};
    check_recipe(I_BRONZE_INGOT, 4, bronze, 2, STATION_FURNACE);
    item_count brass[2] = {{I_COPPER_INGOT, 2}, {I_ZINC_INGOT, 1}};
    check_recipe(I_BRASS_INGOT, 3, brass, 2, STATION_FURNACE);
    item_count steel[2] = {{I_IRON_INGOT, 1}, {I_CHARCOAL, 1}};
    check_recipe(I_STEEL_INGOT, 1, steel, 2, STATION_FURNACE);
    item_count charcoal[1] = {{B_LOG, 1}};
    check_recipe(I_CHARCOAL, 2, charcoal, 1, STATION_FIRE);
    item_count lime[2] = {{B_LIMESTONE, 1}, {I_COAL, 1}};
    check_recipe(I_QUICKLIME, 1, lime, 2, STATION_FURNACE);
    item_count powder[3] = {{I_SALTPETER, 3}, {I_CHARCOAL, 1}, {I_SULFUR, 1}};
    check_recipe(I_GUNPOWDER, 4, powder, 3, 0);
    item_count brick[1] = {{B_CLAY, 1}};
    check_recipe(B_BRICK, 2, brick, 1, STATION_FIRE);
    CHECK(invui_recipe_category(find_recipe(I_BRONZE_INGOT, I_COPPER_INGOT)) == RCAT_SMELTING);
    CHECK(invui_recipe_category(find_recipe(I_CHARCOAL, B_LOG)) == RCAT_SMELTING);
    /* A furnace cannot reach what aluminium, titanium, uranium and lithium
     * take: their ores make nothing yet. */
    const item_id stuck[] = {I_BAUXITE, I_ILMENITE, I_URANINITE, I_SPODUMENE};
    for (int i = 0; i < 4; i++)
        for (int r = 0; r < g_recipe_count; r++)
            for (int k = 0; k < 3; k++) CHECK(g_recipes[r].in[k].id != stuck[i]);
}

/* From the ground to an alloy: tin and copper ore mined with a stone
 * pickaxe, smelted and melted together into bronze. */
static void test_bronze_from_ore(void)
{
    inventory inv = {0};
    inv_init(&inv);
    uint32_t rng = 99;
    const uint8_t dug[4] = {B_COPPER_ORE, B_COPPER_ORE, B_COPPER_ORE, B_TIN_ORE};
    item_id pick = item_tool_id(TOOL_PICKAXE, TIER_STONE);
    for (int i = 0; i < 4; i++) {
        item_stack out[3] = {{0}};
        CHECK(item_harvests(pick, dug[i]));
        int n = item_drops(dug[i], &rng, out);
        for (int k = 0; k < n; k++) inv_add(&inv, out[k].id, out[k].count);
    }
    inv_add(&inv, I_COAL, 4);
    for (int i = 0; i < 3; i++) CHECK(inv_craft(&inv, find_recipe(I_COPPER_INGOT, I_RAW_COPPER), STATION_FURNACE));
    CHECK(inv_craft(&inv, find_recipe(I_TIN_INGOT, I_RAW_TIN), STATION_FURNACE));
    CHECK(inv_craft(&inv, find_recipe(I_BRONZE_INGOT, I_COPPER_INGOT), STATION_FURNACE));
    CHECK(inv_count(&inv, I_BRONZE_INGOT) == 4 && inv_count(&inv, I_COAL) == 0);
}

/* The mean colour of a layer, and how many of its texels are opaque. */
static void layer_stats(int layer, int rgb[3], int *opaque)
{
    long sum[3] = {0};
    *opaque = 0;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            uint8_t px[4] = {0};
            texgen_texel(layer, x, y, px);
            for (int c = 0; c < 3; c++) sum[c] += px[c];
            *opaque += px[3] == 255;
        }
    for (int c = 0; c < 3; c++) rgb[c] = (int)(sum[c] / 256);
}

static int colour_gap(const int a[3], const int b[3])
{
    int d = 0;
    for (int c = 0; c < 3; c++) d += a[c] > b[c] ? a[c] - b[c] : b[c] - a[c];
    return d;
}

/* Every new block face is solid and its own colour; every new item sprite
 * has a shape (opaque and clear texels) and no two look alike. */
static void test_textures(void)
{
    int stone[3] = {0}, op = 0;
    layer_stats(T_STONE, stone, &op);
    for (int l = T_GRANITE; l <= T_GRAPHITE_ORE; l++) {
        int rgb[3] = {0};
        layer_stats(l, rgb, &op);
        CHECK(op == 256);
        if (l < T_TIN_ORE) CHECK(colour_gap(rgb, stone) > 30);
        for (int m = T_GRANITE; m < l; m++) {
            int other[3] = {0};
            layer_stats(m, other, &op);
            if (colour_gap(rgb, other) == 0) fprintf(stderr, "  layers %d and %d look alike\n", l, m);
            CHECK(colour_gap(rgb, other) > 0);
        }
    }
    /* Ores show their mineral: some texels differ from the rock around. */
    for (int l = T_TIN_ORE; l <= T_GRAPHITE_ORE; l++) {
        uint8_t a[4] = {0}, b[4] = {0};
        int varied = 0;
        texgen_texel(l, 0, 0, a);
        for (int i = 0; i < 256; i++) {
            texgen_texel(l, i % 16, i / 16, b);
            varied += abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2]) > 60;
        }
        CHECK(varied > 4);
    }
}

/* An arm of one material out from a pillar of it, `len` blocks long; 1 if
 * the support check then breaks the far end off. */
static int arm_breaks(uint8_t id, int len)
{
    test_world t = {0};
    tw_init(&t);
    int top = GROUND + 3;
    for (int y = GROUND; y <= top; y++) world_set(&t.w, 0, y, 0, id, 0);
    for (int i = 1; i <= len; i++) world_set(&t.w, i, top, 0, id, 0);
    int fell = physics_check_structure(&t.ph, len, top, 0);
    int gone = world_get(&t.w, len, top, 0) == B_AIR;
    tw_free(&t);
    return fell > 0 && gone;
}

/* Each laid rock and cohesive soil holds exactly its span out from a
 * pillar, and the next block breaks off; rock in place holds the 15 the
 * support check counts. */
static void test_spans(void)
{
    const uint8_t laid[] = {B_GRANITE, B_BASALT,   B_LIMESTONE, B_SANDSTONE, B_SHALE,
                            B_MARBLE,  B_OBSIDIAN, B_CLAY,      B_PEAT,      B_PODZOL};
    for (int k = 0; k < (int)(sizeof laid / sizeof laid[0]); k++) {
        int span = block_get(laid[k])->span;
        CHECK(span >= 1 && !arm_breaks(laid[k], span));
        CHECK(arm_breaks(laid[k], span + 1));
    }
    for (int i = 1; i < (int)(sizeof ROCKS / sizeof ROCKS[0]); i++) CHECK(!arm_breaks(ROCKS[i][0], 15));
    for (int i = 3; i < NORES; i++) CHECK(!arm_breaks(ORES[i].block, 15));
}

/* Stuck to the side of a wall, mud slumps like sand; clay sticks. */
static void test_mud_slumps(void)
{
    const uint8_t soils[2] = {B_MUD, B_CLAY};
    for (int k = 0; k < 2; k++) {
        test_world t = {0};
        tw_init(&t);
        world_set(&t.w, 5, GROUND, 5, B_STONE, 0);
        world_set(&t.w, 5, GROUND + 1, 5, B_STONE, 0);
        world_set(&t.w, 6, GROUND + 1, 5, soils[k], 0);
        int fell = physics_check_structure(&t.ph, 6, GROUND + 1, 5);
        CHECK(k == 0 ? fell == 1 : fell == 0);
        CHECK(world_get(&t.w, 6, GROUND + 1, 5) == (k == 0 ? B_AIR : B_CLAY));
        tw_free(&t);
    }
}

/* Dropped 5 m (9.9 m/s at the ground), laid obsidian shatters like the
 * glass it is; basalt from the same height lands whole. */
static void test_obsidian_shatters(void)
{
    const uint8_t rocks[2] = {B_OBSIDIAN, B_BASALT};
    for (int k = 0; k < 2; k++) {
        test_world t = {0};
        tw_init(&t);
        player p = {0};
        player_spawn(&p, &t.w, 20.5, 20.5);
        player_input idle = {0};
        body *b = &t.ph.bodies[t.ph.body_count++];
        *b = (body){.pos = dv3(2, GROUND + 5, 2), .prev_pos = dv3(2, GROUND + 5, 2), .block = rocks[k]};
        b->rot[3] = 1.0f;
        int shattered = 0, landed = 0;
        for (int i = 0; i < 240 && t.ph.body_count; i++) {
            physics_step(&t.ph, &p, &idle);
            shattered += t.ph.shatter_count;
        }
        for (int z = 0; z <= 4; z++)
            for (int x = 0; x <= 4; x++) landed += world_get(&t.w, x, GROUND, z) == rocks[k];
        CHECK(t.ph.body_count == 0);
        CHECK(k == 0 ? shattered == 1 && landed == 0 : shattered == 0 && landed == 1);
        tw_free(&t);
    }
}

/* A column holding every block, the new ones included, saves and loads
 * back unchanged: the decoder's id check covers the whole table. */
static void test_save_every_block(void)
{
    column *c = column_alloc(0, 0), *back = column_alloc(0, 0);
    memset(c->blocks, B_AIR, COL_VOL); /* column_alloc leaves the blocks to the generator */
    for (int b = 1; b < B_COUNT; b++)
        for (int z = 0; z < CHUNK_W; z++) c->blocks[col_index(b % CHUNK_W, 1 + b / CHUNK_W, z)] = (uint8_t)b;
    uint8_t *buf = mem_alloc(SAVE_MAX_FILE);
    size_t n = save_encode_column(c, buf, SAVE_MAX_FILE);
    CHECK(n > 0 && save_decode_column(back, buf, n) == 0);
    int differ = 0;
    for (int i = 0; i < COL_VOL; i++) differ += c->blocks[i] != back->blocks[i];
    CHECK(differ == 0);
    CHECK(back->blocks[col_index(B_GRAPHITE_ORE % CHUNK_W, 1 + B_GRAPHITE_ORE / CHUNK_W, 3)] == B_GRAPHITE_ORE);
    mem_free(buf);
    column_free(back);
    column_free(c);
}

/* The colours of a model, in box order, folded into one number. */
static uint32_t model_colours(item_id id)
{
    item_model m = {0};
    itemmodel_build(id, &m);
    uint32_t h = 2166136261u;
    for (int i = 0; i < m.n; i++) h = (h ^ m.box[i].rgb ^ (uint32_t)m.box[i].layer << 24) * 16777619u;
    return h;
}

/* Every mineral and metal looks like itself: no two share their colours,
 * and the ingots share one cast shape. */
static void test_sprite_shapes(void)
{
    for (item_id id = I_RAW_TIN; id < I_MINERALS_END; id++)
        for (item_id other = I_RAW_TIN; other < id; other++) {
            if (model_colours(id) == model_colours(other))
                fprintf(stderr, "  %s looks like %s\n", item_get(id)->name, item_get(other)->name);
            CHECK(model_colours(id) != model_colours(other));
        }
    item_model iron = {0};
    itemmodel_build(I_IRON_INGOT, &iron);
    for (item_id id = I_TIN_INGOT; id <= I_STEEL_INGOT; id++) {
        item_model m = {0};
        itemmodel_build(id, &m);
        CHECK(m.n == iron.n);
        for (int i = 0; i < m.n; i++)
            for (int a = 0; a < 3; a++) CHECK(m.box[i].size[a] == iron.box[i].size[a]);
    }
}

/* Breaks one block with `held` in hand, through the same frame loop as
 * play, and returns what fell out (an empty stack for nothing). */
static item_stack dig(uint8_t block, item_id held)
{
    test_world t = {0};
    tw_init(&t);
    world_set(&t.w, 4, GROUND, 4, block, 0);
    fx_state *fx = mem_calloc(1, sizeof *fx);
    fx_init(fx, 1);
    player pl = {0};
    pl.pos = dv3(4.5, GROUND + 1.0, 2.5);
    inventory inv = {0};
    inv_init(&inv);
    if (held) inv_add(&inv, held, 1);
    interact ia = {0};
    interact_init(&ia, 3);
    interact_input in = {.attack = 1, .can_act = 1, .speed = 1.0f, .dt = 1.0f / 60.0f};
    ray_hit hit = {.hit = 1, .block = {4, GROUND, 4}, .before = {4, GROUND, 3}, .id = block};
    vec3 dir = {0.0f, 0.0f, 1.0f};
    for (int i = 0; i < 9000 && world_get(&t.w, 4, GROUND, 4) == block; i++)
        interact_frame(&ia, &t.w, &t.ph, NULL, &pl, &inv, fx, pl.pos, dir, hit, &in);
    CHECK(world_get(&t.w, 4, GROUND, 4) == B_AIR);
    item_stack got = {0, 0, 0};
    if (t.ph.item_count) got = (item_stack){t.ph.items[0].id, t.ph.items[0].count, 0};
    mem_free(fx);
    tw_free(&t);
    return got;
}

/* In the world: granite comes away as laid granite, tin ore gives tin only
 * to a stone pickaxe or better, quartz only to iron, and rock salt crumbles
 * into two salt for any pickaxe. */
static void test_digging(void)
{
    item_id wood = item_tool_id(TOOL_PICKAXE, TIER_WOOD), stone = item_tool_id(TOOL_PICKAXE, TIER_STONE);
    item_id copper = item_tool_id(TOOL_PICKAXE, TIER_COPPER), iron = item_tool_id(TOOL_PICKAXE, TIER_IRON);
    CHECK(dig(B_GRANITE_ROCK, wood).id == B_GRANITE);
    CHECK(dig(B_GRANITE_ROCK, 0).count == 0);
    CHECK(dig(B_TIN_ORE, wood).count == 0);
    item_stack tin = dig(B_TIN_ORE, stone);
    CHECK(tin.id == I_RAW_TIN && tin.count == 1);
    CHECK(dig(B_QUARTZ_ORE, copper).count == 0 && dig(B_QUARTZ_ORE, iron).id == I_QUARTZ);
    item_stack salt = dig(B_ROCK_SALT, wood);
    CHECK(salt.id == I_SALT && salt.count == 2);
    CHECK(dig(B_CLAY, 0).id == B_CLAY);
}

void test_materials_all(void)
{
    test_rocks();
    test_ores();
    test_minerals();
    test_unique_names();
    test_smelting();
    test_bronze_from_ore();
    test_textures();
    test_spans();
    test_mud_slumps();
    test_obsidian_shatters();
    test_save_every_block();
    test_sprite_shapes();
    test_digging();
}
