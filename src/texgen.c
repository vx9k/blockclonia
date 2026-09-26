#include "texgen.h"
#include "block.h"
#include "noise.h"

#include <string.h>

typedef struct { uint8_t r, g, b, a; } rgba;

static uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

static rgba shade(int r, int g, int b, int a, int n)
{
    return (rgba){clamp8(r + n), clamp8(g + n), clamp8(b + n), clamp8(a)};
}

/* Per-pixel noise in [-amp, amp], deterministic per layer. */
static int px_noise(int layer, int x, int y, int amp)
{
    return (int)(hash2i(0xC0FFEEu + (uint32_t)layer * 7919u, x, y) % (uint32_t)(2 * amp + 1)) - amp;
}

static rgba texel(int layer, int x, int y)
{
    int n = px_noise(layer, x, y, 12);
    switch (layer) {
    case T_STONE: return shade(125, 125, 125, 255, n);
    case T_BEDROCK: return shade(60, 60, 60, 255, px_noise(layer, x, y, 30));
    case T_DIRT: return shade(134, 96, 67, 255, n);
    case T_GRASS_TOP: return shade(95, 159, 53, 255, n);
    case T_GRASS_SIDE: {
        int edge = 3 + (int)(hash2i(99, x, 0) % 2u);
        return y < edge ? shade(95, 159, 53, 255, n) : shade(134, 96, 67, 255, n);
    }
    case T_SAND: return shade(219, 207, 163, 255, n / 2);
    case T_GRAVEL: {
        int p = px_noise(layer, x / 2, y / 2, 40);
        return shade(136, 126, 126, 255, p);
    }
    case T_LOG_SIDE: return shade(102, 81, 49, 255, (x % 4 == 0 ? -18 : 0) + n / 2);
    case T_LOG_TOP: {
        int dx = x * 2 - 15, dy = y * 2 - 15;
        int d2 = dx * dx + dy * dy;
        if (x == 0 || y == 0 || x == 15 || y == 15) return shade(102, 81, 49, 255, n / 2);
        int ring = (d2 / 40) % 2 ? -14 : 0;
        return shade(176, 144, 90, 255, ring + n / 3);
    }
    case T_LEAVES: return shade(58, 110, 38, 255, px_noise(layer, x, y, 22));
    case T_PLANKS: {
        int seam = (y % 4 == 3) ? -30 : 0;
        int joint = ((x + (y / 4) * 5) % 16 == 0) ? -20 : 0;
        return shade(162, 130, 78, 255, seam + joint + n / 2);
    }
    case T_GLASS: {
        int border = x == 0 || y == 0 || x == 15 || y == 15;
        int glint = (x == y + 3 || x == y + 4) && x > 4 && x < 12;
        if (border) return (rgba){220, 235, 240, 230};
        if (glint) return (rgba){240, 250, 255, 140};
        return (rgba){200, 225, 235, 45};
    }
    case T_BRICK: {
        int row = y / 4, mortar_y = y % 4 == 3;
        int mortar_x = ((x + (row % 2) * 4) % 8) == 7;
        if (mortar_y || mortar_x) return shade(175, 170, 160, 255, n / 3);
        return shade(150, 70, 55, 255, n);
    }
    case T_SNOW: return shade(240, 245, 250, 255, n / 3);
    case T_ICE: return shade(160, 190, 250, 190, n / 2);
    case T_WATER: return shade(40, 90, 200, 170, n / 2);
    default: return (rgba){255, 0, 255, 255};
    }
}

uint32_t texgen_offset(int layer, int mip)
{
    uint32_t off = 0;
    for (int m = 0; m < mip; m++) {
        uint32_t s = TEX_SIZE >> m;
        off += s * s * 4u * T_COUNT;
    }
    uint32_t s = TEX_SIZE >> mip;
    return off + s * s * 4u * (uint32_t)layer;
}

uint32_t texgen_total_bytes(void) { return texgen_offset(0, TEX_MIPS); }

void texgen_build(uint8_t *out)
{
    for (int l = 0; l < T_COUNT; l++) {
        uint8_t *dst = out + texgen_offset(l, 0);
        for (int y = 0; y < TEX_SIZE; y++)
            for (int x = 0; x < TEX_SIZE; x++) {
                rgba c = texel(l, x, y);
                uint8_t *p = dst + (y * TEX_SIZE + x) * 4;
                p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = c.a;
            }
        /* Box-filtered mips keep distant terrain from shimmering. */
        for (int m = 1; m < TEX_MIPS; m++) {
            const uint8_t *src = out + texgen_offset(l, m - 1);
            uint8_t *d = out + texgen_offset(l, m);
            int s = TEX_SIZE >> m, ps = s * 2;
            for (int y = 0; y < s; y++)
                for (int x = 0; x < s; x++)
                    for (int c = 0; c < 4; c++) {
                        int sum = src[((2 * y) * ps + 2 * x) * 4 + c] + src[((2 * y) * ps + 2 * x + 1) * 4 + c] +
                                  src[((2 * y + 1) * ps + 2 * x) * 4 + c] + src[((2 * y + 1) * ps + 2 * x + 1) * 4 + c];
                        d[(y * s + x) * 4 + c] = (uint8_t)((sum + 2) / 4);
                    }
        }
    }
}
