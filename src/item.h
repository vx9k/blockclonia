/* Items: everything that fits in the inventory. Ids below ITEM_FIRST are
 * the blocks themselves (a stone item places stone); ids from ITEM_FIRST
 * up are tools, materials, food and medical supplies. Item ids are 32
 * bits, so there is room for every part a factory or vehicle will want;
 * blocks keep one byte each in the world. */
#ifndef MC_ITEM_H
#define MC_ITEM_H

#include "block.h"
#include <stdint.h>

#define I_NONE 0

typedef uint32_t item_id;
#define ITEM_FIRST 256 /* below: block ids, one byte in the world */

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
    I_RAW_MEAT,
    I_COOKED_MEAT,
    I_GUNPOWDER,
    I_CARTRIDGE,
    I_RIFLE,
    I_ADRENALINE,
    I_MORPHINE,
    I_NALOXONE,
    I_SPIRITS,
    I_TOOL_FIRST /* then TOOL_KINDS x TIER_COUNT tools, kind-major, to I_TOOL_END */
};

#define I_TOOL_END (I_TOOL_FIRST + 5 * 4)

/* Minerals and metals, after the tools so the ids saved before them hold.
 * Each has the texture layer T_ITEM_RAW_TIN + (id - I_RAW_TIN). */
enum {
    I_RAW_TIN = I_TOOL_END, /* cassiterite */
    I_RAW_ZINC,             /* sphalerite */
    I_RAW_LEAD,             /* galena */
    I_RAW_NICKEL,           /* pentlandite */
    I_RAW_SILVER,
    I_RAW_GOLD,
    I_BAUXITE,              /* aluminium ore */
    I_ILMENITE,             /* titanium ore */
    I_URANINITE,            /* uranium ore */
    I_SPODUMENE,            /* lithium ore */
    I_SULFUR,
    I_SALTPETER,
    I_SALT,
    I_QUARTZ,
    I_GRAPHITE,
    I_TIN_INGOT,
    I_ZINC_INGOT,
    I_LEAD_INGOT,
    I_NICKEL_INGOT,
    I_SILVER_INGOT,
    I_GOLD_INGOT,
    I_BRONZE_INGOT,
    I_BRASS_INGOT,
    I_STEEL_INGOT,
    I_CHARCOAL,
    I_QUICKLIME,
    I_MINERALS_END
};
_Static_assert(I_MINERALS_END - I_RAW_TIN == T_ITEM_MINERALS_END - T_ITEM_RAW_TIN, "one texture per mineral item");

#define ITEM_END I_MINERALS_END

/* Tools and blades come in kinds and tiers; the item id is
 * I_TOOL_FIRST + kind * TIER_COUNT + tier. */
enum { TOOL_PICKAXE, TOOL_AXE, TOOL_SHOVEL, TOOL_KNIFE, TOOL_SWORD, TOOL_KINDS };
enum { TIER_WOOD, TIER_STONE, TIER_COPPER, TIER_IRON, TIER_COUNT };
_Static_assert((TOOL_KINDS * TIER_COUNT) == (I_TOOL_END - I_TOOL_FIRST), "one item per tool kind and tier");
_Static_assert((TOOL_KINDS * TIER_COUNT) == (T_COUNT - T_TOOL_FIRST), "one texture per tool kind and tier");

static inline item_id item_tool_id(int kind, int tier) { return (item_id)(I_TOOL_FIRST + kind * TIER_COUNT + tier); }
/* 1 if id is a tool or blade, with its kind and tier. */
static inline int item_tool(item_id id, int *kind, int *tier)
{
    if (id < I_TOOL_FIRST || id >= I_TOOL_END) return 0;
    *kind = (int)(id - I_TOOL_FIRST) / TIER_COUNT;
    *tier = (int)(id - I_TOOL_FIRST) % TIER_COUNT;
    return 1;
}

typedef struct {
    uint32_t id;
    uint8_t count;
    uint8_t wear;        /* uses spent, for items with a durability; else 0 */
} item_stack;

/* An amount of an item, without the state a stack carries (recipes). */
typedef struct {
    uint32_t id;
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
int item_valid(item_id id);
/* Never NULL: unknown ids get a placeholder. */
const item_def *item_get(item_id id);
static inline int item_is_block(item_id id) { return id > 0 && id < ITEM_FIRST; }

/* Food energy (kcal) and water (litres) of one, 0 kcal if it is not food. */
float item_food(item_id id, float *water);

/* Uses before an item wears out (tools and blades), 0 for items that do
 * not wear. */
int item_durability(item_id id);
/* Breaking speed with this item held, as a multiple of the bare hand. */
float item_dig_speed(item_id held, uint8_t block);
/* The tool a block is worked with (TOOL_PICKAXE, _AXE, _SHOVEL), or -1. */
int block_tool(uint8_t block);
/* 1 if breaking the block with this item held yields anything: rock, ore
 * and masonry need a pickaxe (copper and iron ore one of stone or better). */
int item_harvests(item_id held, uint8_t block);

/* What breaking a block yields; rng is advanced. Returns the number of
 * stacks written to out (at most 3). */
int item_drops(uint8_t block, uint32_t *rng, item_stack out[3]);

#endif
