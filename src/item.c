#include "item.h"

static const item_def ITEMS[ITEM_END - ITEM_FIRST] = {
    [I_STICK - ITEM_FIRST] = {.name = "Stick", .stack = 64, .block = B_AIR, .tex = T_ITEM_STICK,
                            .desc = "Fuel, splints. 2 planks make 4.", .density = 600.0f},
    [I_FIBRE - ITEM_FIRST] = {.name = "Plant fibre", .stack = 64, .block = B_AIR, .tex = T_ITEM_FIBRE,
                            .desc = "From leaves. 2 make a rough bandage.", .density = 300.0f},
    [I_BANDAGE - ITEM_FIRST] = {.name = "Sterile bandage", .stack = 16, .block = B_AIR, .tex = T_ITEM_BANDAGE,
                            .desc = "Pressure dressing (H, then B).", .density = 250.0f},
    [I_SPLINT - ITEM_FIRST] = {.name = "Splint", .stack = 16, .block = B_AIR, .tex = T_ITEM_SPLINT,
                            .desc = "Holds a broken limb (H, then S).", .density = 600.0f},
    [I_ANTISEPTIC - ITEM_FIRST] = {.name = "Antiseptic", .stack = 16, .block = B_AIR, .tex = T_ITEM_ANTISEPTIC,
                            .desc = "Cleans wounds (H, then D).", .density = 1050.0f},
    [I_PAINKILLER - ITEM_FIRST] = {.name = "Painkillers", .stack = 32, .block = B_AIR, .tex = T_ITEM_PAINKILLER,
                            .desc = "6 h of relief (H, then P).", .density = 1200.0f},
    [I_ANTIBIOTIC - ITEM_FIRST] = {.name = "Antibiotics", .stack = 16, .block = B_AIR, .tex = T_ITEM_ANTIBIOTIC,
                            .desc = "Treats infection (H, then A).", .density = 1200.0f},
    [I_APPLE - ITEM_FIRST] = {.name = "Apple", .stack = 32, .block = B_AIR, .tex = T_ITEM_APPLE,
                            .desc = "95 kcal. E to eat.", .density = 800.0f},
    [I_BUCKET - ITEM_FIRST] = {.name = "Bucket", .stack = 1, .block = B_AIR, .tex = T_ITEM_BUCKET,
                            .desc = "Right-click water to fill.", .density = 550.0f},
    [I_WATER_BUCKET - ITEM_FIRST] = {.name = "Water bucket", .stack = 1, .block = B_WATER, .tex = T_ITEM_WATER_BUCKET,
                            .desc = "Right-click to pour; R to drink.", .density = 1080.0f},
};

static const item_def UNKNOWN = {.name = "?", .desc = "", .density = 1000.0f, .stack = 1, .block = B_AIR, .tex = T_STONE};

int item_valid(int id)
{
    if (item_is_block(id)) return id < B_COUNT && id != B_WATER && id != B_BEDROCK;
    return id >= ITEM_FIRST && id < ITEM_END;
}

const item_def *item_get(int id)
{
    static item_def blocks[B_COUNT];
    static int built;
    if (!built) {
        for (int b = 0; b < B_COUNT; b++) {
            const block_def *d = &g_blocks[b];
            blocks[b] = (item_def){.name = d->name, .desc = "", .density = d->density, .stack = 64, .block = (uint8_t)b,
                                   .tex = d->tex[0]};
        }
        blocks[B_GLASS].desc = "Brittle: shatters when it falls hard.";
        blocks[B_ICE].desc = "Melts above 0" "\x7f" "C.";
        blocks[B_SNOW].desc = "Melts into a little water.";
        blocks[B_SAND].desc = "Needs support from below.";
        blocks[B_GRAVEL].desc = "Needs support from below.";
        blocks[B_CAMPFIRE].desc = "Heat, light and cooking. Feed it sticks, planks or logs.";
        blocks[B_ASH].desc = "What is left of a fire.";
        built = 1;
    }
    if (item_is_block(id) && id < B_COUNT) return &blocks[id];
    if (id >= ITEM_FIRST && id < ITEM_END) return &ITEMS[id - ITEM_FIRST];
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
        if (chance(rng, 1, 2)) out[n++] = (item_stack){I_FIBRE, 1};
        if (chance(rng, 1, 6)) out[n++] = (item_stack){I_APPLE, 1};
        break;
    case B_GRASS: out[n++] = (item_stack){B_DIRT, 1}; break;
    case B_GLASS: break; /* shatters */
    case B_CAMPFIRE: out[n++] = (item_stack){I_STICK, 2}; break; /* the unburnt ends */
    case B_WATER:
    case B_AIR:
    case B_BEDROCK: break;
    default:
        if (block < B_COUNT) out[n++] = (item_stack){block, 1};
        break;
    }
    return n;
}
