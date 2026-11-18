#include "item.h"

#include <stdio.h>

/* clang-format off */
static const item_def ITEMS[I_TOOL_FIRST - ITEM_FIRST] = {
    [I_STICK - ITEM_FIRST] = {.name = "Stick",
                              .stack = 64,
                              .block = B_AIR,
                              .desc = "Fuel, splints. 2 planks make 4.",
                              .density = 600.0f},
    [I_FIBRE - ITEM_FIRST] = {.name = "Plant fibre",
                              .stack = 64,
                              .block = B_AIR,
                              .desc = "From leaves. 2 make a rough bandage.",
                              .density = 300.0f},
    [I_BANDAGE - ITEM_FIRST] = {.name = "Sterile bandage",
                                .stack = 16,
                                .block = B_AIR,
                                .desc = "Pressure dressing (H, then B).",
                                .density = 250.0f},
    [I_SPLINT - ITEM_FIRST] = {.name = "Splint",
                               .stack = 16,
                               .block = B_AIR,
                               .desc = "Holds a broken limb (H, then S).",
                               .density = 600.0f},
    [I_ANTISEPTIC - ITEM_FIRST] = {.name = "Antiseptic",
                                   .stack = 16,
                                   .block = B_AIR,
                                   .desc = "Cleans wounds (H, then D).",
                                   .density = 1050.0f},
    [I_PAINKILLER - ITEM_FIRST] = {.name = "Painkillers",
                                   .stack = 32,
                                   .block = B_AIR,
                                   .desc = "6 h of relief (H, then P).",
                                   .density = 1200.0f},
    [I_ANTIBIOTIC - ITEM_FIRST] = {.name = "Antibiotics",
                                   .stack = 16,
                                   .block = B_AIR,
                                   .desc = "Treats infection (H, then A).",
                                   .density = 1200.0f},
    [I_APPLE - ITEM_FIRST] = {.name = "Apple",
                              .stack = 32,
                              .block = B_AIR,
                              .desc = "95 kcal. E to eat.",
                              .density = 800.0f},
    [I_BUCKET - ITEM_FIRST] = {.name = "Bucket",
                               .stack = 1,
                               .block = B_AIR,
                               .desc = "Right-click water to fill.",
                               .density = 550.0f},
    [I_WATER_BUCKET - ITEM_FIRST] = {.name = "Water bucket",
                                     .stack = 1,
                                     .block = B_WATER,
                                     .desc = "Right-click to pour; R to drink.",
                                     .density = 1080.0f},
    [I_DEFIBRILLATOR - ITEM_FIRST] = {.name = "Defibrillator",
                                      .stack = 1,
                                      .block = B_AIR,
                                      .desc = "Automatic. Right-click: pads on or off.",
                                      .density = 650.0f},
    [I_COAL - ITEM_FIRST] = {.name = "Coal",
                             .stack = 64,
                             .block = B_AIR,
                             .desc = "Fuel for smelting ore in a furnace.",
                             .density = 1350.0f},
    [I_COPPER_INGOT - ITEM_FIRST] = {.name = "Copper ingot",
                                     .stack = 64,
                                     .block = B_AIR,
                                     .desc = "Smelted copper.",
                                     .density = 8960.0f},
    [I_IRON_INGOT - ITEM_FIRST] = {.name = "Iron ingot",
                                   .stack = 64,
                                   .block = B_AIR,
                                   .desc = "Smelted iron.",
                                   .density = 7870.0f},
    [I_RAW_COPPER - ITEM_FIRST] = {.name = "Raw copper",
                                   .stack = 64,
                                   .block = B_AIR,
                                   .desc = "Copper ore. Smelt with coal in a furnace.",
                                   .density = 4000.0f},
    [I_RAW_IRON - ITEM_FIRST] = {.name = "Raw iron",
                                 .stack = 64,
                                 .block = B_AIR,
                                 .desc = "Iron ore. Smelt with coal in a furnace.",
                                 .density = 5000.0f},
    [I_RAW_MEAT - ITEM_FIRST] = {.name = "Raw meat",
                                 .stack = 16,
                                 .block = B_AIR,
                                 .desc = "Better cooked over a fire. E to eat.",
                                 .density = 1050.0f},
    [I_COOKED_MEAT - ITEM_FIRST] = {.name = "Cooked meat",
                                    .stack = 16,
                                    .block = B_AIR,
                                    .desc = "350 kcal. E to eat.",
                                    .density = 1000.0f},
    [I_GUNPOWDER - ITEM_FIRST] = {.name = "Gunpowder",
                                  .stack = 64,
                                  .block = B_AIR,
                                  .desc = "Coal and the potash in ash. For cartridges.",
                                  .density = 1700.0f},
    [I_CARTRIDGE - ITEM_FIRST] = {.name = "Cartridge",
                                  .stack = 32,
                                  .block = B_AIR,
                                  .desc = "Rifle ammunition.",
                                  .density = 5000.0f},
    [I_RIFLE - ITEM_FIRST] = {.name = "Rifle",
                              .stack = 1,
                              .block = B_AIR,
                              .desc = "Right-click to fire. Uses cartridges.",
                              .density = 900.0f},
    [I_ADRENALINE - ITEM_FIRST] = {.name = "Adrenaline auto-injector",
                                   .stack = 4,
                                   .block = B_AIR,
                                   .desc = "0.3 mg. For a stopped heart (H, then N); risky on a beating one.",
                                   .density = 1100.0f},
    [I_MORPHINE - ITEM_FIRST] = {.name = "Morphine",
                                 .stack = 16,
                                 .block = B_AIR,
                                 .desc = "10 mg. Strong pain relief (H, then M). Slows breathing.",
                                 .density = 1200.0f},
    [I_NALOXONE - ITEM_FIRST] = {.name = "Naloxone spray",
                                 .stack = 4,
                                 .block = B_AIR,
                                 .desc = "Reverses opioids for about an hour (H, then O).",
                                 .density = 1000.0f},
    [I_SPIRITS - ITEM_FIRST] = {.name = "Spirits",
                                .stack = 8,
                                .block = B_AIR,
                                .desc = "Apple brandy. A drink (H, then L), or to clean wounds.",
                                .density = 950.0f},
};
/* clang-format on */

/* The items after the tools, from I_RAW_TIN. Dropped minerals sink at the
 * density of the mineral, ingots at the metal's; charcoal is porous enough
 * to float. */
#define MINERAL(id, nm, text, dens) [(id) - I_TOOL_END] = {.name = (nm), .desc = (text), .density = (dens), .stack = 64}
static const item_def MINERALS[I_MINERALS_END - I_TOOL_END] = {
    MINERAL(I_RAW_TIN, "Raw tin", "Cassiterite. Smelt with coal in a furnace.", 6900.0f),
    MINERAL(I_RAW_ZINC, "Raw zinc", "Sphalerite. Smelt with coal in a furnace.", 4000.0f),
    MINERAL(I_RAW_LEAD, "Raw lead", "Galena. Smelt with coal in a furnace.", 7500.0f),
    MINERAL(I_RAW_NICKEL, "Raw nickel", "Pentlandite. Smelt with coal in a furnace.", 4800.0f),
    MINERAL(I_RAW_SILVER, "Raw silver", "Silver ore. Smelt with coal in a furnace.", 7000.0f),
    MINERAL(I_RAW_GOLD, "Raw gold", "Gold in quartz. Melt it out in a furnace.", 12000.0f),
    MINERAL(I_BAUXITE, "Bauxite", "Aluminium ore. Needs electrolysis, not a furnace.", 2500.0f),
    MINERAL(I_ILMENITE, "Ilmenite", "Titanium ore. Too stable for a furnace to free.", 4700.0f),
    MINERAL(I_URANINITE, "Uraninite", "Uranium ore, pitchblende. Weakly radioactive.", 8000.0f),
    MINERAL(I_SPODUMENE, "Spodumene", "Lithium ore. Needs chemical refining.", 3150.0f),
    MINERAL(I_SULFUR, "Sulfur", "Burns blue. One part in ten of gunpowder.", 2070.0f),
    MINERAL(I_SALTPETER, "Saltpeter", "Potassium nitrate. Three parts in four of gunpowder.", 2110.0f),
    MINERAL(I_SALT, "Salt", "Rock salt, halite.", 2170.0f),
    MINERAL(I_QUARTZ, "Quartz", "Silica crystal. Too hot to melt for a furnace.", 2650.0f),
    MINERAL(I_GRAPHITE, "Graphite", "Soft carbon. Conducts heat and electricity.", 2160.0f),
    MINERAL(I_TIN_INGOT, "Tin ingot", "Melts low. Alloys with copper into bronze.", 7290.0f),
    MINERAL(I_ZINC_INGOT, "Zinc ingot", "Alloys with copper into brass.", 7140.0f),
    MINERAL(I_LEAD_INGOT, "Lead ingot", "Soft and heavy.", 11340.0f),
    MINERAL(I_NICKEL_INGOT, "Nickel ingot", "Hard and slow to corrode.", 8910.0f),
    MINERAL(I_SILVER_INGOT, "Silver ingot", "Smelted silver.", 10490.0f),
    MINERAL(I_GOLD_INGOT, "Gold ingot", "Smelted gold.", 19300.0f),
    MINERAL(I_BRONZE_INGOT, "Bronze ingot", "Three parts copper to one of tin.", 8800.0f),
    MINERAL(I_BRASS_INGOT, "Brass ingot", "Two parts copper to one of zinc.", 8500.0f),
    MINERAL(I_STEEL_INGOT, "Steel ingot", "Iron hardened with carbon from charcoal.", 7850.0f),
    MINERAL(I_CHARCOAL, "Charcoal", "Wood baked without air. Burns hot and clean.", 400.0f),
    MINERAL(I_QUICKLIME, "Quicklime", "Burnt limestone, for mortar and flux.", 3340.0f),
};
#undef MINERAL

static const item_def UNKNOWN = {.name = "?", .desc = "", .density = 1000.0f, .stack = 1, .block = B_AIR};

static const char *const TIER_NAME[TIER_COUNT] = {"Wooden", "Stone", "Copper", "Iron"};
static const char *const KIND_NAME[TOOL_KINDS] = {"pickaxe", "axe", "shovel", "knife", "sword"};
static const char *const KIND_DESC[TOOL_KINDS] = {"Breaks rock and ore", "Fells trees, cuts timber",
                                                  "Digs soil, sand and snow", "A short blade", "A long blade"};
/* Uses per tier: harder edges last longer. Wood about a hundred strikes,
 * iron a few hundred (and wear fits in a byte). */
static const int TIER_USES[TIER_COUNT] = {60, 130, 180, 250};
/* Breaking speed with the right tool, times the bare hand. */
static const float TIER_SPEED[TIER_COUNT] = {2.0f, 4.0f, 5.0f, 6.5f};

static item_def g_tools[TOOL_KINDS * TIER_COUNT];

static void build_tools(void)
{
    static char names[TOOL_KINDS * TIER_COUNT][24], descs[TOOL_KINDS * TIER_COUNT][48];
    for (int k = 0; k < TOOL_KINDS; k++)
        for (int t = 0; t < TIER_COUNT; t++) {
            int i = k * TIER_COUNT + t;
            snprintf(names[i], sizeof names[i], "%s %s", TIER_NAME[t], KIND_NAME[k]);
            snprintf(descs[i], sizeof descs[i], "%s. %d uses.", KIND_DESC[k], TIER_USES[t]);
            /* Densities of the head's material, so a dropped tool floats or
             * sinks as its head would. */
            static const float DENS[TIER_COUNT] = {600.0f, 2600.0f, 8960.0f, 7870.0f};
            g_tools[i] = (item_def){.name = names[i], .desc = descs[i], .density = DENS[t], .stack = 1, .block = B_AIR};
        }
}

float item_food(item_id id, float *water)
{
    /* A 180 g apple, and about 150 g of pork: cooking drives off water
     * but makes more of the energy digestible. */
    float kcal = 0.0f, w = 0.0f;
    switch (id) {
    case I_APPLE:
        kcal = 95.0f;
        w = 0.085f;
        break;
    case I_RAW_MEAT:
        kcal = 200.0f;
        w = 0.09f;
        break;
    case I_COOKED_MEAT:
        kcal = 350.0f;
        w = 0.05f;
        break;
    default: break;
    }
    if (water) *water = w;
    return kcal;
}

int item_durability(item_id id)
{
    int kind = 0, tier = 0;
    if (id == I_RIFLE) return 250; /* shots before the barrel is shot out (wear is a byte) */
    return item_tool(id, &kind, &tier) ? TIER_USES[tier] : 0;
}

int block_tool(uint8_t block)
{
    const block_def *d = block_get(block);
    if (block >= B_COUNT || d->hardness < 0.0f) return -1; /* nothing digs bedrock */
    switch (d->mat) {
    case BM_STONE:
    case BM_ICE: return TOOL_PICKAXE;
    case BM_WOOD: return TOOL_AXE;
    case BM_GRASS:
    case BM_DIRT:
    case BM_SAND:
    case BM_GRAVEL:
    case BM_SNOW: return TOOL_SHOVEL;
    default: return -1;
    }
}

/* The pickaxe tier a block needs before it yields anything, -1 for none:
 * the block table's tier for rock, ore and masonry. */
static int pick_tier(uint8_t block)
{
    const block_def *d = block_get(block);
    return block_tool(block) == TOOL_PICKAXE && d->mat == BM_STONE ? d->tier : -1;
}

/* What an ore breaks into: the item, how many, and one more with a chance
 * of 1 in `extra` (0 for never). Soft deposits crumble into more pieces. */
static const struct {
    item_id item;
    uint8_t count, extra;
} ORE[B_COUNT] = {
    [B_COAL_ORE] = {I_COAL, 1, 3},           [B_COPPER_ORE] = {I_RAW_COPPER, 1, 0},
    [B_IRON_ORE] = {I_RAW_IRON, 1, 0},       [B_TIN_ORE] = {I_RAW_TIN, 1, 0},
    [B_ZINC_ORE] = {I_RAW_ZINC, 1, 0},       [B_LEAD_ORE] = {I_RAW_LEAD, 1, 0},
    [B_NICKEL_ORE] = {I_RAW_NICKEL, 1, 0},   [B_SILVER_ORE] = {I_RAW_SILVER, 1, 0},
    [B_GOLD_ORE] = {I_RAW_GOLD, 1, 0},       [B_BAUXITE] = {I_BAUXITE, 2, 0},
    [B_TITANIUM_ORE] = {I_ILMENITE, 1, 0},   [B_URANIUM_ORE] = {I_URANINITE, 1, 0},
    [B_LITHIUM_ORE] = {I_SPODUMENE, 1, 0},   [B_SULFUR_ORE] = {I_SULFUR, 1, 2},
    [B_SALTPETER_ORE] = {I_SALTPETER, 1, 2}, [B_ROCK_SALT] = {I_SALT, 2, 0},
    [B_QUARTZ_ORE] = {I_QUARTZ, 1, 0},       [B_GRAPHITE_ORE] = {I_GRAPHITE, 1, 2},
};

int item_harvests(item_id held, uint8_t block)
{
    int need = pick_tier(block), kind = 0, tier = 0;
    if (need < 0) return 1;
    return item_tool(held, &kind, &tier) && kind == TOOL_PICKAXE && tier >= need;
}

float item_dig_speed(item_id held, uint8_t block)
{
    int kind = 0, tier = 0;
    if (item_tool(held, &kind, &tier) && kind == block_tool(block)) return TIER_SPEED[tier];
    /* Bare hands (or the wrong tool) on rock: a third of the speed, and
     * item_harvests says it yields nothing. */
    return pick_tier(block) >= 0 ? 1.0f / 3.0f : 1.0f;
}

int item_valid(item_id id)
{
    /* Rock and ore only exist in place: breaking them gives the loose rock
     * or the mineral, so they never become building blocks. */
    if (item_is_block(id))
        return id < B_COUNT && !block_fluid((uint8_t)id) && id != B_BEDROCK && !g_blocks[id].loose && !ORE[id].item;
    return id >= ITEM_FIRST && id < ITEM_END;
}

/* The full buckets after the minerals, by I_OIL_BUCKET onward. */
static const item_def LIQUIDS[I_LIQUIDS_END - I_MINERALS_END] = {
    {.name = "Oil bucket",
     .stack = 1,
     .block = B_CRUDE_OIL,
     .desc = "Crude oil. Right-click to pour.",
     .density = 950.0f},
    {.name = "Brine bucket",
     .stack = 1,
     .block = B_BRINE,
     .desc = "Too salty to drink. Right-click to pour.",
     .density = 1250.0f},
};

item_id item_bucket(uint8_t liquid)
{
    switch (liquid) {
    case B_WATER: return I_WATER_BUCKET;
    case B_CRUDE_OIL: return I_OIL_BUCKET;
    case B_BRINE: return I_BRINE_BUCKET;
    default: return I_NONE;
    }
}

const item_def *item_get(item_id id)
{
    static item_def blocks[B_COUNT], minerals[I_MINERALS_END - I_TOOL_END];
    static int built;
    if (!built) {
        for (int i = 0; i < I_MINERALS_END - I_TOOL_END; i++) minerals[i] = MINERALS[i];
        for (int b = 0; b < B_COUNT; b++) {
            const block_def *d = &g_blocks[b];
            blocks[b] =
                (item_def){.name = d->name, .desc = "", .density = d->density, .stack = 64, .block = (uint8_t)b};
        }
        blocks[B_GLASS].desc = "Brittle: shatters when it falls hard.";
        blocks[B_ICE].desc = "Melts above 0"
                             "\x7f"
                             "C.";
        blocks[B_SNOW].desc = "Melts into a little water.";
        blocks[B_SAND].desc = "Needs support from below.";
        blocks[B_GRAVEL].desc = "Needs support from below.";
        blocks[B_CAMPFIRE].desc = "Heat, light and cooking. Feed it sticks, planks or logs.";
        blocks[B_ASH].desc = "What is left of a fire.";
        blocks[B_FURNACE].desc = "Smelts ore. Craft next to it.";
        blocks[B_CLAY].desc = "Fire it into brick.";
        blocks[B_MUD].desc = "Waterlogged soil. Needs support from below.";
        blocks[B_PEAT].desc = "Half-rotted plants from a bog.";
        blocks[B_OBSIDIAN].desc = "Volcanic glass: shatters when it falls hard.";
        blocks[B_TORCH].desc = "Light. Burns for ever, but gives no heat to speak of.";
        blocks[B_LAMP].desc = "A steady light behind glass.";
        blocks[B_FOXFIRE].desc = "Fungus that glows faintly green.";
        build_tools();
        built = 1;
    }
    if (item_is_block(id) && id < B_COUNT) return &blocks[id];
    if (id >= I_TOOL_FIRST && id < I_TOOL_END) return &g_tools[id - I_TOOL_FIRST];
    if (id >= ITEM_FIRST && id < I_TOOL_FIRST) return &ITEMS[id - ITEM_FIRST];
    if (id >= I_TOOL_END && id < I_MINERALS_END) return &minerals[id - I_TOOL_END];
    if (id >= I_MINERALS_END && id < I_LIQUIDS_END) return &LIQUIDS[id - I_MINERALS_END];
    return &UNKNOWN;
}

static uint32_t next(uint32_t *rng)
{
    *rng = *rng * 1664525u + 1013904223u;
    return *rng >> 8;
}

static int chance(uint32_t *rng, uint32_t num, uint32_t den) { return next(rng) % den < num; }

int item_drops(uint8_t block, uint32_t *rng, item_stack out[3])
{
    int n = 0;
    switch (block) {
    case B_LEAVES:
    case B_BIRCH_LEAVES:
    case B_SPRUCE_LEAVES:
    case B_PINE_LEAVES:
    case B_THORN_LEAVES:
        /* Torn apart by hand: fibre about half the time, and from an oak
         * sometimes an apple. */
        if (chance(rng, 1, 2)) out[n++] = (item_stack){I_FIBRE, 1, 0};
        if (block == B_LEAVES && chance(rng, 1, 6)) out[n++] = (item_stack){I_APPLE, 1, 0};
        break;
    case B_GRASS: out[n++] = (item_stack){B_DIRT, 1, 0}; break;
    case B_GLASS: break; /* shatters */
    case B_CAMPFIRE: out[n++] = (item_stack){I_STICK, 2, 0}; break; /* the unburnt ends */
    case B_WATER:
    case B_AIR:
    case B_BEDROCK: break;
    default:
        if (block >= B_COUNT) break;
        if (ORE[block].item) {
            int extra = ORE[block].extra && chance(rng, 1, ORE[block].extra);
            out[n++] = (item_stack){ORE[block].item, (uint8_t)(ORE[block].count + extra), 0};
        } else {
            out[n++] = (item_stack){g_blocks[block].loose ? g_blocks[block].loose : block, 1, 0};
        }
        break;
    }
    return n;
}
