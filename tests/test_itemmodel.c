/* Item models: every item has one, at a plausible real size, and the
 * instances and icons built from it stay where they belong. */
#include "test_util.h"

#include "fx.h"
#include "invui.h"
#include "itemmodel.h"
#include "mem.h"
#include "texgen.h"
#include "ui.h"
#include "viewmodel.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Longest side of each item in metres, from the real thing: a 0.8 m pick,
 * a 10 cm bar ingot, a 7 cm cartridge, a 26 cm bucket, a 1.1 m rifle. */
typedef struct {
    item_id id;
    float lo, hi;
} size_case;

static const size_case SIZES[] = {
    {I_STICK, 0.35f, 0.5f},         {I_FIBRE, 0.2f, 0.32f},         {I_BANDAGE, 0.06f, 0.15f},
    {I_SPLINT, 0.25f, 0.4f},        {I_ANTISEPTIC, 0.08f, 0.16f},   {I_PAINKILLER, 0.06f, 0.14f},
    {I_ANTIBIOTIC, 0.05f, 0.1f},    {I_APPLE, 0.06f, 0.12f},        {I_BUCKET, 0.25f, 0.45f},
    {I_WATER_BUCKET, 0.25f, 0.45f}, {I_OIL_BUCKET, 0.25f, 0.45f},   {I_BRINE_BUCKET, 0.25f, 0.45f},
    {I_DEFIBRILLATOR, 0.2f, 0.32f}, {I_COAL, 0.05f, 0.13f},         {I_COPPER_INGOT, 0.09f, 0.13f},
    {I_IRON_INGOT, 0.09f, 0.13f},   {I_RAW_COPPER, 0.05f, 0.13f},   {I_RAW_IRON, 0.05f, 0.13f},
    {I_RAW_MEAT, 0.12f, 0.2f},      {I_COOKED_MEAT, 0.12f, 0.2f},   {I_GUNPOWDER, 0.06f, 0.12f},
    {I_CARTRIDGE, 0.05f, 0.09f},    {I_RIFLE, 0.9f, 1.25f},         {I_ADRENALINE, 0.12f, 0.2f},
    {I_MORPHINE, 0.04f, 0.09f},     {I_NALOXONE, 0.06f, 0.11f},     {I_SPIRITS, 0.18f, 0.32f},
    {I_RAW_TIN, 0.05f, 0.13f},      {I_RAW_ZINC, 0.05f, 0.13f},     {I_RAW_LEAD, 0.05f, 0.13f},
    {I_RAW_NICKEL, 0.05f, 0.13f},   {I_RAW_SILVER, 0.05f, 0.13f},   {I_RAW_GOLD, 0.05f, 0.13f},
    {I_BAUXITE, 0.05f, 0.13f},      {I_ILMENITE, 0.05f, 0.13f},     {I_URANINITE, 0.05f, 0.13f},
    {I_SPODUMENE, 0.06f, 0.12f},    {I_SULFUR, 0.06f, 0.12f},       {I_SALTPETER, 0.05f, 0.13f},
    {I_SALT, 0.05f, 0.1f},          {I_QUARTZ, 0.06f, 0.12f},       {I_GRAPHITE, 0.05f, 0.1f},
    {I_TIN_INGOT, 0.09f, 0.13f},    {I_ZINC_INGOT, 0.09f, 0.13f},   {I_LEAD_INGOT, 0.09f, 0.13f},
    {I_NICKEL_INGOT, 0.09f, 0.13f}, {I_SILVER_INGOT, 0.09f, 0.13f}, {I_GOLD_INGOT, 0.09f, 0.13f},
    {I_BRONZE_INGOT, 0.09f, 0.13f}, {I_BRASS_INGOT, 0.09f, 0.13f},  {I_STEEL_INGOT, 0.09f, 0.13f},
    {I_CHARCOAL, 0.07f, 0.14f},     {I_QUICKLIME, 0.05f, 0.13f},
};

/* Each tool kind's longest side: pick, axe, shovel, knife, sword. */
static const float TOOL_LO[TOOL_KINDS] = {0.7f, 0.6f, 0.85f, 0.2f, 0.8f};
static const float TOOL_HI[TOOL_KINDS] = {0.95f, 0.85f, 1.1f, 0.3f, 1.0f};

static int finite3(const float v[3]) { return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]); }

static void test_every_item(void)
{
    /* Every item past the blocks has a model inside the box budget, with
     * sane boxes and a grip inside its bounds. */
    for (item_id id = ITEM_FIRST; id < ITEM_END; id++) {
        item_model m = {0};
        CHECK(itemmodel_build(id, &m) == 1);
        CHECK(m.n > 0 && m.n <= IM_MAX_BOXES);
        CHECK(m.hold == IM_HOLD_PALM || m.hold == IM_HOLD_TOOL || m.hold == IM_HOLD_GUN);
        for (int i = 0; i < m.n; i++) {
            const im_box *b = &m.box[i];
            CHECK(finite3(b->pos) && finite3(b->size));
            CHECK(b->size[0] > 0.0f && b->size[1] > 0.0f && b->size[2] > 0.0f);
            CHECK(fabsf(b->rot.x * b->rot.x + b->rot.y * b->rot.y + b->rot.z * b->rot.z + b->rot.w * b->rot.w - 1.0f) <
                  1e-3f);
            CHECK(b->layer < T_COUNT);
            CHECK(b->rgb <= 0xFFFFFFu);
        }
        for (int a = 0; a < 3; a++) {
            CHECK(m.lo[a] < m.hi[a]);
            CHECK(m.grip[a] >= m.lo[a] - 1e-4f && m.grip[a] <= m.hi[a] + 1e-4f);
        }
        CHECK(itemmodel_extent(&m) <= 1.3f);
    }
    /* Blocks are drawn as blocks, all but the torch. */
    item_model m = {0};
    CHECK(itemmodel_build(B_STONE, &m) == 0 && m.n == 0);
    CHECK(itemmodel_build(B_TORCH, &m) == 1 && m.hold == IM_HOLD_TOOL);
    int glow = 0;
    for (int i = 0; i < m.n; i++) glow += m.box[i].glow;
    CHECK(glow >= 1); /* the flame lights itself */
}

static void test_real_sizes(void)
{
    int covered = 0;
    for (size_t i = 0; i < sizeof SIZES / sizeof SIZES[0]; i++) {
        item_model m = {0};
        CHECK(itemmodel_build(SIZES[i].id, &m));
        float e = itemmodel_extent(&m);
        CHECK(e >= SIZES[i].lo && e <= SIZES[i].hi);
        covered++;
    }
    /* The table names every item but the tools, which follow. */
    CHECK(covered == (ITEM_END - ITEM_FIRST) - (I_TOOL_END - I_TOOL_FIRST));
    for (int kind = 0; kind < TOOL_KINDS; kind++)
        for (int tier = 0; tier < TIER_COUNT; tier++) {
            item_model m = {0};
            CHECK(itemmodel_build(item_tool_id(kind, tier), &m));
            CHECK(m.hold == IM_HOLD_TOOL);
            float e = itemmodel_extent(&m);
            CHECK(e >= TOOL_LO[kind] && e <= TOOL_HI[kind]);
            /* Held by the handle, in its lowest third. */
            CHECK(m.grip[1] - m.lo[1] < 0.34f * (m.hi[1] - m.lo[1]));
        }
}

/* The head's colour: the tint of the box that reaches highest. */
static uint32_t top_rgb(const item_model *m)
{
    int best = 0;
    for (int i = 1; i < m->n; i++)
        if (m->box[i].pos[1] > m->box[best].pos[1]) best = i;
    return m->box[best].rgb;
}

static void test_tiers(void)
{
    for (int kind = 0; kind < TOOL_KINDS; kind++) {
        item_model wood = {0}, iron = {0}, stone = {0}, copper = {0};
        itemmodel_build(item_tool_id(kind, TIER_WOOD), &wood);
        itemmodel_build(item_tool_id(kind, TIER_STONE), &stone);
        itemmodel_build(item_tool_id(kind, TIER_COPPER), &copper);
        itemmodel_build(item_tool_id(kind, TIER_IRON), &iron);
        /* Copper and iron share the shape and differ in the metal. */
        CHECK(copper.n == iron.n);
        for (int i = 0; i < iron.n; i++)
            for (int a = 0; a < 3; a++) CHECK(copper.box[i].size[a] == iron.box[i].size[a]);
        CHECK(top_rgb(&copper) != top_rgb(&iron));
        CHECK(top_rgb(&wood) != top_rgb(&iron));
        /* A stone head is lashed on: more boxes, some of them cloth. */
        CHECK(stone.n >= iron.n);
        int cloth = 0;
        for (int i = 0; i < stone.n; i++) cloth += stone.box[i].layer == T_MAT_CLOTH;
        CHECK(cloth >= 1);
        /* Metal heads use the metal layer, wooden ones the wood. */
        int metal = 0;
        for (int i = 0; i < iron.n; i++) metal += iron.box[i].layer == T_MAT_METAL;
        CHECK(metal >= 2);
        CHECK(wood.box[wood.n - 1].layer == T_MAT_WOOD || wood.box[wood.n - 1].layer == T_MAT_CLOTH);
    }
}

/* The layer of the bucket's liquid: the box at the middle that isn't wood
 * or fibre. */
static int bucket_liquid(item_id id)
{
    item_model m = {0};
    itemmodel_build(id, &m);
    for (int i = 0; i < m.n; i++)
        if (m.box[i].layer != T_MAT_WOOD && m.box[i].layer != T_MAT_CLOTH) return m.box[i].layer;
    return 0;
}

static void test_containers(void)
{
    CHECK(bucket_liquid(I_BUCKET) == 0);
    CHECK(bucket_liquid(I_WATER_BUCKET) == T_WATER);
    CHECK(bucket_liquid(I_OIL_BUCKET) == T_CRUDE_OIL);
    CHECK(bucket_liquid(I_BRINE_BUCKET) == T_BRINE);
    /* Liquids and glass blend; wood and metal don't. */
    CHECK(itemmodel_layer_clear(T_WATER) && itemmodel_layer_clear(T_MAT_GLASS));
    CHECK(itemmodel_layer_clear(T_CRUDE_OIL) && itemmodel_layer_clear(T_BRINE));
    CHECK(!itemmodel_layer_clear(T_MAT_WOOD) && !itemmodel_layer_clear(T_MAT_METAL));
    CHECK(!itemmodel_layer_clear(T_MAT_PLAIN));
    /* Bottles are glass, with something inside that shows through. */
    const item_id glass[] = {I_ANTISEPTIC, I_ANTIBIOTIC, I_SPIRITS, I_MORPHINE};
    for (size_t k = 0; k < sizeof glass / sizeof glass[0]; k++) {
        item_model m = {0};
        itemmodel_build(glass[k], &m);
        int g = 0, inner = -1;
        for (int i = 0; i < m.n; i++) {
            if (m.box[i].layer == T_MAT_GLASS) g++;
            else if (inner < 0) inner = i;
        }
        CHECK(g >= 2);
        CHECK(inner == 0); /* the contents come first, inside the glass */
    }
    /* The glass layer really is see-through, and the metal is opaque. */
    uint8_t t[4] = {0};
    texgen_texel(T_MAT_GLASS, 8, 8, t);
    CHECK(t[3] < 200);
    texgen_texel(T_MAT_METAL, 8, 8, t);
    CHECK(t[3] == 255);
}

/* Volume times density over the boxes, for models whose boxes don't
 * overlap, against the real mass: an iron bar ingot is near a kilogram. */
static float mass_of(item_id id, float density)
{
    item_model m = {0};
    itemmodel_build(id, &m);
    float v = 0.0f;
    for (int i = 0; i < m.n; i++) v += m.box[i].size[0] * m.box[i].size[1] * m.box[i].size[2];
    return v * density;
}

static void test_masses(void)
{
    float ingot = mass_of(I_IRON_INGOT, 7870.0f);
    CHECK(ingot > 0.6f && ingot < 1.5f);
    float gold = mass_of(I_GOLD_INGOT, 19300.0f);
    CHECK(gold > 1.5f && gold < 3.5f);
    /* A full bucket holds about ten litres. */
    item_model m = {0};
    itemmodel_build(I_WATER_BUCKET, &m);
    float litres = 0.0f;
    for (int i = 0; i < m.n; i++)
        if (m.box[i].layer == T_WATER) litres = m.box[i].size[0] * m.box[i].size[1] * m.box[i].size[2] * 1000.0f;
    CHECK(litres > 5.0f && litres < 12.0f);
}

static void test_layers_greyscale(void)
{
    /* The material layers carry no hue of their own: the tint does. */
    const uint8_t mats[] = {T_MAT_WOOD,  T_MAT_METAL, T_MAT_CAST,  T_MAT_ROCK, T_MAT_CLOTH,
                            T_MAT_GLASS, T_MAT_PLAIN, T_MAT_FLESH, T_MAT_GRAIN};
    for (size_t k = 0; k < sizeof mats; k++) {
        int light = 0;
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                uint8_t t[4] = {0};
                texgen_texel(mats[k], x, y, t);
                /* Glass alone keeps a faint cool cast. */
                int tol = mats[k] == T_MAT_GLASS ? 8 : 0;
                CHECK(abs(t[0] - t[1]) <= tol && abs(t[1] - t[2]) <= tol);
                light += t[0];
            }
        CHECK(light / 256 > 150); /* near white, so the tint keeps its colour */
    }
}

static void test_tint(void)
{
    /* Stored as white minus the colour, red in the low byte: zero is no tint,
     * which keeps every other entity untouched. */
    CHECK(entity_tint(0xFFFFFFu) == 0u);
    CHECK(entity_tint(0x000000u) == 0xFFFFFFu);
    CHECK(entity_tint(0xFF0000u) == 0xFFFF00u);
    CHECK(entity_tint(0x00FF00u) == 0xFF00FFu);
    CHECK(entity_tint(0x0000FFu) == 0x00FFFFu);
    CHECK(entity_tint(0x123456u) == ((0xFFu - 0x56u) << 16 | (0xFFu - 0x34u) << 8 | (0xFFu - 0x12u)));
}

static void test_place(void)
{
    item_model m = {0};
    CHECK(itemmodel_build(item_tool_id(TOOL_PICKAXE, TIER_IRON), &m));
    entity_instance e[IM_MAX_BOXES] = {0};
    const float at[3] = {1.0f, 2.0f, 3.0f};
    /* Unturned at scale one, each box lands at its place minus the origin. */
    int n = itemmodel_place(&m, m.grip, at, quat_identity(), 1.0f, 0.4f, e, IM_MAX_BOXES);
    CHECK(n == m.n);
    for (int i = 0; i < n; i++) {
        for (int a = 0; a < 3; a++) {
            CHECK(fabsf(e[i].pos[a] - (at[a] + m.box[i].pos[a] - m.grip[a])) < 1e-5f);
            CHECK(fabsf(e[i].scale[a] - m.box[i].size[a]) < 1e-6f);
        }
        CHECK(e[i].tex == (uint32_t)m.box[i].layer * 0x010101u);
        CHECK(e[i].tint == entity_tint(m.box[i].rgb));
        CHECK(e[i].light == 0.4f);
    }
    /* Half a turn about y and twice the size: x and z flip and double. */
    n = itemmodel_place(&m, m.grip, at, quat_axis(0.0f, 1.0f, 0.0f, 3.14159265f), 2.0f, 0.4f, e, IM_MAX_BOXES);
    for (int i = 0; i < n; i++) {
        CHECK(fabsf(e[i].pos[0] - (at[0] - 2.0f * (m.box[i].pos[0] - m.grip[0]))) < 1e-4f);
        CHECK(fabsf(e[i].pos[1] - (at[1] + 2.0f * (m.box[i].pos[1] - m.grip[1]))) < 1e-4f);
        CHECK(fabsf(e[i].pos[2] - (at[2] - 2.0f * (m.box[i].pos[2] - m.grip[2]))) < 1e-4f);
        CHECK(fabsf(e[i].scale[1] - 2.0f * m.box[i].size[1]) < 1e-5f);
    }
    /* It stops at max, and a glowing box ignores the light. */
    CHECK(itemmodel_place(&m, m.grip, at, quat_identity(), 1.0f, 0.4f, e, 3) == 3);
    CHECK(itemmodel_build(B_TORCH, &m));
    n = itemmodel_place(&m, m.grip, at, quat_identity(), 1.0f, 0.1f, e, IM_MAX_BOXES);
    int lit = 0;
    for (int i = 0; i < n; i++) lit += e[i].light == 1.0f;
    CHECK(lit >= 1);
}

static void test_icons(void)
{
    for (item_id id = ITEM_FIRST; id < ITEM_END; id++) {
        item_model m = {0};
        itemmodel_build(id, &m);
        im_quad q[IM_ICON_QUADS] = {0}, again[IM_ICON_QUADS] = {0};
        int n = itemmodel_icon(&m, q, IM_ICON_QUADS);
        /* At least the faces of one box, and no more than three per box. */
        CHECK(n >= 3 && n <= 3 * m.n);
        float lo[2] = {1.0f, 1.0f}, hi[2] = {0.0f, 0.0f};
        for (int i = 0; i < n; i++) {
            float area = 0.0f;
            for (int k = 0; k < 4; k++) {
                int j = (k + 1) % 4;
                for (int a = 0; a < 2; a++) {
                    CHECK(q[i].xy[2 * k + a] >= -1e-4f && q[i].xy[2 * k + a] <= 1.0001f);
                    lo[a] = fminf(lo[a], q[i].xy[2 * k + a]);
                    hi[a] = fmaxf(hi[a], q[i].xy[2 * k + a]);
                }
                area += q[i].xy[2 * k] * q[i].xy[2 * j + 1] - q[i].xy[2 * j] * q[i].xy[2 * k + 1];
            }
            CHECK(area >= 0.0f); /* clockwise with y down */
            CHECK((q[i].rgba >> 24) > 0);
        }
        /* Fitted: the longer side fills the square bar the margin, centred. */
        float side = fmaxf(hi[0] - lo[0], hi[1] - lo[1]);
        CHECK(fabsf(side - 0.92f) < 0.01f);
        CHECK(fabsf((lo[0] + hi[0]) * 0.5f - 0.5f) < 0.01f && fabsf((lo[1] + hi[1]) * 0.5f - 0.5f) < 0.01f);
        /* The same every time, for the icon cache and the tests. */
        CHECK(itemmodel_icon(&m, again, IM_ICON_QUADS) == n);
        CHECK(memcmp(q, again, (size_t)n * sizeof q[0]) == 0);
        CHECK(itemmodel_icon(&m, again, 2) == 2);
    }
}

static void test_icon_lighting(void)
{
    /* A plain white cube-ish ingot: its top is the brightest face, lit from
     * above, and the colour follows the tint. */
    item_model m = {0};
    itemmodel_build(I_SILVER_INGOT, &m);
    im_quad q[IM_ICON_QUADS] = {0};
    int n = itemmodel_icon(&m, q, IM_ICON_QUADS);
    int bright = 0, dark = 255;
    for (int i = 0; i < n; i++) {
        int r = (int)(q[i].rgba & 255u);
        bright = r > bright ? r : bright;
        dark = r < dark ? r : dark;
    }
    CHECK(bright > dark); /* faces are shaded differently */
    itemmodel_build(I_GOLD_INGOT, &m);
    n = itemmodel_icon(&m, q, IM_ICON_QUADS);
    for (int i = 0; i < n; i++) CHECK((q[i].rgba & 255u) > (q[i].rgba >> 16 & 255u)); /* gold: red over blue */
}

/* ------------------------------------------------- what each one is */

/* Each item's hold and two materials it must show: what the thing is
 * made of, so a wooden bucket can't turn into a metal one unnoticed. */
typedef struct {
    item_id id;
    int hold;
    uint8_t a, b; /* layers that appear; b may be 0 for none */
} feature_case;

static const feature_case FEATURES[] = {
    {I_STICK, IM_HOLD_TOOL, T_MAT_WOOD, 0},
    {I_FIBRE, IM_HOLD_TOOL, T_MAT_PLAIN, T_MAT_CLOTH},
    {I_BANDAGE, IM_HOLD_PALM, T_MAT_CLOTH, 0},
    {I_SPLINT, IM_HOLD_TOOL, T_MAT_WOOD, T_MAT_CLOTH},
    {I_ANTISEPTIC, IM_HOLD_PALM, T_MAT_GLASS, T_MAT_PLAIN},
    {I_PAINKILLER, IM_HOLD_PALM, T_MAT_METAL, T_MAT_PLAIN},
    {I_ANTIBIOTIC, IM_HOLD_PALM, T_MAT_GLASS, T_MAT_PLAIN},
    {I_APPLE, IM_HOLD_PALM, T_MAT_PLAIN, T_MAT_WOOD},
    {I_BUCKET, IM_HOLD_PALM, T_MAT_WOOD, T_MAT_CLOTH},
    {I_WATER_BUCKET, IM_HOLD_PALM, T_WATER, T_MAT_WOOD},
    {I_OIL_BUCKET, IM_HOLD_PALM, T_CRUDE_OIL, T_MAT_WOOD},
    {I_BRINE_BUCKET, IM_HOLD_PALM, T_BRINE, T_MAT_WOOD},
    {I_DEFIBRILLATOR, IM_HOLD_PALM, T_MAT_PLAIN, 0},
    {I_COAL, IM_HOLD_PALM, T_MAT_ROCK, 0},
    {I_COPPER_INGOT, IM_HOLD_PALM, T_MAT_METAL, 0},
    {I_IRON_INGOT, IM_HOLD_PALM, T_MAT_CAST, 0},
    {I_RAW_COPPER, IM_HOLD_PALM, T_MAT_ROCK, 0},
    {I_RAW_IRON, IM_HOLD_PALM, T_MAT_ROCK, T_MAT_METAL},
    {I_RAW_MEAT, IM_HOLD_PALM, T_MAT_FLESH, T_MAT_PLAIN},
    {I_COOKED_MEAT, IM_HOLD_PALM, T_MAT_FLESH, T_MAT_PLAIN},
    {I_GUNPOWDER, IM_HOLD_PALM, T_MAT_GRAIN, 0},
    {I_CARTRIDGE, IM_HOLD_PALM, T_MAT_METAL, 0},
    {I_RIFLE, IM_HOLD_GUN, T_MAT_WOOD, T_MAT_METAL},
    {I_ADRENALINE, IM_HOLD_TOOL, T_MAT_PLAIN, T_MAT_GLASS},
    {I_MORPHINE, IM_HOLD_PALM, T_MAT_GLASS, T_MAT_PLAIN},
    {I_NALOXONE, IM_HOLD_PALM, T_MAT_PLAIN, 0},
    {I_SPIRITS, IM_HOLD_PALM, T_MAT_GLASS, T_MAT_WOOD},
    {I_RAW_TIN, IM_HOLD_PALM, T_MAT_ROCK, T_MAT_METAL},
    {I_RAW_ZINC, IM_HOLD_PALM, T_MAT_ROCK, T_MAT_METAL},
    {I_RAW_LEAD, IM_HOLD_PALM, T_MAT_ROCK, T_MAT_METAL},
    {I_RAW_NICKEL, IM_HOLD_PALM, T_MAT_ROCK, T_MAT_METAL},
    {I_RAW_SILVER, IM_HOLD_PALM, T_MAT_ROCK, T_MAT_METAL},
    {I_RAW_GOLD, IM_HOLD_PALM, T_MAT_ROCK, T_MAT_METAL},
    {I_BAUXITE, IM_HOLD_PALM, T_MAT_ROCK, 0},
    {I_ILMENITE, IM_HOLD_PALM, T_MAT_ROCK, T_MAT_METAL},
    {I_URANINITE, IM_HOLD_PALM, T_MAT_ROCK, T_MAT_GRAIN},
    {I_SPODUMENE, IM_HOLD_PALM, T_MAT_ROCK, T_MAT_PLAIN},
    {I_SULFUR, IM_HOLD_PALM, T_MAT_ROCK, T_MAT_PLAIN},
    {I_SALTPETER, IM_HOLD_PALM, T_MAT_ROCK, T_MAT_GRAIN},
    {I_SALT, IM_HOLD_PALM, T_MAT_PLAIN, 0},
    {I_QUARTZ, IM_HOLD_PALM, T_MAT_ROCK, T_MAT_GLASS},
    {I_GRAPHITE, IM_HOLD_PALM, T_MAT_METAL, 0},
    {I_TIN_INGOT, IM_HOLD_PALM, T_MAT_METAL, 0},
    {I_ZINC_INGOT, IM_HOLD_PALM, T_MAT_CAST, 0},
    {I_LEAD_INGOT, IM_HOLD_PALM, T_MAT_CAST, 0},
    {I_NICKEL_INGOT, IM_HOLD_PALM, T_MAT_METAL, 0},
    {I_SILVER_INGOT, IM_HOLD_PALM, T_MAT_METAL, 0},
    {I_GOLD_INGOT, IM_HOLD_PALM, T_MAT_METAL, 0},
    {I_BRONZE_INGOT, IM_HOLD_PALM, T_MAT_METAL, 0},
    {I_BRASS_INGOT, IM_HOLD_PALM, T_MAT_METAL, 0},
    {I_STEEL_INGOT, IM_HOLD_PALM, T_MAT_METAL, 0},
    {I_CHARCOAL, IM_HOLD_PALM, T_MAT_WOOD, 0},
    {I_QUICKLIME, IM_HOLD_PALM, T_MAT_ROCK, T_MAT_GRAIN},
};

static int has_layer(const item_model *m, uint8_t layer)
{
    for (int i = 0; i < m->n; i++)
        if (m->box[i].layer == layer) return 1;
    return 0;
}

static void test_features(void)
{
    for (size_t i = 0; i < sizeof FEATURES / sizeof FEATURES[0]; i++) {
        const feature_case *f = &FEATURES[i];
        item_model m = {0};
        CHECK(itemmodel_build(f->id, &m));
        if (m.hold != f->hold || !has_layer(&m, f->a) || (f->b && !has_layer(&m, f->b)))
            fprintf(stderr, "  %s is not what it should be\n", item_get(f->id)->name);
        CHECK(m.hold == f->hold);
        CHECK(has_layer(&m, f->a));
        CHECK(!f->b || has_layer(&m, f->b));
    }
    /* With the tools, the table covers every item. */
    CHECK(sizeof FEATURES / sizeof FEATURES[0] + (I_TOOL_END - I_TOOL_FIRST) == (size_t)(ITEM_END - ITEM_FIRST));
}

/* ------------------------------------------------------- minerals */

/* A colour some box of the item must have (any = 1), or that every box
 * keeps within (any = 0): gold is yellow, malachite green on copper ore,
 * coal black, salt and quicklime white. */
typedef struct {
    item_id id;
    int any;
    uint8_t lo[3], hi[3];
} colour_case;

/* clang-format off */
static const colour_case COLOURS[] = {
    {I_RAW_GOLD, 1, {200, 150, 0}, {255, 220, 100}},     /* native gold */
    {I_RAW_GOLD, 1, {200, 200, 190}, {255, 255, 255}},   /* in white quartz */
    {I_RAW_COPPER, 1, {0, 100, 0}, {100, 200, 150}},     /* malachite green */
    {I_RAW_IRON, 1, {100, 30, 20}, {160, 90, 70}},       /* rusty haematite */
    {I_RAW_TIN, 1, {30, 20, 10}, {80, 60, 50}},          /* black-brown cassiterite */
    {I_RAW_LEAD, 1, {120, 120, 130}, {160, 160, 170}},   /* grey galena */
    {I_RAW_SILVER, 1, {200, 200, 200}, {255, 255, 255}}, /* bright native silver */
    {I_BAUXITE, 1, {150, 70, 40}, {230, 160, 110}},      /* red-brown */
    {I_URANINITE, 1, {0, 0, 0}, {50, 50, 50}},           /* pitch-black pitchblende */
    {I_URANINITE, 1, {180, 180, 0}, {240, 230, 100}},    /* yellow alteration */
    {I_SPODUMENE, 1, {180, 160, 200}, {230, 220, 240}},  /* pale lilac */
    {I_SULFUR, 1, {220, 200, 0}, {255, 240, 80}},        /* yellow */
    {I_SALT, 0, {200, 190, 180}, {255, 255, 255}},       /* white throughout */
    {I_QUICKLIME, 0, {190, 190, 180}, {255, 255, 255}},
    {I_COAL, 0, {0, 0, 0}, {60, 60, 60}},                /* black throughout */
    {I_GRAPHITE, 0, {40, 40, 40}, {100, 100, 110}},      /* dark grey */
    {I_CHARCOAL, 0, {20, 20, 20}, {70, 60, 60}},
    {I_GOLD_INGOT, 1, {200, 150, 0}, {255, 220, 100}},
    {I_COPPER_INGOT, 1, {180, 90, 50}, {240, 160, 120}},
    {I_GUNPOWDER, 0, {30, 30, 30}, {80, 80, 80}},
};
/* clang-format on */

static int rgb_in(uint32_t rgb, const colour_case *c)
{
    for (int ch = 0; ch < 3; ch++) {
        int v = (int)(rgb >> (16 - 8 * ch) & 255u);
        if (v < c->lo[ch] || v > c->hi[ch]) return 0;
    }
    return 1;
}

static void test_colours(void)
{
    for (size_t i = 0; i < sizeof COLOURS / sizeof COLOURS[0]; i++) {
        const colour_case *c = &COLOURS[i];
        item_model m = {0};
        itemmodel_build(c->id, &m);
        int hits = 0;
        for (int b = 0; b < m.n; b++) hits += rgb_in(m.box[b].rgb, c);
        if (c->any ? hits == 0 : hits != m.n) fprintf(stderr, "  %s has the wrong colours\n", item_get(c->id)->name);
        CHECK(c->any ? hits > 0 : hits == m.n);
    }
}

/* ---------------------------------------------------- tool shapes */

/* The bounds of the boxes whose centres pass a test, by the box centres. */
static float span_x(const item_model *m, float y_above)
{
    float lo = 9.0f, hi = -9.0f;
    for (int i = 0; i < m->n; i++)
        if (m->box[i].pos[1] > y_above) {
            lo = fminf(lo, m->box[i].pos[0] - 0.5f * m->box[i].size[0]);
            hi = fmaxf(hi, m->box[i].pos[0] + 0.5f * m->box[i].size[0]);
        }
    return hi - lo;
}

static float thinnest(const item_model *m, float y_above)
{
    float t = 9.0f;
    for (int i = 0; i < m->n; i++)
        if (m->box[i].pos[1] > y_above) t = fminf(t, m->box[i].size[2]);
    return t;
}

static void test_tool_shapes(void)
{
    for (int tier = 0; tier < TIER_COUNT; tier++) {
        item_model m = {0};
        /* A pick's head reaches across half a metre, at the top. */
        itemmodel_build(item_tool_id(TOOL_PICKAXE, tier), &m);
        CHECK(span_x(&m, 0.6f) > 0.5f && span_x(&m, 0.6f) < 0.75f);
        CHECK(m.hi[1] - m.lo[1] > 0.75f);
        /* An axe's bit is on one side: it reaches further than the poll. */
        itemmodel_build(item_tool_id(TOOL_AXE, tier), &m);
        CHECK(m.hi[0] > 0.12f && -m.lo[0] < 0.07f);
        /* A spade's blade is about 20 cm wide, and the D grip 11 cm. */
        itemmodel_build(item_tool_id(TOOL_SHOVEL, tier), &m);
        CHECK(span_x(&m, 0.75f) > 0.18f && span_x(&m, 0.75f) < 0.24f);
        CHECK(m.box[0].size[0] > 0.1f && m.box[0].pos[1] < 0.02f);
        /* A sword has a guard wider than its blade and a blade over 60 cm. */
        itemmodel_build(item_tool_id(TOOL_SWORD, tier), &m);
        CHECK(span_x(&m, 0.15f) > 0.15f);
        CHECK(m.hi[1] - 0.19f > 0.6f);
    }
    /* Knapped flint is thick; a forged blade is thin. */
    item_model stone = {0}, iron = {0};
    itemmodel_build(item_tool_id(TOOL_KNIFE, TIER_STONE), &stone);
    itemmodel_build(item_tool_id(TOOL_KNIFE, TIER_IRON), &iron);
    CHECK(thinnest(&stone, 0.15f) > thinnest(&iron, 0.15f));
    CHECK(thinnest(&iron, 0.15f) <= 0.005f);
    itemmodel_build(item_tool_id(TOOL_SWORD, TIER_STONE), &stone);
    itemmodel_build(item_tool_id(TOOL_SWORD, TIER_IRON), &iron);
    CHECK(thinnest(&stone, 0.3f) > thinnest(&iron, 0.3f));
    /* The rifle is held at the wrist of its stock, a third of the way up,
     * with the muzzle at the far end. */
    item_model rifle = {0};
    itemmodel_build(I_RIFLE, &rifle);
    CHECK(rifle.grip[1] > 0.25f && rifle.grip[1] < 0.45f);
    CHECK(rifle.hi[1] > 1.0f);
}

/* ------------------------------------------------------ the model */

static void test_bounds_tight(void)
{
    /* The bounds touch the boxes on every side, and building twice gives
     * the same model. */
    for (item_id id = ITEM_FIRST; id < ITEM_END; id++) {
        item_model m = {0}, again = {0};
        itemmodel_build(id, &m);
        itemmodel_build(id, &again);
        CHECK(again.n == m.n && again.hold == m.hold);
        float lo[3] = {9.0f, 9.0f, 9.0f}, hi[3] = {-9.0f, -9.0f, -9.0f};
        for (int i = 0; i < m.n; i++) {
            const im_box *b = &m.box[i];
            CHECK(again.box[i].rgb == b->rgb && again.box[i].layer == b->layer);
            for (int a = 0; a < 3; a++) CHECK(again.box[i].pos[a] == b->pos[a]);
            /* A box's reach from its centre is at most half its diagonal. */
            float r = 0.5f * sqrtf(b->size[0] * b->size[0] + b->size[1] * b->size[1] + b->size[2] * b->size[2]);
            for (int a = 0; a < 3; a++) {
                lo[a] = fminf(lo[a], b->pos[a] - r);
                hi[a] = fmaxf(hi[a], b->pos[a] + r);
                CHECK(b->pos[a] >= m.lo[a] - 1e-5f && b->pos[a] <= m.hi[a] + 1e-5f);
            }
        }
        for (int a = 0; a < 3; a++) CHECK(m.lo[a] >= lo[a] - 1e-5f && m.hi[a] <= hi[a] + 1e-5f);
        float c[3] = {0.0f, 0.0f, 0.0f};
        itemmodel_centre(&m, c);
        for (int a = 0; a < 3; a++) CHECK(fabsf(c[a] - 0.5f * (m.lo[a] + m.hi[a])) < 1e-6f);
    }
}

static void test_material_layers(void)
{
    /* Each material looks different from the others: their 16x16
     * patterns differ texel by texel. */
    const uint8_t mats[] = {T_MAT_WOOD,  T_MAT_METAL, T_MAT_CAST,  T_MAT_ROCK, T_MAT_CLOTH,
                            T_MAT_GLASS, T_MAT_PLAIN, T_MAT_FLESH, T_MAT_GRAIN};
    for (size_t i = 0; i < sizeof mats; i++)
        for (size_t j = 0; j < i; j++) {
            int diff = 0;
            for (int y = 0; y < 16; y++)
                for (int x = 0; x < 16; x++) {
                    uint8_t a[4] = {0}, b[4] = {0};
                    texgen_texel(mats[i], x, y, a);
                    texgen_texel(mats[j], x, y, b);
                    diff += abs(a[0] - b[0]) + abs(a[3] - b[3]);
                }
            CHECK(diff > 256);
        }
    /* Plain is flat; wood, rock and grain have texture to them. */
    const uint8_t rough[] = {T_MAT_WOOD, T_MAT_ROCK, T_MAT_GRAIN, T_MAT_PLAIN};
    int spread[4] = {0};
    for (int k = 0; k < 4; k++) {
        int lo = 255, hi = 0;
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                uint8_t t[4] = {0};
                texgen_texel(rough[k], x, y, t);
                lo = t[0] < lo ? t[0] : lo;
                hi = t[0] > hi ? t[0] : hi;
            }
        spread[k] = hi - lo;
    }
    CHECK(spread[0] > spread[3] && spread[1] > spread[3] && spread[2] > spread[3]);
    CHECK(itemmodel_layer_clear(T_FIRE0) && itemmodel_layer_clear(T_FIRE3));
}

/* ------------------------------------------------------- in the hand */

/* The held parts of the view model for an item, after it is raised. */
static int held_parts(item_id id, entity_instance *out)
{
    viewmodel vm;
    viewmodel_init(&vm);
    viewmodel_input in;
    memset(&in, 0, sizeof in);
    in.held_id = id;
    in.light = 1.0f;
    in.dt = 1.0f / 60.0f;
    int n = 0;
    for (int i = 0; i < 90; i++) n = viewmodel_build(&vm, &in, out, VIEWMODEL_MAX);
    return n;
}

static float parts_span(const entity_instance *e, int from, int n)
{
    float lo[3] = {9.0f, 9.0f, 9.0f}, hi[3] = {-9.0f, -9.0f, -9.0f};
    for (int i = from; i < n; i++)
        for (int a = 0; a < 3; a++) {
            lo[a] = fminf(lo[a], e[i].pos[a]);
            hi[a] = fmaxf(hi[a], e[i].pos[a]);
        }
    return fmaxf(hi[0] - lo[0], fmaxf(hi[1] - lo[1], hi[2] - lo[2]));
}

static void test_held(void)
{
    entity_instance out[VIEWMODEL_MAX] = {{0}};
    /* Every item fits the view model with a forearm and a fist for each
     * hand it takes. */
    for (item_id id = ITEM_FIRST; id < ITEM_END; id++) {
        item_model m = {0};
        itemmodel_build(id, &m);
        CHECK(held_parts(id, out) == 2 * viewmodel_hands(id) + m.n);
    }
    /* A cartridge is shown larger than it is, a bucket smaller. */
    item_model m = {0};
    itemmodel_build(I_CARTRIDGE, &m);
    int n = held_parts(I_CARTRIDGE, out);
    float shown = 0.0f;
    for (int i = 2; i < n; i++) shown = fmaxf(shown, out[i].scale[1]);
    CHECK(shown > 1.5f * m.box[0].size[1]);
    itemmodel_build(I_BUCKET, &m);
    CHECK(held_parts(I_BUCKET, out) == 2 + m.n);
    CHECK(out[2].scale[0] < m.box[0].size[0]);
    /* A pick's head is above the hand and its handle reaches into it. */
    n = held_parts(item_tool_id(TOOL_PICKAXE, TIER_IRON), out);
    float top = -9.0f;
    for (int i = 2; i < n; i++) top = fmaxf(top, out[i].pos[1]);
    CHECK(top > out[1].pos[1] + 0.2f);
    CHECK(parts_span(out, 2, n) > 0.2f);
    /* The rifle points away: its muzzle is well ahead of the hand. */
    n = held_parts(I_RIFLE, out);
    float ahead = 0.0f;
    for (int i = 2; i < n; i++) ahead = fmaxf(ahead, out[1].pos[2] - out[i].pos[2]);
    CHECK(ahead > 0.4f);
    /* The torch's flame lights itself in the hand. */
    n = held_parts(B_TORCH, out);
    int lit = 0;
    for (int i = 2; i < n; i++) lit += out[i].light >= 1.0f;
    CHECK(lit >= 1);
}

/* --------------------------------------------------------- dropped */

static void test_dropped(void)
{
    test_world t;
    tw_init(&t);
    entity_instance *ents = mem_alloc(sizeof(entity_instance) * 512);
    int no = 0, nt = 0;
    const dvec3 eye = dv3(0.5, GROUND + 1.6, -3.0);
    /* One pick: its boxes, all opaque, lying flat (wider than tall). */
    item_model pick = {0};
    itemmodel_build(item_tool_id(TOOL_PICKAXE, TIER_IRON), &pick);
    CHECK(physics_drop_item(&t.ph, dv3(0.5, GROUND + 0.5, 0.5), dv3(0, 0, 0), item_tool_id(TOOL_PICKAXE, TIER_IRON), 1,
                            0.0f) >= 0);
    fx_build_entities(NULL, &t.ph, NULL, eye, 1.0, 0.0f, 1.0f, NULL, ents, 512, &no, &nt);
    CHECK(no == pick.n && nt == 0);
    float ylo = 9.0f, yhi = -9.0f;
    for (int i = 0; i < no; i++) {
        ylo = fminf(ylo, ents[i].pos[1]);
        yhi = fmaxf(yhi, ents[i].pos[1]);
        CHECK(ents[i].tint == entity_tint(pick.box[i].rgb));
    }
    CHECK(yhi - ylo < 0.15f);
    CHECK(parts_span(ents, 0, no) > 0.35f);
    /* A bottle: its glass blends, its contents don't. */
    t.ph.item_count = 0;
    item_model bottle = {0};
    itemmodel_build(I_ANTISEPTIC, &bottle);
    int glass = 0;
    for (int i = 0; i < bottle.n; i++) glass += itemmodel_layer_clear(bottle.box[i].layer);
    physics_drop_item(&t.ph, dv3(0.5, GROUND + 0.5, 0.5), dv3(0, 0, 0), I_ANTISEPTIC, 1, 0.0f);
    fx_build_entities(NULL, &t.ph, NULL, eye, 1.0, 0.0f, 1.0f, NULL, ents, 512, &no, &nt);
    CHECK(nt == glass && no == bottle.n - glass);
    for (int i = no; i < no + nt; i++) CHECK(itemmodel_layer_clear((uint8_t)(ents[i].tex & 255u)));
    /* A cartridge is enlarged so it can be found; a pile shows three. */
    t.ph.item_count = 0;
    physics_drop_item(&t.ph, dv3(0.5, GROUND + 0.5, 0.5), dv3(0, 0, 0), I_CARTRIDGE, 40, 0.0f);
    fx_build_entities(NULL, &t.ph, NULL, eye, 1.0, 0.0f, 1.0f, NULL, ents, 512, &no, &nt);
    item_model cart = {0};
    itemmodel_build(I_CARTRIDGE, &cart);
    CHECK(no == 3 * cart.n);
    float tallest = 0.0f;
    for (int i = 0; i < no; i++) tallest = fmaxf(tallest, ents[i].scale[1]);
    CHECK(tallest > 2.0f * cart.box[0].size[1]);
    /* A block item is still a cube. */
    t.ph.item_count = 0;
    physics_drop_item(&t.ph, dv3(0.5, GROUND + 0.5, 0.5), dv3(0, 0, 0), B_STONE, 1, 0.0f);
    fx_build_entities(NULL, &t.ph, NULL, eye, 1.0, 0.0f, 1.0f, NULL, ents, 512, &no, &nt);
    CHECK(no == 1 && fabsf(ents[0].scale[0] - 0.25f) < 1e-6f && ents[0].tint == 0u);
    /* Under a sunset's orange sky the pick takes its colour; a torch's
     * flame keeps its own, its handle does not glow. */
    lightfx_frame dusk = {0};
    dusk.sky[0] = 1.0f;
    dusk.sky[1] = 0.5f;
    dusk.sky[2] = 0.25f;
    t.ph.item_count = 0;
    physics_drop_item(&t.ph, dv3(0.5, GROUND + 0.5, 0.5), dv3(0, 0, 0), item_tool_id(TOOL_PICKAXE, TIER_IRON), 1, 0.0f);
    fx_build_entities(NULL, &t.ph, NULL, eye, 1.0, 0.0f, 1.0f, &dusk, ents, 512, &no, &nt);
    CHECK(no == pick.n);
    int bluer = 0;
    for (int i = 0; i < no; i++) {
        uint32_t was = entity_tint(pick.box[i].rgb);
        CHECK((ents[i].tint & 255u) == (was & 255u)); /* red: full light */
        bluer += (ents[i].tint >> 16 & 255u) > (was >> 16 & 255u);
    }
    CHECK(bluer > 0);
    t.ph.item_count = 0;
    physics_drop_item(&t.ph, dv3(0.5, GROUND + 0.5, 0.5), dv3(0, 0, 0), B_TORCH, 1, 0.0f);
    fx_build_entities(NULL, &t.ph, NULL, eye, 1.0, 0.0f, 1.0f, &dusk, ents, 512, &no, &nt);
    int glowing = 0;
    for (int i = 0; i < no + nt; i++) glowing += (ents[i].tint & ENTITY_GLOWS) != 0;
    CHECK(glowing > 0 && glowing < no + nt);
    CHECK(!(ents[0].tint & ENTITY_GLOWS)); /* the handle */
    mem_free(ents);
    tw_free(&t);
}

/* ------------------------------------------------------------ icons */

static void test_icon_draw(void)
{
    ui_vertex *mem = mem_alloc(sizeof(ui_vertex) * 4 * 4096);
    ui u;
    ui_begin(&u, mem, 4096, 640, 360);
    CHECK(u.scale == 1.0f);
    /* A model's icon is its cached quads, inside the square it is given. */
    item_model m = {0};
    itemmodel_build(item_tool_id(TOOL_AXE, TIER_COPPER), &m);
    im_quad q[IM_ICON_QUADS] = {0};
    int nq = itemmodel_icon(&m, q, IM_ICON_QUADS);
    invui_icon(&u, item_tool_id(TOOL_AXE, TIER_COPPER), 10.0f, 20.0f, 32.0f, 1.0f);
    CHECK(u.quads == nq);
    for (int i = 0; i < 4 * u.quads; i++) {
        CHECK(u.v[i].x >= 10.0f - 0.01f && u.v[i].x <= 42.0f + 0.01f);
        CHECK(u.v[i].y >= 20.0f - 0.01f && u.v[i].y <= 52.0f + 0.01f);
    }
    /* Faded, every face keeps its colour at half the alpha. */
    int before = u.quads;
    invui_icon(&u, item_tool_id(TOOL_AXE, TIER_COPPER), 10.0f, 20.0f, 32.0f, 0.5f);
    CHECK(u.quads == 2 * before);
    for (int i = 0; i < before; i++) {
        uint32_t full = u.v[4 * i].rgba, half = u.v[4 * (before + i)].rgba;
        CHECK((full & 0xFFFFFFu) == (half & 0xFFFFFFu));
        CHECK(abs((int)(half >> 24) - (int)((full >> 24) + 1) / 2) <= 1);
    }
    /* The torch has a model icon too; a block is still a cube; nothing
     * past the items draws. */
    before = u.quads;
    invui_icon(&u, B_TORCH, 0.0f, 0.0f, 32.0f, 1.0f);
    CHECK(u.quads > before);
    before = u.quads;
    invui_icon(&u, B_STONE, 0.0f, 0.0f, 32.0f, 1.0f);
    CHECK(u.quads - before == 3 * 16);
    before = u.quads;
    invui_icon(&u, (item_id)(ITEM_END + 3), 0.0f, 0.0f, 32.0f, 1.0f);
    CHECK(u.quads == before);
    mem_free(mem);
}

/* The mean corner of some icon quads: where that part lands. */
static void icon_where(const im_quad *q, int nq, float out[2])
{
    float sx = 0.0f, sy = 0.0f;
    for (int i = 0; i < nq; i++)
        for (int k = 0; k < 4; k++) {
            sx += q[i].xy[2 * k];
            sy += q[i].xy[2 * k + 1];
        }
    out[0] = sx / (float)(4 * nq);
    out[1] = sy / (float)(4 * nq);
}

static void test_icon_angles(void)
{
    /* A pick lies diagonal: drawn as just its handle and the eye of its
     * head, the iron lands right of and above the ash. */
    item_model both = {0};
    itemmodel_build(item_tool_id(TOOL_PICKAXE, TIER_IRON), &both);
    both.n = 2;
    im_quad q[IM_ICON_QUADS] = {0}, wood[IM_ICON_QUADS] = {0}, iron[IM_ICON_QUADS] = {0};
    int nq = itemmodel_icon(&both, q, IM_ICON_QUADS), nw = 0, ni = 0;
    for (int i = 0; i < nq; i++) {
        int r = (int)(q[i].rgba & 255u), g = (int)(q[i].rgba >> 8 & 255u);
        if (r > g + 10) wood[nw++] = q[i]; /* the ash is warm */
        else iron[ni++] = q[i];            /* the iron is grey */
    }
    CHECK(nw > 0 && ni > 0);
    float a[2] = {0.0f, 0.0f}, b[2] = {0.0f, 0.0f};
    icon_where(wood, nw, a);
    icon_where(iron, ni, b);
    CHECK(b[0] > a[0] && b[1] < a[1]);
    /* A palm item is seen from above: a bar's top face is the brightest
     * and sits highest on screen. */
    item_model bar = {0};
    itemmodel_build(I_STEEL_INGOT, &bar);
    bar.n = 1;
    nq = itemmodel_icon(&bar, q, IM_ICON_QUADS);
    CHECK(nq == 3);
    int top = 0;
    for (int i = 1; i < nq; i++)
        if ((q[i].rgba & 255u) > (q[top].rgba & 255u)) top = i;
    float ty = 0.0f, oy = 0.0f;
    for (int k = 0; k < 4; k++) ty += q[top].xy[2 * k + 1];
    for (int i = 0; i < nq; i++)
        if (i != top)
            for (int k = 0; k < 4; k++) oy = fmaxf(oy, q[i].xy[2 * k + 1]);
    CHECK(ty / 4.0f < oy);
    /* An empty model draws nothing, and place writes nothing into no room. */
    item_model none = {0};
    CHECK(itemmodel_icon(&none, q, IM_ICON_QUADS) == 0);
    entity_instance e[1] = {{0}};
    const float zero[3] = {0.0f, 0.0f, 0.0f};
    CHECK(itemmodel_place(&both, zero, zero, quat_identity(), 1.0f, 1.0f, e, 0) == 0);
}

void test_itemmodel_all(void)
{
    test_every_item();
    test_real_sizes();
    test_tiers();
    test_containers();
    test_masses();
    test_layers_greyscale();
    test_tint();
    test_place();
    test_icons();
    test_icon_lighting();
    test_features();
    test_colours();
    test_tool_shapes();
    test_bounds_tight();
    test_material_layers();
    test_held();
    test_dropped();
    test_icon_draw();
    test_icon_angles();
}
