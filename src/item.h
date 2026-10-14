/* Items: everything that fits in the inventory. Ids below ITEM_FIRST are
 * the blocks themselves (a stone item places stone); ids from ITEM_FIRST
 * up are tools, materials, food and medical supplies. */
#ifndef MC_ITEM_H
#define MC_ITEM_H

#include "block.h"
#include <stdint.h>

#define I_NONE 0
#define ITEM_FIRST 64 /* below: block ids */

enum {
    I_STICK = ITEM_FIRST,
    I_FIBRE,
    I_BANDAGE,
    I_SPLINT,
    I_ANTISEPTIC,
    I_PAINKILLER,
    I_ANTIBIOTIC,
    I_APPLE,
    I_BUCKET,
    I_WATER_BUCKET,
    I_DEFIBRILLATOR,
    I_COAL,
    I_COPPER_INGOT,
    I_IRON_INGOT,
    I_RAW_COPPER,
    I_RAW_IRON,
    I_TOOL_FIRST,                                   /* TOOL_KINDS x TIER_COUNT tools, kind-major */
    ITEM_END = I_TOOL_FIRST + 5 * 4
};

/* Tools and blades come in kinds and tiers; the item id is
 * I_TOOL_FIRST + kind * TIER_COUNT + tier. */
enum { TOOL_PICKAXE, TOOL_AXE, TOOL_SHOVEL, TOOL_KNIFE, TOOL_SWORD, TOOL_KINDS };
enum { TIER_WOOD, TIER_STONE, TIER_COPPER, TIER_IRON, TIER_COUNT };
_Static_assert(TOOL_KINDS * TIER_COUNT == ITEM_END - I_TOOL_FIRST, "one item per tool kind and tier");

static inline int item_tool_id(int kind, int tier) { return I_TOOL_FIRST + kind * TIER_COUNT + tier; }
/* 1 if id is a tool or blade, with its kind and tier. */
static inline int item_tool(int id, int *kind, int *tier)
{
    if (id < I_TOOL_FIRST || id >= ITEM_END) return 0;
    *kind = (id - I_TOOL_FIRST) / TIER_COUNT;
    *tier = (id - I_TOOL_FIRST) % TIER_COUNT;
    return 1;
}

typedef struct {
    uint8_t id;
    uint8_t count;
    uint8_t wear;        /* uses spent, for items with a durability; else 0 */
} item_stack;

/* An amount of an item, without the state a stack carries (recipes). */
typedef struct {
    uint8_t id;
    uint8_t count;
} item_count;

typedef struct {
    const char *name;
    const char *desc;    /* one-line tooltip */
    float density;       /* kg/m^3 of a dropped item: floats below 1000 */
    uint8_t stack;       /* most per slot */
    uint8_t block;       /* block it places, B_AIR if none */
    uint8_t tex;         /* texture layer: the sprite (items) or the side face (blocks) */
} item_def;

/* 1 for ids that exist as items (not air, loose water or bedrock). */
int item_valid(int id);
/* Never NULL: unknown ids get a placeholder. */
const item_def *item_get(int id);
static inline int item_is_block(int id) { return id > 0 && id < ITEM_FIRST; }

/* Uses before an item wears out (tools and blades), 0 for items that do
 * not wear. */
int item_durability(int id);
/* Breaking speed with this item held, as a multiple of the bare hand. */
float item_dig_speed(int held, uint8_t block);
/* The tool a block is worked with (TOOL_PICKAXE, _AXE, _SHOVEL), or -1. */
int block_tool(uint8_t block);
/* 1 if breaking the block with this item held yields anything: rock, ore
 * and masonry need a pickaxe (copper and iron ore one of stone or better). */
int item_harvests(int held, uint8_t block);

/* What breaking a block yields; rng is advanced. Returns the number of
 * stacks written to out (at most 3). */
int item_drops(uint8_t block, uint32_t *rng, item_stack out[3]);

#endif
