/* Block types and their physical properties. 1 block = 1 m^3. */
#ifndef MC_BLOCK_H
#define MC_BLOCK_H

#include <stdint.h>

typedef enum {
    B_AIR = 0,
    B_BEDROCK,
    B_STONE,
    B_DIRT,
    B_GRASS,
    B_SAND,
    B_GRAVEL,
    B_LOG,
    B_LEAVES,
    B_PLANKS,
    B_GLASS,
    B_BRICK,
    B_SNOW,
    B_ICE,
    B_WATER,
    B_COUNT
} block_id;

/* Sentinel returned for positions in chunks that are not loaded. It is
 * solid for collisions (nothing falls into unloaded terrain) and anchors
 * structures. It is never stored. */
#define B_UNLOADED 0xFF

enum {
    BF_SOLID       = 1 << 0, /* collides with bodies */
    BF_OPAQUE      = 1 << 1, /* hides neighbouring faces */
    BF_TRANSLUCENT = 1 << 2, /* drawn in the blended pass */
    BF_GRANULAR    = 1 << 3, /* only supported from directly below (sand) */
    BF_FLUID       = 1 << 4,
    BF_ANCHOR      = 1 << 5, /* never falls (bedrock) */
};

/* Texture layers in the block texture array. */
typedef enum {
    T_STONE, T_DIRT, T_GRASS_TOP, T_GRASS_SIDE, T_SAND, T_GRAVEL, T_LOG_SIDE,
    T_LOG_TOP, T_LEAVES, T_PLANKS, T_GLASS, T_BRICK, T_SNOW, T_ICE, T_WATER,
    T_BEDROCK, T_COUNT
} tex_id;

typedef struct {
    const char *name;
    uint8_t flags;
    uint8_t tex[6];     /* per face: +X -X +Y -Y +Z -Z */
    int8_t span;        /* max horizontal cantilever in blocks */
    float density;      /* kg/m^3 */
    float friction;     /* Coulomb friction coefficient against a shoe sole */
} block_def;

extern const block_def g_blocks[B_COUNT];

static inline const block_def *block_get(uint8_t id)
{
    return &g_blocks[id < B_COUNT ? id : B_STONE];
}

static inline int block_solid(uint8_t id)
{
    return id == B_UNLOADED || (id < B_COUNT && (g_blocks[id].flags & BF_SOLID));
}

static inline int block_opaque(uint8_t id)
{
    return id < B_COUNT && (g_blocks[id].flags & BF_OPAQUE);
}

#endif
