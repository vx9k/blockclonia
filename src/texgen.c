#include "texgen.h"
#include "block.h"
#include "item.h"
#include "mobskin.h"
#include "noise.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>

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

/* ---------------------------------------------------------- item art */

/* 16x16 sprites, '.' transparent; other characters index the sprite's
 * palette. Rows top to bottom. */
typedef struct {
    char ch;
    rgba c;
} pal_entry;

typedef struct {
    int layer;
    const char *rows[16];
    pal_entry pal[6];
} sprite;

static const sprite SPRITES[] = {
    {T_ITEM_TORCH,
     {"................", "..........ff....", ".........fyyf...", "........fyWyf...", "........fyyf....",
      ".......#hhf.....", "......#hh#......", ".....#w#........", "....#w#.........", "...#w#..........",
      "..#w#...........", ".#w#............", "#w#.............", "##..............", "................",
      "................"},
     {{'#', {84, 58, 30, 255}},
      {'w', {150, 110, 62, 255}},
      {'h', {40, 30, 24, 255}},
      {'f', {225, 90, 30, 255}},
      {'y', {250, 185, 60, 255}},
      {'W', {255, 245, 190, 255}}}},
};

static int sprite_texel(int layer, int x, int y, rgba *out)
{
    for (int i = 0; i < (int)(sizeof SPRITES / sizeof SPRITES[0]); i++) {
        const sprite *s = &SPRITES[i];
        if (s->layer != layer) continue;
        char ch = s->rows[y][x];
        *out = (rgba){0, 0, 0, 0};
        for (int k = 0; k < 6; k++)
            if (s->pal[k].ch == ch && ch) {
                int n = px_noise(layer, x, y, 6);
                rgba c = s->pal[k].c;
                *out = shade(c.r, c.g, c.b, c.a, n);
            }
        return 1;
    }
    return 0;
}

/* A crack pattern: random walks from the centre, more and longer at each
 * stage. Only the crack texels are opaque. */
static rgba crack(int stage, int x, int y)
{
    static uint8_t mask[4][16][16];
    static int built;
    if (!built) {
        for (int s = 0; s < 4; s++) {
            int walks = 2 + s * 2, len = 4 + s * 3;
            for (int w = 0; w < walks; w++) {
                int px = 8, py = 8;
                uint32_t h = hash2i(0xC7AC0u, w, 77);
                for (int k = 0; k < len; k++) {
                    mask[s][py & 15][px & 15] = 1;
                    h = h * 1664525u + 1013904223u;
                    int d = (int)(h >> 29); /* 0..7 */
                    px += (d & 1) ? 1 : -1;
                    if (d & 2) py += (d & 4) ? 1 : -1;
                    else px += (w & 1) ? 1 : -1;
                }
            }
        }
        /* Each stage also keeps the cracks of the stages before it. */
        for (int s = 1; s < 4; s++)
            for (int yy = 0; yy < 16; yy++)
                for (int xx = 0; xx < 16; xx++) mask[s][yy][xx] |= mask[s - 1][yy][xx];
        built = 1;
    }
    return mask[stage][y][x] ? (rgba){20, 18, 16, 210} : (rgba){0, 0, 0, 0};
}

/* Flames rising from the bottom: a noisy height per column that flickers
 * between frames, yellow at the core fading to red and then transparent. */
static rgba flame(int frame, int x, int y)
{
    int h0 = 7 + (int)(hash2i(0xF1A3Eu + (uint32_t)frame * 31u, x, 0) % 7u);
    int hx = (x == 0 || x == 15) ? h0 / 2 : h0;
    int up = 15 - y; /* 0 at the bottom */
    if (up > hx) return (rgba){0, 0, 0, 0};
    float t = (float)up / (float)(hx > 0 ? hx : 1);
    int n = px_noise(T_FIRE0 + frame, x, y, 18);
    if (t < 0.35f) return shade(255, 236, 140, 235, n);
    if (t < 0.7f) return shade(250, 160, 40, 225, n);
    return shade(215, 70, 25, (int)(200.0f * (1.2f - t)), n);
}

/* Ore blocks: the rock around the mineral, the mineral, and how sparse
 * its clusters are (higher, fewer). Most sit in plain stone; bauxite and
 * rock salt are the rock themselves, gold and lithium sit in quartz-rich
 * rock. */
static const struct {
    int layer;
    rgba rock, mineral;
    int sparse;
} ORE_ART[] = {
    {T_COAL_ORE, {125, 125, 125, 255}, {30, 30, 32, 255}, 55},
    {T_COPPER_ORE, {125, 125, 125, 255}, {200, 110, 60, 255}, 55},
    {T_IRON_ORE, {125, 125, 125, 255}, {196, 160, 130, 255}, 55},
    {T_TIN_ORE, {125, 125, 125, 255}, {45, 35, 30, 255}, 55}, /* cassiterite, near black */
    {T_ZINC_ORE, {125, 125, 125, 255}, {150, 110, 50, 255}, 55}, /* resinous sphalerite */
    {T_LEAD_ORE, {125, 125, 125, 255}, {165, 170, 180, 255}, 55}, /* metallic galena */
    {T_NICKEL_ORE, {125, 125, 125, 255}, {180, 160, 95, 255}, 55}, /* brassy pentlandite */
    {T_SILVER_ORE, {125, 125, 125, 255}, {220, 220, 228, 255}, 62},
    {T_GOLD_ORE, {205, 200, 192, 255}, {240, 195, 50, 255}, 65}, /* flecks in white quartz */
    {T_BAUXITE, {175, 100, 60, 255}, {120, 60, 35, 255}, 40}, /* red earth with darker pisoliths */
    {T_TITANIUM_ORE, {125, 125, 125, 255}, {35, 35, 40, 255}, 55}, /* black ilmenite */
    {T_URANIUM_ORE, {125, 125, 125, 255}, {185, 190, 60, 255}, 55}, /* pitchblende's yellow alteration crust */
    {T_LITHIUM_ORE, {190, 185, 180, 255}, {205, 160, 200, 255}, 50}, /* pink spodumene in pale pegmatite */
    {T_SULFUR_ORE, {125, 125, 125, 255}, {230, 210, 50, 255}, 50},
    {T_SALTPETER_ORE, {125, 125, 125, 255}, {240, 240, 235, 255}, 50}, /* a white crust */
    {T_ROCK_SALT, {225, 205, 200, 255}, {245, 240, 238, 255}, 40}, /* halite, pink-white throughout */
    {T_QUARTZ_ORE, {125, 125, 125, 255}, {235, 235, 240, 255}, 45},
    {T_GRAPHITE_ORE, {110, 110, 115, 255}, {50, 50, 55, 255}, 45},
};

/* Stretched over the front of the head, 16 texels to its 0.20 m: brows,
 * eyes 6.5 cm apart at 0.94 of the height, the shadow under the nose (a box
 * of its own) and the mouth. */
static rgba face(int x, int y, int n)
{
    int eye = y == 7 && (x == 4 || x == 5 || x == 10 || x == 11);
    if (eye) return (x == 5 || x == 10) ? shade(52, 40, 32, 255, 0) : shade(232, 226, 218, 255, 0);
    if (y == 5 && ((x >= 3 && x <= 6) || (x >= 9 && x <= 12))) return shade(88, 62, 44, 255, n / 3);
    if (y == 10 && (x == 7 || x == 8)) return shade(190, 146, 114, 255, 0);
    if (y == 12 && x >= 6 && x <= 9) return shade(176, 112, 96, 255, 0);
    return shade(214, 170, 136, 255, n / 3);
}

static int leaf_gap(int layer, int x, int y, int holes)
{
    return (int)((hash2i(0x1EAFu + (uint32_t)layer, x / 2, y) + hash2i(0x1EAFu, x, y / 2) % 7u) % 100u) < holes;
}

/* Leaves cut out against the sky: a share of `holes` percent of the
 * texels is see-through, in clumps of two so the gaps read at a distance.
 * No 4x4 block has more than 6 gaps in 16, so from the third mip on (the
 * box filter averages a block into a texel) the leaves are solid and a
 * distant canopy closes up. A gap keeps a darker leaf colour for what
 * draws the texture without cutting it out (a falling block, an icon). */
static rgba leaf(int layer, int x, int y, rgba c, int holes, int grain)
{
    rgba t = shade(c.r, c.g, c.b, 255, px_noise(layer, x, y, grain));
    if (!leaf_gap(layer, x, y, holes)) return t;
    int before = 0;
    for (int i = 0; i < (y & 3) * 4 + (x & 3); i++)
        before += leaf_gap(layer, (x & ~3) + (i & 3), (y & ~3) + i / 4, holes);
    if (before >= 6) return t;
    return (rgba){(uint8_t)(t.r * 3 / 5), (uint8_t)(t.g * 3 / 5), (uint8_t)(t.b * 3 / 5), 0};
}

/* One stem of a plant drawn bottom up: stem i of `seed` stands at a
 * hashed column, lo..hi texels tall, leaning up to `lean` texels at its
 * tip. Returns how far row `up` (0 at the foot) is below the tip, 0 above
 * it, and the stem's column there in *cx. */
static int stem(uint32_t seed, int i, int up, int lo, int hi, int lean, int *cx)
{
    uint32_t h = hash2i(seed, i, 7);
    int height = lo + (int)((h >> 8) % (uint32_t)(hi - lo + 1));
    int l = (int)((h >> 16) % (uint32_t)(2 * lean + 1)) - lean;
    *cx = 2 + (int)(h % 12u) + l * up * up / (15 * 15);
    return up < height ? height - up : 0;
}

/* A flower's head: a diamond of petals round (cx, cy), its middle in the
 * centre colour. */
static int petal(int x, int y, int cx, int cy) { return abs(x - cx) + abs(y - cy) <= (x == cx || y == cy ? 2 : 1); }

/* Ground plants, drawn on crossed quads, rows top down. A clear texel
 * keeps the plant's colour, so the box-filtered mips fade a plant out
 * instead of darkening its edges; past a few metres the thin ones are
 * gone, which the eye takes for distance. */
static rgba plant(int layer, int x, int y)
{
    int up = 15 - y, n = px_noise(layer, x, y, 10), cx = 0;
    const rgba clear = {70, 104, 46, 0};
    uint32_t seed = 0x9A5u + (uint32_t)layer * 131u, h = hash2i(seed, x, y);
    switch (layer) {
    case T_GRASS_TUFT:
    case T_TALL_GRASS: {
        int tall = layer == T_TALL_GRASS;
        for (int i = 0; i < (tall ? 10 : 8); i++) {
            int tip = stem(seed, i, up, tall ? 9 : 4, tall ? 15 : 9, tall ? 3 : 2, &cx);
            if (!tip || x != cx) continue;
            /* Tall grass in seed: a pale head on the tallest stems. */
            if (tall && tip <= 2 && up >= 10 && (i & 1)) return shade(150, 150, 92, 255, n);
            return shade(64 + up * 3, 112 + up * 4, 42, 255, n);
        }
        return clear;
    }
    case T_FERN:
        /* Fronds arching out from the middle, leaflets on alternate rows. */
        for (int i = 0; i < 5; i++) {
            int tip = stem(seed, i, up, 8, 13, 6, &cx);
            if (!tip) continue;
            if (x == cx || (tip > 2 && abs(x - cx) == 1 && ((up + i) & 1)))
                return shade(48 + up * 2, 104 + up * 3, 40, 255, n);
        }
        return clear;
    case T_REEDS:
        for (int i = 0; i < 6; i++) {
            int tip = stem(seed, i, up, 11, 16, 1, &cx);
            if (!tip) continue;
            /* A brown seed head near the top of some stems. */
            if (i < 3 && tip >= 2 && tip <= 5 && (x == cx || x == cx + 1)) return shade(110, 70, 40, 255, n);
            if (x == cx) return shade(96 + up, 122 + up * 2, 58, 255, n);
        }
        return clear;
    case T_HEATHER: {
        /* A low mound, its tips flowering pink. */
        float dx = ((float)x - 7.5f) / 7.5f, dy = (float)up / 6.5f;
        if (dx * dx + dy * dy >= 1.0f || h % 5u == 0) return clear;
        if (up >= 2 && h % 100u < 18u + (uint32_t)up * 5u) return shade(178, 92, 152, 255, n);
        return shade(58, 82, 50, 255, n);
    }
    case T_DRY_SHRUB:
        for (int i = 0; i < 7; i++) {
            int tip = stem(seed, i, up, 6, 11, 4, &cx);
            if (tip && (x == cx || (up % 3 == 1 && x == cx + 1 && tip > 1))) return shade(128, 104, 78, 255, n);
        }
        return clear;
    case T_BUSH: {
        /* A round, dense bush with a few gaps and red berries. */
        float dx = (float)x - 7.5f, dy = (float)y - 8.5f;
        if (dx * dx + dy * dy > 49.0f || h % 9u == 0) return clear;
        if (h % 31u == 1) return shade(176, 34, 40, 255, n / 2);
        return shade(48, 94, 40, 255, n);
    }
    default: {
        /* Flowers: two to a texture, a tall one and a short one, on
         * stems with a leaf. */
        static const rgba HEAD[4] = {{204, 40, 32, 255},
                                     {240, 200, 44, 255},
                                     {74, 104, 214, 255},
                                     {236, 236, 226, 255}};
        static const rgba MID[4] = {{40, 20, 20, 255}, {226, 168, 30, 255}, {44, 60, 150, 255}, {232, 188, 40, 255}};
        int k = layer - T_POPPY;
        for (int f = 0; f < 2; f++) {
            int sx = f ? 11 : 5, top = f ? 9 : 12;
            if (petal(x, y, sx, 15 - top))
                return x == sx && y == 15 - top ? MID[k] : shade(HEAD[k].r, HEAD[k].g, HEAD[k].b, 255, n / 2);
            if (x == sx && up < top - 1) return shade(64, 116, 44, 255, n);
            if (up == top / 2 && x == sx + (f ? -1 : 1)) return shade(70, 126, 48, 255, n);
        }
        return clear;
    }
    }
}

/* The trees' bark, and the birch's end grain. */
static rgba bark(int layer, int x, int y, int n)
{
    switch (layer) {
    case T_BIRCH_SIDE: {
        /* White bark with dark lenticels in short horizontal dashes. */
        int dash = hash2i(0xB1Cu, x / 3, y) % 9u == 0;
        return dash ? shade(46, 42, 38, 255, n / 3) : shade(214, 210, 198, 255, n / 3);
    }
    case T_BIRCH_TOP: {
        int dx = x * 2 - 15, dy = y * 2 - 15;
        if (x == 0 || y == 0 || x == 15 || y == 15) return shade(214, 210, 198, 255, n / 3);
        return shade(222, 200, 150, 255, ((dx * dx + dy * dy) / 40) % 2 ? -12 : 0);
    }
    case T_SPRUCE_SIDE: return shade(80, 60, 44, 255, (hash2i(0x5B7u, x, y / 5) % 4u == 0 ? -20 : 0) + n / 2);
    case T_PINE_SIDE: {
        /* Orange-brown plates between darker furrows. */
        int plate = (int)(hash2i(0x914Eu, x / 3, (y + (x / 3) * 2) / 4) % 3u) * 10;
        return (x + y / 4) % 3 == 0 ? shade(96, 58, 38, 255, n / 3) : shade(150, 92, 54, 255, plate + n / 3);
    }
    default: break;
    }
    return shade(80, 60, 44, 255, n / 2);
}

static rgba texel(int layer, int x, int y)
{
    int n = px_noise(layer, x, y, 12);
    rgba sp = {0};
    if (sprite_texel(layer, x, y, &sp)) return sp;
    uint8_t px[4] = {0};
    if (mobskin_texel(layer, x, y, px)) return (rgba){px[0], px[1], px[2], px[3]};
    if (layer >= T_CRACK0 && layer <= T_CRACK3) return crack(layer - T_CRACK0, x, y);
    if (layer >= T_FIRE0 && layer <= T_FIRE3) return flame(layer - T_FIRE0, x, y);
    if (layer >= T_PLANT_FIRST && layer <= T_PLANT_LAST) return plant(layer, x, y);
    for (int i = 0; i < (int)(sizeof ORE_ART / sizeof ORE_ART[0]); i++) {
        if (ORE_ART[i].layer != layer) continue;
        /* Rock with clusters of mineral: a texel is ore where a coarse
         * hash says so and a neighbour agrees, giving blobs, not dust. */
        rgba r = ORE_ART[i].rock, m = ORE_ART[i].mineral;
        int ore = px_noise(layer, x / 2, y / 2, 100) > ORE_ART[i].sparse && px_noise(layer, x, y, 100) > -20;
        return ore ? shade(m.r, m.g, m.b, 255, n / 2) : shade(r.r, r.g, r.b, 255, n);
    }
    switch (layer) {
    case T_SKIN: return shade(214, 170, 136, 255, n / 3);
    case T_LINEN:
    case T_LINEN_DARK: {
        /* Plain weave: warp and weft alternate, and the odd slub (a
         * thicker stretch of thread) runs across. */
        int weave = ((x + y) & 1) ? 5 : -5, slub = (hash2i(0x11E, 0, y) & 7u) == 0 ? -8 : 0;
        if (layer == T_LINEN) return shade(214, 204, 180, 255, weave + slub + n / 4);
        return shade(176, 163, 132, 255, weave + slub + n / 4);
    }
    case T_HAIR: return shade(58, 42, 30, 255, px_noise(layer, x, 0, 14) + n / 3); /* strands run down */
    case T_FACE: return face(x, y, n);
    case T_CAMPFIRE_BASE: {
        /* Crossed logs over glowing embers. */
        int log = (y >= 5 && y <= 10) || (x >= 5 && x <= 10);
        if (log) return shade(96, 70, 40, 255, (x % 4 == 0 ? -14 : 0) + n / 2);
        return (hash2i(0xE3B, x, y) & 3u) ? shade(60, 44, 36, 255, n) : shade(240, 110, 30, 255, n);
    }
    case T_ASH: return shade(110, 106, 102, 255, px_noise(layer, x, y, 20));
    case T_STONE: return shade(125, 125, 125, 255, n);
    case T_GRANITE: {
        /* Coarse crystals: black mica and pink feldspar in grey quartz. */
        int v = px_noise(layer, x, y, 100);
        if (v > 70) return shade(40, 38, 40, 255, n / 3);
        if (v < -60) return shade(205, 160, 150, 255, n / 2);
        return shade(170, 162, 158, 255, n / 2);
    }
    case T_BASALT: /* fine-grained and dark, with gas bubbles frozen in */
        if (px_noise(layer, x / 2, y / 2, 100) > 88) return shade(32, 32, 35, 255, n / 4);
        return shade(62, 62, 66, 255, n / 2);
    case T_LIMESTONE: /* pale, with a fleck of shell here and there */
        if (px_noise(layer, x, y, 100) > 88) return shade(160, 150, 125, 255, n / 3);
        return shade(198, 190, 165, 255, n / 2);
    case T_SANDSTONE: { /* laid down in beds of slightly different sand */
        int bed = (y + (px_noise(layer, x / 4, 0, 1) + 1)) % 5 == 0;
        return shade(bed ? 180 : 205, bed ? 140 : 165, bed ? 95 : 115, 255, n / 2);
    }
    case T_SHALE: /* thin laminae of compacted mud */ return shade(82, 84, 90, 255, (y % 3 == 0 ? -14 : 0) + n / 3);
    case T_MARBLE: { /* white calcite with grey veins running across */
        int v = ((x + y + px_noise(layer, 0, y / 4, 2)) % 9 + 9) % 9;
        if (v <= 1) return shade(150, 150, 156, 255, n / 3);
        return shade(228, 226, 222, 255, n / 4);
    }
    case T_OBSIDIAN: /* black glass catching the light along its conchoidal faces */
        if ((x + y) % 7 == 0 && px_noise(layer, x / 2, y, 100) > 0) return shade(78, 72, 92, 255, n / 3);
        return shade(28, 22, 36, 255, n / 3);
    case T_CLAY: return shade(150, 128, 112, 255, n / 3);
    case T_MUD:
        if (px_noise(layer, x, y, 100) > 80) return shade(110, 90, 75, 255, n / 3); /* a wet shine */
        return shade(78, 58, 42, 255, n / 2);
    case T_PEAT: /* dark, with the fibres of the plants it was */
        if (px_noise(layer, x, y / 3, 100) > 60) return shade(90, 65, 40, 255, n / 3);
        return shade(58, 40, 28, 255, n / 2);
    case T_PODZOL: { /* fallen needles over the dark top of the soil */
        int v = px_noise(layer, x, y, 100);
        if (v > 70) return shade(150, 95, 45, 255, n / 3);
        if (v < -80) return shade(70, 55, 40, 255, n / 3);
        return shade(100, 75, 48, 255, n / 2);
    }
    case T_FURNACE_TOP: {
        int edge = x == 0 || y == 0 || x == 15 || y == 15;
        return shade(edge ? 100 : 118, edge ? 100 : 118, edge ? 100 : 118, 255, n / 2);
    }
    case T_FURNACE_FRONT: {
        /* Stone blocks around a dark firebox mouth with embers inside. */
        if (x >= 4 && x <= 11 && y >= 7 && y <= 13) {
            if (y >= 11) return shade(220, 90, 25, 255, px_noise(layer, x, y, 60));
            return shade(28, 24, 22, 255, n / 4);
        }
        int row = y / 4, mortar = y % 4 == 3 || ((x + (row % 2) * 4) % 8) == 7;
        return mortar ? shade(80, 80, 80, 255, n / 3) : shade(130, 130, 130, 255, n);
    }
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
    case T_LEAVES: return leaf(layer, x, y, (rgba){58, 110, 38, 255}, 26, 22);
    case T_BIRCH_LEAVES: return leaf(layer, x, y, (rgba){108, 158, 58, 255}, 32, 22);
    case T_SPRUCE_LEAVES: return leaf(layer, x, y, (rgba){36, 70, 48, 255}, 18, 14);
    case T_PINE_LEAVES: return leaf(layer, x, y, (rgba){52, 92, 50, 255}, 24, 16);
    case T_THORN_LEAVES: return leaf(layer, x, y, (rgba){104, 122, 56, 255}, 38, 18);
    case T_BIRCH_SIDE:
    case T_BIRCH_TOP:
    case T_SPRUCE_SIDE:
    case T_PINE_SIDE: return bark(layer, x, y, n);
    case T_THORN_SIDE: return shade(112, 98, 80, 255, (y % 5 == 0 ? -12 : 0) + n / 2);
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
    case T_WATER: return shade(30, 75, 185, 195, n / 2);
    case T_LAVA: {
        /* Glowing melt under a broken, darker skin that has started to
         * cool: a coarse hash picks the skin plates. */
        int skin = hash2i(0x1A7Au, x / 3, y / 3) % 5 == 0;
        return skin ? shade(120, 35, 15, 255, n / 2) : shade(245, 120 + n, 25, 255, n / 3);
    }
    case T_CRUDE_OIL: return shade(28, 22, 16, 240, n / 4);
    case T_BRINE: return shade(55, 105, 150, 205, n / 2);
    case T_LAMP: {
        /* A copper frame round a glass chimney with the flame inside. */
        int frame = x <= 1 || x >= 14 || y <= 1 || y >= 14;
        if (frame) return shade(175, 105, 60, 255, n / 2);
        int dx = x * 2 - 15, dy = y * 2 - 17, d2 = dx * dx + dy * dy;
        if (d2 < 20) return shade(255, 240, 170, 255, n / 4);
        if (d2 < 60) return shade(250, 190, 90, 255, n / 3);
        return shade(225, 200, 150, 255, n / 2);
    }
    case T_FOXFIRE: {
        /* Pale green threads through dark rotten matter. */
        int glow = px_noise(layer, x / 2, y, 100) > 30 || px_noise(layer, x, y / 2, 100) > 55;
        return glow ? shade(150, 235, 160, 255, n / 2) : shade(45, 40, 32, 255, n);
    }
    /* The item materials: grey, so the tint sets the colour, and gentle,
     * since a face stretches the layer over a 3 cm handle or a 20 cm
     * bucket alike. */
    case T_MAT_WOOD: {
        /* Grain along the length, wandering a texel or two. */
        int g = (x + (int)(hash2i(0x600Du, 0, y / 5) % 3u)) % 5 == 0;
        return shade(232, 232, 232, 255, (g ? -34 : 0) + n / 2);
    }
    case T_MAT_METAL: {
        /* Brushed: streaks along the rows and one bright band. */
        int band = y == 4 || y == 5 ? 16 : 0;
        return shade(228, 228, 228, 255, band + px_noise(layer, 0, y, 8) + n / 4);
    }
    case T_MAT_CAST: return shade(218, 218, 218, 255, px_noise(layer, x / 2, y / 2, 22) + n / 3);
    case T_MAT_ROCK: return shade(214, 214, 214, 255, px_noise(layer, x / 4, y / 4, 36) + n / 2);
    case T_MAT_CLOTH: return shade(236, 236, 236, 255, ((x + y) % 2 ? -12 : 4) + n / 4);
    case T_MAT_GLASS: return x == 3 || x == 4 ? (rgba){255, 255, 255, 200} : (rgba){236, 240, 242, 130};
    case T_MAT_PLAIN: return shade(238, 238, 238, 255, n / 3);
    case T_MAT_FLESH: {
        /* Muscle with paler streaks of fat. */
        int fat = (y + (int)(hash2i(0xFA7u, x / 3, 0) % 4u)) % 6 == 0;
        return shade(214, 214, 214, 255, (fat ? 36 : 0) + n);
    }
    case T_MAT_GRAIN: return shade(212, 212, 212, 255, px_noise(layer, x, y, 40));
    case T_SMOKE0:
    case T_SMOKE1:
    case T_SMOKE2: {
        /* A soft round puff, densest in the middle, so a cube of it reads as
         * a blob from any side; each later step lets more light through. */
        static const int DENSITY[3] = {170, 105, 50};
        int dx = x * 2 - 15, dy = y * 2 - 15, d2 = dx * dx + dy * dy;
        int a = DENSITY[layer - T_SMOKE0] * (256 - d2) / 256 + px_noise(layer, x, y, 20);
        return shade(196, 194, 188, (uint8_t)(a < 0 ? 0 : (a > 255 ? 255 : a)), n / 3);
    }
    case T_FLASH: {
        /* Burning powder gas: white at the core, yellow toward the edge. */
        int dx = x * 2 - 15, dy = y * 2 - 15, d2 = dx * dx + dy * dy;
        return d2 < 90 ? shade(255, 250, 225, 255, n / 4) : shade(255, 205, 110, 255, n / 3);
    }
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

void texgen_texel(int layer, int x, int y, uint8_t out[4])
{
    rgba c = texel(layer, x & (TEX_SIZE - 1), y & (TEX_SIZE - 1));
    out[0] = c.r;
    out[1] = c.g;
    out[2] = c.b;
    out[3] = c.a;
}

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
