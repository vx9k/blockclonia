/* Heightmap terrain with beaches, snowy peaks and trees, carved by cave
 * tunnels and seeded with ore. The surface is 2D noise; the caves are 3D
 * noise sampled on a coarse grid and interpolated, which costs ~1/64 of
 * evaluating it per block, so low-end CPUs keep up with streaming. */
#include "world.h"
#include "noise.h"
#include "mathlib.h"

#include <stdlib.h>
#include <string.h>

static int height_v1(uint32_t seed, int x, int z)
{
    float fx = (float)x, fz = (float)z;
    float continent = fbm2(seed, fx / 700.0f, fz / 700.0f, 3);
    float hills = fbm2(seed + 17, fx / 110.0f, fz / 110.0f, 4);
    float ridge = 1.0f - fabsf(noise2(seed + 31, fx / 260.0f, fz / 260.0f));
    float mountain = fmaxf(0.0f, continent + 0.15f) * ridge * ridge * ridge;
    float h = 51.0f + continent * 24.0f + hills * 8.0f + mountain * 60.0f;
    return clampi((int)h, 4, WORLD_H - 12);
}

int worldgen_height(uint32_t seed, int x, int z)
{
    float fx = (float)x, fz = (float)z;
    float continent = fbm2(seed, fx / 700.0f, fz / 700.0f, 3);
    float hills = fbm2(seed + 17, fx / 110.0f, fz / 110.0f, 4);
    /* Ranges: a slow mask decides where mountains rise at all, and two
     * octaves of ridged noise give them sharp crests and steep flanks. */
    float range = fbm2(seed + 43, fx / 1300.0f, fz / 1300.0f, 2);
    float mask = fminf(1.0f, fmaxf(0.0f, (range + continent * 0.5f + 0.05f) * 2.5f));
    float r1 = 1.0f - fabsf(noise2(seed + 31, fx / 260.0f, fz / 260.0f));
    float r2 = 1.0f - fabsf(noise2(seed + 37, fx / 90.0f, fz / 90.0f));
    float ridge = r1 * r1 * r1 * (0.75f + 0.25f * r2 * r2);
    float h = 51.0f + continent * 24.0f + hills * 8.0f + mask * ridge * 78.0f;
    /* Ease into the build limit instead of flattening peaks against it. */
    const float knee = 96.0f, room = (float)(WORLD_H - 12) - knee;
    if (h > knee) h = knee + room * (1.0f - expf(-(h - knee) / room));
    return clampi((int)h, 4, WORLD_H - 12);
}

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

/* ------------------------------------------------------------- caves */

#define CAVE_STEP 4
#define CAVE_NX (CHUNK_W / CAVE_STEP + 1)
#define CAVE_NY (WORLD_H / CAVE_STEP + 1)

/* Tunnels are where two independent noise fields are both near zero: the
 * intersection of two surfaces, a winding tube ("spaghetti" caves). A third,
 * slower field widens them in places. Their roofs are rock (B_ROCK), which
 * the support check lets hang 15 blocks, so digging into a cave does not
 * bring its ceiling down. */
static void carve_caves(uint32_t seed, column *c, const int hm[CHUNK_W + 2][CHUNK_W + 2])
{
    static const int FIELDS = 3;
    float g[3][CAVE_NY][CAVE_NX][CAVE_NX] = {{{{0}}}};
    int bx = c->cx * CHUNK_W, bz = c->cz * CHUNK_W, top = 0;
    for (int z = 0; z < CHUNK_W; z++)
        for (int x = 0; x < CHUNK_W; x++) top = hm[z + 1][x + 1] > top ? hm[z + 1][x + 1] : top;
    /* Nothing is carved above the ground, so the grid stops there. */
    int ny = (top - 1) / CAVE_STEP + 2;
    if (ny > CAVE_NY) ny = CAVE_NY;
    for (int f = 0; f < FIELDS; f++)
        for (int j = 0; j < ny; j++)
            for (int k = 0; k < CAVE_NX; k++)
                for (int i = 0; i < CAVE_NX; i++) {
                    float x = (float)(bx + i * CAVE_STEP), y = (float)(j * CAVE_STEP), z = (float)(bz + k * CAVE_STEP);
                    g[f][j][k][i] = f < 2 ? noise3(seed + 71u + (uint32_t)f * 2u, x / 44.0f, y / 22.0f, z / 44.0f)
                                          : noise3(seed + 79u, x / 90.0f, y / 45.0f, z / 90.0f);
                }

    for (int z = 0; z < CHUNK_W; z++)
        for (int x = 0; x < CHUNK_W; x++) {
            int h = hm[z + 1][x + 1];
            /* On land a tunnel may break out of the ground; under water
             * and on beaches it stays sealed below the stone. */
            int land = h > SEA_LEVEL + 3;
            int i = x / CAVE_STEP, k = z / CAVE_STEP;
            float u = (float)(x % CAVE_STEP) / CAVE_STEP, w = (float)(z % CAVE_STEP) / CAVE_STEP;
            /* Blend across x and z once per grid height; each block then
             * only blends between two heights. */
            float cv[3][CAVE_NY] = {{0}};
            for (int f = 0; f < FIELDS; f++)
                for (int j = 0; j <= (h - 1) / CAVE_STEP + 1 && j < CAVE_NY; j++) {
                    float a = g[f][j][k][i] + u * (g[f][j][k][i + 1] - g[f][j][k][i]);
                    float b = g[f][j][k + 1][i] + u * (g[f][j][k + 1][i + 1] - g[f][j][k + 1][i]);
                    cv[f][j] = a + w * (b - a);
                }
            uint8_t *col = &c->blocks[col_index(x, 0, z)];
            int open = 0; /* the block above was carved, or is the sky */
            for (int y = h - 1; y >= 3; y--) {
                int j = y / CAVE_STEP;
                float v = (float)(y % CAVE_STEP) / CAVE_STEP, val[3] = {0};
                for (int f = 0; f < FIELDS; f++) val[f] = cv[f][j] + v * (cv[f][j + 1] - cv[f][j]);
                float width = 0.010f + 0.012f * fmaxf(0.0f, val[2]);
                int tunnel = val[0] * val[0] + val[1] * val[1] < width;
                /* Underground a tunnel keeps at least four layers of rock over
                 * it (one layer under soil gave way over wide spots); nearer
                 * the surface only an opening carved from the sky down, so no
                 * soil or sand is ever left hanging over a hole. */
                int deep = y < h - 9;
                int carve = tunnel && (deep || (land && (open || y == h - 1)));
                uint8_t above = col[(y + 1) * COL_AREA];
                if (above == B_WATER || (block_get(above)->flags & BF_GRANULAR)) carve = 0;
                if (carve) col[y * COL_AREA] = B_AIR;
                open = carve;
            }
        }
}

/* -------------------------------------------------------------- ores */

typedef struct {
    int veins, size, ymin, ymax; /* per column; blocks per vein; height band */
    uint8_t block;
} ore_rule;

/* Coal is common and shallow, copper mid-depth, iron deeper and rarer:
 * roughly the order they turn up in sedimentary and igneous rock. */
static const ore_rule ORES[] = {
    {14, 9, 8, 110, B_COAL_ORE},
    {7, 7, 16, 72, B_COPPER_ORE},
    {6, 6, 4, 54, B_IRON_ORE},
};

static uint32_t lcg(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

/* Veins are short random walks from hashed starts, replacing rock only and
 * clipped to the column (like trees), so no neighbour data is needed. */
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
                    if (*p == B_ROCK) *p = r->block;
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

/* ------------------------------------------------------------ column */

static void build(uint32_t seed, column *c, int version)
{
    int bx = c->cx * CHUNK_W, bz = c->cz * CHUNK_W;
    int hm[CHUNK_W + 2][CHUNK_W + 2] = {0};
    for (int z = 0; z < CHUNK_W + 2; z++)
        for (int x = 0; x < CHUNK_W + 2; x++)
            hm[z][x] = (version == 1 ? height_v1 : worldgen_height)(seed, bx + x - 1, bz + z - 1);

    memset(c->blocks, B_AIR, COL_VOL);

    for (int z = 0; z < CHUNK_W; z++)
        for (int x = 0; x < CHUNK_W; x++) {
            int h = hm[z + 1][x + 1];
            int slope = clampi(abs(hm[z + 1][x] - hm[z + 1][x + 2]) + abs(hm[z][x + 1] - hm[z + 2][x + 1]), 0, 64);
            uint8_t top = 0, filler = 0;
            const uint8_t rock = version > 1 ? B_ROCK : B_STONE;
            if (h <= SEA_LEVEL - 5) {
                top = B_GRAVEL;
                filler = B_GRAVEL;
            } else if (h <= SEA_LEVEL + 1) {
                top = B_SAND;
                filler = B_SAND;
            } else if (slope > 5) {
                top = rock;
                filler = rock;
            } else if (version > 1 && h > 100) {
                top = B_SNOW; /* bare rock under the snow */
                filler = rock;
            } else if (h > 92) {
                top = B_SNOW;
                filler = B_DIRT;
            } else {
                top = B_GRASS;
                filler = B_DIRT;
            }

            uint8_t *col = &c->blocks[col_index(x, 0, z)];
            const int stride = COL_AREA;
            col[0] = B_BEDROCK;
            for (int y = 1; y < h; y++) {
                uint8_t id = y < h - 4 ? rock : (y == h - 1 ? top : filler);
                col[y * stride] = id;
            }
            for (int y = h; y < SEA_LEVEL; y++) col[y * stride] = B_WATER;
        }

    if (version > 1) {
        place_ores(seed, c);
        carve_caves(seed, c, (const int (*)[CHUNK_W + 2]) hm);
    }

    /* Trees only where the whole canopy fits in this column: avoids needing
     * neighbour data on worker threads, at the cost of no trees on the
     * outermost 2 blocks of each column. A cave mouth has taken the grass,
     * so no tree stands over one. */
    for (int z = 2; z < CHUNK_W - 2; z++)
        for (int x = 2; x < CHUNK_W - 2; x++) {
            uint32_t h = hash2i(seed ^ 0xA5A5A5A5U, bx + x, bz + z);
            if (h % 1000 >= 12) continue;
            int y = hm[z + 1][x + 1];
            if (y <= SEA_LEVEL + 1 || y > 88) continue;
            if (c->blocks[col_index(x, y - 1, z)] != B_GRASS) continue;
            /* Over a cave the soil, not the ground, would carry the tree, and
             * its canopy would drop at the first dig nearby. */
            int hollow = 0;
            for (int ly = 1; ly < y - 1 && !hollow; ly++) hollow = c->blocks[col_index(x, ly, z)] == B_AIR;
            if (hollow) continue;
            c->blocks[col_index(x, y - 1, z)] = B_DIRT;
            grow_tree(c, h >> 8, x, y, z);
        }
}

void worldgen_column(uint32_t seed, column *c) { build(seed, c, WORLDGEN_VERSION); }

void worldgen_column_v1(uint32_t seed, column *c) { build(seed, c, 1); }
