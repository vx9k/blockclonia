/* Terrain generation: the noise it is built from, the climate and its
 * biomes, the columns with their ore and caves, and caves that stand up
 * to the collapse check. */
#include "inventory.h"
#include "item.h"
#include "jobs.h"
#include "mem.h"
#include "noise.h"
#include "physics.h"
#include "test_util.h"
#include "texgen.h"
#include "thermo.h"
#include "world.h"
#include "worldgen.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint64_t hash_columns(uint32_t seed)
{
    uint64_t h = 1469598103934665603ull;
    for (int z = 0; z < 16; z++)
        for (int x = 0; x < 16; x++) {
            column *c = column_alloc(x - 8, z - 8);
            worldgen_column(seed, c);
            for (int i = 0; i < COL_VOL; i++) h = (h ^ c->blocks[i]) * 1099511628211ull;
            column_free(c);
        }
    return h;
}

/* The ore bands in worldgen.c, widened by how far a vein can wander. */
static int ore_band_ok(uint8_t b, int y)
{
    static const struct {
        uint8_t block;
        int lo, hi;
    } BAND[] = {
        {B_COAL_ORE, 8 - 9, 180 + 9},     {B_COPPER_ORE, 4 - 7, 160 + 7},  {B_IRON_ORE, 4 - 6, 80 + 6},
        {B_TIN_ORE, 8 - 5, 160 + 5},      {B_ZINC_ORE, 8 - 6, 150 + 6},    {B_LEAD_ORE, 8 - 6, 150 + 6},
        {B_SILVER_ORE, 8 - 6, 90 + 6},    {B_NICKEL_ORE, 4 - 5, 100 + 5},  {B_TITANIUM_ORE, 4 - 5, 120 + 5},
        {B_URANIUM_ORE, 0, 100 + 4},      {B_LITHIUM_ORE, 8 - 6, 160 + 6}, {B_SULFUR_ORE, 20 - 5, 240 + 5},
        {B_QUARTZ_ORE, 4 - 10, 200 + 10}, {B_GOLD_ORE, 4 - 10, 140 + 10},  {B_GRAPHITE_ORE, 8 - 5, 180 + 5},
    };
    for (size_t i = 0; i < sizeof BAND / sizeof BAND[0]; i++)
        if (b == BAND[i].block) return y >= BAND[i].lo && y < BAND[i].hi;
    return 1;
}

/* Rock and ore in place: what a cliff or a crag shows. */
static int bare_rock(uint8_t b) { return block_get(b)->mat == BM_STONE && !item_valid(b); }

/* What may stand on the ground: nothing that belongs to the ground. */
static int above_ground(uint8_t b)
{
    return b == B_AIR || b == B_WATER || b == B_ICE || block_log(b) || block_leaves(b) || block_plant(b);
}

/* The derivative noise2d_grad reports is the slope of noise2d. */
static void test_noise_grad(void)
{
    float worst = 0.0f;
    for (int i = 0; i < 500; i++) {
        double x = i * 0.731 - 40.0, z = i * 0.377 + 1e6, e = 1e-3;
        float dx = 0.0f, dz = 0.0f;
        float n = noise2d_grad(9, x, z, &dx, &dz);
        float fx = (noise2d(9, x + e, z) - noise2d(9, x - e, z)) / (float)(2.0 * e);
        float fz = (noise2d(9, x, z + e) - noise2d(9, x, z - e)) / (float)(2.0 * e);
        CHECK(n == noise2d(9, x, z));
        worst = fmaxf(worst, fmaxf(fabsf(fx - dx), fabsf(fz - dz)));
    }
    CHECK(worst < 0.01f);
}

/* The climate changes smoothly, every biome turns up within the 24 km
 * around the origin for most seeds, and the air thermo works out at the
 * ground is the climate the biome was chosen by. */
static void test_climate(void)
{
    float dt = 0.0f, dr = 0.0f;
    for (int i = 0; i < 300; i++) {
        int x = i * 397 - 70000, z = i * 541 - 90000;
        worldgen_point a = worldgen_sample(1337, x, z), b = worldgen_sample(1337, x + 1, z);
        worldgen_point c = worldgen_sample(1337, x, z + 1);
        float t = worldgen_sea_temp(1337, x, z);
        dt = fmaxf(dt,
                   fmaxf(fabsf(worldgen_sea_temp(1337, x + 1, z) - t), fabsf(worldgen_sea_temp(1337, x, z + 1) - t)));
        dr = fmaxf(dr, fmaxf(fabsf(b.rain - a.rain), fabsf(c.rain - a.rain)));
        CHECK(a.rain >= 0.0f && a.rain <= 1.0f && (int)a.b >= 0 && a.b < BIOME_COUNT);
    }
    CHECK(dt < 0.05f && dr < 0.02f);

    int full = 0;
    for (uint32_t seed = 1; seed <= 4; seed++) {
        int seen[BIOME_COUNT] = {0}, kinds = 0;
        for (int z = -12000; z < 12000; z += 300)
            for (int x = -12000; x < 12000; x += 300) seen[worldgen_sample(seed, x, z).b] = 1;
        for (int b = 0; b < BIOME_COUNT; b++) kinds += seen[b];
        full += kinds == BIOME_COUNT;
    }
    CHECK(full >= 3);
    for (int b = 0; b < BIOME_COUNT; b++) CHECK(biome_name((biome)b) != NULL);

    test_world tw = {0};
    tw_init(&tw);
    thermo *th = thermo_create(1337);
    thermo_set_day_time(th, 0.375); /* no diurnal swing */
    thermo_set_climate(th, 1);
    float worst = 0.0f, tundra = -99.0f, desert = 99.0f;
    for (int i = 0; i < 600; i++) {
        int x = i * 7919 % 40000 - 20000, z = i * 6151 % 40000 - 20000;
        worldgen_point p = worldgen_sample(1337, x, z);
        if (p.height < SEA_LEVEL) continue;
        /* Far from the loaded columns, so the air is open. */
        float air = thermo_ambient(th, &tw.w, x + 0.5, p.height, z + 0.5);
        worst = fmaxf(worst, fabsf(air - p.temp));
        if (p.b == BIOME_TUNDRA) tundra = fmaxf(tundra, air);
        if (p.b == BIOME_DESERT) desert = fminf(desert, air);
    }
    CHECK(worst < 1.6f); /* the biome's own jitter, up to 1.5 K */
    CHECK(tundra > -99.0f && tundra < 5.0f);
    CHECK(desert < 99.0f && desert > 10.0f);
    thermo_destroy(th);
    tw_free(&tw);
}

static void test_generators(void)
{
    /* Worlds keep growing exactly as they began. */
    CHECK(hash_columns(1337) == 0x49a91d26b64c5f7dull);

    long ore[3] = {0}, cave = 0, under = 0, bad_band = 0, hanging = 0, no_bedrock = 0, bad_ground = 0;
    long mouths = 0, mouth_cols = 0, cols = 0, fox = 0, bad_fox = 0;
    /* Around the origin, and where seed 1337 has its nearest cave mouths. */
    static const int AREA[][4] = {{-3, -3, 3, 3}, {4, 0, 8, 4}};
    for (int a = 0; a < 2; a++)
        for (int cz = AREA[a][1]; cz < AREA[a][3]; cz++)
            for (int cx = AREA[a][0]; cx < AREA[a][2]; cx++) {
                column *c = column_alloc(cx, cz);
                worldgen_column(1337, c);
                long had = mouths;
                for (int z = 0; z < CHUNK_W; z++)
                    for (int x = 0; x < CHUNK_W; x++) {
                        int h = worldgen_height(1337, cx * CHUNK_W + x, cz * CHUNK_W + z);
                        no_bedrock += c->blocks[col_index(x, 0, z)] != B_BEDROCK;
                        /* The ground ends where worldgen_height says, in
                         * every column, so their borders meet. */
                        bad_ground += !above_ground(c->blocks[col_index(x, h, z)]);
                        for (int y = 1; y < WORLD_H - 1; y++) {
                            uint8_t b = c->blocks[col_index(x, y, z)], up = c->blocks[col_index(x, y + 1, z)];
                            if (b >= B_COAL_ORE && b <= B_IRON_ORE) ore[b - B_COAL_ORE]++;
                            if (!ore_band_ok(b, y)) bad_band++;
                            if (y < h) under++;
                            if (b == B_AIR && y < h - 5) cave++;
                            /* Foxfire only on deep cave floors. */
                            if (b == B_FOXFIRE) {
                                fox++;
                                bad_fox += up != B_AIR || y >= h - 25;
                            }
                            /* A hole under water would flood, under sand it
                             * would pour in: neither is ever carved. */
                            if (b == B_AIR && y < h && (up == B_WATER || (block_get(up)->flags & BF_GRANULAR)))
                                hanging++;
                        }
                        if (h > SEA_LEVEL + 3 && c->blocks[col_index(x, h - 1, z)] == B_AIR) mouths++;
                        else bad_ground += h > 0 && c->blocks[col_index(x, h - 1, z)] == B_AIR;
                    }
                mouth_cols += mouths > had;
                cols++;
                column_free(c);
            }
    CHECK(ore[0] > ore[1] && ore[1] > 0 && ore[2] > 0); /* coal the most common */
    CHECK(ore[0] + ore[1] + ore[2] < under / 50);
    CHECK(bad_band == 0);
    CHECK(cave > under / 50 && cave < under / 8);
    CHECK(hanging == 0);
    CHECK(fox > 0 && fox < cave / 20 && bad_fox == 0);
    CHECK(no_bedrock == 0 && bad_ground == 0);
    /* Caves break out of the ground, but seldom. */
    CHECK(mouths > 0 && mouth_cols * 4 < cols);
}

/* The noise stays in its range, gives the same value for the same input
 * and a different field for a different seed. */
/* The finer octaves at the world's edge sample lattice cells past INT_MAX.
 * Cells count modulo 2^32, so the noise repeats there instead of
 * overflowing an int. */
static void test_noise_wrap(void)
{
    const double wrap = 4294967296.0;
    for (int i = 0; i < 64; i++) {
        double x = i * 0.375 - 12.0, z = i * 0.625 + 3.0, y = i * 0.125; /* exact at 2^32 too */
        float gx = 0.0f, gz = 0.0f, hx = 0.0f, hz = 0.0f;
        CHECK(noise2d(5, x, z) == noise2d(5, x + wrap, z - wrap));
        CHECK(noise3d(5, x, y, z) == noise3d(5, x - wrap, y + wrap, z + wrap));
        CHECK(noise2d_grad(5, x, z, &gx, &gz) == noise2d_grad(5, x + 2.0 * wrap, z, &hx, &hz));
        CHECK(gx == hx && gz == hz);
    }
    /* 2e9 blocks out at a 90-block scale, 12 octaves reach 2^42 cells. */
    float e = eroded2d(5, 2e9 / 90.0, -2e9 / 90.0, 12), f = fbm2d(5, 2e9 / 90.0, 2e9 / 90.0, 12);
    CHECK(e >= -1.0f && e <= 1.0f && f >= -1.0f && f <= 1.0f);
}

static void test_noise_ranges(void)
{
    float lo2 = 0.0f, hi2 = 0.0f, lo3 = 0.0f, hi3 = 0.0f, fbm = 0.0f, ero = 0.0f;
    int same = 0, differ = 0;
    for (int i = 0; i < 4000; i++) {
        double x = i * 0.913 - 1800.0, y = i * 0.271, z = i * 1.377 + 3e8;
        float n2 = noise2d(5, x, z), n3 = noise3d(5, x, y, z);
        lo2 = fminf(lo2, n2);
        hi2 = fmaxf(hi2, n2);
        lo3 = fminf(lo3, n3);
        hi3 = fmaxf(hi3, n3);
        fbm = fmaxf(fbm, fabsf(fbm2d(5, x / 7.0, z / 7.0, 5)));
        ero = fmaxf(ero, fabsf(eroded2d(5, x / 7.0, z / 7.0, 7)));
        same += n2 == noise2d(5, x, z) && n3 == noise3d(5, x, y, z);
        differ += fabsf(n2 - noise2d(6, x, z)) > 1e-3f;
    }
    CHECK(lo2 >= -1.0f && hi2 <= 1.0f && lo2 < -0.5f && hi2 > 0.5f);
    CHECK(lo3 >= -1.2f && hi3 <= 1.2f && lo3 < -0.4f && hi3 > 0.4f);
    CHECK(fbm <= 1.0f && fbm > 0.3f);
    /* Eroded octaves damp each other: the field stays well inside +-1. */
    CHECK(ero < 0.8f && ero > 0.1f);
    CHECK(same == 4000 && differ > 3600);
}

/* Steepness at one block from its four neighbours, as worldgen.c sees it. */
static int slope_at(uint32_t seed, int x, int z, int h, int *rise)
{
    int w = worldgen_height(seed, x - 1, z), e = worldgen_height(seed, x + 1, z);
    int n = worldgen_height(seed, x, z - 1), s = worldgen_height(seed, x, z + 1);
    int hi = w > e ? w : e;
    hi = n > hi ? n : hi;
    hi = s > hi ? s : hi;
    *rise = hi - h;
    int slope = abs(w - e) + abs(n - s);
    return slope > 64 ? 64 : slope;
}

typedef struct {
    long seen[BIOME_COUNT], wrong, warm_snow, cold_grass, bad_ice;
    long cliffs, scree, sand_beach, desert_sand, snow, ice, beds;
} soil_tally;

/* What the top block of the ground should be (B_ROCK for any bare rock),
 * or B_AIR where the biome leaves a choice (grass or snow by the air, dirt or grass in scrub). */
static uint8_t soil_want(worldgen_point p, int slope, int rise, soil_tally *t)
{
    if (p.height < SEA_LEVEL) return slope > 5 ? B_ROCK : p.height <= SEA_LEVEL - 5 ? B_GRAVEL : B_SAND;
    if (slope > 5) {
        t->cliffs++;
        return B_ROCK;
    }
    if (p.b == BIOME_BEACH) return slope <= 2 ? B_SAND : B_GRAVEL;
    if (slope >= 3 && rise >= 3 && p.b != BIOME_DESERT) {
        t->scree++;
        return B_GRAVEL;
    }
    if (p.b == BIOME_DESERT) t->desert_sand++;
    return p.b == BIOME_DESERT ? B_SAND : p.b == BIOME_ROCKY ? B_ROCK : p.b == BIOME_SNOW ? B_SNOW : B_AIR;
}

/* Checks the blocks of one column STEP apart against the sampled climate. */
static void soil_column(int cx, int cz, int step, soil_tally *t)
{
    column *c = column_alloc(cx, cz);
    worldgen_column(1337, c);
    for (int z = 2; z < CHUNK_W - 1; z += step)
        for (int x = 2; x < CHUNK_W - 1; x += step) {
            int wx = cx * CHUNK_W + x, wz = cz * CHUNK_W + z, rise = 0;
            worldgen_point p = worldgen_sample(1337, wx, wz);
            int h = p.height, slope = slope_at(1337, wx, wz, h, &rise);
            uint8_t top = c->blocks[col_index(x, h - 1, z)];
            t->seen[p.b]++;
            if (top == B_AIR) continue; /* a cave mouth */
            if (p.level > SEA_LEVEL) {
                /* An inland river or lake: a bed of gravel, sand or clay. */
                t->beds++;
                t->wrong += top != B_GRAVEL && top != B_SAND && top != B_CLAY;
                continue;
            }
            uint8_t want = soil_want(p, slope, rise, t);
            t->wrong += want != B_AIR && (want == B_ROCK ? !bare_rock(top) : top != want);
            t->sand_beach += p.b == BIOME_BEACH && top == B_SAND;
            t->snow += top == B_SNOW;
            t->warm_snow += top == B_SNOW && p.temp >= 0.0f;
            t->cold_grass += top == B_GRASS && p.temp < -6.0f;
            if (h < SEA_LEVEL - 1) {
                uint8_t surf = c->blocks[col_index(x, SEA_LEVEL - 1, z)];
                t->ice += surf == B_ICE;
                t->bad_ice += (surf == B_ICE) != (p.temp < -2.0f) || (surf != B_ICE && surf != B_WATER);
            }
        }
    column_free(c);
}

/* The ground's top block follows from the biome and the slope at it: bare
 * rock on cliffs, scree at the foot of a rise, sand or shingle on beaches,
 * sand in deserts, and snow only where the air is below freezing, so the
 * snow line is the 0 C isotherm. Cold seas freeze over. */
static void test_soil(void)
{
    soil_tally t = {0};
    /* Columns scattered over 24 km, then a patch of mesa country. */
    for (int gz = 0; gz < 12; gz++)
        for (int gx = 0; gx < 12; gx++)
            soil_column((gx * 2000 - 11000) / CHUNK_W + gz, (gz * 2000 - 11000) / CHUNK_W - gx, 4, &t);
    for (int cz = 40; cz < 44; cz++)
        for (int cx = -64; cx < -60; cx++) soil_column(cx, cz, 2, &t);
    CHECK(t.wrong == 0 && t.warm_snow == 0 && t.cold_grass == 0 && t.bad_ice == 0);
    CHECK(t.cliffs > 0 && t.scree > 0 && t.sand_beach > 0 && t.desert_sand > 0 && t.snow > 0 && t.ice > 0);
    CHECK(t.seen[BIOME_OCEAN] && t.seen[BIOME_DESERT] && t.seen[BIOME_TUNDRA] && t.seen[BIOME_FOREST]);
}

/* Trees grow by the rain: thick in forest and taiga, sparse on the
 * steppe, none in the desert or the sea. */
/* Which wood a trunk is, by the climate it grows in: temperate (forest,
 * meadow, wetland), cold (taiga, alpine) and dry (scrub, steppe). */
static int tree_climate(biome b)
{
    if (b == BIOME_TAIGA || b == BIOME_ALPINE) return 1;
    if (b == BIOME_SCRUB || b == BIOME_STEPPE) return 2;
    return 0;
}

static void test_trees(void)
{
    long trunks[BIOME_COUNT] = {0}, ground[BIOME_COUNT] = {0}, kind[3][B_COUNT] = {{0}};
    long young = 0, tall = 0, hollow = 0, standing = 0;
    for (int i = 0; i < 160; i++) {
        int cx = (i * 1187) % 1500 - 750, cz = (i * 1733) % 1500 - 750;
        column *c = column_alloc(cx, cz);
        worldgen_column(1337, c);
        for (int z = 2; z < CHUNK_W - 2; z += 2)
            for (int x = 2; x < CHUNK_W - 2; x += 2) {
                worldgen_point p = worldgen_sample(1337, cx * CHUNK_W + x, cz * CHUNK_W + z);
                ground[p.b]++;
                trunks[p.b] += p.height < WORLD_H && block_log(c->blocks[col_index(x, p.height, z)]);
            }
        /* Every trunk, by its foot on the ground: its kind by climate, its
         * height, and whether it has leaves at all. */
        for (int z = 0; z < CHUNK_W; z++)
            for (int x = 0; x < CHUNK_W; x++)
                for (int y = 2; y < WORLD_H - 2; y++) {
                    uint8_t b = c->blocks[col_index(x, y, z)], under = c->blocks[col_index(x, y - 1, z)];
                    if (!block_log(b) || block_log(under) || block_leaves(under) || under == B_AIR) continue;
                    int top = y;
                    while (top < WORLD_H - 1 && block_log(c->blocks[col_index(x, top + 1, z)])) top++;
                    worldgen_point p = worldgen_sample(1337, cx * CHUNK_W + x, cz * CHUNK_W + z);
                    kind[tree_climate(p.b)][b]++;
                    young += top - y < 4;
                    tall += top - y >= 7;
                    /* A standing trunk has rock under it as far as a
                     * structure check looks, so no dig nearby drops it. */
                    /* cppcheck-suppress knownConditionTrueFalse ; the loop above grows top */
                    if (top - y >= 2) {
                        int under_air = 0;
                        for (int d = 1; d <= STRUCT_RADIUS + 1 && y - d > 0; d++)
                            under_air += c->blocks[col_index(x, y - d, z)] == B_AIR;
                        hollow += under_air > 0;
                        standing++;
                    }
                    break;
                }
        column_free(c);
    }
    /* Oak and birch in the temperate woods, conifers in the cold, the
     * thorn tree only where it is dry. */
    CHECK(kind[0][B_LOG] > 0 && kind[0][B_BIRCH_LOG] > 0);
    CHECK(kind[1][B_SPRUCE_LOG] > kind[1][B_LOG] && kind[1][B_PINE_LOG] > 0);
    CHECK(kind[0][B_THORN_LOG] == 0 && kind[1][B_THORN_LOG] == 0 && kind[1][B_LOG] == 0);
    CHECK(kind[2][B_THORN_LOG] > 0);
    /* Mostly grown trees, some young and short, a few tall. (Snags and
     * fallen trunks: test_trees.c.) */
    long all = 0;
    for (int q = 0; q < 3; q++)
        for (int b = 0; b < B_COUNT; b++) all += kind[q][b];
    CHECK(young > 0 && tall > 0 && young < all / 2 && all > 60);
    CHECK(standing > 40 && hollow == 0);
    double forest = (double)(trunks[BIOME_FOREST] + trunks[BIOME_TAIGA]) /
                    (double)(ground[BIOME_FOREST] + ground[BIOME_TAIGA] + 1);
    double steppe = (double)trunks[BIOME_STEPPE] / (double)(ground[BIOME_STEPPE] + 1);
    CHECK(ground[BIOME_FOREST] + ground[BIOME_TAIGA] > 200 && ground[BIOME_STEPPE] > 200);
    CHECK(forest > 0.003 && forest > 4.0 * steppe);
    CHECK(trunks[BIOME_DESERT] == 0 && ground[BIOME_DESERT] > 0 && trunks[BIOME_OCEAN] == 0);
}

/* Near the surface a cave is only ever an opening from the sky down, so
 * no soil hangs over a hole; deeper, some caves open into chambers. */
static void test_cave_shapes(void)
{
    long hanging = 0, chambers = 0;
    for (int cz = 4; cz < 8; cz++)
        for (int cx = -2; cx < 8; cx++) {
            column *c = column_alloc(cx, cz % 4 + (cx < 4 ? -2 : 0));
            worldgen_column(1337, c);
            for (int z = 0; z < CHUNK_W; z++)
                for (int x = 0; x < CHUNK_W; x++) {
                    int h = worldgen_height(1337, c->cx * CHUNK_W + x, c->cz * CHUNK_W + z), open = 1;
                    for (int y = h - 1; y >= h - 9 && y > 0; y--) {
                        int air = c->blocks[col_index(x, y, z)] == B_AIR;
                        hanging += air && !open;
                        open = open && air;
                    }
                    int run = 0, tall = 0;
                    for (int y = 7; y < h - 12; y++) {
                        run = c->blocks[col_index(x, y, z)] == B_AIR ? run + 1 : 0;
                        tall = tall || run >= 6;
                    }
                    chambers += tall;
                }
            column_free(c);
        }
    CHECK(hanging == 0);
    CHECK(chambers > 50);
}

/* The shape of the land over 24 km: some sea, mountains reaching high
 * but kept under the soft cap, wetter coasts and dry lee slopes. */
static void test_landforms(void)
{
    long n = 0, sea = 0, high = 0, top = 0;
    double coast = 0.0, inland = 0.0, lee = 0.0, open = 0.0;
    long ncoast = 0, ninland = 0, nlee = 0, nopen = 0;
    for (int z = -12000; z < 12000; z += 400)
        for (int x = -12000; x < 12000; x += 400) {
            worldgen_point p = worldgen_sample(1337, x, z);
            n++;
            sea += p.height < SEA_LEVEL;
            high += p.height > SEA_LEVEL + 100;
            top = p.height > top ? p.height : top;
            if (p.height < SEA_LEVEL || p.height > SEA_LEVEL + 40) continue;
            int wet = 0, upwind = 0;
            for (int k = 1; k <= 4; k++) {
                wet += worldgen_sample(1337, x + (k & 1 ? 150 : -150) * (k < 3), z + (k & 1 ? 150 : -150) * (k >= 3))
                           .height < SEA_LEVEL;
                int u = worldgen_height(1337, x - 250 * k, z);
                upwind = u > upwind ? u : upwind;
            }
            double rain = (double)p.rain;
            if (wet) {
                coast += rain;
                ncoast++;
            } else {
                inland += rain;
                ninland++;
            }
            if (upwind > SEA_LEVEL + 130) {
                lee += rain;
                nlee++;
            } else if (upwind < SEA_LEVEL + 50) {
                open += rain;
                nopen++;
            }
        }
    coast /= (double)(ncoast + !ncoast);
    inland /= (double)(ninland + !ninland);
    lee /= (double)(nlee + !nlee);
    open /= (double)(nopen + !nopen);
    CHECK(sea * 20 > n && sea * 2 < n);
    CHECK(high * 50 > n && high * 5 < n);
    CHECK(top > SEA_LEVEL + 160 && top <= SEA_LEVEL + 250);
    CHECK(ncoast > 20 && coast > inland + 0.03);
    CHECK(nlee > 10 && lee < open - 0.1);
}

/* A point sampled on its own has the height its column is built to, the
 * same every time, and another seed grows another land. */
static void test_sample(void)
{
    int differ = 0;
    for (int i = 0; i < 300; i++) {
        int x = i * 3571 % 30000 - 15000, z = i * 2917 % 30000 - 15000;
        worldgen_point p = worldgen_sample(1337, x, z), q = worldgen_sample(1337, x, z);
        CHECK(p.height == worldgen_height(1337, x, z));
        CHECK(p.height == q.height && p.temp == q.temp && p.rain == q.rain && p.b == q.b);
        CHECK(p.height >= 4 && p.height <= WORLD_H - 12);
        worldgen_point o = worldgen_sample(1338, x, z);
        differ += o.b != p.b || o.height != p.height;
    }
    CHECK(differ > 250);
}

/* The first block of an inland river or lake found walking out from the
 * origin in 64 m steps, or 0 when there is none within 8 km. */
static int find_river(uint32_t seed, int *rx, int *rz)
{
    for (int r = 0; r < 8000; r += 64)
        for (int a = -r; a <= r; a += 64) {
            int pts[4][2] = {{a, -r}, {a, r}, {-r, a}, {r, a}};
            for (int k = 0; k < 4; k++)
                if (worldgen_sample(seed, pts[k][0], pts[k][1]).level > SEA_LEVEL) {
                    *rx = pts[k][0];
                    *rz = pts[k][1];
                    return 1;
                }
        }
    return 0;
}

/* Rivers and lakes run above the sea, and their water lies still as
 * generated: over the 5 x 5 columns around one, no water block has air
 * beside it or below it, across column borders too, and every column has
 * the ground and water the single-point query says. */
static void test_rivers(void)
{
    int rx = 0, rz = 0;
    CHECK(find_river(1337, &rx, &rz));
    int cx0 = (int)floor(rx / (double)CHUNK_W) - 2, cz0 = (int)floor(rz / (double)CHUNK_W) - 2;
    enum { N = 5, W = N * CHUNK_W };
    uint8_t *b = mem_alloc((size_t)W * W * WORLD_H);
    for (int cz = 0; cz < N; cz++)
        for (int cx = 0; cx < N; cx++) {
            column *c = column_alloc(cx0 + cx, cz0 + cz);
            worldgen_column(1337, c);
            for (int y = 0; y < WORLD_H; y++)
                for (int z = 0; z < CHUNK_W; z++)
                    for (int x = 0; x < CHUNK_W; x++)
                        b[((size_t)y * W + (size_t)(cz * CHUNK_W + z)) * W + (size_t)(cx * CHUNK_W + x)] =
                            c->blocks[col_index(x, y, z)];
            column_free(c);
        }
#define AT(x, y, z) b[((size_t)(y) * W + (size_t)(z)) * W + (size_t)(x)]
    long water = 0, inland = 0, leaks = 0, wrong = 0;
    for (int z = 0; z < W; z++)
        for (int x = 0; x < W; x++) {
            worldgen_point p = worldgen_sample(1337, cx0 * CHUNK_W + x, cz0 * CHUNK_W + z);
            /* The water (or its ice) fills from the ground to the level, under
             * nothing but air or a canopy from the bank. */
            if (p.level) {
                uint8_t at = AT(x, p.height, z), surf = AT(x, p.level - 1, z), over = AT(x, p.level, z);
                wrong += (at != B_WATER && at != B_ICE) || (surf != B_WATER && surf != B_ICE) ||
                         (over != B_AIR && over != B_LEAVES && over != B_LOG);
                inland += p.level > SEA_LEVEL;
            } else {
                wrong += AT(x, p.height - 1, z) == B_WATER;
            }
            for (int y = 1; y < WORLD_H; y++) {
                if (AT(x, y, z) != B_WATER) continue;
                water++;
                leaks += AT(x, y - 1, z) == B_AIR;
                if (x > 0) leaks += AT(x - 1, y, z) == B_AIR;
                if (x < W - 1) leaks += AT(x + 1, y, z) == B_AIR;
                if (z > 0) leaks += AT(x, y, z - 1) == B_AIR;
                if (z < W - 1) leaks += AT(x, y, z + 1) == B_AIR;
            }
        }
#undef AT
    mem_free(b);
    CHECK(inland > 20 && water > 100);
    CHECK(leaks == 0 && wrong == 0);
}

/* Over a 6 km square sampled about every 30 m: inland water stays below the hills
 * (no level above 70 m over the sea) and comes in terraces 3 m apart, every water column's ground lies under its level, the dry ground
 * beside water is never below it, and another seed puts its rivers
 * elsewhere. */
static void test_river_levels(void)
{
    long inland = 0, high = 0, off_step = 0, under = 0, low_bank = 0, other = 0, same = 0;
    for (int z = -3000; z < 3000; z += 31)
        for (int x = -3000; x < 3000; x += 29) {
            worldgen_point p = worldgen_sample(1337, x, z);
            if (p.level) under += p.height >= p.level;
            if (p.level <= SEA_LEVEL) continue;
            inland++;
            high += p.level > SEA_LEVEL + 70;
            off_step += (p.level - SEA_LEVEL) % 3 != 0;
            static const int D[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (int k = 0; k < 4; k++) {
                worldgen_point q = worldgen_sample(1337, x + D[k][0], z + D[k][1]);
                /* A drier neighbour holds this water in: as high as it. */
                low_bank += !q.level && q.height < p.level;
            }
            worldgen_point o = worldgen_sample(4242, x, z);
            other += o.level > SEA_LEVEL;
            same += o.level == p.level;
        }
    CHECK(inland > 30 && high == 0 && off_step == 0);
    CHECK(under == 0 && low_bank == 0);
    CHECK(other > 0 && same < inland / 2);
}

/* Rivers sit in lowland and run dry in the desert: most inland water lies
 * within 30 m of the sea, and none lies in the driest country. */
static void test_river_climate(void)
{
    long low = 0, all = 0, dry = 0;
    for (int z = -6000; z < 6000; z += 67)
        for (int x = -6000; x < 6000; x += 61) {
            worldgen_point p = worldgen_sample(1337, x, z);
            if (p.level <= SEA_LEVEL) continue;
            all++;
            low += p.level <= SEA_LEVEL + 30;
            dry += p.rain < 0.12f;
        }
    CHECK(all > 30 && low * 2 > all && dry == 0);
}

/* A river's bed and banks hold: over the columns around one, no cave opens
 * within a block of inland water (water would drain into it), and the beds
 * are gravel, sand or clay rather than the soil of the land around. */
static void test_river_beds(void)
{
    int rx = 0, rz = 0;
    CHECK(find_river(1337, &rx, &rz));
    int cx0 = (int)floor(rx / (double)CHUNK_W) - 1, cz0 = (int)floor(rz / (double)CHUNK_W) - 1;
    long beds = 0, odd = 0, holes = 0;
    for (int cz = cz0; cz < cz0 + 3; cz++)
        for (int cx = cx0; cx < cx0 + 3; cx++) {
            column *c = column_alloc(cx, cz);
            worldgen_column(1337, c);
            for (int z = 0; z < CHUNK_W; z++)
                for (int x = 0; x < CHUNK_W; x++) {
                    worldgen_point p = worldgen_sample(1337, cx * CHUNK_W + x, cz * CHUNK_W + z);
                    if (p.level <= SEA_LEVEL) continue;
                    uint8_t bed = c->blocks[col_index(x, p.height - 1, z)];
                    beds++;
                    odd += bed != B_GRAVEL && bed != B_SAND && bed != B_CLAY;
                    /* The ground under the bed down to the stone is solid. */
                    for (int y = p.height - 6; y < p.height; y++)
                        holes += y > 0 && c->blocks[col_index(x, y, z)] == B_AIR;
                }
            column_free(c);
        }
    CHECK(beds > 20 && odd == 0 && holes == 0);
}

/* At the world's edge, 2000 km out, the terrain is as sane as near the
 * origin: heights in range, known biomes, no cliff between neighbouring
 * columns that the land's slope could not make, and the small-scale
 * detail still there rather than lost to rounding. */
static void test_far_edge(void)
{
    int jumps = 0, bad = 0, detail = 0;
    for (int i = 0; i < 64; i++) {
        int x = WORLD_LIMIT - 4000 + i * 61, z = -(WORLD_LIMIT - 4000) + i * 37;
        worldgen_point p = worldgen_sample(1337, x, z), e = worldgen_sample(1337, x + 1, z);
        worldgen_point s = worldgen_sample(1337, x, z + 1);
        bad += p.height < 4 || p.height > WORLD_H - 12 || (int)p.b < 0 || (int)p.b >= BIOME_COUNT;
        jumps += abs(p.height - e.height) > 24 || abs(p.height - s.height) > 24;
        detail += p.height != e.height || p.height != s.height;
    }
    CHECK(bad == 0 && jumps == 0 && detail >= 4);
}

/* From the cold belt 6 km north (-z) to the hot belt 6 km south it gets
 * warmer band by band, and the hot belt is the drier. */
static void test_climate_zones(void)
{
    float prev = -99.0f, rain_mid = 0.0f, rain_hot = 0.0f;
    for (int band = -3; band <= 3; band++) {
        float t = 0.0f;
        for (int i = 0; i < 40; i++) {
            int x = i * 1213 - 24000;
            t += worldgen_sea_temp(1337, x, band * 1800) / 40.0f;
            if (band == 0) rain_mid += worldgen_sample(1337, x, 0).rain / 40.0f;
            if (band == 3) rain_hot += worldgen_sample(1337, x, band * 1800).rain / 40.0f;
        }
        CHECK(t > prev + 1.0f);
        prev = t;
    }
    CHECK(rain_hot < rain_mid);
}

/* Share of blocks in a SIZE square at (x0, z0) whose slope is in [lo, hi]. */
static double slope_share(int x0, int z0, int size, int lo, int hi)
{
    long in = 0;
    for (int z = z0; z < z0 + size; z++)
        for (int x = x0; x < x0 + size; x++) {
            int rise = 0, s = slope_at(1337, x, z, worldgen_height(1337, x, z), &rise);
            in += s >= lo && s <= hi;
        }
    return (double)in / (double)(size * size);
}

/* Dry country: mesas stand in flat terraces broken by cliffs, dunes roll
 * without any. */
static void test_dry_landforms(void)
{
    double flat = slope_share(-1000, 650, 64, 0, 0), cliff = slope_share(-1000, 650, 64, 6, 64);
    double dunes = slope_share(-2700, 2500, 48, 1, 2), dune_cliff = slope_share(-2700, 2500, 48, 3, 64);
    int lo = WORLD_H, hi = 0;
    for (int i = 0; i < 160; i++) {
        int h = worldgen_height(1337, -2700 + i, 2500 + i);
        lo = h < lo ? h : lo;
        hi = h > hi ? h : hi;
    }
    CHECK(flat > 0.6 && cliff > 0.01);
    CHECK(dunes > 0.1 && dune_cliff == 0.0);
    CHECK(hi - lo >= 4 && hi - lo <= 20);
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

/* Every tree in a stretch of mixed forest stands as generated: no leaf
 * hangs beyond what its span carries. */
static void test_trees_stand(void)
{
    jobs *js = jobs_create(0);
    world w = {0};
    world_init(&w, 1337, 4, js, NULL);
    world_load_blocking(&w, 72.0, -72.0, 3);
    physics ph = {0};
    physics_init(&ph, &w);
    int leaves = 0, logs = 0, fell = 0;
    for (int z = -104; z < -40; z++)
        for (int x = 40; x < 104; x++)
            for (int y = 40; y < WORLD_H - 1; y++) {
                uint8_t b = world_get(&w, x, y, z);
                if (!block_log(b) && !block_leaves(b)) continue;
                leaves += block_leaves(b);
                logs += block_log(b);
                fell += physics_check_structure(&ph, x, y, z);
            }
    CHECK(leaves > 300 && logs > 50 && leaves > 3 * logs);
    CHECK(fell == 0);
    physics_destroy(&ph);
    world_destroy(&w);
    jobs_destroy(js);
}

/* Leaves are cut out: a share of each leaf texture is see-through, but under
 * half, so every coarser mip, averaged over the gaps, is solid and a
 * distant canopy closes up. */
static void test_leaf_textures(void)
{
    uint8_t *tex = mem_alloc(texgen_total_bytes());
    texgen_build(tex);
    for (int i = 0; i < TREE_KINDS; i++) {
        int gaps = 0, layer = tree_tex_leaves(i);
        CHECK(block_get(g_tree_leaves[i])->tex[0] == layer);
        for (int y = 0; y < TEX_SIZE; y++)
            for (int x = 0; x < TEX_SIZE; x++) {
                uint8_t t[4] = {0};
                texgen_texel(layer, x, y, t);
                gaps += t[3] < 128;
                /* A gap keeps a leaf colour, for whatever draws it whole,
                 * never the black that would rim the leaves in the mips. */
                CHECK(t[3] >= 128 || t[0] + t[1] + t[2] > 40);
            }
        CHECK(gaps > TEX_SIZE * TEX_SIZE / 10 && gaps < TEX_SIZE * TEX_SIZE * 2 / 5);
        int thin = 0;
        for (int m = 2; m < TEX_MIPS; m++) {
            const uint8_t *mip = tex + texgen_offset(layer, m);
            int n = TEX_SIZE >> m;
            for (int t = 0; t < n * n; t++) thin += mip[4 * t + 3] < 128;
        }
        CHECK(thin == 0);
    }
    /* Bark and stone stay solid. */
    uint8_t t[4] = {0};
    texgen_texel(tree_tex_bark(TREE_BIRCH, 0), 3, 3, t);
    CHECK(t[3] == 255 && t[0] > 150);
    mem_free(tex);
}

/* Every kind of log splits into planks and chars, and only an oak's
 * leaves give apples. */
static void test_tree_items(void)
{
    for (int i = 0; i < TREE_KINDS; i++) {
        uint8_t log = g_tree_log[i], leaf = g_tree_leaves[i];
        CHECK(block_log(log) && !block_leaves(log) && block_leaves(leaf) && !block_log(leaf));
        CHECK(block_tree(log) == i && block_tree(leaf) == i && block_has_meta(log) && !block_has_meta(leaf));
        int planks = 0, charcoal = 0, fibre = 0;
        uint32_t r8 = 3;
        for (int k = 0; k < 100; k++) {
            item_stack o[3] = {{0}};
            int n = item_drops(leaf, &r8, o);
            for (int j = 0; j < n; j++) fibre += o[j].id == I_FIBRE;
        }
        CHECK(fibre > 20);
        for (int r = 0; r < g_recipe_count; r++) {
            if (g_recipes[r].in[0].id != log) continue;
            planks += g_recipes[r].out.id == B_PLANKS && g_recipes[r].out.count == 4;
            charcoal += g_recipes[r].out.id == I_CHARCOAL;
        }
        CHECK(planks == 1 && charcoal == 1);
    }
    CHECK(!block_log(B_PLANKS) && !block_leaves(B_GRASS) && !block_leaves(B_COUNT) && block_tree(B_PLANKS) < 0);
    CHECK(block_plant(B_VINES) && !block_solid(B_VINES) && block_tree(B_VINES) < 0);
    uint32_t rng = 11;
    int apples[2] = {0}, fibre = 0;
    for (int k = 0; k < 600; k++) {
        item_stack out[3] = {{0}};
        int n = item_drops(k & 1 ? B_PINE_LEAVES : B_LEAVES, &rng, out);
        for (int j = 0; j < n; j++) {
            apples[k & 1] += out[j].id == I_APPLE;
            fibre += out[j].id == I_FIBRE;
        }
    }
    CHECK(apples[0] > 20 && apples[1] == 0 && fibre > 200);
}

/* Plants are cut out like leaves, but thin: a share of each texture is
 * clear and a share solid, and a clear texel keeps a plant's colour (no
 * dark rim in the mips). Each flower shows its own colour. */
static void test_plant_textures(void)
{
    for (int l = T_PLANT_FIRST; l <= T_PLANT_LAST; l++) {
        int clear = 0, dark = 0;
        for (int y = 0; y < TEX_SIZE; y++)
            for (int x = 0; x < TEX_SIZE; x++) {
                uint8_t t[4] = {0};
                texgen_texel(l, x, y, t);
                clear += t[3] == 0;
                dark += t[3] == 0 && t[0] + t[1] + t[2] < 120;
                CHECK(t[3] == 0 || t[3] == 255);
            }
        CHECK(clear > TEX_SIZE * TEX_SIZE / 5 && clear < TEX_SIZE * TEX_SIZE - 12 && dark == 0);
    }
    /* Red, yellow, blue and white heads; the stem green. */
    static const int RGB[4][3] = {{1, 0, 0}, {1, 1, 0}, {0, 0, 1}, {1, 1, 1}};
    for (int k = 0; k < 4; k++) {
        int head = 0;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < TEX_SIZE; x++) {
                uint8_t t[4] = {0};
                texgen_texel(T_POPPY + k, x, y, t);
                const int on[3] = {t[0] > 180, t[1] > 180, t[2] > 180};
                head += t[3] && on[0] == RGB[k][0] && on[1] == RGB[k][1] && on[2] == RGB[k][2];
            }
        CHECK(head >= 6);
    }
}

/* Every plant stands on soil in open air, never under water; reeds stand
 * by water; and each country has its own cover: flowers and grass on the
 * meadows, ferns in the woods' shade, tall grass on the steppe, heather in
 * the cold, dry shrubs in the dry. */
static void test_plants(void)
{
    long kinds[BIOME_COUNT][B_COUNT] = {{0}}, open[BIOME_COUNT] = {0};
    int bad = 0, dry_reeds = 0, reeds = 0, shaded_ferns = 0, vines = 0;
    /* A spread of columns, and a shore at 24,-31 for the reeds. */
    for (int i = 0; i < 64; i++) {
        int cx = i < 48 ? (i * 1187) % 1500 - 750 : 24 + (i - 48) % 4;
        int cz = i < 48 ? (i * 1733) % 1500 - 750 : -31 + (i - 48) / 4;
        column *c = column_alloc(cx, cz);
        worldgen_column(1337, c);
        for (int z = 0; z < CHUNK_W; z++)
            for (int x = 0; x < CHUNK_W; x++)
                for (int y = 1; y < WORLD_H - 1; y++) {
                    uint8_t b = c->blocks[col_index(x, y, z)];
                    if (!block_plant(b)) continue;
                    uint8_t below = c->blocks[col_index(x, y - 1, z)], above = c->blocks[col_index(x, y + 1, z)];
                    /* Vines hang from a tree, not from the ground. */
                    if (b == B_VINES) {
                        vines++;
                        bad += !block_leaves(above) && !block_log(above) && above != B_VINES;
                        continue;
                    }
                    int m = block_get(below)->mat;
                    bad += !block_solid(below) || (m != BM_GRASS && m != BM_DIRT && m != BM_SAND);
                    bad += block_fluid(above);
                    worldgen_point p = worldgen_sample(1337, cx * CHUNK_W + x, cz * CHUNK_W + z);
                    kinds[p.b][b]++;
                    open[p.b]++;
                    if (b == B_REEDS && x > 0 && x < CHUNK_W - 1 && z > 0 && z < CHUNK_W - 1) {
                        int wet = 0;
                        for (int k = 0; k < 4; k++) {
                            int nx = x + (k == 0) - (k == 1), nz = z + (k == 2) - (k == 3);
                            wet |= c->blocks[col_index(nx, y - 1, nz)] == B_WATER ||
                                   c->blocks[col_index(nx, y, nz)] == B_WATER;
                        }
                        dry_reeds += !wet;
                        reeds++;
                    }
                    if (b == B_FERN)
                        for (int ly = y + 1; ly < y + 12 && ly < WORLD_H; ly++)
                            if (block_leaves(c->blocks[col_index(x, ly, z)])) {
                                shaded_ferns++;
                                break;
                            }
                }
        column_free(c);
    }
    CHECK(bad == 0 && vines > 0);
    CHECK(reeds > 0 && dry_reeds == 0);
    long meadow_flowers = 0;
    for (int f = B_POPPY; f <= B_OXEYE; f++) meadow_flowers += kinds[BIOME_MEADOW][f];
    CHECK(kinds[BIOME_MEADOW][B_GRASS_TUFT] > kinds[BIOME_MEADOW][B_TALL_GRASS] && meadow_flowers > 0);
    CHECK(kinds[BIOME_FOREST][B_FERN] > 0 && shaded_ferns > 0);
    CHECK(kinds[BIOME_STEPPE][B_TALL_GRASS] > kinds[BIOME_STEPPE][B_GRASS_TUFT]);
    CHECK(kinds[BIOME_TAIGA][B_HEATHER] + kinds[BIOME_TUNDRA][B_HEATHER] + kinds[BIOME_ALPINE][B_HEATHER] > 0);
    CHECK(kinds[BIOME_SCRUB][B_DRY_SHRUB] > 0 && kinds[BIOME_DESERT][B_GRASS_TUFT] == 0);
    CHECK(kinds[BIOME_MEADOW][B_HEATHER] == 0 && kinds[BIOME_TUNDRA][B_TALL_GRASS] == 0);
    /* Ground cover, not a carpet: under half the open ground of a meadow. */
    CHECK(open[BIOME_MEADOW] > 100);
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

/* Does a kind of tree belong where the ground's climate is p? Its own
 * rules (worldgen.c tree_kind_at), loosened by a degree or a little rain
 * for the blend between the sample and the site. Willows grow by water
 * anywhere but sand. */
static int tree_belongs(int k, worldgen_point p)
{
    biome b = p.b;
    int dry = b == BIOME_SCRUB || b == BIOME_STEPPE, wood = b == BIOME_FOREST || b == BIOME_MEADOW;
    switch (k) {
    case TREE_PALM: return b == BIOME_BEACH && p.temp > 17.0f;
    case TREE_REDWOOD: return b == BIOME_FOREST && p.rain > 0.6f && p.temp < 17.0f;
    case TREE_MOUNTAIN_PINE: return b == BIOME_ALPINE;
    case TREE_BAOBAB: return dry && p.temp > 15.0f;
    case TREE_THORN: return dry;
    case TREE_UMBRELLA: return dry || (b == BIOME_FOREST && p.temp > 13.0f);
    case TREE_SPRUCE: return b == BIOME_TAIGA || b == BIOME_ALPINE || (b == BIOME_FOREST && p.temp < 6.0f);
    case TREE_MAPLE:
    case TREE_CHERRY: return wood;
    default: return b != BIOME_BEACH && b != BIOME_DESERT;
    }
}

/* Over generated ground far apart: every trunk is of a kind its climate
 * grows, most kinds turn up, and the trunks lying on the ground lie only
 * in the deep woods, where nothing clears the dead. */
static void test_tree_climate(void)
{
    long feet = 0, wrong = 0, lying = 0, lying_wrong = 0;
    int seen[TREE_KINDS] = {0};
    for (int i = 0; i < 200; i++) {
        int cx = (i * 2741) % 2400 - 1200, cz = (i * 3571) % 2400 - 1200;
        column *c = column_alloc(cx, cz);
        worldgen_column(1337, c);
        for (int z = 0; z < CHUNK_W; z++)
            for (int x = 0; x < CHUNK_W; x++)
                for (int y = 2; y < WORLD_H - 1; y++) {
                    int ci = col_index(x, y, z);
                    uint8_t b = c->blocks[ci], under = c->blocks[ci - COL_AREA], m = c->meta ? c->meta[ci] : 0;
                    if (!block_log(b) || block_log(under) || block_leaves(under) || under == B_AIR) continue;
                    worldgen_point p = worldgen_sample(1337, cx * CHUNK_W + x, cz * CHUNK_W + z);
                    if ((m & 3) == LOG_X || (m & 3) == LOG_Z) {
                        lying++;
                        lying_wrong += p.b != BIOME_FOREST && p.b != BIOME_TAIGA && p.b != BIOME_WETLAND;
                        continue;
                    }
                    if ((m & 3) != LOG_UP || !block_log(c->blocks[ci + COL_AREA])) continue;
                    int k = block_tree(b);
                    feet++;
                    seen[k]++;
                    wrong += !tree_belongs(k, p);
                }
        column_free(c);
    }
    int kinds = 0;
    for (int k = 0; k < TREE_KINDS; k++) kinds += seen[k] > 0;
    /* A wide trunk's far corner can stand a block over a biome's edge. */
    CHECK(feet > 40 && wrong * 50 <= feet && kinds >= 8);
    CHECK(lying >= 3 && lying_wrong * 20 <= lying);
}

/* Columns of a forest, generated one by one as the game does: no piece of
 * a tree floats where a neighbour should have grown the rest of it, and
 * crowns do reach across the columns' edges. */
#define SEAM_N 5
#define SEAM_W (SEAM_N * CHUNK_W)
static void test_tree_seams(void)
{
    const int cx0 = 4 - SEAM_N / 2, cz0 = -5 - SEAM_N / 2; /* around (72, -72) */
    uint8_t *g = mem_alloc((size_t)SEAM_W * SEAM_W * WORLD_H);
    for (int n = 0; n < SEAM_N * SEAM_N; n++) {
        column *c = column_alloc(cx0 + n % SEAM_N, cz0 + n / SEAM_N);
        worldgen_column(1337, c);
        for (int y = 0; y < WORLD_H; y++)
            for (int z = 0; z < CHUNK_W; z++)
                for (int x = 0; x < CHUNK_W; x++) {
                    int gx = n % SEAM_N * CHUNK_W + x, gz = n / SEAM_N * CHUNK_W + z;
                    g[((size_t)y * SEAM_W + (size_t)gz) * SEAM_W + (size_t)gx] = c->blocks[col_index(x, y, z)];
                }
        column_free(c);
    }
#define G(x, y, z) g[((size_t)(y) * SEAM_W + (size_t)(z)) * SEAM_W + (size_t)(x)]
    /* Pairs of one tree's blocks either side of a column edge. */
    long across = 0;
    for (int y = 1; y < WORLD_H; y++)
        for (int z = 0; z < SEAM_W; z++)
            for (int x = CHUNK_W; x < SEAM_W; x += CHUNK_W) {
                int k = block_tree(G(x, y, z));
                across += k >= 0 && block_tree(G(x - 1, y, z)) == k;
                k = block_tree(G(z, y, x));
                across += k >= 0 && block_tree(G(z, y, x - 1)) == k;
            }
    /* Flood every piece of tree; one that touches no other solid block and
     * stays inside the grid is floating. */
    uint8_t *done = mem_calloc((size_t)SEAM_W * SEAM_W * WORLD_H, 1);
    int *queue = mem_alloc(sizeof(int) * 3 * 200000);
    static const int D[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    long pieces = 0, floating = 0;
    for (int y = 1; y < WORLD_H - 1; y++)
        for (int z = 0; z < SEAM_W; z++)
            for (int x = 0; x < SEAM_W; x++) {
                uint8_t b = G(x, y, z);
                if (done[((size_t)y * SEAM_W + (size_t)z) * SEAM_W + (size_t)x] || (block_tree(b) < 0 && b != B_VINES))
                    continue;
                int n = 0, held = 0;
                queue[n++] = x;
                queue[n++] = y;
                queue[n++] = z;
                done[((size_t)y * SEAM_W + (size_t)z) * SEAM_W + (size_t)x] = 1;
                for (int h = 0; h < n && n < 3 * 200000 - 3; h += 3)
                    for (int d = 0; d < 6; d++) {
                        int a = queue[h] + D[d][0], e = queue[h + 1] + D[d][1], f = queue[h + 2] + D[d][2];
                        if (a < 0 || f < 0 || a >= SEAM_W || f >= SEAM_W) {
                            held = 1; /* runs on outside the grid */
                            continue;
                        }
                        if (e < 1 || e >= WORLD_H) continue;
                        uint8_t nb = G(a, e, f);
                        size_t di = ((size_t)e * SEAM_W + (size_t)f) * SEAM_W + (size_t)a;
                        if (block_tree(nb) < 0 && nb != B_VINES) {
                            held |= block_solid(nb);
                            continue;
                        }
                        if (done[di]) continue;
                        done[di] = 1;
                        queue[n++] = a;
                        queue[n++] = e;
                        queue[n++] = f;
                    }
                pieces++;
                floating += !held;
            }
    /* What the trees left on the ground: metas only on logs, and valid;
     * vines hang from a tree or from vines above; no trunk stands on
     * grass or podzol, which turns to dirt under it; and the old woods
     * have mossy logs. */
    long bad_meta = 0, loose_vines = 0, grassy = 0, mossy = 0, trunks = 0;
    for (int n = 0; n < SEAM_N * SEAM_N; n++) {
        column *c = column_alloc(cx0 + n % SEAM_N, cz0 + n / SEAM_N);
        worldgen_column(1337, c);
        for (int y = 2; y < WORLD_H - 1; y++)
            for (int z = 0; z < CHUNK_W; z++)
                for (int x = 0; x < CHUNK_W; x++) {
                    int ci = col_index(x, y, z);
                    uint8_t b = c->blocks[ci], m = c->meta ? c->meta[ci] : 0, up = c->blocks[ci + COL_AREA];
                    if (block_log(b)) {
                        bad_meta += m > (LOG_BRANCH | LOG_MOSS);
                        mossy += (m & LOG_MOSS) != 0;
                        uint8_t under = c->blocks[ci - COL_AREA];
                        if ((m & 3) == LOG_UP && !block_log(under) && block_solid(under)) {
                            trunks++;
                            grassy += under == B_GRASS || under == B_PODZOL;
                        }
                    } else if (!block_fluid(b)) {
                        bad_meta += m != 0;
                    }
                    if (b == B_VINES) loose_vines += !block_leaves(up) && !block_log(up) && up != B_VINES;
                }
        column_free(c);
    }
#undef G
    CHECK(pieces > 20 && floating == 0 && across > 100);
    CHECK(bad_meta == 0 && loose_vines == 0 && grassy == 0 && trunks > 20 && mossy > 0);
    mem_free(queue);
    mem_free(done);
    mem_free(g);
}

/* The generator remembers the slow fields it worked out last (and, for
 * trees, which grow where). A remembered answer must be the one it would
 * work out again: the same points, sampled cold, after another seed and
 * after enough other points to push them out, come back bit for bit, and
 * a column grown after its neighbours is the column grown alone. */
static void test_worldgen_memo(void)
{
    enum { N = 40 };
    worldgen_point first[N] = {{0}};
    for (int i = 0; i < N; i++) first[i] = worldgen_sample(4242, i * 97 - 1900, i * 61 + 300);
    for (int i = 0; i < 3000; i++) (void)worldgen_sample(i & 1 ? 4242 : 4243, i * 13 - 20000, i * 7);
    int same = 1;
    for (int i = 0; i < N; i++) {
        (void)worldgen_sample(4243, i * 97 - 1900, i * 61 + 300); /* the same point in another world */
        worldgen_point again = worldgen_sample(4242, i * 97 - 1900, i * 61 + 300);
        same &= again.height == first[i].height && again.level == first[i].level && again.temp == first[i].temp &&
                again.rain == first[i].rain && again.b == first[i].b;
    }
    CHECK(same);

    column *alone = column_alloc(3, -2);
    worldgen_column(4242, alone);
    for (int dz = -1; dz <= 1; dz++)
        for (int dx = -1; dx <= 1; dx++) {
            column *n = column_alloc(3 + dx, -2 + dz);
            worldgen_column(dx ? 4243 : 4242, n);
            column_free(n);
        }
    column *after = column_alloc(3, -2);
    worldgen_column(4242, after);
    CHECK(memcmp(alone->blocks, after->blocks, COL_VOL) == 0);
    CHECK(!alone->meta == !after->meta && (!alone->meta || memcmp(alone->meta, after->meta, COL_VOL) == 0));
    column_free(alone);
    column_free(after);
}

/* Standing trunks in a column by kind: a log standing on something that
 * is neither log, leaves nor air, with a log above it. */
static int count_trunks(const column *c, int seen[TREE_KINDS])
{
    int feet = 0;
    for (int z = 0; z < CHUNK_W; z++)
        for (int x = 0; x < CHUNK_W; x++)
            for (int y = 2; y < WORLD_H - 1; y++) {
                int ci = col_index(x, y, z);
                uint8_t b = c->blocks[ci], under = c->blocks[ci - COL_AREA], m = c->meta ? c->meta[ci] : 0;
                if (!block_log(b) || block_log(under) || block_leaves(under) || under == B_AIR) continue;
                if ((m & 3) != LOG_UP || !block_log(c->blocks[ci + COL_AREA])) continue;
                seen[block_tree(b)]++;
                feet++;
            }
    return feet;
}

/* Trunks counted around up to eight spots of the given biomes, far apart:
 * how many spots, how many trunks were of each spot's commonest kind, of
 * how many, and how many different kinds were commonest somewhere. A spot
 * is span x span columns whose middle and four points around it all lie
 * in one of the biomes, with at least 8 trunks to judge by. */
typedef struct {
    int spots, kinds;
    long main_feet, all_feet;
} woods_tally;

static woods_tally tally_woods(const biome *want, int nwant, int span)
{
    woods_tally t = {0, 0, 0, 0};
    int mains[TREE_KINDS] = {0}, r = span * CHUNK_W / 2;
    for (int i = 0; i < 6000 && t.spots < 8; i++) {
        int x = (i * 7919) % 60000 - 30000, z = (i * 104729) % 60000 - 30000;
        int inside = 1;
        for (int k = 0; k < 5 && inside; k++) {
            worldgen_point p = worldgen_sample(1337, x + (k == 1) * r - (k == 2) * r, z + (k == 3) * r - (k == 4) * r);
            inside = 0;
            for (int w = 0; w < nwant; w++) inside |= p.b == want[w];
        }
        if (!inside) continue;
        int seen[TREE_KINDS] = {0}, feet = 0;
        int cx = (int)floor((x - r) / (double)CHUNK_W), cz = (int)floor((z - r) / (double)CHUNK_W);
        for (int dz = 0; dz < span; dz++)
            for (int dx = 0; dx < span; dx++) {
                column *c = column_alloc(cx + dx, cz + dz);
                worldgen_column(1337, c);
                feet += count_trunks(c, seen);
                column_free(c);
            }
        if (feet < 8) continue;
        int top = 0;
        for (int k = 1; k < TREE_KINDS; k++)
            if (seen[k] > seen[top]) top = k;
        mains[top] = 1;
        t.main_feet += seen[top];
        t.all_feet += feet;
        t.spots++;
    }
    for (int k = 0; k < TREE_KINDS; k++) t.kinds += mains[k];
    return t;
}

/* Woods are mostly of one kind, and not every wood is of the same kind.
 * Trees used to take their kind one by one, so a forest mixed four or
 * five kinds evenly: around a spot the commonest was 58 % of the trunks
 * in forest, 55 % in taiga and 84 % in dry country. Counting willows by
 * the streams and each wood's companion kind, woods make it 70 %, 78 %
 * and 94 %. */
static void test_forest_woods(void)
{
    static const biome FOREST[] = {BIOME_FOREST}, CONIFER[] = {BIOME_TAIGA}, DRY[] = {BIOME_SCRUB, BIOME_STEPPE};
    woods_tally f = tally_woods(FOREST, 1, 2);
    CHECK(f.spots >= 5 && f.kinds >= 2);
    CHECK(f.main_feet * 100 >= f.all_feet * 65);
    woods_tally c = tally_woods(CONIFER, 1, 2);
    CHECK(c.spots >= 3 && c.kinds >= 2);
    CHECK(c.main_feet * 100 >= c.all_feet * 70);
    /* Dry country is thinly wooded: look over nine columns a spot. */
    woods_tally d = tally_woods(DRY, 2, 3);
    CHECK(d.spots >= 3);
    CHECK(d.main_feet * 100 >= d.all_feet * 90);
}

/* Whether a trunk stands on the ground at (x, z) of a column: an upright
 * log with an upright log above it and soil under it. */
static int trunk_foot(const column *c, int x, int z)
{
    for (int y = 2; y < WORLD_H - 1; y++) {
        int ci = col_index(x, y, z);
        uint8_t b = c->blocks[ci], under = c->blocks[ci - COL_AREA], m = c->meta ? c->meta[ci] : 0;
        if (!block_log(b) || block_log(under) || block_leaves(under) || under == B_AIR) continue;
        if ((m & 3) == LOG_UP && block_log(c->blocks[ci + COL_AREA])) return 1;
    }
    return 0;
}

/* Trees root at most one to a 4 x 4 cell. Trunk feet that touch (a
 * redwood's broad foot and buttresses, or two trees side by side across a
 * cell edge) count as one tree, at their middle. Over the inner cells of
 * 2 x 2 columns at a dozen forest spots, returns the most trees in one
 * cell and counts the trees. */
static int trees_per_cell(int *trees)
{
    static uint8_t foot[32][32], done[32][32];
    static int stack[1024][2];
    int most = 0, spots = 0;
    *trees = 0;
    for (int i = 0; i < 6000 && spots < 12; i++) {
        int x = (i * 7919) % 60000 - 30000, z = (i * 104729) % 60000 - 30000;
        biome b = worldgen_sample(1337, x + 16, z + 16).b;
        if (b != BIOME_FOREST && b != BIOME_TAIGA) continue;
        spots++;
        memset(done, 0, sizeof done);
        int cx = (int)floor(x / (double)CHUNK_W), cz = (int)floor(z / (double)CHUNK_W);
        for (int q = 0; q < 4; q++) {
            column *c = column_alloc(cx + (q & 1), cz + (q >> 1));
            worldgen_column(1337, c);
            for (int bz = 0; bz < CHUNK_W; bz++)
                for (int bx = 0; bx < CHUNK_W; bx++)
                    foot[(q >> 1) * CHUNK_W + bz][(q & 1) * CHUNK_W + bx] = (uint8_t)trunk_foot(c, bx, bz);
            column_free(c);
        }
        int count[8][8] = {{0}};
        for (int z0 = 0; z0 < 32; z0++)
            for (int x0 = 0; x0 < 32; x0++) {
                if (!foot[z0][x0] || done[z0][x0]) continue;
                int n = 1, sx = 0, sz = 0, k = 0, edge = 0;
                stack[0][0] = x0;
                stack[0][1] = z0;
                done[z0][x0] = 1;
                while (n > 0) {
                    n--;
                    int px = stack[n][0], pz = stack[n][1];
                    sx += px;
                    sz += pz;
                    k++;
                    edge |= px == 0 || pz == 0 || px == 31 || pz == 31;
                    for (int d = 0; d < 9; d++) {
                        int nx = px + d % 3 - 1, nz = pz + d / 3 - 1;
                        if (nx < 0 || nz < 0 || nx > 31 || nz > 31 || !foot[nz][nx] || done[nz][nx]) continue;
                        done[nz][nx] = 1;
                        stack[n][0] = nx;
                        stack[n][1] = nz;
                        n++;
                    }
                }
                if (edge) continue;
                int *cell = &count[sz / k / 4][sx / k / 4];
                (*cell)++;
                (*trees)++;
                if (*cell > most) most = *cell;
            }
    }
    return most;
}

/* Trees used to be tried block by block, so two could root a block or two
 * apart (main had 2 in one cell of 71 trees here); now each 4 x 4 cell
 * roots one tree at a jittered spot or none. */
static void test_tree_cells(void)
{
    int trees = 0, most = trees_per_cell(&trees);
    CHECK(trees >= 40);
    CHECK(most == 1);
}

/* Columns grown on several workers at once, sharing the remembered tree
 * sites as they fill them in, are the columns one thread grows alone. */
typedef struct {
    job j;
    column *c;
} gen_job;

static void gen_run(job *j) { worldgen_column(777001, ((gen_job *)j)->c); }
static void gen_done(job *j) { (void)j; }

static void test_worldgen_threads(void)
{
    enum { W = 4, N = W * W };
    column *ref[N] = {NULL}, *par[N] = {NULL};
    gen_job work[N] = {{{NULL, NULL, NULL}, NULL}};
    worldgen_forget();
    for (int i = 0; i < N; i++) {
        ref[i] = column_alloc(i % W, i / W);
        worldgen_column(777001, ref[i]);
    }
    worldgen_forget();
    jobs *js = jobs_create(4);
    for (int i = 0; i < N; i++) {
        par[i] = column_alloc(i % W, i / W);
        work[i] = (gen_job){{gen_run, gen_done, NULL}, par[i]};
        jobs_submit(js, &work[i].j);
    }
    jobs_wait_idle(js);
    jobs_poll(js, -1);
    jobs_destroy(js);
    int same = 1;
    for (int i = 0; i < N; i++) {
        same &= memcmp(ref[i]->blocks, par[i]->blocks, COL_VOL) == 0;
        same &= !ref[i]->meta == !par[i]->meta && (!ref[i]->meta || memcmp(ref[i]->meta, par[i]->meta, COL_VOL) == 0);
        column_free(ref[i]);
        column_free(par[i]);
    }
    CHECK(same);
}

void test_worldgen_all(void)
{
    test_worldgen_threads();
    test_forest_woods();
    test_tree_cells();
    test_worldgen_memo();
    test_noise_grad();
    test_noise_wrap();
    test_noise_ranges();
    test_climate();
    test_generators();
    test_soil();
    test_trees();
    test_tree_climate();
    test_tree_seams();
    test_cave_shapes();
    test_landforms();
    test_sample();
    test_rivers();
    test_river_levels();
    test_river_climate();
    test_river_beds();
    test_far_edge();
    test_climate_zones();
    test_dry_landforms();
    test_caves_stand();
    test_trees_stand();
    test_leaf_textures();
    test_tree_items();
    test_plant_textures();
    test_plants();
    test_smelting();
}
