/* Terrain generation: the first generator unchanged for old worlds, caves
 * and ore in the second, and caves that stand up to the collapse check. */
#include "inventory.h"
#include "item.h"
#include "jobs.h"
#include "physics.h"
#include "test_util.h"
#include "world.h"

#include <stdlib.h>

static uint64_t hash_columns(void (*gen)(uint32_t, column *), uint32_t seed)
{
    uint64_t h = 1469598103934665603ull;
    for (int z = 0; z < 16; z++)
        for (int x = 0; x < 16; x++) {
            column *c = column_alloc(x - 8, z - 8);
            gen(seed, c);
            for (int i = 0; i < COL_VOL; i++) h = (h ^ c->blocks[i]) * 1099511628211ull;
            column_free(c);
        }
    return h;
}

/* The ore bands in worldgen.c. */
static int ore_band_ok(uint8_t b, int y)
{
    if (b == B_COAL_ORE) return y >= 8 - 9 && y < 110 + 9;
    if (b == B_COPPER_ORE) return y >= 16 - 7 && y < 72 + 7;
    if (b == B_IRON_ORE) return y >= 4 - 6 && y < 54 + 6;
    return 1;
}

static void test_generators(void)
{
    /* Worlds made before caves keep growing exactly as they began: this is
     * the hash of the generator as it was before version 2. */
    CHECK(hash_columns(worldgen_column_v1, 1337) == 0x34b58b19e1292375ull);
    CHECK(hash_columns(worldgen_column, 1337) != 0x34b58b19e1292375ull);

    long ore[3] = {0}, cave = 0, under = 0, bad_band = 0, hanging = 0, mouths = 0, no_bedrock = 0;
    for (int cz = -3; cz < 3; cz++)
        for (int cx = -3; cx < 3; cx++) {
            column *c = column_alloc(cx, cz);
            worldgen_column(1337, c);
            for (int z = 0; z < CHUNK_W; z++)
                for (int x = 0; x < CHUNK_W; x++) {
                    int h = worldgen_height(1337, cx * CHUNK_W + x, cz * CHUNK_W + z);
                    no_bedrock += c->blocks[col_index(x, 0, z)] != B_BEDROCK;
                    for (int y = 1; y < WORLD_H - 1; y++) {
                        uint8_t b = c->blocks[col_index(x, y, z)], up = c->blocks[col_index(x, y + 1, z)];
                        if (b >= B_COAL_ORE && b <= B_IRON_ORE) ore[b - B_COAL_ORE]++;
                        if (!ore_band_ok(b, y)) bad_band++;
                        if (y < h) under++;
                        if (b == B_AIR && y < h - 5) cave++;
                        /* A hole under water would flood, under sand it would
                         * pour in: neither is ever carved. */
                        if (b == B_AIR && y < h && (up == B_WATER || (block_get(up)->flags & BF_GRANULAR))) hanging++;
                    }
                    if (h > SEA_LEVEL + 3 && c->blocks[col_index(x, h - 1, z)] == B_AIR) mouths++;
                }
            column_free(c);
        }
    CHECK(ore[0] > ore[1] && ore[1] > 0 && ore[2] > 0); /* coal the most common */
    CHECK(ore[0] + ore[1] + ore[2] < under / 50);
    CHECK(bad_band == 0);
    CHECK(cave > under / 50 && cave < under / 5);
    CHECK(hanging == 0);
    CHECK(mouths > 0);
    CHECK(no_bedrock == 0);
}

/* Digging into a cave must not bring its roof down: every roof over air
 * (caves, overhangs, tree canopies) is checked the way an edit beside it
 * would check it. */
static void test_caves_stand(void)
{
    jobs *js = jobs_create(0);
    world w = {0};
    world_init(&w, 1337, 4, js, NULL);
    world_load_blocking(&w, 8.0, 8.0, 3);
    physics ph = {0};
    physics_init(&ph, &w);
    int checked = 0, fell = 0;
    for (int z = -24; z < 40; z += 3)
        for (int x = -24; x < 40; x += 3)
            for (int y = 4; y < WORLD_H - 1; y++) {
                uint8_t roof = world_get(&w, x, y + 1, z);
                if (world_get(&w, x, y, z) != B_AIR || !block_solid(roof) || roof == B_UNLOADED) continue;
                fell += physics_check_structure(&ph, x, y + 1, z);
                checked++;
            }
    CHECK(checked > 200);
    CHECK(fell == 0);
    physics_destroy(&ph);
    world_destroy(&w);
    jobs_destroy(js);
}

static void test_smelting(void)
{
    int furnace = -1, iron = -1;
    for (int r = 0; r < g_recipe_count; r++) {
        if (g_recipes[r].out.id == B_FURNACE) furnace = r;
        if (g_recipes[r].out.id == I_IRON_INGOT) iron = r;
    }
    CHECK(furnace >= 0 && iron >= 0);
    inventory inv = {0};
    inv_init(&inv);
    inv_add(&inv, B_STONE, 8);
    CHECK(inv_craft(&inv, furnace, 0) && inv_count(&inv, B_FURNACE) == 1);
    inv_add(&inv, I_RAW_IRON, 1);
    inv_add(&inv, I_COAL, 2);
    /* A campfire is not hot enough; a furnace is. */
    CHECK(!inv_can_craft(&inv, iron, 0) && !inv_can_craft(&inv, iron, STATION_FIRE));
    CHECK(inv_craft(&inv, iron, STATION_FURNACE) && inv_count(&inv, I_IRON_INGOT) == 1 && inv_count(&inv, I_COAL) == 0);

    /* Rock gives stone, ores coal or raw metal to smelt. */
    uint32_t rng = 7;
    item_stack out[3] = {{0}};
    CHECK(item_drops(B_COAL_ORE, &rng, out) == 1 && out[0].id == I_COAL && out[0].count >= 1);
    CHECK(item_drops(B_IRON_ORE, &rng, out) == 1 && out[0].id == I_RAW_IRON);
    CHECK(item_drops(B_ROCK, &rng, out) == 1 && out[0].id == B_STONE);
    CHECK(item_valid(I_COAL) && item_valid(B_FURNACE) && item_valid(I_RAW_COPPER));
    CHECK(!item_valid(B_ROCK) && !item_valid(B_IRON_ORE)); /* only ever in place */
}

void test_worldgen_all(void)
{
    test_generators();
    test_caves_stand();
    test_smelting();
}
