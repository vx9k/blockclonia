/* Trees, grown into a box of their own and then copied into a column.
 *
 * Every kind of tree grows its own way: a trunk, limbs that leave it and
 * climb, and pads of leaves on their ends, as an oak spreads, a spruce
 * stacks tiers and a palm leans and drops its fronds. A tree is a pure
 * function of its kind, age and hash, so each column a tree reaches grows
 * the same tree and keeps the blocks that fall inside it: trees cross
 * column edges without either column seeing the other.
 *
 * Before it is placed a tree is checked with the structure solver's rules
 * (physics.c): every block must be held by the trunk within its span, as
 * when a player digs nearby, and what would fall is left out. Where trees
 * grow and of what kind is the generator's business (worldgen.c). */
#ifndef MC_TREES_H
#define MC_TREES_H

#include "block.h"
#include "world.h"

#include <stdint.h>

/* How far from its root a tree reaches across, and the box it grows in:
 * TREE_DOWN blocks below the ground (roots) to TREE_UP above. */
#define TREE_REACH 8
#define TREE_DOWN 3
#define TREE_UP 38
#define TREE_W (2 * TREE_REACH + 1)
#define TREE_H (TREE_DOWN + TREE_UP)
#define TREE_BOX (TREE_W * TREE_W * TREE_H)

typedef enum {
    AGE_YOUNG,
    AGE_GROWN,
    AGE_OLD,
    AGE_SNAG,   /* dead and broken off */
    AGE_FALLEN, /* a stump, its trunk lying beside it */
    AGE_COUNT
} tree_age;

/* A grown tree: block ids (B_AIR where nothing grew) and log axes, local
 * to its root, the first cell above the ground. */
typedef struct {
    uint8_t id[TREE_BOX];
    uint8_t meta[TREE_BOX];
    int blocks; /* how many cells are not air */
} tree_shape;

static inline int tree_cell(int x, int y, int z)
{
    return ((y + TREE_DOWN) * TREE_W + z + TREE_REACH) * TREE_W + x + TREE_REACH;
}

/* Where a fallen trunk lies: from `from` to `to` cells out along (dx, dz)
 * from the root, on the ground's level. The generator checks that ground
 * before asking for AGE_FALLEN. */
typedef struct {
    int dx, dz, from, to;
} tree_fall;

tree_fall tree_fall_of(uint32_t h);

/* Grows a tree of kind and age from hash h. vines hangs vines from its
 * crown (warm, wet woods). */
void tree_grow(tree_shape *t, int kind, tree_age age, uint32_t h, int vines);

/* Copies the part of t rooted at world (x, y, z) that falls in column c:
 * leaves and vines only into air, wood into air or leaves, and roots into
 * the soil under the ground. */
void tree_place(const tree_shape *t, column *c, int x, int y, int z);

#endif
