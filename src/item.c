#include "item.h"

#include <stdio.h>

static const item_def ITEMS[I_TOOL_FIRST - ITEM_FIRST] = {
    [I_STICK - ITEM_FIRST] = {.name = "Stick",
                              .stack = 64,
                              .block = B_AIR,
                              .tex = T_ITEM_STICK,
                              .desc = "Fuel, splints. 2 planks make 4.",
                              .density = 600.0f},
    [I_FIBRE - ITEM_FIRST] = {.name = "Plant fibre",
                              .stack = 64,
                              .block = B_AIR,
                              .tex = T_ITEM_FIBRE,
                              .desc = "From leaves. 2 make a rough bandage.",
                              .density = 300.0f},
    [I_BANDAGE - ITEM_FIRST] = {.name = "Sterile bandage",
                                .stack = 16,
                                .block = B_AIR,
                                .tex = T_ITEM_BANDAGE,
                                .desc = "Pressure dressing (H, then B).",
                                .density = 250.0f},
    [I_SPLINT - ITEM_FIRST] = {.name = "Splint",
                               .stack = 16,
                               .block = B_AIR,
                               .tex = T_ITEM_SPLINT,
                               .desc = "Holds a broken limb (H, then S).",
                               .density = 600.0f},
    [I_ANTISEPTIC - ITEM_FIRST] = {.name = "Antiseptic",
                                   .stack = 16,
                                   .block = B_AIR,
                                   .tex = T_ITEM_ANTISEPTIC,
                                   .desc = "Cleans wounds (H, then D).",
                                   .density = 1050.0f},
    [I_PAINKILLER - ITEM_FIRST] = {.name = "Painkillers",
                                   .stack = 32,
                                   .block = B_AIR,
                                   .tex = T_ITEM_PAINKILLER,
                                   .desc = "6 h of relief (H, then P).",
                                   .density = 1200.0f},
    [I_ANTIBIOTIC - ITEM_FIRST] = {.name = "Antibiotics",
                                   .stack = 16,
                                   .block = B_AIR,
                                   .tex = T_ITEM_ANTIBIOTIC,
                                   .desc = "Treats infection (H, then A).",
                                   .density = 1200.0f},
    [I_APPLE - ITEM_FIRST] = {.name = "Apple",
                              .stack = 32,
                              .block = B_AIR,
                              .tex = T_ITEM_APPLE,
                              .desc = "95 kcal. E to eat.",
                              .density = 800.0f},
    [I_BUCKET - ITEM_FIRST] = {.name = "Bucket",
                               .stack = 1,
                               .block = B_AIR,
                               .tex = T_ITEM_BUCKET,
                               .desc = "Right-click water to fill.",
                               .density = 550.0f},
    [I_WATER_BUCKET - ITEM_FIRST] = {.name = "Water bucket",
                                     .stack = 1,
                                     .block = B_WATER,
                                     .tex = T_ITEM_WATER_BUCKET,
                                     .desc = "Right-click to pour; R to drink.",
                                     .density = 1080.0f},
    [I_DEFIBRILLATOR - ITEM_FIRST] = {.name = "Defibrillator",
                                      .stack = 1,
                                      .block = B_AIR,
                                      .tex = T_ITEM_DEFIBRILLATOR,
                                      .desc = "Automatic. Right-click: pads on or off.",
                                      .density = 650.0f},
    [I_COAL - ITEM_FIRST] = {.name = "Coal",
                             .stack = 64,
                             .block = B_AIR,
                             .tex = T_ITEM_COAL,
                             .desc = "Fuel for smelting ore in a furnace.",
                             .density = 1350.0f},
    [I_COPPER_INGOT - ITEM_FIRST] = {.name = "Copper ingot",
                                     .stack = 64,
                                     .block = B_AIR,
                                     .tex = T_ITEM_COPPER_INGOT,
                                     .desc = "Smelted copper.",
                                     .density = 8960.0f},
    [I_IRON_INGOT - ITEM_FIRST] = {.name = "Iron ingot",
                                   .stack = 64,
                                   .block = B_AIR,
                                   .tex = T_ITEM_IRON_INGOT,
                                   .desc = "Smelted iron.",
                                   .density = 7870.0f},
    [I_RAW_COPPER - ITEM_FIRST] = {.name = "Raw copper",
                                   .stack = 64,
                                   .block = B_AIR,
                                   .tex = T_ITEM_RAW_COPPER,
                                   .desc = "Copper ore. Smelt with coal in a furnace.",
                                   .density = 4000.0f},
    [I_RAW_IRON - ITEM_FIRST] = {.name = "Raw iron",
                                 .stack = 64,
                                 .block = B_AIR,
                                 .tex = T_ITEM_RAW_IRON,
                                 .desc = "Iron ore. Smelt with coal in a furnace.",
                                 .density = 5000.0f},
    [I_RAW_MEAT - ITEM_FIRST] = {.name = "Raw meat",
                                 .stack = 16,
                                 .block = B_AIR,
                                 .tex = T_ITEM_RAW_MEAT,
                                 .desc = "Better cooked over a fire. E to eat.",
                                 .density = 1050.0f},
    [I_COOKED_MEAT - ITEM_FIRST] = {.name = "Cooked meat",
                                    .stack = 16,
                                    .block = B_AIR,
                                    .tex = T_ITEM_COOKED_MEAT,
                                    .desc = "350 kcal. E to eat.",
                                    .density = 1000.0f},
    [I_GUNPOWDER - ITEM_FIRST] = {.name = "Gunpowder",
                                  .stack = 64,
                                  .block = B_AIR,
                                  .tex = T_ITEM_GUNPOWDER,
                                  .desc = "Coal and the potash in ash. For cartridges.",
                                  .density = 1700.0f},
    [I_CARTRIDGE - ITEM_FIRST] = {.name = "Cartridge",
                                  .stack = 32,
                                  .block = B_AIR,
                                  .tex = T_ITEM_CARTRIDGE,
                                  .desc = "Rifle ammunition.",
                                  .density = 5000.0f},
    [I_RIFLE - ITEM_FIRST] = {.name = "Rifle",
                              .stack = 1,
                              .block = B_AIR,
                              .tex = T_ITEM_RIFLE,
                              .desc = "Right-click to fire. Uses cartridges.",
                              .density = 900.0f},
    [I_ADRENALINE - ITEM_FIRST] = {.name = "Adrenaline auto-injector",
                                   .stack = 4,
                                   .block = B_AIR,
                                   .tex = T_ITEM_ADRENALINE,
                                   .desc = "0.3 mg. For a stopped heart (H, then N); risky on a beating one.",
                                   .density = 1100.0f},
};

static const item_def UNKNOWN = {
    .name = "?", .desc = "", .density = 1000.0f, .stack = 1, .block = B_AIR, .tex = T_STONE};

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
            g_tools[i] = (item_def){.name = names[i],
                                    .desc = descs[i],
                                    .density = DENS[t],
                                    .stack = 1,
                                    .block = B_AIR,
                                    .tex = (uint8_t)(T_TOOL_FIRST + i)};
        }
}

float item_food(int id, float *water)
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

int item_durability(int id)
{
    int kind = 0, tier = 0;
    if (id == I_RIFLE) return 250; /* shots before the barrel is shot out (wear is a byte) */
    return item_tool(id, &kind, &tier) ? TIER_USES[tier] : 0;
}

int block_tool(uint8_t block)
{
    switch (block) {
    case B_STONE:
    case B_ROCK:
    case B_BRICK:
    case B_FURNACE:
    case B_COAL_ORE:
    case B_COPPER_ORE:
    case B_IRON_ORE:
    case B_ICE: return TOOL_PICKAXE;
    case B_LOG:
    case B_PLANKS:
    case B_CAMPFIRE: return TOOL_AXE;
    case B_DIRT:
    case B_GRASS:
    case B_SAND:
    case B_GRAVEL:
    case B_SNOW:
    case B_ASH: return TOOL_SHOVEL;
    default: return -1;
    }
}

/* The pickaxe tier a block needs before it yields anything, -1 for none. */
static int pick_tier(uint8_t block)
{
    switch (block) {
    case B_STONE:
    case B_ROCK:
    case B_BRICK:
    case B_FURNACE:
    case B_COAL_ORE: return TIER_WOOD;
    case B_COPPER_ORE:
    case B_IRON_ORE: return TIER_STONE;
    default: return -1;
    }
}

int item_harvests(int held, uint8_t block)
{
    int need = pick_tier(block), kind = 0, tier = 0;
    if (need < 0) return 1;
    return item_tool(held, &kind, &tier) && kind == TOOL_PICKAXE && tier >= need;
}

float item_dig_speed(int held, uint8_t block)
{
    int kind = 0, tier = 0;
    if (item_tool(held, &kind, &tier) && kind == block_tool(block)) return TIER_SPEED[tier];
    /* Bare hands (or the wrong tool) on rock: a third of the speed, and
     * item_harvests says it yields nothing. */
    return pick_tier(block) >= 0 ? 1.0f / 3.0f : 1.0f;
}

int item_valid(int id)
{
    /* Rock and ore only exist in place: breaking them gives stone, coal or
     * raw metal, so they never become building blocks. */
    if (item_is_block(id))
        return id < B_COUNT && id != B_WATER && id != B_BEDROCK && id != B_ROCK && id != B_COAL_ORE &&
               id != B_COPPER_ORE && id != B_IRON_ORE;
    return id >= ITEM_FIRST && id < ITEM_END;
}

const item_def *item_get(int id)
{
    static item_def blocks[B_COUNT];
    static int built;
    if (!built) {
        for (int b = 0; b < B_COUNT; b++) {
            const block_def *d = &g_blocks[b];
            blocks[b] = (item_def){
                .name = d->name, .desc = "", .density = d->density, .stack = 64, .block = (uint8_t)b, .tex = d->tex[0]};
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
        build_tools();
        built = 1;
    }
    if (item_is_block(id) && id < B_COUNT) return &blocks[id];
    if (id >= I_TOOL_FIRST && id < ITEM_END) return &g_tools[id - I_TOOL_FIRST];
    if (id >= ITEM_FIRST && id < I_TOOL_FIRST) return &ITEMS[id - ITEM_FIRST];
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
        /* Torn apart by hand: fibre about half the time, sometimes an apple. */
        if (chance(rng, 1, 2)) out[n++] = (item_stack){I_FIBRE, 1, 0};
        if (chance(rng, 1, 6)) out[n++] = (item_stack){I_APPLE, 1, 0};
        break;
    case B_GRASS: out[n++] = (item_stack){B_DIRT, 1, 0}; break;
    case B_ROCK: out[n++] = (item_stack){B_STONE, 1, 0}; break;
    case B_COAL_ORE: out[n++] = (item_stack){I_COAL, (uint8_t)(1 + chance(rng, 1, 3)), 0}; break;
    case B_COPPER_ORE: out[n++] = (item_stack){I_RAW_COPPER, 1, 0}; break;
    case B_IRON_ORE: out[n++] = (item_stack){I_RAW_IRON, 1, 0}; break;
    case B_GLASS: break; /* shatters */
    case B_CAMPFIRE: out[n++] = (item_stack){I_STICK, 2, 0}; break; /* the unburnt ends */
    case B_WATER:
    case B_AIR:
    case B_BEDROCK: break;
    default:
        if (block < B_COUNT) out[n++] = (item_stack){block, 1, 0};
        break;
    }
    return n;
}
