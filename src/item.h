/* Items: everything that fits in the inventory. Ids below ITEM_FIRST are
 * the blocks themselves (a stone item places stone); ids from ITEM_FIRST
 * up are tools, materials, food and medical supplies. */
#ifndef MC_ITEM_H
#define MC_ITEM_H

#include <stdint.h>
#include "block.h"

enum {
    I_NONE = 0,
    ITEM_FIRST = 64,
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
    ITEM_END
};

typedef struct {
    uint8_t id;
    uint8_t count;
} item_stack;

typedef struct {
    const char *name;
    uint8_t stack;       /* most per slot */
    uint8_t block;       /* block it places, B_AIR if none */
    uint8_t tex;         /* texture layer: the sprite (items) or the side face (blocks) */
    const char *desc;    /* one-line tooltip */
    float density;       /* kg/m^3 of a dropped item: floats below 1000 */
} item_def;

/* 1 for ids that exist as items (not air, loose water or bedrock). */
int item_valid(int id);
/* Never NULL: unknown ids get a placeholder. */
const item_def *item_get(int id);
static inline int item_is_block(int id) { return id > 0 && id < ITEM_FIRST; }

/* What breaking a block by hand yields; rng is advanced. Returns the
 * number of stacks written to out (at most 3). */
int item_drops(uint8_t block, uint32_t *rng, item_stack out[3]);

#endif
