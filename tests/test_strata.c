/* The ground under the soil: rock beds that run on from column to column,
 * a crystalline basement that rises into the ranges, each ore in the rock
 * it forms in, and soil whose depth follows the slope, climate and rock. */
#include "block.h"
#include "item.h"
#include "mem.h"
#include "test_util.h"
#include "world.h"
#include "worldgen.h"

#include <stdio.h>
#include <stdlib.h>

#define SEED 1337

/* Beds laid down in water or wind, as opposed to the basement under them. */
static int is_bed(uint8_t b)
{
    return b == B_LIMESTONE_ROCK || b == B_SANDSTONE_ROCK || b == B_SHALE_ROCK || b == B_MARBLE_ROCK ||
           b == B_ROCK_SALT;
}

static int is_basement(uint8_t b)
{
    return b == B_GRANITE_ROCK || b == B_BASALT_ROCK || b == B_ROCK || b == B_OBSIDIAN_ROCK;
}

/* Rock and ore in place: what the soil lies on. */
static int in_place(uint8_t b) { return block_get(b)->mat == BM_STONE && !item_valid(b); }

/* The rocks each ore forms in (gold and silver ride in quartz and galena). */
static int ore_host(uint8_t ore, uint8_t b)
{
    switch (ore) {
    case B_COAL_ORE: return b == B_SHALE_ROCK || b == B_SANDSTONE_ROCK;
    case B_COPPER_ORE: return b == B_GRANITE_ROCK || b == B_BASALT_ROCK || b == B_ROCK;
    case B_IRON_ORE: return b == B_ROCK || b == B_BASALT_ROCK || b == B_SANDSTONE_ROCK;
    case B_TIN_ORE:
    case B_LITHIUM_ORE: return b == B_GRANITE_ROCK;
    case B_ZINC_ORE:
    case B_LEAD_ORE: return b == B_LIMESTONE_ROCK || b == B_MARBLE_ROCK;
    case B_SILVER_ORE: return b == B_LIMESTONE_ROCK || b == B_MARBLE_ROCK || b == B_LEAD_ORE;
    case B_NICKEL_ORE: return b == B_BASALT_ROCK;
    case B_TITANIUM_ORE: return b == B_BASALT_ROCK || b == B_ROCK;
    case B_URANIUM_ORE: return b == B_GRANITE_ROCK || b == B_SANDSTONE_ROCK;
    case B_SULFUR_ORE: return b == B_BASALT_ROCK || b == B_ROCK_SALT;
    case B_QUARTZ_ORE: return b == B_GRANITE_ROCK || b == B_ROCK || b == B_SANDSTONE_ROCK;
    case B_GOLD_ORE: return b == B_QUARTZ_ORE || b == B_GRANITE_ROCK || b == B_ROCK || b == B_SANDSTONE_ROCK;
    case B_GRAPHITE_ORE: return b == B_MARBLE_ROCK || b == B_ROCK;
    default: return -1; /* not an ore */
    }
}

static int slope_of(int x, int z)
{
    int w = worldgen_height(SEED, x - 1, z), e = worldgen_height(SEED, x + 1, z);
    int n = worldgen_height(SEED, x, z - 1), s = worldgen_height(SEED, x, z + 1);
    return abs(w - e) + abs(n - s);
}

typedef struct {
    long kind[B_COUNT], tops[B_COUNT];
    long same, pairs, border_same, border_pairs; /* beds beside each other */
    long low, low_beds, high, high_base;         /* what lies 10 m down */
    long ores, hosted, salt_high, glass_deep;
    long flat, flat_depth, steep, steep_depth, too_deep;
} tally;

/* One column and its eastern neighbour, every block of them. */
static void survey(int cx, int cz, tally *t)
{
    column *c = column_alloc(cx, cz), *e = column_alloc(cx + 1, cz);
    worldgen_column(SEED, c);
    worldgen_column(SEED, e);
    for (int z = 0; z < CHUNK_W; z++)
        for (int x = 0; x < CHUNK_W; x++) {
            int wx = cx * CHUNK_W + x, wz = cz * CHUNK_W + z, h = worldgen_height(SEED, wx, wz);
            for (int y = 1; y < h; y++) {
                uint8_t b = c->blocks[col_index(x, y, z)];
                uint8_t east = x + 1 < CHUNK_W ? c->blocks[col_index(x + 1, y, z)] : e->blocks[col_index(0, y, z)];
                t->kind[b]++;
                if (is_bed(b) && is_bed(east)) {
                    *(x + 1 < CHUNK_W ? &t->pairs : &t->border_pairs) += 1;
                    *(x + 1 < CHUNK_W ? &t->same : &t->border_same) += b == east;
                }
                t->salt_high += b == B_ROCK_SALT && y >= SEA_LEVEL - 8;
                t->glass_deep += b == B_OBSIDIAN_ROCK && y < h - 8;
                if (ore_host(b, B_AIR) < 0 || x == 0 || x == CHUNK_W - 1 || z == 0 || z == CHUNK_W - 1) continue;
                const uint8_t *p = &c->blocks[col_index(x, y, z)];
                int host = ore_host(b, p[1]) || ore_host(b, p[-1]) || ore_host(b, p[CHUNK_W]) ||
                           ore_host(b, p[-CHUNK_W]) || ore_host(b, p[COL_AREA]) || ore_host(b, p[-COL_AREA]);
                t->ores++;
                t->hosted += host;
            }
            if (h <= SEA_LEVEL + 2) continue;
            uint8_t top = c->blocks[col_index(x, h - 1, z)];
            t->tops[top]++;
            if (h < SEA_LEVEL + 30) {
                t->low++;
                t->low_beds += is_bed(c->blocks[col_index(x, h - 10, z)]);
            }
            if (h > SEA_LEVEL + 100) {
                int y = h - 30;
                while (y > 1 && c->blocks[col_index(x, y, z)] == B_AIR) y--; /* under a cave */
                t->high++;
                t->high_base += is_basement(c->blocks[col_index(x, y, z)]);
            }
            /* Soil depth: down to the first rock. */
            if (top == B_AIR || in_place(top) || top == B_SAND || top == B_GRAVEL || top == B_SNOW) continue;
            int d = 0;
            while (d < 12 && !in_place(c->blocks[col_index(x, h - 1 - d, z)])) d++;
            t->too_deep += d > 5;
            int slope = slope_of(wx, wz);
            if (slope <= 1) {
                t->flat++;
                t->flat_depth += d;
            } else if (slope >= 3) {
                t->steep++;
                t->steep_depth += d;
            }
        }
    column_free(e);
    column_free(c);
}

/* Over 24 km: every rock, ore and soil turns up; beds run on between
 * blocks and across column borders; lowlands stand on beds and the ranges
 * on basement; salt only lies deep, obsidian only near the surface; ore
 * sits in its host rock; and soil thins on slopes. */
static void test_strata(void)
{
    tally *t = mem_calloc(1, sizeof *t);
    for (int gz = 0; gz < 10; gz++)
        for (int gx = 0; gx < 10; gx++) survey((gx * 2400 - 12000) / CHUNK_W + gz, (gz * 2400 - 12000) / CHUNK_W - gx, t);
    static const uint8_t MUST[] = {B_ROCK,        B_GRANITE_ROCK, B_BASALT_ROCK, B_LIMESTONE_ROCK, B_SANDSTONE_ROCK,
                                   B_SHALE_ROCK,  B_MARBLE_ROCK,  B_OBSIDIAN_ROCK, B_ROCK_SALT,    B_CLAY,
                                   B_PEAT,        B_PODZOL,       B_BAUXITE,     B_SALTPETER_ORE,  B_COAL_ORE,
                                   B_COPPER_ORE,  B_IRON_ORE};
    for (size_t i = 0; i < sizeof MUST; i++) {
        if (!t->kind[MUST[i]]) fprintf(stderr, "  no %s in 24 km\n", block_get(MUST[i])->name);
        CHECK(t->kind[MUST[i]] > 0);
    }
    for (int b = B_TIN_ORE; b <= B_GRAPHITE_ORE; b++) CHECK(t->kind[b] > 0);
    CHECK(t->kind[B_COAL_ORE] > t->kind[B_COPPER_ORE] && t->kind[B_QUARTZ_ORE] > t->kind[B_GOLD_ORE]);
    CHECK(t->pairs > 100000 && t->same > t->pairs * 98 / 100);
    CHECK(t->border_pairs > 10000 && t->border_same > t->border_pairs * 97 / 100);
    CHECK(t->low > 1000 && t->low_beds > t->low * 7 / 10);
    CHECK(t->high > 100 && t->high_base > t->high * 9 / 10);
    CHECK(t->ores > 1000 && t->hosted > t->ores * 98 / 100);
    CHECK(t->salt_high == 0 && t->glass_deep == 0);
    CHECK(t->too_deep == 0 && t->flat > 1000 && t->steep > 50);
    CHECK((double)t->flat_depth / (double)t->flat > (double)t->steep_depth / (double)t->steep + 0.75);
    CHECK(t->tops[B_PODZOL] > 0 && t->tops[B_GRASS] > t->tops[B_PODZOL]);
    mem_free(t);
}

/* In the nearest wetland, mud fills the hollows and the soil under the
 * marsh is peat or clay. */
static void test_wetland(void)
{
    int wx = 0, wz = 0, found = 0;
    for (int r = 0; r < 12000 && !found; r += 48)
        for (int a = 0; a < 8 && !found; a++) {
            int x = a & 1 ? r : -r, z = a & 2 ? r / 2 : -r / 2;
            if (a >= 4) {
                int tmp = x;
                x = z;
                z = tmp;
            }
            found = worldgen_sample(SEED, x, z).b == BIOME_WETLAND;
            wx = x;
            wz = z;
        }
    CHECK(found);
    long mud = 0, wet_soil = 0, marsh = 0;
    int cx0 = (wx >= 0 ? wx : wx - CHUNK_W + 1) / CHUNK_W, cz0 = (wz >= 0 ? wz : wz - CHUNK_W + 1) / CHUNK_W;
    for (int cz = cz0 - 2; cz <= cz0 + 2; cz++)
        for (int cx = cx0 - 2; cx <= cx0 + 2; cx++) {
            column *c = column_alloc(cx, cz);
            worldgen_column(SEED, c);
            for (int z = 0; z < CHUNK_W; z++)
                for (int x = 0; x < CHUNK_W; x++) {
                    worldgen_point p = worldgen_sample(SEED, cx * CHUNK_W + x, cz * CHUNK_W + z);
                    if (p.b != BIOME_WETLAND) continue;
                    marsh++;
                    mud += c->blocks[col_index(x, p.height - 1, z)] == B_MUD;
                    uint8_t under = c->blocks[col_index(x, p.height - 2, z)];
                    wet_soil += under == B_PEAT || under == B_CLAY;
                }
            column_free(c);
        }
    CHECK(marsh > 50 && mud > 0 && wet_soil > marsh / 2);
}

void test_strata_all(void)
{
    test_strata();
    test_wetland();
}
