#include "itemmodel.h"

#include "texgen.h"
#include "torch_model.h"
#include "ui.h"

#include <math.h>
#include <string.h>

/* Woods and fibres, the colours shared across families. */
#define ASH 0xB48C5Au     /* pale handle wood */
#define WALNUT 0x6A4024u  /* a rifle's stock */
#define DARKWOOD 0x5E4028u
#define FIBRE 0x9C9658u   /* dried plant fibre, lashings and rope */
#define GUNMETAL 0x3C3E42u

static const uint32_t TIER_RGB[TIER_COUNT] = {0x7A5634u, 0x85827Cu, 0xC8784Au, 0xA2A6AAu};
static const uint8_t TIER_LAYER[TIER_COUNT] = {T_MAT_WOOD, T_MAT_ROCK, T_MAT_METAL, T_MAT_METAL};

static float hash01(uint32_t v)
{
    v ^= v >> 16;
    v *= 0x7feb352du;
    v ^= v >> 15;
    v *= 0x846ca68bu;
    v ^= v >> 16;
    return (float)(v >> 8) / 16777216.0f;
}

/* Brighter, for a ground edge or a highlight: halfway to white. */
static uint32_t lighter(uint32_t rgb) { return ((rgb >> 1) & 0x7F7F7Fu) + 0x7F7F7Fu; }
static uint32_t darker(uint32_t rgb) { return (rgb >> 1) & 0x7F7F7Fu; }

/* The same colour in shade, a seventh darker. */
static uint32_t dim(uint32_t rgb)
{
    uint32_t out = 0;
    for (int s = 0; s < 24; s += 8) out |= ((rgb >> s & 255u) * 6u / 7u) << s;
    return out;
}

/* A box; past the budget it is dropped (the tests keep every model inside). */
static im_box *box(item_model *m, float x, float y, float z, float sx, float sy, float sz, uint8_t layer, uint32_t rgb)
{
    static im_box spill;
    if (m->n >= IM_MAX_BOXES) return &spill;
    im_box *b = &m->box[m->n++];
    *b = (im_box){{x, y, z}, {sx, sy, sz}, quat_identity(), layer, 0, rgb};
    return b;
}

static void turn(im_box *b, float ax, float ay, float az, float a)
{ b->rot = quat_norm(quat_mul(quat_axis(ax, ay, az, a), b->rot)); }

/* ------------------------------------------------------------ tools */

/* Tools stand head up: the handle runs up y from its foot at 0. */
static void handle(item_model *m, float from, float to, float w)
{ box(m, 0.0f, (from + to) * 0.5f, 0.0f, w, to - from, w, T_MAT_WOOD, ASH); }

/* A stone head is lashed on with fibre, crossed over the handle. */
static void lash(item_model *m, float y)
{
    box(m, 0.0f, y, 0.0f, 0.046f, 0.028f, 0.046f, T_MAT_CLOTH, FIBRE);
    turn(box(m, 0.0f, y, 0.0f, 0.05f, 0.022f, 0.05f, T_MAT_CLOTH, darker(FIBRE) + 0x303018u), 0.0f, 0.0f, 1.0f, 0.7f);
}

/* A pick: 0.78 m of handle under a curved head about 0.6 m across, a point
 * on one side and a chisel on the other. */
static void pickaxe(item_model *m, uint8_t hl, uint32_t hc, int stone)
{
    const float top = 0.78f;
    handle(m, 0.0f, top + 0.02f, 0.034f);
    box(m, 0.0f, top - 0.03f, 0.0f, 0.06f, 0.07f, 0.05f, hl, hc);
    for (int s = -1; s <= 1; s += 2) {
        float f = (float)s;
        turn(box(m, f * 0.10f, top - 0.035f, 0.0f, 0.12f, 0.05f, 0.042f, hl, hc), 0.0f, 0.0f, 1.0f, -f * 0.12f);
        turn(box(m, f * 0.20f, top - 0.062f, 0.0f, 0.11f, 0.038f, 0.034f, hl, hc), 0.0f, 0.0f, 1.0f, -f * 0.30f);
        /* The point (+x) narrows both ways; the chisel (-x) only in height. */
        float tz = s > 0 ? 0.022f : 0.04f;
        turn(box(m, f * 0.27f, top - 0.097f, 0.0f, 0.06f, s > 0 ? 0.022f : 0.012f, tz, hl, lighter(hc)), 0.0f, 0.0f,
             1.0f, -f * 0.55f);
    }
    if (stone) lash(m, top - 0.075f);
}

/* An axe: a wedge on a 0.7 m handle, the bit flaring to its edge and a
 * short poll behind the eye. */
static void axe(item_model *m, uint8_t hl, uint32_t hc, int stone)
{
    const float y = 0.65f;
    handle(m, 0.0f, 0.71f, 0.034f);
    box(m, 0.0f, y, 0.0f, 0.056f, 0.10f, 0.048f, hl, hc);
    box(m, 0.075f, y - 0.005f, 0.0f, 0.10f, 0.105f, 0.03f, hl, hc);
    box(m, 0.135f, y - 0.01f, 0.0f, 0.03f, 0.16f, 0.016f, hl, lighter(hc));
    box(m, -0.042f, y, 0.0f, 0.03f, 0.075f, 0.044f, hl, darker(hc) + 0x202020u);
    if (stone) lash(m, y - 0.07f);
}

/* A spade, blade up: a D grip at the foot, the shaft, the socket and a
 * dished blade 20 cm wide. */
static void shovel(item_model *m, uint8_t hl, uint32_t hc, int stone)
{
    box(m, 0.0f, 0.0125f, 0.0f, 0.11f, 0.025f, 0.03f, T_MAT_WOOD, ASH);
    turn(box(m, 0.045f, 0.06f, 0.0f, 0.018f, 0.09f, 0.022f, T_MAT_WOOD, ASH), 0.0f, 0.0f, 1.0f, 0.3f);
    turn(box(m, -0.045f, 0.06f, 0.0f, 0.018f, 0.09f, 0.022f, T_MAT_WOOD, ASH), 0.0f, 0.0f, 1.0f, -0.3f);
    handle(m, 0.10f, 0.66f, 0.034f);
    box(m, 0.0f, 0.68f, 0.0f, 0.045f, 0.08f, 0.04f, hl, hc);
    turn(box(m, 0.0f, 0.82f, 0.0f, 0.20f, 0.22f, 0.014f, hl, hc), 1.0f, 0.0f, 0.0f, 0.15f);
    turn(box(m, 0.0f, 0.95f, 0.018f, 0.12f, 0.045f, 0.014f, hl, lighter(hc)), 1.0f, 0.0f, 0.0f, 0.15f);
    if (stone) lash(m, 0.66f);
}

/* A knife, 24 cm: the handle, a bolster (or the lashing of a flint
 * blade), the blade and its tip. */
static void knife(item_model *m, uint8_t hl, uint32_t hc, int stone)
{
    box(m, 0.0f, 0.055f, 0.0f, 0.026f, 0.11f, 0.02f, T_MAT_WOOD, DARKWOOD);
    if (stone) box(m, 0.0f, 0.115f, 0.0f, 0.032f, 0.018f, 0.026f, T_MAT_CLOTH, FIBRE);
    else box(m, 0.0f, 0.115f, 0.0f, 0.03f, 0.012f, 0.024f, hl, darker(hc) + 0x303030u);
    box(m, 0.002f, 0.175f, 0.0f, 0.024f, 0.11f, stone ? 0.008f : 0.004f, hl, hc);
    turn(box(m, -0.003f, 0.24f, 0.0f, 0.014f, 0.035f, stone ? 0.007f : 0.004f, hl, lighter(hc)), 0.0f, 0.0f, 1.0f,
         0.3f);
}

/* A sword, 0.9 m: pommel, wrapped grip, cross-guard, a blade with its
 * fuller and a narrowing point. */
static void sword(item_model *m, uint8_t hl, uint32_t hc, int stone)
{
    box(m, 0.0f, 0.02f, 0.0f, 0.045f, 0.035f, 0.04f, hl, darker(hc) + 0x303030u);
    box(m, 0.0f, 0.10f, 0.0f, 0.03f, 0.13f, 0.028f, T_MAT_CLOTH, stone ? FIBRE : 0x5A3A22u);
    box(m, 0.0f, 0.175f, 0.0f, 0.18f, 0.024f, 0.032f, hl, darker(hc) + 0x303030u);
    box(m, 0.0f, 0.51f, 0.0f, 0.048f, 0.65f, stone ? 0.014f : 0.008f, hl, hc);
    box(m, 0.0f, 0.46f, 0.0f, 0.012f, 0.48f, stone ? 0.015f : 0.0095f, hl, darker(hc) + 0x404040u);
    box(m, 0.0f, 0.86f, 0.0f, 0.03f, 0.06f, stone ? 0.012f : 0.007f, hl, lighter(hc));
    box(m, 0.0f, 0.90f, 0.0f, 0.014f, 0.03f, stone ? 0.01f : 0.006f, hl, lighter(hc));
}

static void tool(item_model *m, int kind, int tier)
{
    uint8_t hl = TIER_LAYER[tier];
    uint32_t hc = TIER_RGB[tier];
    int stone = tier == TIER_STONE;
    m->hold = IM_HOLD_TOOL;
    switch (kind) {
    case TOOL_PICKAXE: pickaxe(m, hl, hc, stone); break;
    case TOOL_AXE: axe(m, hl, hc, stone); break;
    case TOOL_SHOVEL: shovel(m, hl, hc, stone); break;
    case TOOL_KNIFE: knife(m, hl, hc, stone); break;
    default: sword(m, hl, hc, stone); break;
    }
    /* Held near the foot of the handle (a spade by its D grip). */
    m->grip[1] = kind == TOOL_KNIFE ? 0.05f : kind == TOOL_SWORD ? 0.09f : (kind == TOOL_SHOVEL ? 0.03f : 0.09f);
}

/* ------------------------------------------------------ minerals */

/* A cast bar about 100 x 45 x 30 mm, tapering to its top, with the
 * founder's stamp. */
static void ingot(item_model *m, uint32_t rgb, uint8_t layer)
{
    box(m, 0.0f, 0.011f, 0.0f, 0.11f, 0.022f, 0.05f, layer, rgb);
    box(m, 0.0f, 0.028f, 0.0f, 0.094f, 0.014f, 0.036f, layer, lighter(rgb));
    box(m, 0.0f, 0.0352f, 0.0f, 0.03f, 0.001f, 0.012f, layer, darker(rgb));
}

enum { M_LUMP, M_CUBES, M_CRYSTALS, M_FLAKES, M_CRUST };

typedef struct {
    item_id id;
    uint32_t rock;  /* the host rock, 0 for none */
    uint32_t grain; /* the mineral */
    uint8_t shape;
    uint8_t layer;
} mineral_art;

/* What each looks like in the hand: the host rock and the mineral in it
 * (cassiterite's black-brown, sphalerite's resin, galena's grey cubes,
 * malachite's green on copper ore, gold in white quartz). */
static const mineral_art MINERAL_ART[] = {
    {I_RAW_COPPER, 0x6A5E54u, 0x3A9A6Au, M_LUMP, T_MAT_ROCK},
    {I_RAW_IRON, 0x7A3C2Cu, 0x3A3A3Eu, M_LUMP, T_MAT_METAL},
    {I_RAW_TIN, 0x8A847Cu, 0x3A2A22u, M_LUMP, T_MAT_METAL},
    {I_RAW_ZINC, 0x8A847Cu, 0x9A6A2Au, M_LUMP, T_MAT_METAL},
    {I_RAW_LEAD, 0x8A847Cu, 0x8C9098u, M_CUBES, T_MAT_METAL},
    {I_RAW_NICKEL, 0x5A5650u, 0xC0A858u, M_LUMP, T_MAT_METAL},
    {I_RAW_SILVER, 0x4A4846u, 0xD8D8DCu, M_LUMP, T_MAT_METAL},
    {I_RAW_GOLD, 0xEAE6DEu, 0xE8B840u, M_LUMP, T_MAT_METAL},
    {I_BAUXITE, 0xB4643Eu, 0xD4925Eu, M_LUMP, T_MAT_ROCK},
    {I_ILMENITE, 0x2E2C2Cu, 0x5A5A60u, M_LUMP, T_MAT_METAL},
    {I_URANINITE, 0x22201Eu, 0xD0C840u, M_LUMP, T_MAT_GRAIN},
    {I_SPODUMENE, 0x9A9690u, 0xC8B8D8u, M_CRYSTALS, T_MAT_PLAIN},
    {I_SULFUR, 0xC8B860u, 0xF0D830u, M_CRYSTALS, T_MAT_PLAIN},
    {I_SALTPETER, 0x8A847Cu, 0xF2F0E8u, M_CRUST, T_MAT_GRAIN},
    {I_SALT, 0, 0xF2E8E4u, M_CUBES, T_MAT_PLAIN},
    {I_QUARTZ, 0x8A847Cu, 0xF6F8FAu, M_CRYSTALS, T_MAT_GLASS},
    {I_GRAPHITE, 0, 0x3C3E44u, M_FLAKES, T_MAT_METAL},
    {I_COAL, 0x2A2A2Eu, 0x1C1C1Eu, M_LUMP, T_MAT_ROCK},
    {I_QUICKLIME, 0xEEEBE2u, 0xF8F6F0u, M_LUMP, T_MAT_GRAIN},
};

/* A lump of rock, about 8 cm, turned by the item's own hash. */
static void rock(item_model *m, uint32_t seed, uint32_t rgb)
{
    turn(box(m, 0.0f, 0.028f, 0.0f, 0.075f, 0.052f, 0.064f, T_MAT_ROCK, rgb), 0.3f, 1.0f, 0.2f, 6.28f * hash01(seed));
    turn(box(m, 0.03f, 0.026f, 0.022f, 0.05f, 0.044f, 0.05f, T_MAT_ROCK, dim(rgb)), 1.0f, 0.4f, 0.0f,
         0.8f + hash01(seed + 1u));
}

static void mineral(item_model *m, const mineral_art *a)
{
    uint32_t s = a->id * 2654435761u;
    switch (a->shape) {
    case M_LUMP:
        rock(m, s, a->rock);
        /* Grains of the mineral breaking the surface. */
        for (int k = 0; k < 3; k++) {
            float u = hash01(s + 7u * (uint32_t)k + 2u) - 0.5f, v = hash01(s + 7u * (uint32_t)k + 3u) - 0.5f;
            float g = 0.018f + 0.012f * hash01(s + (uint32_t)k + 9u);
            turn(box(m, 0.06f * u, 0.046f + 0.006f * v, 0.05f * v, g, g, g, a->layer, a->grain), 1.0f, 1.0f, 0.0f,
                 3.0f * u);
        }
        break;
    case M_CUBES:
        /* Cubic habit: galena and halite break into stepped cubes. */
        if (a->rock) box(m, 0.0f, 0.012f, 0.0f, 0.08f, 0.024f, 0.065f, T_MAT_ROCK, a->rock);
        {
            float b = a->rock ? 0.024f : 0.0f;
            box(m, -0.012f, b + 0.0175f, 0.0f, 0.035f, 0.035f, 0.035f, a->layer, a->grain);
            box(m, 0.02f, b + 0.014f, 0.012f, 0.028f, 0.028f, 0.028f, a->layer, lighter(a->grain));
            box(m, 0.006f, b + 0.042f, -0.006f, 0.022f, 0.022f, 0.022f, a->layer, a->grain);
        }
        break;
    case M_CRYSTALS:
        /* Prisms leaning out of a base, each with its pointed tip. */
        box(m, 0.0f, 0.012f, 0.0f, 0.07f, 0.024f, 0.06f, T_MAT_ROCK, a->rock);
        for (int k = 0; k < 3; k++) {
            float ang = -0.35f + 0.35f * (float)k, h = 0.06f + 0.012f * (float)(k % 2);
            float x = -0.018f + 0.018f * (float)k, cx = x - sinf(ang) * h * 0.5f, cy = 0.024f + cosf(ang) * h * 0.5f;
            turn(box(m, cx, cy, 0.006f * (float)(k - 1), 0.018f, h, 0.018f, a->layer, a->grain), 0.0f, 0.0f, 1.0f, ang);
            im_box *t = box(m, cx - sinf(ang) * h * 0.55f, cy + cosf(ang) * h * 0.55f, 0.006f * (float)(k - 1), 0.012f,
                            0.012f, 0.012f, a->layer, lighter(a->grain));
            turn(t, 0.0f, 1.0f, 0.0f, 0.785f);
            turn(t, 0.0f, 0.0f, 1.0f, ang);
        }
        break;
    case M_FLAKES:
        /* Graphite splits into soft, shiny flakes. */
        for (int k = 0; k < 3; k++)
            turn(box(m, 0.006f * (float)k, 0.005f + 0.009f * (float)k, 0.0f, 0.07f - 0.012f * (float)k, 0.008f,
                     0.05f - 0.008f * (float)k, a->layer, k == 1 ? a->grain + 0x181818u : a->grain),
                 0.0f, 1.0f, 0.0f, 0.4f * (float)k);
        break;
    default:
        /* A white crust of crystals grown over the rock. */
        rock(m, s, a->rock);
        for (int k = 0; k < 4; k++)
            box(m, -0.02f + 0.014f * (float)k, 0.054f, -0.012f + 0.008f * (float)(k % 2), 0.026f, 0.01f, 0.026f,
                a->layer, a->grain);
        break;
    }
}

/* A small heap, three layers turned against each other so it reads round. */
static void heap(item_model *m, uint32_t rgb)
{
    for (int k = 0; k < 3; k++)
        turn(box(m, 0.0f, 0.008f + 0.014f * (float)k, 0.0f, 0.09f - 0.026f * (float)k, 0.016f,
                 0.09f - 0.026f * (float)k, T_MAT_GRAIN, rgb),
             0.0f, 1.0f, 0.0f, 0.785f * (float)k);
}

/* ---------------------------------------------------- containers */

typedef struct {
    float h, w, neck;  /* body height and width, neck length (m) */
    uint32_t glass, liquid, label, cap;
    uint8_t cap_layer;
    float fill;        /* of the body */
} bottle_art;

/* A glass bottle: the liquid inside, a label round it, shoulder, neck and
 * a cap or cork. The glass is see-through, so the liquid shows. */
static void bottle(item_model *m, const bottle_art *b)
{
    float lh = b->h * b->fill;
    box(m, 0.0f, 0.004f + lh * 0.5f, 0.0f, b->w - 0.008f, lh, b->w - 0.008f, T_MAT_PLAIN, b->liquid);
    box(m, 0.0f, b->h * 0.5f, 0.0f, b->w, b->h, b->w, T_MAT_GLASS, b->glass);
    turn(box(m, 0.0f, b->h * 0.5f, 0.0f, b->w * 0.92f, b->h * 0.96f, b->w * 0.92f, T_MAT_GLASS, b->glass), 0.0f, 1.0f,
         0.0f, 0.785f);
    if (b->label) box(m, 0.0f, b->h * 0.45f, 0.0f, b->w + 0.002f, b->h * 0.45f, b->w + 0.002f, T_MAT_PLAIN, b->label);
    box(m, 0.0f, b->h + 0.007f, 0.0f, b->w * 0.7f, 0.014f, b->w * 0.7f, T_MAT_GLASS, b->glass);
    box(m, 0.0f, b->h + 0.014f + b->neck * 0.5f, 0.0f, b->w * 0.36f, b->neck, b->w * 0.36f, T_MAT_GLASS, b->glass);
    box(m, 0.0f, b->h + 0.014f + b->neck + 0.009f, 0.0f, b->w * 0.42f, 0.018f, b->w * 0.42f, b->cap_layer, b->cap);
}

/* A coopered wooden bucket, about ten litres (26 cm across and high):
 * four staves, a lashed hoop at the foot and one round the rim, a rope
 * bail, and the liquid in it, if any (its own see-through layer). */
static void bucket(item_model *m, uint8_t liquid)
{
    const float r = 0.12f, h = 0.24f;
    for (int s = -1; s <= 1; s += 2) {
        float f = (float)s;
        box(m, 0.0f, h * 0.5f, f * r, 2.0f * r + 0.018f, h, 0.018f, T_MAT_WOOD, ASH);
        box(m, f * r, h * 0.5f, 0.0f, 0.018f, h, 2.0f * r - 0.018f, T_MAT_WOOD, darker(ASH) + 0x303030u);
        box(m, 0.0f, h - 0.03f, f * (r + 0.011f), 2.0f * r + 0.04f, 0.022f, 0.006f, T_MAT_CLOTH, FIBRE);
        box(m, f * (r + 0.011f), h - 0.03f, 0.0f, 0.006f, 0.022f, 2.0f * r + 0.04f, T_MAT_CLOTH, FIBRE);
        box(m, f * (r + 0.012f), h - 0.01f, 0.0f, 0.02f, 0.04f, 0.03f, T_MAT_WOOD, ASH);
        turn(box(m, f * 0.09f, h + 0.065f, 0.0f, 0.11f, 0.012f, 0.012f, T_MAT_CLOTH, FIBRE), 0.0f, 0.0f, 1.0f,
             f * 0.95f);
    }
    box(m, 0.0f, 0.045f, 0.0f, 2.0f * r + 0.04f, 0.03f, 2.0f * r + 0.04f, T_MAT_CLOTH, FIBRE);
    box(m, 0.0f, h + 0.11f, 0.0f, 0.08f, 0.012f, 0.012f, T_MAT_CLOTH, FIBRE);
    if (liquid) box(m, 0.0f, 0.06f + 0.07f, 0.0f, 2.0f * r - 0.02f, 0.14f, 2.0f * r - 0.02f, liquid, 0xFFFFFFu);
}

/* ----------------------------------------------------- one-offs */

static void stick(item_model *m)
{
    m->hold = IM_HOLD_TOOL;
    box(m, 0.0f, 0.2f, 0.0f, 0.024f, 0.4f, 0.024f, T_MAT_WOOD, 0x8A6A44u);
    /* The stub of a side twig. */
    turn(box(m, 0.022f, 0.27f, 0.0f, 0.012f, 0.07f, 0.012f, T_MAT_WOOD, 0x7A5C3Au), 0.0f, 0.0f, 1.0f, -0.6f);
    m->grip[1] = 0.06f;
}

/* Strands of dried fibre, tied at the middle. */
static void fibre(item_model *m)
{
    m->hold = IM_HOLD_TOOL;
    for (int k = 0; k < 4; k++)
        turn(box(m, -0.009f + 0.006f * (float)k, 0.13f, 0.004f * (float)(k % 2), 0.005f, 0.26f, 0.005f, T_MAT_PLAIN,
                 k % 2 ? FIBRE : 0x8A8A4Au),
             0.0f, 0.0f, 1.0f, -0.06f + 0.04f * (float)k);
    box(m, 0.0f, 0.13f, 0.0f, 0.032f, 0.02f, 0.02f, T_MAT_CLOTH, 0x6A6438u);
    m->grip[1] = 0.13f;
}

/* A roll of white gauze: two boxes turned 45 degrees make an octagonal
 * prism, with the loose end hanging off. */
static void bandage(item_model *m)
{
    box(m, 0.0f, 0.03f, 0.0f, 0.06f, 0.06f, 0.07f, T_MAT_CLOTH, 0xF2F0EAu);
    turn(box(m, 0.0f, 0.03f, 0.0f, 0.06f, 0.06f, 0.07f, T_MAT_CLOTH, 0xE6E4DEu), 0.0f, 0.0f, 1.0f, 0.785f);
    box(m, 0.0f, 0.0015f, 0.062f, 0.05f, 0.003f, 0.06f, T_MAT_CLOTH, 0xF2F0EAu);
}

static void splint(item_model *m)
{
    m->hold = IM_HOLD_TOOL;
    for (int s = -1; s <= 1; s += 2) box(m, (float)s * 0.02f, 0.16f, 0.0f, 0.032f, 0.32f, 0.01f, T_MAT_WOOD, 0xC4A06Cu);
    box(m, 0.0f, 0.08f, 0.0f, 0.078f, 0.02f, 0.018f, T_MAT_CLOTH, FIBRE);
    box(m, 0.0f, 0.24f, 0.0f, 0.078f, 0.02f, 0.018f, T_MAT_CLOTH, FIBRE);
    m->grip[1] = 0.16f;
}

/* A blister strip of eight tablets on foil. */
static void blister(item_model *m)
{
    box(m, 0.0f, 0.002f, 0.0f, 0.10f, 0.004f, 0.048f, T_MAT_METAL, 0xC8CCD2u);
    for (int k = 0; k < 8; k++)
        turn(box(m, -0.036f + 0.024f * (float)(k >> 1), 0.006f, k % 2 ? 0.012f : -0.012f, 0.014f, 0.006f, 0.014f,
                 T_MAT_PLAIN, 0xF6F6F2u),
             0.0f, 1.0f, 0.0f, 0.785f);
}

static void apple(item_model *m)
{
    box(m, 0.0f, 0.037f, 0.0f, 0.072f, 0.066f, 0.072f, T_MAT_PLAIN, 0x9A1A1Eu);
    turn(box(m, 0.0f, 0.036f, 0.0f, 0.078f, 0.052f, 0.078f, T_MAT_PLAIN, 0xAE2A1Cu), 0.0f, 1.0f, 0.0f, 0.785f);
    turn(box(m, 0.0f, 0.038f, 0.0f, 0.058f, 0.074f, 0.058f, T_MAT_PLAIN, 0x881618u), 0.0f, 1.0f, 0.0f, 0.785f);
    turn(box(m, 0.003f, 0.08f, 0.0f, 0.005f, 0.02f, 0.005f, T_MAT_WOOD, 0x5A3A1Eu), 0.0f, 0.0f, 1.0f, -0.25f);
    turn(box(m, 0.016f, 0.084f, 0.0f, 0.024f, 0.003f, 0.012f, T_MAT_PLAIN, 0x4A8A2Au), 0.0f, 0.0f, 1.0f, 0.4f);
}

/* An automatic external defibrillator: a case with a carrying handle, a
 * dark face with its shock button and power switch. */
static void defibrillator(item_model *m)
{
    box(m, 0.0f, 0.09f, 0.0f, 0.25f, 0.18f, 0.08f, T_MAT_PLAIN, 0xE8B020u);
    box(m, 0.0f, 0.08f, 0.0405f, 0.19f, 0.11f, 0.002f, T_MAT_PLAIN, 0x2A2C30u);
    box(m, 0.05f, 0.08f, 0.042f, 0.032f, 0.032f, 0.004f, T_MAT_PLAIN, 0xD8282Au);
    box(m, -0.05f, 0.08f, 0.042f, 0.02f, 0.02f, 0.004f, T_MAT_PLAIN, 0x30B050u);
    box(m, 0.0f, 0.205f, 0.0f, 0.12f, 0.018f, 0.025f, T_MAT_PLAIN, 0x3A3C40u);
    for (int s = -1; s <= 1; s += 2)
        box(m, (float)s * 0.055f, 0.19f, 0.0f, 0.018f, 0.03f, 0.022f, T_MAT_PLAIN, 0x3A3C40u);
}

/* A steak with its rim of fat and a round of bone; cooked, it is browned
 * with the grill's marks across it. */
static void meat(item_model *m, int cooked)
{
    box(m, 0.0f, 0.016f, 0.0f, 0.15f, 0.032f, 0.10f, T_MAT_FLESH, cooked ? 0x7A4426u : 0xB0343Au);
    box(m, 0.0f, 0.016f, -0.055f, 0.15f, 0.03f, 0.012f, T_MAT_PLAIN, cooked ? 0xC08A50u : 0xF2E2D2u);
    box(m, 0.045f, 0.017f, 0.012f, 0.02f, 0.0335f, 0.02f, T_MAT_PLAIN, 0xEEE6D6u);
    if (cooked)
        for (int k = -1; k <= 1; k++)
            turn(box(m, 0.035f * (float)k, 0.0325f, 0.0f, 0.006f, 0.002f, 0.09f, T_MAT_PLAIN, 0x2E1A0Eu), 0.0f, 1.0f,
                 0.0f, 0.6f);
}

/* A rifle cartridge, 7 cm: brass case (an octagon of two boxes), rim and
 * a copper-jacketed bullet. */
static void cartridge(item_model *m)
{
    box(m, 0.0f, 0.024f, 0.0f, 0.012f, 0.042f, 0.012f, T_MAT_METAL, 0xD4AE5Cu);
    turn(box(m, 0.0f, 0.024f, 0.0f, 0.012f, 0.042f, 0.012f, T_MAT_METAL, 0xC49E4Cu), 0.0f, 1.0f, 0.0f, 0.785f);
    box(m, 0.0f, 0.0015f, 0.0f, 0.014f, 0.003f, 0.014f, T_MAT_METAL, 0xB08A40u);
    box(m, 0.0f, 0.054f, 0.0f, 0.009f, 0.018f, 0.009f, T_MAT_METAL, 0xB46A3Au);
    turn(box(m, 0.0f, 0.066f, 0.0f, 0.006f, 0.008f, 0.006f, T_MAT_METAL, 0xC47A4Au), 0.0f, 1.0f, 0.0f, 0.785f);
}

/* A bolt-action rifle, 1.1 m, muzzle up y and its top toward -x: stock,
 * wrist and fore-end in walnut, receiver, barrel, the bolt's handle,
 * trigger guard, magazine and iron sights (viewmodel.c aims down them). */
static void rifle(item_model *m)
{
    m->hold = IM_HOLD_GUN;
    turn(box(m, 0.03f, 0.14f, 0.0f, 0.10f, 0.28f, 0.04f, T_MAT_WOOD, WALNUT), 0.0f, 0.0f, 1.0f, -0.08f);
    box(m, 0.0f, 0.33f, 0.0f, 0.05f, 0.12f, 0.035f, T_MAT_WOOD, WALNUT);
    box(m, -0.005f, 0.66f, 0.0f, 0.045f, 0.52f, 0.038f, T_MAT_WOOD, darker(WALNUT) + 0x281810u);
    box(m, -0.02f, 0.44f, 0.0f, 0.04f, 0.15f, 0.03f, T_MAT_METAL, GUNMETAL);
    box(m, -0.034f, 0.80f, 0.0f, 0.018f, 0.62f, 0.018f, T_MAT_METAL, GUNMETAL);
    box(m, -0.02f, 0.40f, 0.03f, 0.012f, 0.012f, 0.05f, T_MAT_METAL, GUNMETAL);
    box(m, -0.02f, 0.40f, 0.058f, 0.018f, 0.018f, 0.018f, T_MAT_METAL, 0x5A5C60u);
    box(m, 0.045f, 0.37f, 0.0f, 0.01f, 0.07f, 0.012f, T_MAT_METAL, GUNMETAL);
    box(m, 0.034f, 0.375f, 0.0f, 0.014f, 0.006f, 0.006f, T_MAT_METAL, 0x5A5C60u);
    box(m, 0.035f, 0.46f, 0.0f, 0.022f, 0.08f, 0.03f, T_MAT_METAL, GUNMETAL);
    box(m, -0.048f, 1.09f, 0.0f, 0.012f, 0.012f, 0.006f, T_MAT_METAL, GUNMETAL);
    /* The rear sight is a notch, two posts 9 mm apart: down the sights the
     * front post stands in the gap. */
    for (int s = -1; s <= 1; s += 2)
        box(m, -0.047f, 0.53f, (float)s * 0.0075f, 0.012f, 0.02f, 0.006f, T_MAT_METAL, GUNMETAL);
    m->grip[0] = 0.0f;
    m->grip[1] = 0.34f;
}

/* An auto-injector pen: the needle end under an orange cap, a label, a
 * window onto the dose and the blue safety release. */
static void injector(item_model *m)
{
    m->hold = IM_HOLD_TOOL;
    box(m, 0.0f, 0.075f, 0.0f, 0.02f, 0.11f, 0.02f, T_MAT_PLAIN, 0xF0F0EAu);
    box(m, 0.0f, 0.07f, 0.0f, 0.021f, 0.05f, 0.021f, T_MAT_PLAIN, 0xF0B020u);
    box(m, 0.0f, 0.105f, 0.0f, 0.021f, 0.016f, 0.021f, T_MAT_GLASS, 0xE8E0B0u);
    box(m, 0.0f, 0.14f, 0.0f, 0.022f, 0.022f, 0.022f, T_MAT_PLAIN, 0xE86020u);
    box(m, 0.0f, 0.009f, 0.0f, 0.016f, 0.018f, 0.016f, T_MAT_PLAIN, 0x2A5AC0u);
    m->grip[1] = 0.07f;
}

/* A nasal spray: the plunger body with its colour band, finger flanges
 * and the nozzle. */
static void spray(item_model *m)
{
    box(m, 0.0f, 0.02f, 0.0f, 0.032f, 0.04f, 0.02f, T_MAT_PLAIN, 0xF4F4F0u);
    box(m, 0.0f, 0.02f, 0.0f, 0.033f, 0.012f, 0.021f, T_MAT_PLAIN, 0xE0602Au);
    box(m, 0.0f, 0.044f, 0.0f, 0.05f, 0.006f, 0.022f, T_MAT_PLAIN, 0xE8E8E4u);
    box(m, 0.0f, 0.065f, 0.0f, 0.012f, 0.036f, 0.012f, T_MAT_PLAIN, 0xF4F4F0u);
    box(m, 0.0f, 0.085f, 0.0f, 0.007f, 0.006f, 0.007f, T_MAT_PLAIN, 0xE8E8E4u);
}

/* Charcoal: split lengths of wood, burnt black, still showing the grain. */
static void charcoal(item_model *m)
{
    turn(box(m, 0.0f, 0.016f, 0.0f, 0.03f, 0.032f, 0.10f, T_MAT_WOOD, 0x2C2826u), 0.0f, 1.0f, 0.0f, 0.3f);
    turn(box(m, 0.03f, 0.014f, 0.01f, 0.026f, 0.028f, 0.08f, T_MAT_WOOD, 0x34302Cu), 0.0f, 1.0f, 0.0f, -0.4f);
    turn(box(m, 0.012f, 0.04f, 0.0f, 0.024f, 0.022f, 0.07f, T_MAT_WOOD, 0x242120u), 0.0f, 1.0f, 0.0f, 1.2f);
}

/* The torch block's item: the torch model, still. */
static void torch(item_model *m)
{
    entity_instance t[TORCH_PARTS] = {0};
    const float zero[3] = {0.0f, 0.0f, 0.0f};
    int k = torch_model(zero, quat_identity(), 0.5f, 0.0, 7u, 1.0f, t);
    for (int i = 0; i < k && i < 6; i++) { /* the sparks are left out */
        im_box *b = box(m, t[i].pos[0], t[i].pos[1], t[i].pos[2], t[i].scale[0], t[i].scale[1], t[i].scale[2],
                        (uint8_t)(t[i].tex & 255u), 0xFFFFFFu);
        b->rot = (quat){t[i].rot[0], t[i].rot[1], t[i].rot[2], t[i].rot[3]};
        b->glow = t[i].light >= 1.0f;
    }
    m->hold = IM_HOLD_TOOL;
    m->grip[1] = 0.06f;
}

/* ------------------------------------------------------------ build */

static const bottle_art ANTISEPTIC = {0.09f,     0.05f,     0.022f,      0x8A4A1Au, 0x5A1A08u,
                                      0xF0ECE0u, 0xF4F4F4u, T_MAT_PLAIN, 0.8f};
static const bottle_art ANTIBIOTIC = {0.06f,     0.04f,     0.006f,      0xE07A20u, 0xD84A3Au,
                                      0xF4F2EAu, 0xF4F4F4u, T_MAT_PLAIN, 0.6f};
static const bottle_art SPIRITS = {0.15f, 0.072f, 0.06f, 0xE8F0ECu, 0xC07A2Au, 0xE8DCC0u, 0xC4A070u, T_MAT_WOOD, 0.75f};
/* A glass ampoule: no label, a blue ring at the neck where it snaps. */
static const bottle_art MORPHINE = {0.035f, 0.014f, 0.018f, 0xF2F4F6u, 0xE4E6EEu, 0, 0x3A5AC0u, T_MAT_PLAIN, 0.7f};

static void finish(item_model *m)
{
    for (int a = 0; a < 3; a++) {
        m->lo[a] = 1e9f;
        m->hi[a] = -1e9f;
    }
    for (int i = 0; i < m->n; i++) {
        const im_box *b = &m->box[i];
        for (int c = 0; c < 8; c++) {
            const float h[3] = {(c & 1 ? 0.5f : -0.5f) * b->size[0], (c & 2 ? 0.5f : -0.5f) * b->size[1],
                                (c & 4 ? 0.5f : -0.5f) * b->size[2]};
            float p[3] = {0.0f, 0.0f, 0.0f};
            quat_apply(b->rot, h, p);
            for (int a = 0; a < 3; a++) {
                m->lo[a] = fminf(m->lo[a], b->pos[a] + p[a]);
                m->hi[a] = fmaxf(m->hi[a], b->pos[a] + p[a]);
            }
        }
    }
    if (m->hold == IM_HOLD_PALM) itemmodel_centre(m, m->grip);
}

int itemmodel_build(item_id id, item_model *m)
{
    memset(m, 0, sizeof *m);
    int kind = 0, tier = 0;
    if (id == B_TORCH) torch(m);
    else if (item_tool(id, &kind, &tier)) tool(m, kind, tier);
    else if (id >= I_TIN_INGOT && id <= I_STEEL_INGOT) {
        static const uint32_t METAL[] = {0xC8CCD0u, 0xA9B3B8u, 0x6E7378u, 0xC4C0B0u, 0xE0E2E6u,
                                         0xE8B840u, 0xB07A3Au, 0xD4AE5Cu, 0xB4BAC0u};
        _Static_assert(sizeof METAL / sizeof METAL[0] == I_STEEL_INGOT - I_TIN_INGOT + 1, "a colour per ingot");
        uint32_t i = id - I_TIN_INGOT;
        /* Zinc and lead stay dull and grainy as cast; the rest take a polish. */
        ingot(m, METAL[i], id == I_ZINC_INGOT || id == I_LEAD_INGOT ? T_MAT_CAST : T_MAT_METAL);
    } else {
        for (size_t i = 0; i < sizeof MINERAL_ART / sizeof MINERAL_ART[0]; i++)
            if (MINERAL_ART[i].id == id) mineral(m, &MINERAL_ART[i]);
        switch (id) {
        case I_STICK: stick(m); break;
        case I_FIBRE: fibre(m); break;
        case I_BANDAGE: bandage(m); break;
        case I_SPLINT: splint(m); break;
        case I_ANTISEPTIC: bottle(m, &ANTISEPTIC); break;
        case I_PAINKILLER: blister(m); break;
        case I_ANTIBIOTIC: bottle(m, &ANTIBIOTIC); break;
        case I_APPLE: apple(m); break;
        case I_BUCKET: bucket(m, 0); break;
        case I_WATER_BUCKET: bucket(m, T_WATER); break;
        case I_OIL_BUCKET: bucket(m, T_CRUDE_OIL); break;
        case I_BRINE_BUCKET: bucket(m, T_BRINE); break;
        case I_DEFIBRILLATOR: defibrillator(m); break;
        case I_COPPER_INGOT: ingot(m, 0xC8784Au, T_MAT_METAL); break;
        case I_IRON_INGOT: ingot(m, 0x8A8D90u, T_MAT_CAST); break;
        case I_RAW_MEAT: meat(m, 0); break;
        case I_COOKED_MEAT: meat(m, 1); break;
        case I_GUNPOWDER: heap(m, 0x3A3A3Eu); break;
        case I_CARTRIDGE: cartridge(m); break;
        case I_RIFLE: rifle(m); break;
        case I_ADRENALINE: injector(m); break;
        case I_MORPHINE: bottle(m, &MORPHINE); break;
        case I_NALOXONE: spray(m); break;
        case I_SPIRITS: bottle(m, &SPIRITS); break;
        case I_CHARCOAL: charcoal(m); break;
        default: break;
        }
    }
    if (m->n == 0) return 0;
    finish(m);
    return 1;
}

void itemmodel_centre(const item_model *m, float out[3])
{
    for (int a = 0; a < 3; a++) out[a] = (m->lo[a] + m->hi[a]) * 0.5f;
}

float itemmodel_extent(const item_model *m)
{ return fmaxf(m->hi[0] - m->lo[0], fmaxf(m->hi[1] - m->lo[1], m->hi[2] - m->lo[2])); }

int itemmodel_place(const item_model *m, const float origin[3], const float at[3], quat rot, float scale, float light,
                    entity_instance *out, int max)
{
    int n = 0;
    for (int i = 0; i < m->n && n < max; i++) {
        const im_box *b = &m->box[i];
        const float local[3] = {(b->pos[0] - origin[0]) * scale, (b->pos[1] - origin[1]) * scale,
                                (b->pos[2] - origin[2]) * scale};
        float r[3] = {0.0f, 0.0f, 0.0f};
        quat_apply(rot, local, r);
        entity_instance e = {0};
        for (int a = 0; a < 3; a++) {
            e.pos[a] = at[a] + r[a];
            e.scale[a] = b->size[a] * scale;
        }
        entity_set_rot(&e, quat_norm(quat_mul(rot, b->rot)));
        e.tex = (uint32_t)b->layer * 0x010101u;
        e.tint = entity_tint(b->rgb);
        e.light = b->glow ? 1.0f : light;
        out[n++] = e;
    }
    return n;
}

int itemmodel_layer_clear(uint8_t layer)
{
    return layer == T_MAT_GLASS || layer == T_WATER || layer == T_CRUDE_OIL || layer == T_BRINE ||
           (layer >= T_FIRE0 && layer <= T_FIRE3);
}

/* ------------------------------------------------------------- icon */

/* The average colour of a layer's texels, for flat-shaded icons. */
static void layer_mean(uint8_t layer, float out[4])
{
    float s[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            uint8_t t[4] = {0};
            texgen_texel(layer, x, y, t);
            for (int c = 0; c < 4; c++) s[c] += (float)t[c];
        }
    for (int c = 0; c < 4; c++) out[c] = s[c] / (256.0f * 255.0f);
}

typedef struct {
    float depth;
    im_quad q;
} face_sort;

int itemmodel_icon(const item_model *m, im_quad *out, int max)
{
    /* Tools and guns lie diagonal, handle to the lower left, turned a
     * little to show their depth; anything else is seen from above its
     * front right corner. */
    int diag = m->hold != IM_HOLD_PALM;
    quat view = diag ? quat_mul(quat_axis(1.0f, 0.0f, 0.0f, 0.3f),
                                quat_mul(quat_axis(0.0f, 1.0f, 0.0f, -0.35f), quat_axis(0.0f, 0.0f, 1.0f, -0.785f)))
                     : quat_mul(quat_axis(1.0f, 0.0f, 0.0f, 0.5f), quat_axis(0.0f, 1.0f, 0.0f, -0.6f));
    /* Light from the upper left, in front. */
    const float lx = -0.36f, ly = 0.74f, lz = 0.57f;
    face_sort f[IM_ICON_QUADS] = {0};
    int nf = 0;
    float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
    float centre[3] = {0.0f, 0.0f, 0.0f};
    itemmodel_centre(m, centre);
    for (int i = 0; i < m->n; i++) {
        const im_box *b = &m->box[i];
        quat q = quat_mul(view, b->rot);
        const float rel[3] = {b->pos[0] - centre[0], b->pos[1] - centre[1], b->pos[2] - centre[2]};
        float c[3] = {0.0f, 0.0f, 0.0f};
        quat_apply(view, rel, c);
        float mean[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        layer_mean(b->layer, mean);
        for (int axis = 0; axis < 3; axis++)
            for (int sgn = -1; sgn <= 1; sgn += 2) {
                float nl[3] = {0.0f, 0.0f, 0.0f}, n[3] = {0.0f, 0.0f, 0.0f};
                nl[axis] = (float)sgn;
                quat_apply(q, nl, n);
                if (n[2] <= 1e-4f || nf >= IM_ICON_QUADS) continue; /* faces away */
                int u = (axis + 1) % 3, v = (axis + 2) % 3;
                static const float CU[4] = {-0.5f, 0.5f, 0.5f, -0.5f}, CV[4] = {-0.5f, -0.5f, 0.5f, 0.5f};
                face_sort *fs = &f[nf++];
                float z = 0.0f;
                for (int k = 0; k < 4; k++) {
                    float h[3] = {0.0f, 0.0f, 0.0f}, p[3] = {0.0f, 0.0f, 0.0f};
                    h[axis] = 0.5f * (float)sgn * b->size[axis];
                    h[u] = CU[k] * b->size[u];
                    h[v] = CV[k] * b->size[v];
                    quat_apply(q, h, p);
                    fs->q.xy[2 * k] = c[0] + p[0];
                    fs->q.xy[2 * k + 1] = -(c[1] + p[1]);
                    z += c[2] + p[2];
                }
                fs->depth = z * 0.25f;
                float lit = 0.5f + 0.6f * fmaxf(0.0f, n[0] * lx + n[1] * ly + n[2] * lz);
                if (b->glow) lit = 1.15f;
                int rgb[3] = {0, 0, 0};
                for (int ch = 0; ch < 3; ch++) {
                    float tint = (float)(b->rgb >> (16 - 8 * ch) & 255u) / 255.0f;
                    float val = mean[ch] * tint * lit * 255.0f;
                    rgb[ch] = (int)(val > 255.0f ? 255.0f : val);
                }
                fs->q.rgba = ui_rgba(rgb[0], rgb[1], rgb[2], (int)(mean[3] * 255.0f));
                for (int k = 0; k < 4; k++)
                    for (int a = 0; a < 2; a++) {
                        lo[a] = fminf(lo[a], fs->q.xy[2 * k + a]);
                        hi[a] = fmaxf(hi[a], fs->q.xy[2 * k + a]);
                    }
            }
    }
    /* Far faces first; then fit the whole into the square with a margin. */
    for (int i = 1; i < nf; i++)
        for (int j = i; j > 0 && f[j - 1].depth > f[j].depth; j--) {
            face_sort t = f[j];
            f[j] = f[j - 1];
            f[j - 1] = t;
        }
    float side = fmaxf(hi[0] - lo[0], hi[1] - lo[1]);
    float s = side > 1e-6f ? 0.92f / side : 1.0f;
    float ox = 0.5f - (lo[0] + hi[0]) * 0.5f * s, oy = 0.5f - (lo[1] + hi[1]) * 0.5f * s;
    int n = 0;
    for (int i = 0; i < nf && n < max; i++) {
        im_quad q = f[i].q;
        for (int k = 0; k < 4; k++) {
            q.xy[2 * k] = q.xy[2 * k] * s + ox;
            q.xy[2 * k + 1] = q.xy[2 * k + 1] * s + oy;
        }
        /* Clockwise on screen (y down), as ui_quad takes it. */
        float area = 0.0f;
        for (int k = 0; k < 4; k++) {
            int j = (k + 1) % 4;
            area += q.xy[2 * k] * q.xy[2 * j + 1] - q.xy[2 * j] * q.xy[2 * k + 1];
        }
        if (area < 0.0f) {
            im_quad r = q;
            for (int k = 0; k < 4; k++) {
                r.xy[2 * k] = q.xy[2 * (3 - k)];
                r.xy[2 * k + 1] = q.xy[2 * (3 - k) + 1];
            }
            q = r;
        }
        out[n++] = q;
    }
    return n;
}
