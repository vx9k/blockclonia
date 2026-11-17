/* Terrain grown from a climate: continents and coasts, mountain ranges,
 * mesas and dunes, biomes by temperature and rainfall (see worldgen.h),
 * then the rock beneath (see strata), cave tunnels and chambers, ore in
 * its host rock and trees. The surface is 2D noise;
 * the caves are 3D noise sampled on a coarse grid and interpolated, which
 * costs ~1/64 of evaluating it per block, so low-end CPUs keep up with
 * streaming. Every coordinate goes into the noise in double, so the land
 * 2e9 blocks out is as fine as at the origin. */
/* A seed grows the same land on every machine: a fused multiply-add
 * (ARM, or x86 built for its own CPU) rounds once where two operations
 * round twice, and moves a height or a cave wall by a block. */
#pragma STDC FP_CONTRACT OFF
#include "worldgen.h"
#include "mathlib.h"
#include "mem.h"
#include "noise.h"
#include "physics.h"
#include "world.h"

#include <stdlib.h>
#include <string.h>

#define SEA ((float)SEA_LEVEL)
/* The slow fields (warp, continents, ranges, mesas, climate) change over
 * hundreds of metres. They are sampled where x and z are multiples of
 * SLOW and blended in between, on the same grid for every column, so the
 * land stays seamless for a sixty-fourth of the cost. */
#define SLOW 8
#define ZONE 24000.0 /* m: one cycle of the north-south climate zones */
/* Peaks ease from the knee towards the top instead of flattening. */
#define KNEE 200.0f
#define TOP 250.0f

static float sat(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

static float smooth(float a, float b, float v)
{
    float t = sat((v - a) / (b - a));
    return t * t * (3.0f - 2.0f * t);
}

/* Written so that u = 0 gives a and u = 1 gives b exactly, which keeps
 * a block on a grid line the same from the columns on either side. */
static float lerp(float a, float b, float u) { return a * (1.0f - u) + b * u; }

/* fbm clusters near 0: stretch it so the extremes (desert, tundra) cover
 * real ground instead of specks, then soften it back into range. */
static float stretch(float f)
{
    f *= 2.2f;
    return f / (1.0f + fabsf(f) * 0.35f);
}

/* ---------------------------------------------------------- climate */

/* The zones: cold to the north (-z), hot and dry to the south, temperate
 * at z = 0 where players spawn, repeating every ZONE metres so a long
 * journey crosses them all. A triangle wave rounded at its turns: a cubic
 * rather than sin, whose last bits differ between maths libraries. */
static float zone_at(int z)
{
    double p = (double)z / ZONE + 0.25;
    float s = (float)(p - floor(p));
    float tri = 1.0f - 4.0f * fabsf(s - 0.5f);
    return tri * (1.5f - 0.5f * tri * tri);
}

/* Mean air temperature at sea level and rainfall before the land has its
 * say: the zone and a regional field a few km across. The warm zone is
 * also the dry one (a subtropical high), broken by wetter regions. */
static void climate_at(uint32_t seed, int x, int z, float *temp, float *rain)
{
    float zone = zone_at(z);
    float ft = stretch(fbm2d(seed + 201, x / 3000.0, z / 3000.0, 3));
    float fr = stretch(fbm2d(seed + 211, x / 2200.0, z / 2200.0, 3));
    *temp = 12.0f + 11.0f * zone + 6.0f * ft;
    *rain = 0.45f + 0.45f * fr - 0.32f * smooth(0.3f, 1.0f, zone);
}

/* -------------------------------------------------------- the land */

typedef struct {
    float ox, oz; /* domain warp, m: bends coasts and ranges */
    float cont;   /* continents: land where > 0 */
    float mask;   /* 0..1: inside a mountain range */
} shape;

static shape shape_at(uint32_t seed, int x, int z)
{
    shape s = {0};
    s.ox = 700.0f * fbm2d(seed + 3, x / 1800.0, z / 1800.0, 3);
    s.oz = 700.0f * fbm2d(seed + 12, x / 1800.0, z / 1800.0, 3);
    double wx = x + (double)s.ox, wz = z + (double)s.oz;
    /* Biased towards land: about a third of the world is sea. */
    s.cont = fbm2d(seed, wx / 5000.0, wz / 5000.0, 4) + 0.26f;
    /* Ranges rise where a slow field is high, more readily inland. */
    float range = fbm2d(seed + 43, wx / 2600.0, wz / 2600.0, 3);
    s.mask = smooth(0.0f, 0.3f, range + s.cont * 0.3f - 0.1f) * smooth(-0.02f, 0.18f, s.cont);
    return s;
}

/* The ground from the slow fields alone, roughly: enough to tell where
 * high ground stands in the way of the wind. */
static float coarse_height(const shape *s)
{
    return SEA - 14.0f + 26.0f * smooth(-0.02f, 0.18f, s->cont) + 90.0f * s->mask;
}

typedef struct {
    shape sh;
    float rug;  /* 0 rolling .. 1 broken country */
    float mesa; /* 0..1: mesa country */
    float temp; /* mean C at sea level */
    float rain; /* 0..1 */
} slow;

static slow slow_at(uint32_t seed, int x, int z)
{
    slow s = {shape_at(seed, x, z), 0.0f, 0.0f, 0.0f, 0.0f};
    double wx = x + (double)s.sh.ox, wz = z + (double)s.sh.oz;
    s.rug = smooth(-0.3f, 0.4f, fbm2d(seed + 23, wx / 1500.0, wz / 1500.0, 2));
    climate_at(seed, x, z, &s.temp, &s.rain);
    /* Wetter by the sea; drier in the lee of high ground (the wind blows
     * towards +x), the more so the lower this spot lies beneath it. */
    s.rain += 0.15f * (1.0f - smooth(0.0f, 0.3f, s.sh.cont));
    float upwind = 0.0f;
    for (int k = 1; k <= 4; k++) {
        shape u = shape_at(seed, x - 250 * k, z);
        upwind = fmaxf(upwind, coarse_height(&u));
    }
    s.rain -= 0.35f * smooth(SEA + 30.0f, SEA + 130.0f, upwind) * smooth(-20.0f, 20.0f, upwind - coarse_height(&s.sh));
    /* Flat-lying beds erode into mesas only in dry country. */
    s.mesa = smooth(0.25f, 0.45f, fbm2d(seed + 71, wx / 2000.0, wz / 2000.0, 2)) * (1.0f - smooth(0.25f, 0.4f, s.rain));
    return s;
}

/* Bilinear between the four grid points around a block: a at (x0, z0), b
 * at (x0 + SLOW, z0), c at (x0, z0 + SLOW), d at (x0 + SLOW, z0 + SLOW). */
static float mix4(float a, float b, float c, float d, float u, float w)
{
    return lerp(lerp(a, b, u), lerp(c, d, u), w);
}

static slow slow_mix(const slow *a, const slow *b, const slow *c, const slow *d, float u, float w)
{
#define MIX(f) mix4(a->f, b->f, c->f, d->f, u, w)
    slow s = {{MIX(sh.ox), MIX(sh.oz), MIX(sh.cont), MIX(sh.mask)}, MIX(rug), MIX(mesa), MIX(temp), MIX(rain)};
#undef MIX
    return s;
}

/* The slow fields at one block, from the four grid points around it. */
static slow slow_point(uint32_t seed, int x, int z)
{
    int x0 = (int)(floor(x / (double)SLOW) * SLOW), z0 = (int)(floor(z / (double)SLOW) * SLOW);
    slow a = slow_at(seed, x0, z0), b = slow_at(seed, x0 + SLOW, z0);
    slow c = slow_at(seed, x0, z0 + SLOW), d = slow_at(seed, x0 + SLOW, z0 + SLOW);
    return slow_mix(&a, &b, &c, &d, (float)(x - x0) / SLOW, (float)(z - z0) / SLOW);
}

/* Desert: hot, dry, lowland away from the ranges. */
static float desert_of(const slow *s)
{
    float coast = smooth(-0.02f, 0.18f, s->sh.cont);
    return smooth(16.0f, 21.0f, s->temp) * (1.0f - smooth(0.12f, 0.24f, s->rain)) * coast * (1.0f - s->sh.mask);
}

/* Ground height at a block, in metres (blocks), before caves. fine gets
 * the finest relief field, -1..1, which also patches the soil. */
static float land_at(uint32_t seed, int x, int z, const slow *s, float *fine)
{
    double wx = x + (double)s->sh.ox, wz = z + (double)s->sh.oz;
    float coast = smooth(-0.02f, 0.18f, s->sh.cont), deep = smooth(0.0f, 0.25f, -s->sh.cont);
    float mask = s->sh.mask, rug = s->rug, desert = desert_of(s);
    float h = SEA - 14.0f + 26.0f * coast - 18.0f * deep;
    *fine = 0.0f;
    if (coast > 0.0f) {
        h += 6.0f * coast * fbm2d(seed + 17, wx / 300.0, wz / 300.0, 4);
        /* Hills, fewer inside the ranges (which bring their own) and in
         * deserts (which have dunes instead). */
        float hills = 0.5f + 0.5f * eroded2d(seed + 61, wx / 350.0, wz / 350.0, 5);
        h += coast * (1.0f - 0.7f * mask) * (1.0f - 0.7f * desert) * hills * (8.0f + 30.0f * rug);
        *fine = fbm2d(seed + 67, wx / 55.0, wz / 55.0, 3);
        h += coast * *fine * (1.5f + 3.5f * rug);
    }
    if (mask > 0.0f) {
        /* Eroded relief on a raised base, so foothills lead up to the
         * peaks, and crests sharpened where the eroded field rounds them.
         * Its octaves damp each other down to about +-0.3 (1st to 99th
         * percentile), so it is stretched to fill 0..1. */
        float rel = sat(0.5f + 1.5f * eroded2d(seed + 51, wx / 600.0, wz / 600.0, 7));
        float crest = 1.0f - fabsf(noise2d(seed + 53, wx / 380.0, wz / 380.0));
        h += mask * (28.0f + rel * rel * sqrtf(rel) * 230.0f + crest * crest * crest * 45.0f * rel);
    }
    if (desert > 0.0f) {
        /* Dunes: long crests across the wind, wavering along their length. */
        double dx = wx + 30.0 * (double)noise2d(seed + 83, wx / 200.0, wz / 200.0);
        float dune = 1.0f - fabsf(noise2d(seed + 81, dx / 70.0, wz / 260.0));
        h += desert * dune * dune * 9.0f;
    }
    if (s->mesa > 0.0f && h > SEA + 10.0f) {
        /* Hard beds every 14 m: flat tops, a steep rise at each step. */
        const float step = 14.0f;
        float above = h - (SEA + 10.0f), bed = floorf(above / step);
        float terrace = SEA + 10.0f + step * (bed + smooth(0.8f, 1.0f, above / step - bed));
        h += (terrace - h) * s->mesa;
    }
    if (h > KNEE) {
        float d = h - KNEE;
        h = KNEE + (TOP - KNEE) * d / (d + (TOP - KNEE));
    }
    return h;
}

static int height_of(float h) { return clampi((int)h, 4, WORLD_H - 12); }

/* Biome from the ground's mean temperature and rainfall (a Whittaker
 * diagram), height and the coast; jit wobbles the edges by tens of
 * metres. */
static biome biome_at(int height, const slow *s, float temp, float rain, float jit)
{
    float h = (float)height;
    if (height < SEA_LEVEL) return BIOME_OCEAN;
    if (h < SEA + 2.5f && s->sh.cont < 0.3f) return BIOME_BEACH;
    if (temp < -3.0f) return h > SEA + 110.0f ? BIOME_SNOW : BIOME_TUNDRA;
    if (h > SEA + 150.0f) return temp < 0.0f ? BIOME_SNOW : BIOME_ROCKY;
    if (h > SEA + 120.0f + 8.0f * jit) return BIOME_ALPINE;
    if (temp < 3.0f) return rain > 0.35f ? BIOME_TAIGA : BIOME_TUNDRA;
    if (desert_of(s) > 0.5f || (rain < 0.14f && temp > 17.0f)) return BIOME_DESERT;
    if (rain > 0.78f && h < SEA + 12.0f) return BIOME_WETLAND;
    if (rain > 0.55f) return BIOME_FOREST;
    if (rain > 0.38f) return BIOME_MEADOW;
    if (rain > 0.22f || temp < 12.0f) return BIOME_STEPPE;
    return BIOME_SCRUB;
}

typedef struct {
    float temp, rain;
    biome b;
} local;

static local local_at(uint32_t seed, int x, int z, const slow *s, int height)
{
    float jit = fbm2d(seed + 401, x / 45.0, z / 45.0, 2);
    local l = {0};
    l.temp = s->temp - (float)CLIMATE_LAPSE * fmaxf(0.0f, (float)height - SEA) + 1.5f * jit;
    l.rain = sat(s->rain + 0.07f * jit);
    l.b = biome_at(height, s, l.temp, l.rain, jit);
    return l;
}

worldgen_point worldgen_sample(uint32_t seed, int x, int z)
{
    slow s = slow_point(seed, x, z);
    float fine = 0.0f;
    int height = height_of(land_at(seed, x, z, &s, &fine));
    local l = local_at(seed, x, z, &s, height);
    worldgen_point p = {height, l.temp, l.rain, l.b};
    return p;
}

int worldgen_height(uint32_t seed, int x, int z)
{
    slow s = slow_point(seed, x, z);
    float fine = 0.0f;
    return height_of(land_at(seed, x, z, &s, &fine));
}

float worldgen_sea_temp(uint32_t seed, int x, int z)
{
    int x0 = (int)(floor(x / (double)SLOW) * SLOW), z0 = (int)(floor(z / (double)SLOW) * SLOW);
    float t[4] = {0}, r = 0.0f;
    for (int i = 0; i < 4; i++) climate_at(seed, x0 + (i & 1) * SLOW, z0 + (i >> 1) * SLOW, &t[i], &r);
    return mix4(t[0], t[1], t[2], t[3], (float)(x - x0) / SLOW, (float)(z - z0) / SLOW);
}

const char *biome_name(biome b)
{
    static const char *const NAMES[BIOME_COUNT] = {
        "ocean", "beach",  "tundra",  "taiga",  "forest", "meadow", "steppe",
        "scrub", "desert", "wetland", "alpine", "rocky",  "snow",
    };
    return (unsigned)b < BIOME_COUNT ? NAMES[b] : "?";
}

/* ------------------------------------------------------------- caves */

#define CAVE_STEP 4
#define CAVE_NX (CHUNK_W / CAVE_STEP + 1)
#define CAVE_NY (WORLD_H / CAVE_STEP + 1)
#define CAVE_FIELDS 4

/* Tunnels are where two independent noise fields are both near zero: the
 * intersection of two surfaces, a winding tube ("spaghetti" caves). A
 * third, slower field widens them in places, and a fourth opens chambers
 * deep down where it peaks. Their roofs are rock in place, which the
 * support check lets hang 15 blocks, so digging into a cave does not bring
 * its ceiling down. A tunnel breaks out of the ground only in a few
 * patches, so cave mouths are something to find, not a pockmark every few
 * metres. */
/* The last grid row field f is read at under ground of height h: the
 * chambers (field 3) keep 12 blocks below the surface. */
static int rows_of(int f, int h) { return (f == 3 ? h - 13 : h - 1) / CAVE_STEP + 1; }

/* A row of the height map, with the one-block rim on each side. */
typedef int hm_row[CHUNK_W + 2];

static void carve_caves(uint32_t seed, column *c, const hm_row *hm)
{
    int bx = c->cx * CHUNK_W, bz = c->cz * CHUNK_W, top = 0;
    for (int z = 0; z < CHUNK_W; z++)
        for (int x = 0; x < CHUNK_W; x++) top = hm[z + 1][x + 1] > top ? hm[z + 1][x + 1] : top;
    /* Nothing is carved above the ground, so the grid stops there. */
    int ny = (top - 1) / CAVE_STEP + 2;
    if (ny > CAVE_NY) ny = CAVE_NY;
    size_t plane = (size_t)ny * CAVE_NX * CAVE_NX;
    float *g = mem_calloc((size_t)CAVE_FIELDS * plane, sizeof *g);
#define G(f, j, k, i) g[(size_t)(f) * plane + ((size_t)(j) * CAVE_NX + (size_t)(k)) * CAVE_NX + (size_t)(i)]
    static const struct {
        uint32_t seed;
        double xz, y; /* feature sizes, m */
    } FIELD[CAVE_FIELDS] = {{71, 44.0, 22.0}, {73, 44.0, 22.0}, {79, 90.0, 45.0}, {89, 28.0, 14.0}};
    for (int f = 0; f < CAVE_FIELDS; f++)
        for (int j = 0; j < ny && j <= rows_of(f, top); j++)
            for (int k = 0; k < CAVE_NX; k++)
                for (int i = 0; i < CAVE_NX; i++) {
                    int x = bx + i * CAVE_STEP, y = j * CAVE_STEP, z = bz + k * CAVE_STEP;
                    G(f, j, k, i) = noise3d(seed + FIELD[f].seed, x / FIELD[f].xz, y / FIELD[f].y, z / FIELD[f].xz);
                }

    for (int z = 0; z < CHUNK_W; z++)
        for (int x = 0; x < CHUNK_W; x++) {
            int h = hm[z + 1][x + 1];
            /* On land a tunnel may break out of the ground; under water
             * and on beaches it stays sealed below the stone. */
            int land = h > SEA_LEVEL + 3, mouth = -1; /* -1: not looked up yet */
            int i = x / CAVE_STEP, k = z / CAVE_STEP;
            float u = (float)(x % CAVE_STEP) / CAVE_STEP, w = (float)(z % CAVE_STEP) / CAVE_STEP;
            /* Blend across x and z once per grid height; each block then
             * only blends between two heights. */
            float cv[CAVE_FIELDS][CAVE_NY] = {{0}};
            for (int f = 0; f < CAVE_FIELDS; f++)
                for (int j = 0; j <= rows_of(f, h) && j < ny; j++) {
                    float a = G(f, j, k, i) + u * (G(f, j, k, i + 1) - G(f, j, k, i));
                    float b = G(f, j, k + 1, i) + u * (G(f, j, k + 1, i + 1) - G(f, j, k + 1, i));
                    cv[f][j] = a + w * (b - a);
                }
            uint8_t *col = &c->blocks[col_index(x, 0, z)];
            int open = 0; /* the block above was carved, or is the sky */
            for (int y = h - 1; y >= 3; y--) {
                int j = y / CAVE_STEP;
                float v = (float)(y % CAVE_STEP) / CAVE_STEP, val[CAVE_FIELDS] = {0};
                for (int f = 0; f < CAVE_FIELDS; f++) val[f] = cv[f][j] + v * (cv[f][j + 1] - cv[f][j]);
                float width = 0.010f + 0.012f * fmaxf(0.0f, val[2]);
                int tunnel = val[0] * val[0] + val[1] * val[1] < width;
                /* Underground a tunnel keeps at least four layers of rock over
                 * it (one layer under soil gave way over wide spots); nearer
                 * the surface only an opening carved from the sky down, so no
                 * soil or sand is ever left hanging over a hole. */
                int deep = y < h - 9, surface = land && (open || y == h - 1);
                if (tunnel && !deep && surface && mouth < 0)
                    mouth = noise2d(seed + 87, (bx + x) / 160.0, (bz + z) / 160.0) > 0.45f;
                int carve = tunnel && (deep || (surface && mouth > 0));
                /* Chambers stay a dozen blocks down, under a thick roof. */
                carve |= val[3] > 0.55f && y > 6 && y < h - 12;
                uint8_t above = col[(y + 1) * COL_AREA];
                if (above == B_WATER || (block_get(above)->flags & BF_GRANULAR)) carve = 0;
                if (carve) col[y * COL_AREA] = B_AIR;
                open = carve;
            }
        }
#undef G
    mem_free(g);
}

/* ------------------------------------------------------------ strata */

/* Under the soil lies a basement of crystalline rock (granite in some
 * regions, basalt in others, plain stone between) and over it beds laid
 * down flat in old seas and deserts: limestone, sandstone and shale, with
 * rock salt where a sea dried out. The basement rises into the mountain
 * ranges, so their cores are bare granite while the lowlands sit on tens
 * of metres of beds, and it caps the land on basalt plateaus. Beds follow
 * height, gently folded, so a cliff shows them as bands and a bed runs on
 * from one column into the next. */
#define BED 3                       /* m: the thinnest bed */
#define BED_LO (-16)                /* the lowest bed unit, under the deepest fold */
#define BED_N ((WORLD_H + 64) / BED) /* bed units from BED_LO up */

typedef struct {
    float felsic; /* > 0.15 granite, < -0.25 basalt, plain stone between */
    float base;   /* m: basement top below sea level, before the ranges lift it */
    float marine; /* 0..1: how much of the beds is limestone */
    float fold;   /* -1..1: how the beds are bent */
} geo;

static geo geo_at(uint32_t seed, int x, int z)
{
    geo g = {fbm2d(seed + 501, x / 3200.0, z / 3200.0, 2), 30.0f + 25.0f * fbm2d(seed + 503, x / 2400.0, z / 2400.0, 2),
             sat(0.5f + 1.2f * fbm2d(seed + 509, x / 2800.0, z / 2800.0, 2)),
             fbm2d(seed + 507, x / 500.0, z / 500.0, 2)};
    return g;
}

/* The rock under one block of ground. */
typedef struct {
    int shift;        /* m added to a height to find its bed: the fold, kept positive */
    float lime, sand; /* shares of limestone and sandstone; shale has the rest */
    float salt;       /* share of the shale beds that is rock salt, deep down */
    int basement;     /* the basement's top, y */
    int glass;        /* from this y up the basement is obsidian */
    int granite_to;   /* the basement is granite below this y */
    int basalt_from;  /* and basalt from this y up; plain stone between */
    uint8_t granite;  /* granite under the beds, which bakes limestone into marble */
    uint8_t meta;     /* squeezed in a range: limestone has turned to marble */
    uint8_t run_rock; /* filling the column: the rock of the current run */
    int run_end;      /* and the height where it may change */
} strata;

/* One draw per 3 m unit, shared by every column so a bed meets itself
 * across their borders. Half the time a bed runs on through the next
 * unit, so beds come 3, 6, 9 m thick. */
static void bed_sequence(uint32_t seed, float *u)
{
    float prev = 0.0f;
    for (int k = 0; k < BED_N; k++) {
        uint32_t h = hash2i(seed ^ 0xBEDBEDu, k, 0);
        prev = k > 0 && (h & 1u) ? prev : (float)(h >> 8) / 16777216.0f;
        u[k] = prev;
    }
}

static uint8_t basement_at(const strata *s, int y)
{
    uint8_t above = y >= s->basalt_from ? B_BASALT_ROCK : B_ROCK;
    return y < s->granite_to ? B_GRANITE_ROCK : above;
}

static strata strata_at(uint32_t seed, int x, int z, const slow *s, const geo *g, int h, float fine)
{
    strata st = {0};
    float basalt = smooth(-0.2f, -0.4f, g->felsic);
    /* Granite where the province field, with a waver along its contacts,
     * is over 0.15, basalt under -0.25. Granite bodies widen with depth and
     * basalt flows thin out, so the field leans 0.0004 per metre towards
     * granite the deeper it is read: the contacts dip at about 60 degrees
     * instead of falling as sheer walls. */
    float felsic = g->felsic + 0.03f * fine;
    st.granite_to = clampi((int)ceilf(SEA + (felsic - 0.15f) * 2500.0f), 0, WORLD_H);
    st.basalt_from = clampi((int)floorf(SEA + (felsic + 0.25f) * 2500.0f) + 1, 0, WORLD_H);
    /* Lava flows cap a basalt plateau, so its basement reaches the top. A
     * range is basement pushed up through the beds, which erosion has
     * stripped from its heart: there the cover thins to nothing. */
    float flat = SEA - g->base + 150.0f * s->sh.mask - 60.0f * s->mesa + 50.0f * basalt;
    st.basement = (int)lerp(flat, (float)h + 4.0f, smooth(0.55f, 0.95f, s->sh.mask));
    st.glass = WORLD_H;
    st.granite = basement_at(&st, st.basement) == B_GRANITE_ROCK;
    if (basement_at(&st, h) == B_BASALT_ROCK && noise2d(seed + 511, x / 40.0, z / 40.0) > 0.35f) st.glass = h - 8;
    /* Beds bend more in a range. A height is whole metres, so the fold's
     * whole metres are all that decide which bed it falls in. */
    st.shift = (int)floorf((2.0f + 18.0f * s->sh.mask) * g->fold) - BED * BED_LO;
    /* Desert and mesa country lie on old dunes; elsewhere old seas left
     * limestone and mud (shale). */
    st.sand = 0.15f + 0.35f * sat(1.0f - 2.0f * s->rain) + 0.35f * s->mesa;
    st.lime = (1.0f - st.sand) * (0.25f + 0.55f * g->marine);
    st.salt = 0.6f * (1.0f - smooth(0.2f, 0.35f, s->rain));
    st.meta = s->sh.mask > 0.55f;
    return st;
}

/* What a bed unit was laid down as, before depth has its say: 0 limestone,
 * 1 sandstone, 2 shale, 3 shale that is salt where deep enough. */
static int bed_kind(const strata *s, float u)
{
    if (u < s->lime) return 0;
    if (u < s->lime + s->sand) return 1;
    return (u - s->lime - s->sand) / (1.0f - s->lime - s->sand) < s->salt ? 3 : 2;
}

static uint8_t bed_rock(const strata *s, int kind, int y)
{
    /* Limestone turns to marble in a range and where granite intruded
     * beneath it. */
    if (kind == 0) return s->meta || (s->granite && y < s->basement + 5) ? B_MARBLE_ROCK : B_LIMESTONE_ROCK;
    if (kind == 1) return B_SANDSTONE_ROCK;
    /* Salt dissolves near the surface, so it lasts only deep down. */
    return kind == 3 && y < SEA_LEVEL - 8 ? B_ROCK_SALT : B_SHALE_ROCK;
}

static int unit_of(const strata *s, int y) { return clampi((y + s->shift) / BED, 0, BED_N - 1); }

/* The rock at y, and in end the height up to which it runs unchanged:
 * the next bed unit, contact or depth where marble or salt give out. The
 * column is filled a layer at a time, as contiguous memory, looking the
 * rock up only where a run ends. */
static uint8_t rock_run(const strata *s, const float *beds, int y, int *end)
{
    if (y < s->basement) {
        *end = s->basement;
        if (y >= s->glass) return B_OBSIDIAN_ROCK;
        *end = s->glass < *end ? s->glass : *end;
        if (y < s->granite_to) {
            *end = s->granite_to < *end ? s->granite_to : *end;
            return B_GRANITE_ROCK;
        }
        if (y >= s->basalt_from) return B_BASALT_ROCK;
        *end = s->basalt_from < *end ? s->basalt_from : *end;
        return B_ROCK;
    }
    int k = unit_of(s, y);
    *end = k == BED_N - 1 ? WORLD_H : (k + 1) * BED - s->shift; /* the next unit's start */
    if (y < s->basement + 5 && s->basement + 5 < *end) *end = s->basement + 5;
    if (y < SEA_LEVEL - 8 && SEA_LEVEL - 8 < *end) *end = SEA_LEVEL - 8;
    return bed_rock(s, bed_kind(s, beds[k]), y);
}

static uint8_t rock_at(const strata *s, const float *beds, int y)
{
    int end = 0;
    return rock_run(s, beds, y, &end);
}

/* -------------------------------------------------------------- ores */

typedef struct {
    int veins, size, ymin, ymax; /* per column; blocks per vein; height band */
    uint8_t block;
    uint8_t host[3]; /* the rocks it forms in; the vein replaces only these */
    uint8_t rich;    /* a richer ore it carries now and then, below ymax - 60 */
    uint8_t odds;    /* one block in odds of a vein is the rich ore */
} ore_rule;

/* Each ore in the rock it really forms in. Coal in the beds; copper in
 * granite and basalt; tin and lithium (pegmatite) in granite; lead with
 * silver and zinc in limestone; nickel and ilmenite in basalt; gold in
 * quartz veins; graphite in marble and the old stone; sulfur round
 * basalt and salt. */
static const ore_rule ORES[] = {
    {26, 9, 8, 180, B_COAL_ORE, {B_SHALE_ROCK, B_SANDSTONE_ROCK, B_SHALE_ROCK}, 0, 0},
    {12, 7, 4, 160, B_COPPER_ORE, {B_GRANITE_ROCK, B_BASALT_ROCK, B_ROCK}, 0, 0},
    {9, 6, 4, 80, B_IRON_ORE, {B_ROCK, B_BASALT_ROCK, B_SANDSTONE_ROCK}, 0, 0},
    {6, 5, 8, 160, B_TIN_ORE, {B_GRANITE_ROCK, B_GRANITE_ROCK, B_GRANITE_ROCK}, 0, 0},
    {6, 6, 8, 150, B_ZINC_ORE, {B_LIMESTONE_ROCK, B_MARBLE_ROCK, B_LIMESTONE_ROCK}, 0, 0},
    {6, 6, 8, 150, B_LEAD_ORE, {B_LIMESTONE_ROCK, B_MARBLE_ROCK, B_LIMESTONE_ROCK}, B_SILVER_ORE, 6},
    {4, 5, 4, 100, B_NICKEL_ORE, {B_BASALT_ROCK, B_BASALT_ROCK, B_BASALT_ROCK}, 0, 0},
    {4, 5, 4, 120, B_TITANIUM_ORE, {B_BASALT_ROCK, B_ROCK, B_BASALT_ROCK}, 0, 0},
    {2, 4, 4, 100, B_URANIUM_ORE, {B_GRANITE_ROCK, B_SANDSTONE_ROCK, B_GRANITE_ROCK}, 0, 0},
    {2, 6, 8, 160, B_LITHIUM_ORE, {B_GRANITE_ROCK, B_GRANITE_ROCK, B_GRANITE_ROCK}, 0, 0},
    {4, 5, 20, 240, B_SULFUR_ORE, {B_BASALT_ROCK, B_ROCK_SALT, B_BASALT_ROCK}, 0, 0},
    {5, 10, 4, 200, B_QUARTZ_ORE, {B_GRANITE_ROCK, B_ROCK, B_SANDSTONE_ROCK}, B_GOLD_ORE, 7},
    {3, 5, 8, 180, B_GRAPHITE_ORE, {B_MARBLE_ROCK, B_ROCK, B_MARBLE_ROCK}, 0, 0},
};

static uint32_t lcg(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

/* Veins are short random walks from hashed starts, replacing their host
 * rock only and clipped to the column (like trees), so no neighbour data
 * is needed. */
/* Foxfire: fungus glowing on the floor of caves deep enough to stay damp
 * and dark, more than FOXFIRE_DEPTH under the ground. It grows in patches
 * (a 4 x 4 m cell in 23 has some), three floor blocks in four, so deep
 * caves are never quite black and a dim green marks the way. */
#define FOXFIRE_DEPTH 24
static void place_foxfire(uint32_t seed, column *c, const int (*hm)[CHUNK_W + 2])
{
    int bx = c->cx * CHUNK_W, bz = c->cz * CHUNK_W;
    for (int z = 0; z < CHUNK_W; z++)
        for (int x = 0; x < CHUNK_W; x++) {
            if (hash2i(seed ^ 0x5F0F17E5u, (bx + x) >> 2, (bz + z) >> 2) % 23u) continue;
            for (int y = 2; y < hm[z + 1][x + 1] - FOXFIRE_DEPTH; y++) {
                uint8_t *floor = &c->blocks[col_index(x, y - 1, z)];
                if (c->blocks[col_index(x, y, z)] != B_AIR || !(block_get(*floor)->flags & BF_OPAQUE)) continue;
                if (hash2i(seed ^ 0x0F0F1235u ^ (uint32_t)y * 0x9E3779B9u, bx + x, bz + z) % 4u) *floor = B_FOXFIRE;
            }
        }
}

static void place_ores(uint32_t seed, column *c)
{
    for (int o = 0; o < (int)(sizeof ORES / sizeof ORES[0]); o++) {
        const ore_rule *r = &ORES[o];
        uint32_t s = hash2i(seed ^ (0x0E5E0000u + (uint32_t)o), c->cx, c->cz);
        for (int v = 0; v < r->veins; v++) {
            int x = (int)(lcg(&s) % CHUNK_W), z = (int)(lcg(&s) % CHUNK_W);
            int y = r->ymin + (int)(lcg(&s) % (uint32_t)(r->ymax - r->ymin));
            int n = r->size / 2 + (int)(lcg(&s) % (uint32_t)r->size);
            for (int b = 0; b < n; b++) {
                if (x >= 0 && x < CHUNK_W && z >= 0 && z < CHUNK_W && y > 0 && y < WORLD_H) {
                    uint8_t *p = &c->blocks[col_index(x, y, z)];
                    int rich = r->rich && y < r->ymax - 60 && lcg(&s) % r->odds == 0;
                    if (*p == r->host[0] || *p == r->host[1] || *p == r->host[2]) *p = rich ? r->rich : r->block;
                }
                switch (lcg(&s) % 6) {
                case 0: x++; break;
                case 1: x--; break;
                case 2: y++; break;
                case 3: y--; break;
                case 4: z++; break;
                default: z--; break;
                }
            }
        }
    }
}

/* ------------------------------------------------------------- trees */

static void put(column *c, int x, int y, int z, uint8_t id)
{
    if (x < 0 || x >= CHUNK_W || z < 0 || z >= CHUNK_W || y < 1 || y >= WORLD_H) return;
    uint8_t *b = &c->blocks[col_index(x, y, z)];
    if (*b == B_AIR || (*b == B_LEAVES && id == B_LOG)) *b = id;
}

static void grow_tree(column *c, uint32_t h, int x, int y, int z)
{
    int trunk = 4 + (int)(h % 3);
    int top = y + trunk;
    if (top + 2 >= WORLD_H) return;
    for (int ly = top - 2; ly <= top + 1; ly++) {
        int r = ly >= top ? 1 : 2;
        for (int dz = -r; dz <= r; dz++)
            for (int dx = -r; dx <= r; dx++) {
                /* Canopy corners would be 4 leaf-steps from the trunk, past
                 * the 3 that leaves can span, so they would collapse. */
                if (r == 2 && dx * dx == 4 && dz * dz == 4) continue;
                put(c, x + dx, ly, z + dz, B_LEAVES);
            }
    }
    for (int ly = y; ly < top; ly++) put(c, x, ly, z, B_LOG);
}

/* Trees per 10,000 blocks of ground, by biome. */
static const uint16_t TREES[BIOME_COUNT] = {
    [BIOME_FOREST] = 260, [BIOME_TAIGA] = 220, [BIOME_WETLAND] = 120, [BIOME_MEADOW] = 25,
    [BIOME_SCRUB] = 10,   [BIOME_STEPPE] = 5,  [BIOME_ALPINE] = 3,
};

/* ------------------------------------------------------------ column */

/* Soil over the rock: what lies on top, what under it, the lowest layer
 * (0: more filler) and how deep. Depth 0 leaves the rock bare. */
typedef struct {
    uint8_t top, filler, base, depth;
} soil;

/* Soil weathers out of the rock under it and washes off slopes, so it is
 * deepest on flat, warm, wet ground. Shale weathers into clay and makes
 * more of it; limestone dissolves and leaves little. Cohesive soil stays
 * within 5 blocks, so a tunnel under it keeps four layers of rock. */
static soil soil_at(biome b, int h, int slope, int rise, const local *l, float fine, uint8_t rock)
{
    if (h < SEA_LEVEL) {
        if (slope > 5) return (soil){0, 0, 0, 0};
        return h <= SEA_LEVEL - 5 ? (soil){B_GRAVEL, B_GRAVEL, 0, 4} : (soil){B_SAND, B_SAND, 0, 4};
    }
    if (slope > 5 || b == BIOME_ROCKY) return (soil){0, 0, 0, 0}; /* cliffs and crags: the beds show */
    /* Beaches: sand where the shore is flat, shingle where it steepens. */
    if (b == BIOME_BEACH) return slope <= 2 ? (soil){B_SAND, B_SAND, 0, 4} : (soil){B_GRAVEL, B_GRAVEL, 0, 3};
    /* Scree gathers on the slope at the foot of a rise. */
    if (slope >= 3 && rise >= 3 && b != BIOME_DESERT) return (soil){B_GRAVEL, B_GRAVEL, 0, 2};
    /* Saltpeter crusts form where rain never washes the nitrates out. */
    uint8_t crust = l->rain < 0.25f && fine < -0.3f ? B_SALTPETER_ORE : 0;
    if (b == BIOME_DESERT) return (soil){B_SAND, B_SAND, crust, 6};
    if (b == BIOME_SNOW) return (soil){B_SNOW, B_SNOW, 0, 1};
    int depth = 1 + (int)(3.0f * l->rain) + (l->temp > 8.0f) - slope / 2 + (rock == B_SHALE_ROCK) -
                (rock == B_LIMESTONE_ROCK || rock == B_MARBLE_ROCK);
    soil so = {l->temp < -1.0f ? B_SNOW : B_GRASS, B_DIRT, crust, (uint8_t)clampi(depth, 1, 5)};
    /* Wet lowland subsoil is clay, and so is soil from shale; hot, wet
     * ground weathers down to a crust of bauxite (laterite). */
    if (rock == B_SHALE_ROCK || (l->rain > 0.6f && h < SEA_LEVEL + 15)) so.base = B_CLAY;
    if (l->temp > 16.0f && l->rain > 0.45f && fine > 0.0f) so.base = B_BAUXITE;
    switch (b) {
    case BIOME_TUNDRA: so.top = l->temp < -6.0f ? B_SNOW : B_GRASS; break;
    case BIOME_SCRUB: so.top = fine > 0.15f ? B_DIRT : B_GRASS; break;
    /* Under conifers the rain leaches the topsoil grey (podzol); flat
     * hollows fill with peat bogs. */
    case BIOME_TAIGA:
        if (so.top == B_GRASS) so.top = slope == 0 && fine < -0.4f ? B_PEAT : B_PODZOL;
        break;
    /* Wetlands: mud in the hollows, peat under cold marsh, clay under warm. */
    case BIOME_WETLAND:
        so.filler = l->temp < 8.0f ? B_PEAT : B_CLAY;
        if (so.top == B_GRASS && slope <= 1 && fine < -0.2f) so.top = B_MUD;
        break;
    default: break;
    }
    return so;
}

void worldgen_column(uint32_t seed, column *c)
{
    int bx = c->cx * CHUNK_W, bz = c->cz * CHUNK_W;
    /* Grid points from one SLOW before the column to one after: they cover
     * the column and the ring of neighbours its slopes look at. */
    slow node[4][4];
    geo gnode[4][4];
    for (int k = 0; k < 4; k++)
        for (int i = 0; i < 4; i++) {
            node[k][i] = slow_at(seed, bx + (i - 1) * SLOW, bz + (k - 1) * SLOW);
            gnode[k][i] = geo_at(seed, bx + (i - 1) * SLOW, bz + (k - 1) * SLOW);
        }

    int hm[CHUNK_W + 2][CHUNK_W + 2] = {0};
    local loc[CHUNK_W][CHUNK_W] = {{{0}}};
    float fine[CHUNK_W][CHUNK_W] = {{0}};
    strata *st = mem_calloc(COL_AREA, sizeof *st);
    float beds[BED_N] = {0};
    bed_sequence(seed, beds);
    for (int z = 0; z < CHUNK_W + 2; z++)
        for (int x = 0; x < CHUNK_W + 2; x++) {
            /* Offsets 7..24 from the first grid point; the last falls on
             * the far grid point with u = 1, which lerp makes exact. */
            int rx = x + SLOW - 1, rz = z + SLOW - 1;
            int i = rx / SLOW < 2 ? rx / SLOW : 2, k = rz / SLOW < 2 ? rz / SLOW : 2;
            slow s = slow_mix(&node[k][i], &node[k][i + 1], &node[k + 1][i], &node[k + 1][i + 1],
                              (float)(rx - i * SLOW) / SLOW, (float)(rz - k * SLOW) / SLOW);
            int wx = bx + x - 1, wz = bz + z - 1;
            float f = 0.0f;
            hm[z][x] = height_of(land_at(seed, wx, wz, &s, &f));
            if (x < 1 || x > CHUNK_W || z < 1 || z > CHUNK_W) continue;
            fine[z - 1][x - 1] = f;
            loc[z - 1][x - 1] = local_at(seed, wx, wz, &s, hm[z][x]);
            float u = (float)(rx - i * SLOW) / SLOW, w = (float)(rz - k * SLOW) / SLOW;
            const geo *g00 = &gnode[k][i], *g10 = &gnode[k][i + 1], *g01 = &gnode[k + 1][i],
                      *g11 = &gnode[k + 1][i + 1];
            geo g = {mix4(g00->felsic, g10->felsic, g01->felsic, g11->felsic, u, w),
                     mix4(g00->base, g10->base, g01->base, g11->base, u, w),
                     mix4(g00->marine, g10->marine, g01->marine, g11->marine, u, w),
                     mix4(g00->fold, g10->fold, g01->fold, g11->fold, u, w)};
            st[(z - 1) * CHUNK_W + x - 1] = strata_at(seed, wx, wz, &s, &g, hm[z][x], f);
        }

    /* The soil of every block first, then the column a layer at a time:
     * a layer is contiguous in memory, a block's own stack is not. */
    soil so[COL_AREA] = {{0}};
    int hs[COL_AREA] = {0}, top = SEA_LEVEL;
    for (int z = 0; z < CHUNK_W; z++)
        for (int x = 0; x < CHUNK_W; x++) {
            int h = hm[z + 1][x + 1], i = z * CHUNK_W + x;
            int w = hm[z + 1][x], e = hm[z + 1][x + 2], n = hm[z][x + 1], s = hm[z + 2][x + 1];
            int slope = clampi(abs(w - e) + abs(n - s), 0, 64);
            int rise = imax(imax(w, e), imax(n, s)) - h;
            /* The soil depends on the rock a few blocks down, where it
             * weathers from. */
            so[i] = soil_at(loc[z][x].b, h, slope, rise, &loc[z][x], fine[z][x], rock_at(&st[i], beds, h - 4));
            hs[i] = h;
            top = imax(top, h);
        }
    memset(c->blocks, B_BEDROCK, COL_AREA);
    for (int y = 1; y < top; y++) {
        uint8_t *layer = &c->blocks[y * COL_AREA];
        for (int i = 0; i < COL_AREA; i++) {
            int h = hs[i], d = so[i].depth;
            if (y >= h) layer[i] = y < SEA_LEVEL ? B_WATER : B_AIR;
            else if (y < h - d) {
                if (y >= st[i].run_end) st[i].run_rock = rock_run(&st[i], beds, y, &st[i].run_end);
                layer[i] = st[i].run_rock;
            } else if (y == h - 1) layer[i] = so[i].top;
            else layer[i] = y == h - d && so[i].base ? so[i].base : so[i].filler;
        }
    }
    mem_free(st);
    memset(&c->blocks[top * COL_AREA], B_AIR, (size_t)(WORLD_H - top) * COL_AREA);
    /* Cold seas freeze over. */
    for (int i = 0; i < COL_AREA; i++)
        if (hs[i] < SEA_LEVEL && loc[i / CHUNK_W][i % CHUNK_W].temp < -2.0f)
            c->blocks[(SEA_LEVEL - 1) * COL_AREA + i] = B_ICE;

    place_ores(seed, c);
    carve_caves(seed, c, (const hm_row *)hm);
    place_foxfire(seed, c, (const hm_row *)hm);

    /* Trees only where the whole canopy fits in this column: avoids needing
     * neighbour data on worker threads, at the cost of no trees on the
     * outermost 2 blocks of each column. They grow in stands with
     * clearings between. A cave mouth has taken the grass, so no tree
     * stands over one. */
    for (int z = 2; z < CHUNK_W - 2; z++)
        for (int x = 2; x < CHUNK_W - 2; x++) {
            uint32_t h = hash2i(seed ^ 0xA5A5A5A5U, bx + x, bz + z);
            uint32_t roll = h % 10000u, rate = TREES[loc[z][x].b];
            if (roll >= rate) continue;
            float stand = fbm2d(seed + 301, (bx + x) / 120.0, (bz + z) / 120.0, 2);
            if ((float)roll >= (float)rate * sat(0.55f + 1.4f * stand)) continue;
            int y = hm[z + 1][x + 1];
            uint8_t ground = c->blocks[col_index(x, y - 1, z)];
            if (ground != B_GRASS && ground != B_PODZOL) continue;
            /* Over a cave the soil, not the ground, would carry the tree, and
             * its canopy would drop at the first dig nearby. A structural
             * check takes everything STRUCT_RADIUS below where it starts as
             * standing, so only a cave within that reach counts: looking all
             * the way down left most of the deep ground's forests bare. */
            int hollow = 0;
            for (int ly = imax(1, y - 1 - STRUCT_RADIUS); ly < y - 1 && !hollow; ly++)
                hollow = c->blocks[col_index(x, ly, z)] == B_AIR;
            if (hollow) continue;
            c->blocks[col_index(x, y - 1, z)] = B_DIRT;
            grow_tree(c, h >> 8, x, y, z);
        }
}
