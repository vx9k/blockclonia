/* Heightmap terrain with beaches, snowy peaks and trees. Deliberately 2D
 * noise only: 3D noise costs ~100x more per column and low-end CPUs would
 * stall streaming. */
#include "world.h"
#include "noise.h"
#include "mathlib.h"

#include <stdlib.h>
#include <string.h>

int worldgen_height(uint32_t seed, int x, int z)
{
    float fx = (float)x, fz = (float)z;
    float continent = fbm2(seed, fx / 700.0f, fz / 700.0f, 3);
    float hills = fbm2(seed + 17, fx / 110.0f, fz / 110.0f, 4);
    float ridge = 1.0f - fabsf(noise2(seed + 31, fx / 260.0f, fz / 260.0f));
    float mountain = fmaxf(0.0f, continent + 0.15f) * ridge * ridge * ridge;
    float h = 54.0f + continent * 16.0f + hills * 9.0f + mountain * 60.0f;
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

void worldgen_column(uint32_t seed, column *c)
{
    int bx = c->cx * CHUNK_W, bz = c->cz * CHUNK_W;
    int hm[CHUNK_W + 2][CHUNK_W + 2];
    for (int z = 0; z < CHUNK_W + 2; z++)
        for (int x = 0; x < CHUNK_W + 2; x++)
            hm[z][x] = worldgen_height(seed, bx + x - 1, bz + z - 1);

    memset(c->blocks, B_AIR, COL_VOL);

    for (int z = 0; z < CHUNK_W; z++)
        for (int x = 0; x < CHUNK_W; x++) {
            int h = hm[z + 1][x + 1];
            int slope = clampi(abs(hm[z + 1][x] - hm[z + 1][x + 2]) + abs(hm[z][x + 1] - hm[z + 2][x + 1]), 0, 64);
            uint8_t top, filler;
            if (h <= SEA_LEVEL - 5) { top = B_GRAVEL; filler = B_GRAVEL; }
            else if (h <= SEA_LEVEL + 1) { top = B_SAND; filler = B_SAND; }
            else if (slope > 5) { top = B_STONE; filler = B_STONE; }
            else if (h > 92) { top = B_SNOW; filler = B_DIRT; }
            else { top = B_GRASS; filler = B_DIRT; }

            uint8_t *col = &c->blocks[col_index(x, 0, z)];
            const int stride = COL_AREA;
            col[0] = B_BEDROCK;
            for (int y = 1; y < h; y++) {
                uint8_t id = y < h - 4 ? B_STONE : (y == h - 1 ? top : filler);
                col[y * stride] = id;
            }
            for (int y = h; y < SEA_LEVEL; y++) col[y * stride] = B_WATER;
        }

    /* Trees only where the whole canopy fits in this column: avoids needing
     * neighbour data on worker threads, at the cost of no trees on the
     * outermost 2 blocks of each column. */
    for (int z = 2; z < CHUNK_W - 2; z++)
        for (int x = 2; x < CHUNK_W - 2; x++) {
            uint32_t h = hash2i(seed ^ 0xA5A5A5A5U, bx + x, bz + z);
            if (h % 1000 >= 12) continue;
            int y = hm[z + 1][x + 1];
            if (y <= SEA_LEVEL + 1 || y > 88) continue;
            if (c->blocks[col_index(x, y - 1, z)] != B_GRASS) continue;
            c->blocks[col_index(x, y - 1, z)] = B_DIRT;
            grow_tree(c, h >> 8, x, y, z);
        }
}
