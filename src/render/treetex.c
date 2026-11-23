#include "treetex.h"
#include "block.h"
#include "noise.h"

#include <math.h>
#include <stdlib.h>

typedef struct {
    int r, g, b;
} col;

static uint8_t c8(int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

static int wrap(int v, int p)
{
    v %= p;
    return v < 0 ? v + p : v;
}

/* [0, 1), from a hash: per texel, per cell, per anything. */
static float hashf(uint32_t seed, int x, int y) { return (float)(hash2i(seed, x, y) & 0xFFFFu) / 65536.0f; }

static col add(col c, int n) { return (col){c.r + n, c.g + n, c.b + n}; }

static col mix(col a, col b, float t)
{
    return (col){a.r + (int)lroundf((float)(b.r - a.r) * t), a.g + (int)lroundf((float)(b.g - a.g) * t),
                 a.b + (int)lroundf((float)(b.b - a.b) * t)};
}

static float fract(float v) { return v - floorf(v); }

/* Smooth value noise in [0, 1] on a lattice of cw x ch texels that wraps
 * every px x py texels (whole cells), so the field tiles. */
static float vnoise(uint32_t seed, int x, int y, int cw, int ch, int px, int py)
{
    int nx = px / cw, ny = py / ch;
    x = wrap(x, px);
    y = wrap(y, py);
    /* Texel centres: the nodes sit on cell corners. */
    float fx = ((float)x + 0.5f) / (float)cw, fy = ((float)y + 0.5f) / (float)ch;
    int i = (int)floorf(fx), j = (int)floorf(fy);
    float u = fx - (float)i, v = fy - (float)j;
    u = u * u * (3.0f - 2.0f * u);
    v = v * v * (3.0f - 2.0f * v);
    float a = hashf(seed, wrap(i, nx), wrap(j, ny)), b = hashf(seed, wrap(i + 1, nx), wrap(j, ny));
    float c = hashf(seed, wrap(i, nx), wrap(j + 1, ny)), d = hashf(seed, wrap(i + 1, nx), wrap(j + 1, ny));
    float top = a + (b - a) * u, bot = c + (d - c) * u;
    return top + (bot - top) * v;
}

/* Cellular noise that tiles like vnoise: one point per cw x ch cell, and
 * the distances (in cells) to the nearest and second nearest, with the
 * nearest cell's id. Plates of bark are the cells, their cracks where the
 * two distances meet. */
typedef struct {
    float f1, f2;
    int id;
} cells;

static cells worley(uint32_t seed, int x, int y, int cw, int ch, int px, int py)
{
    int nx = px / cw, ny = py / ch;
    x = wrap(x, px);
    y = wrap(y, py);
    float tx = ((float)x + 0.5f) / (float)cw, ty = ((float)y + 0.5f) / (float)ch;
    int ci = x / cw, cj = y / ch;
    cells r = {1e9f, 1e9f, 0};
    for (int dj = -1; dj <= 1; dj++)
        for (int di = -1; di <= 1; di++) {
            int i = ci + di, j = cj + dj, wi = wrap(i, nx), wj = wrap(j, ny);
            uint32_t h = hash2i(seed, wi, wj);
            float fx = (float)i + 0.15f + 0.7f * (float)(h & 255u) / 255.0f;
            float fy = (float)j + 0.15f + 0.7f * (float)((h >> 8) & 255u) / 255.0f;
            float d = sqrtf((fx - tx) * (fx - tx) + (fy - ty) * (fy - ty));
            if (d < r.f1) {
                r.f2 = r.f1;
                r.f1 = d;
                r.id = wj * nx + wi;
            } else if (d < r.f2) {
                r.f2 = d;
            }
        }
    return r;
}

/* A ridge line that wanders across x as it runs up the strip: the shift
 * at row y, -amp..amp texels, the same all the way across. */
static float wander(uint32_t seed, int y, int cell, int period, float amp)
{
    return (vnoise(seed, 0, y, 16, cell, 16, period) - 0.5f) * 2.0f * amp;
}

/* Oak: grey-brown, deep regular furrows between long ridges broken into
 * blocks by cross-cracks, and a green dusting of algae on the damp side. */
static col oak(int x, int y, int p)
{
    float u = (float)x + wander(0x0A1u, y, 8, p, 1.6f);
    int band = wrap((int)floorf(u / 4.0f), 4);
    u += wander(0x0A2u + (uint32_t)band, y, 4, p, 0.7f);
    float f = fract(u / 4.0f);
    col c = {104, 92, 76};
    if (f < 0.3f) c = f < 0.15f ? (col){50, 43, 37} : (col){70, 60, 50};
    else if (f > 0.5f && f < 0.8f) c = add(c, 14);
    if (f >= 0.3f && hashf(0x0A3u, band, wrap(y, p)) < 0.1f) c = add(c, -26);
    float g = vnoise(0x0A4u, x, y, 8, 8, 16, p);
    if (g > 0.6f) c = mix(c, (col){92, 112, 66}, fminf(1.0f, (g - 0.6f) * 2.5f));
    return c;
}

/* Birch: chalk white with grey smudges, black lenticels in short
 * horizontal dashes, a peeling tan edge after some, and now and then a
 * dark chevron under an old branch. */
static col birch(int x, int y, int p)
{
    y = wrap(y, p);
    col c = {222, 218, 208};
    if (vnoise(0xB1u, x, y, 8, 8, 16, p) > 0.62f) c = add(c, -20);
    if (y % 2 == 0 && hashf(0xB2u, 0, y) < 0.45f) {
        int cell = x / 8, start = (int)(hash2i(0xB3u, cell, y) % 4u), len = 3 + (int)(hash2i(0xB4u, cell, y) % 3u);
        int lx = x - cell * 8;
        if (lx >= start && lx < start + len) return (col){38, 36, 34};
        if (lx == start + len && hash2i(0xB5u, cell, y) % 3u == 0) return (col){206, 178, 146};
    }
    /* One chevron per 16 x 32 patch at most. */
    int patch = y / 32 * 32;
    if (hashf(0xB6u, 0, patch) < 0.5f) {
        int cx = 3 + (int)(hash2i(0xB7u, 0, patch) % 10u), cy = patch + 4 + (int)(hash2i(0xB8u, 0, patch) % 20u);
        int dy = y - cy, dx = abs(x - cx);
        if (dy >= 0 && dy < 3 && dx >= dy && dx <= dy + 1) return (col){30, 28, 28};
    }
    return c;
}

/* Spruce: thin, rounded grey-brown scales in staggered rows, each with a
 * dark lower lip, some redder. */
static col spruce(int x, int y, int p)
{
    y = wrap(y, p);
    int row = y / 4, lx = wrap(x + (row & 1) * 2, 16), cell = lx / 4, ly = y % 4, cx = lx % 4;
    col c = {112, 90, 74};
    int tone = (int)(hash2i(0x5B1u, cell, row) % 29u) - 14;
    c = add(c, tone);
    if (hash2i(0x5B2u, cell, row) % 4u == 0) c.r += 12;
    if (ly == 3 || (ly == 2 && (cx == 0 || cx == 3))) return (col){62, 50, 42};
    if (ly == 0) c = add(c, 10);
    return c;
}

/* Scots pine: the copper upper trunk, flaking orange plates between grey
 * furrows, paler where a flake has just come away. */
static col pine(int x, int y, int p)
{
    cells w = worley(0x91u, x, y, 4, 8, 16, p);
    if (w.f2 - w.f1 < 0.18f) return (col){84, 62, 50};
    float t = hashf(0x92u, w.id, 0);
    col c = mix((col){166, 92, 56}, (col){200, 126, 76}, t);
    if (hashf(0x93u, wrap(x, 16), wrap(y, p)) < 0.12f) c = (col){214, 152, 100};
    return c;
}

/* Thorn tree: dark, deeply fissured, rusty in the fissures, with pale
 * paired thorns. */
static col thorn(int x, int y, int p)
{
    float u = (float)x + wander(0x7A1u, y, 8, p, 1.4f);
    float f = fract(u * 3.0f / 16.0f);
    if (f < 0.22f) return f > 0.08f && f < 0.15f ? (col){110, 62, 46} : (col){50, 42, 38};
    col c = {88, 78, 70};
    if (hashf(0x7A2u, wrap(x, 16), wrap(y, p)) < 0.012f) return (col){216, 206, 184};
    if (hashf(0x7A2u, wrap(x, 16), wrap(y - 1, p)) < 0.012f) return (col){48, 42, 38};
    return add(c, (int)(vnoise(0x7A3u, x, y, 4, 4, 16, p) * 16.0f) - 8);
}

/* Willow: thick corky ridges crossing in long diamonds. */
static col willow(int x, int y, int p)
{
    float a = fract(((float)x + (float)y * 0.5f + wander(0x3A1u, y, 8, p, 0.8f)) / 4.0f);
    float b = fract(((float)x - (float)y * 0.5f + wander(0x3A2u, y, 8, p, 0.8f)) / 4.0f);
    float m = fminf(a, b);
    if (m < 0.18f) return (col){66, 58, 50};
    col c = {116, 104, 90};
    if (m > 0.4f) c = add(c, 10);
    return add(c, (int)(vnoise(0x3A3u, x, y, 4, 4, 16, p) * 12.0f) - 6);
}

/* Maple: grey bark crackled into small plates, like old china glaze. */
static col maple(int x, int y, int p)
{
    cells w = worley(0x6A1u, x, y, 4, 4, 16, p);
    if (w.f2 - w.f1 < 0.14f) return (col){78, 72, 68};
    col c = add((col){134, 128, 120}, (int)(hashf(0x6A2u, w.id, 0) * 24.0f) - 12);
    if (w.f2 - w.f1 < 0.3f) c = add(c, 8);
    return c;
}

/* Cherry: glossy red-brown, banded with rows of pale lenticel dashes and
 * the faint horizontal peel lines between them. */
static col cherry(int x, int y, int p)
{
    y = wrap(y, p);
    col c = add((col){124, 60, 52}, (int)(vnoise(0xC1u, x, y, 16, 2, 16, p) * 14.0f) - 4);
    int band = y / 4, ly = y % 4;
    if (ly == 1 && hashf(0xC2u, 0, band) < 0.8f) {
        int seg = wrap(x + (int)(hash2i(0xC3u, 0, band) % 3u), 16) / 3;
        if (hash2i(0xC4u, seg, band) % 4u != 0 && wrap(x + (int)(hash2i(0xC3u, 0, band) % 3u), 16) % 3 != 2)
            return (col){198, 152, 120};
    }
    if (ly == 0) c = add(c, -12);
    if (ly == 3 && hashf(0xC5u, x / 4, band) < 0.3f) c = (col){150, 82, 68};
    return c;
}

/* Redwood: cinnamon, soft and stringy, in long fibres with deep wavy
 * grooves. */
static col redwood(int x, int y, int p)
{
    float u = (float)x + wander(0x8A1u, y, 16, p, 2.0f);
    float f = fract(u * 3.0f / 16.0f);
    if (f < 0.16f) return (col){86, 40, 28};
    int strand = wrap((int)floorf(u), 16);
    col c = add((col){158, 82, 54}, (int)(vnoise(0x8A2u + (uint32_t)strand, 0, y, 16, 8, 16, p) * 36.0f) - 18);
    if (hashf(0x8A3u, strand, wrap(y / 6, p / 6 > 0 ? p / 6 : 1)) < 0.08f) c = (col){192, 112, 78};
    return c;
}

/* Palm: rings where old fronds fell, each a dark line under a raised pale
 * band, and the diamond scars of the leaf bases between. */
static col palm(int x, int y, int p)
{
    y = wrap(y, p);
    int band = y / 4, ly = y % 4;
    if (ly == 0) return (col){92, 78, 64};
    int o = (int)(hash2i(0x9A1u, 0, band) % 8u);
    int dx = abs(wrap(x - o, 8) - 4), dy = abs(ly - 2);
    if (dx + dy <= 2) return add((col){112, 94, 74}, hashf(0x9A2u, wrap(x, 16), y) < 0.3f ? -14 : 0);
    col c = ly == 1 ? (col){176, 156, 124} : (col){146, 126, 100};
    return add(c, (int)(hashf(0x9A3u, wrap(x, 16), y) * 10.0f) - 5);
}

/* Baobab: smooth grey-pink skin, wrinkled in soft wavy rings, mottled
 * paler. */
static col baobab(int x, int y, int p)
{
    float w = (float)y + (vnoise(0xBA1u, x, y, 8, 16, 16, p) - 0.5f) * 3.0f;
    float f = fract(w * 3.0f / 16.0f);
    col c = {168, 148, 138};
    if (f < 0.14f) c = (col){130, 112, 106};
    else if (f < 0.24f) c = add(c, 8);
    float m = vnoise(0xBA2u, x, y, 8, 8, 16, p);
    if (m > 0.6f) c = mix(c, (col){190, 170, 160}, (m - 0.6f) * 2.0f);
    return c;
}

/* Umbrella pine: big flat plates of orange-brown and grey between deep
 * dark fissures. */
static col umbrella(int x, int y, int p)
{
    cells w = worley(0xDA1u, x, y, 8, 8, 16, p);
    if (w.f2 - w.f1 < 0.12f) return (col){60, 46, 38};
    col c = mix((col){160, 96, 62}, (col){126, 110, 98}, hashf(0xDA2u, w.id, 0));
    if (hashf(0xDA3u, wrap(x, 16), wrap(y, p)) < 0.1f) c = add(c, 18);
    return c;
}

/* Mountain pine: dark grey, almost black, in small scaly plates, a few
 * rusty where one has flaked. */
static col mountain_pine(int x, int y, int p)
{
    cells w = worley(0xEA1u, x, y, 4, 4, 16, p);
    if (w.f2 - w.f1 < 0.16f) return (col){40, 36, 34};
    float t = hashf(0xEA2u, w.id, 0);
    return t < 0.12f ? (col){112, 76, 60} : add((col){80, 72, 68}, (int)(t * 20.0f) - 10);
}

static col bark(int kind, int x, int y, int p)
{
    switch (kind) {
    case TREE_OAK: return oak(x, y, p);
    case TREE_BIRCH: return birch(x, y, p);
    case TREE_SPRUCE: return spruce(x, y, p);
    case TREE_PINE: return pine(x, y, p);
    case TREE_THORN: return thorn(x, y, p);
    case TREE_WILLOW: return willow(x, y, p);
    case TREE_MAPLE: return maple(x, y, p);
    case TREE_CHERRY: return cherry(x, y, p);
    case TREE_REDWOOD: return redwood(x, y, p);
    case TREE_PALM: return palm(x, y, p);
    case TREE_BAOBAB: return baobab(x, y, p);
    case TREE_UMBRELLA: return umbrella(x, y, p);
    default: return mountain_pine(x, y, p);
    }
}

/* A little grain on everything, as the rest of the world has. */
static col grain(col c, uint32_t seed, int x, int y, int amp)
{
    return add(c, (int)(hash2i(seed, x, y) % (uint32_t)(2 * amp + 1)) - amp);
}

void treetex_bark(int kind, int x, int y, int period, uint8_t out[4])
{
    col c = grain(bark(kind, x, y, period), 0x7EEu + (uint32_t)kind, wrap(x, 16), wrap(y, period), 4);
    out[0] = c8(c.r);
    out[1] = c8(c.g);
    out[2] = c8(c.b);
    out[3] = 255;
}

/* Moss over the bark in soft patches, brighter tufts on top. */
static col mossy(int kind, int x, int y)
{
    col c = bark(kind, x, y, 16);
    float m = vnoise(0x705u, x, y, 4, 4, 16, 16) * 0.75f + vnoise(0x706u, x, y, 8, 8, 16, 16) * 0.25f;
    if (m < 0.42f) return c;
    col moss = grain((col){74, 104, 44}, 0x707u, x, y, 10);
    if (hashf(0x708u, x, y) < 0.15f) moss = (col){112, 142, 62};
    return mix(c, moss, fminf(1.0f, (m - 0.42f) * 6.0f));
}

typedef struct {
    col heart, rim;
    float rings; /* texels between growth rings; 0: none (palm, baobab) */
} end_look;

static const end_look END[TREE_KINDS] = {
    [TREE_OAK] = {{176, 140, 88}, {96, 84, 68}, 1.6f},
    [TREE_BIRCH] = {{226, 206, 160}, {214, 210, 200}, 1.4f},
    [TREE_SPRUCE] = {{214, 190, 140}, {104, 84, 70}, 1.5f},
    [TREE_PINE] = {{204, 152, 98}, {150, 86, 54}, 1.7f},
    [TREE_THORN] = {{150, 96, 62}, {78, 68, 62}, 1.2f},
    [TREE_WILLOW] = {{212, 188, 150}, {104, 94, 82}, 2.0f},
    [TREE_MAPLE] = {{222, 198, 154}, {128, 122, 114}, 1.5f},
    [TREE_CHERRY] = {{178, 104, 72}, {118, 56, 48}, 1.5f},
    [TREE_REDWOOD] = {{162, 72, 50}, {150, 76, 50}, 2.2f},
    [TREE_PALM] = {{184, 154, 114}, {140, 120, 96}, 0.0f},
    [TREE_BAOBAB] = {{204, 188, 154}, {160, 140, 130}, 0.0f},
    [TREE_UMBRELLA] = {{198, 148, 100}, {140, 92, 62}, 1.6f},
    [TREE_MOUNTAIN_PINE] = {{188, 142, 98}, {74, 66, 62}, 1.0f},
};

/* End grain: the bark round the edge, a paler band of sapwood inside it,
 * and growth rings round a dark pith, wobbling and now and then split by
 * a radial check. Palms have no rings, only fibre ends; a baobab's soft
 * pulp hardly any. */
static col end_grain(int kind, int x, int y)
{
    const end_look *e = &END[kind];
    if (x == 0 || y == 0 || x == 15 || y == 15) return grain(e->rim, 0xE0u, x, y, 8);
    float dx = (float)x - 7.5f, dy = (float)y - 7.5f, r = sqrtf(dx * dx + dy * dy);
    float ang = atan2f(dy, dx);
    col c = r > 6.0f ? add(e->heart, 18) : e->heart;
    if (e->rings <= 0.0f) {
        int dots = kind == TREE_PALM ? 26 : 10;
        return hashf(0xE1u + (uint32_t)kind, x, y) * 100.0f < (float)dots ? add(c, -30) : grain(c, 0xE2u, x, y, 6);
    }
    float rr = r + 0.45f * (hashf(0xE3u + (uint32_t)kind, (int)floorf(ang * 2.0f + 8.0f), 0) - 0.5f);
    if (r < 1.0f) return add(c, -46);
    if (fmodf(rr, e->rings) < 0.7f) c = add(c, -18);
    float check = -1.2f + 2.4f * hashf(0xE4u + (uint32_t)kind, 0, 0);
    if (r > 2.0f && fabsf(ang - check) < 0.09f) c = add(c, -40);
    return grain(c, 0xE5u, x, y, 4);
}

typedef struct {
    col c;
    int holes, grain; /* percent see-through, per-texel noise */
} leaf_look;

static const leaf_look LEAF[TREE_KINDS] = {
    [TREE_OAK] = {{58, 110, 38}, 26, 22},          [TREE_BIRCH] = {{108, 158, 58}, 32, 22},
    [TREE_SPRUCE] = {{36, 70, 48}, 18, 14},        [TREE_PINE] = {{52, 92, 50}, 24, 16},
    [TREE_THORN] = {{104, 122, 56}, 38, 18},       [TREE_WILLOW] = {{122, 162, 72}, 34, 18},
    [TREE_MAPLE] = {{192, 70, 34}, 28, 26},        [TREE_CHERRY] = {{236, 168, 190}, 30, 14},
    [TREE_REDWOOD] = {{40, 78, 56}, 20, 14},       [TREE_PALM] = {{88, 140, 58}, 34, 16},
    [TREE_BAOBAB] = {{112, 150, 62}, 40, 20},      [TREE_UMBRELLA] = {{46, 88, 52}, 16, 14},
    [TREE_MOUNTAIN_PINE] = {{40, 72, 50}, 20, 14},
};

/* Where a kind's leaves would let the sky through, before the cap below:
 * clumps of two for broad leaves, thin vertical slits between a willow's
 * hanging leaves, the gaps between a palm frond's leaflets. */
static int leaf_open(int kind, int x, int y)
{
    uint32_t s = 0x1EAFu + (uint32_t)kind * 977u;
    int holes = LEAF[kind].holes;
    switch (kind) {
    case TREE_WILLOW: return (int)(hash2i(s, x, y / 3) % 100u) < holes;
    case TREE_PALM: return fract((float)(x + y) / 3.0f) < 0.34f && (int)(hash2i(s, x, y) % 100u) < holes * 2;
    default: return (int)((hash2i(s, x / 2, y) + hash2i(s, x, y / 2) % 7u) % 100u) < holes;
    }
}

/* Leaves cut out against the sky. No 4x4 block has more than 6 gaps in
 * 16, so from the third mip on (the box filter averages a block into a
 * texel) a distant canopy closes up. A gap keeps a darker leaf colour for
 * what draws the texture without cutting it out (a falling block, an
 * icon). */
static void leaves(int kind, int x, int y, uint8_t out[4])
{
    const leaf_look *l = &LEAF[kind];
    col c = grain(l->c, 0xCAFEu + (uint32_t)kind, x, y, l->grain);
    uint32_t h = hash2i(0x1EA5u + (uint32_t)kind, x, y);
    switch (kind) {
    case TREE_MAPLE:
        /* Autumn: red, with orange and a few yellow leaves. */
        if (h % 5u == 0) c = (col){222, 124, 40};
        else if (h % 17u == 0) c = (col){230, 186, 56};
        break;
    case TREE_CHERRY:
        /* Blossom in clusters, white at their hearts, a few green leaves. */
        if (vnoise(0xC7u, x, y, 4, 4, 16, 16) < 0.28f) c = grain((col){96, 140, 64}, 0xC8u, x, y, 12);
        else if (h % 7u == 0) c = (col){250, 232, 238};
        break;
    case TREE_SPRUCE:
    case TREE_REDWOOD:
    case TREE_MOUNTAIN_PINE:
    case TREE_PINE:
    case TREE_UMBRELLA:
        /* Needles: short dark diagonal strokes. */
        if ((x + y * 2) % 5 == 0) c = add(c, -18);
        break;
    case TREE_PALM:
        /* The leaflets' paler midribs. */
        if ((x + y) % 3 == 1) c = add(c, 16);
        break;
    default: break;
    }
    int gap = leaf_open(kind, x, y);
    if (gap) {
        int before = 0;
        for (int i = 0; i < (y & 3) * 4 + (x & 3); i++) before += leaf_open(kind, (x & ~3) + (i & 3), (y & ~3) + i / 4);
        gap = before < 6;
    }
    out[0] = c8(gap ? c.r * 3 / 5 : c.r);
    out[1] = c8(gap ? c.g * 3 / 5 : c.g);
    out[2] = c8(gap ? c.b * 3 / 5 : c.b);
    out[3] = gap ? 0 : 255;
}

int treetex_texel(int layer, int x, int y, uint8_t out[4])
{
    if (layer < T_TREE_FIRST || layer > T_TREE_LAST) return 0;
    if (layer >= T_TREE_LEAVES_FIRST) {
        leaves(layer - T_TREE_LEAVES_FIRST, x, y, out);
        return 1;
    }
    if (layer < T_SIDEWAYS_FIRST) {
        /* Window w of the strip: its rows w * 16 .. w * 16 + 15. */
        int k = (layer - T_BARK_FIRST) / TREE_BARK_WINDOWS, w = (layer - T_BARK_FIRST) % TREE_BARK_WINDOWS;
        treetex_bark(k, x, y + w * 16, 16 * TREE_BARK_WINDOWS, out);
        return 1;
    }
    if (layer < T_MOSSY_FIRST) {
        /* On its side the grain runs along u: the strip turned a quarter. */
        treetex_bark(layer - T_SIDEWAYS_FIRST, y, x, 16, out);
        return 1;
    }
    col c = layer < T_END_FIRST ? mossy(layer - T_MOSSY_FIRST, x, y) : end_grain(layer - T_END_FIRST, x, y);
    out[0] = c8(c.r);
    out[1] = c8(c.g);
    out[2] = c8(c.b);
    out[3] = 255;
    return 1;
}
