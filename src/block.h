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
    B_CAMPFIRE,  /* burns its fuel (meta) and heats its surroundings */
    B_ASH,       /* what a campfire leaves */
    B_COAL_ORE,
    B_COPPER_ORE,
    B_IRON_ORE,
    B_FURNACE,   /* a crafting station for smelting; not a heat source itself */
    B_ROCK,      /* stone in place, as generated: one mass, so caves hold up */
    /* Rocks, each loose (what breaking it gives, and what a builder sets)
     * and in place (one mass with the ground around it, like B_ROCK). */
    B_GRANITE,
    B_GRANITE_ROCK,
    B_BASALT,
    B_BASALT_ROCK,
    B_LIMESTONE,
    B_LIMESTONE_ROCK,
    B_SANDSTONE,
    B_SANDSTONE_ROCK,
    B_SHALE,
    B_SHALE_ROCK,
    B_MARBLE,
    B_MARBLE_ROCK,
    B_OBSIDIAN,
    B_OBSIDIAN_ROCK,
    B_CLAY,
    B_MUD,
    B_PEAT,
    B_PODZOL,
    /* Ores and mineral deposits, in place only: they break into items. */
    B_TIN_ORE,
    B_ZINC_ORE,
    B_LEAD_ORE,
    B_NICKEL_ORE,
    B_SILVER_ORE,
    B_GOLD_ORE,
    B_BAUXITE,
    B_TITANIUM_ORE,
    B_URANIUM_ORE,
    B_LITHIUM_ORE,
    B_SULFUR_ORE,
    B_SALTPETER_ORE,
    B_ROCK_SALT,
    B_QUARTZ_ORE,
    B_GRAPHITE_ORE,
    /* Liquids besides water (fluid.c has how each flows). */
    B_LAVA,      /* molten basalt, about 1100 C */
    B_CRUDE_OIL, /* lighter than water: floats on it */
    B_BRINE,     /* salt water, heavier than fresh: sinks under it */
    B_TORCH,     /* a light on a stick; drawn as a small model, not a block */
    B_LAMP,      /* an oil lamp behind glass */
    B_FOXFIRE,   /* glowing fungus on a damp cave floor */
    /* Trees besides the oak (B_LOG, B_LEAVES): the logs, then the leaves,
     * each in the same order. */
    B_BIRCH_LOG,
    B_SPRUCE_LOG,
    B_PINE_LOG,
    B_THORN_LOG, /* the flat-crowned tree of dry country */
    B_BIRCH_LEAVES,
    B_SPRUCE_LEAVES,
    B_PINE_LEAVES,
    B_THORN_LEAVES,
    /* Ground plants: crossed quads, no collision, broken by hand. Grasses
     * and low shrubs, then the flowers. */
    B_GRASS_TUFT,
    B_TALL_GRASS,
    B_FERN,
    B_REEDS,
    B_HEATHER,
    B_DRY_SHRUB,
    B_BUSH,
    B_POPPY,
    B_DANDELION,
    B_CORNFLOWER,
    B_OXEYE,
    B_VINES, /* hangs from a branch or leaves: a plant rooted above, not below */
    /* The trees added with grown limbs (tree_kind order from TREE_WILLOW):
     * the logs, then the leaves. */
    B_WILLOW_LOG,
    B_MAPLE_LOG,
    B_CHERRY_LOG,
    B_REDWOOD_LOG,
    B_PALM_LOG,
    B_BAOBAB_LOG,
    B_UMBRELLA_LOG,
    B_MOUNTAIN_PINE_LOG,
    B_WILLOW_LEAVES,
    B_MAPLE_LEAVES,
    B_CHERRY_BLOSSOM,
    B_REDWOOD_LEAVES,
    B_PALM_FRONDS,
    B_BAOBAB_LEAVES,
    B_UMBRELLA_LEAVES,
    B_MOUNTAIN_PINE_LEAVES,
    B_COUNT
} block_id;

/* The kinds of tree. Each has a log and leaves (g_tree_log, g_tree_leaves)
 * and its own texture layers (tree_tex_*). */
typedef enum {
    TREE_OAK,
    TREE_BIRCH,
    TREE_SPRUCE,
    TREE_PINE, /* the Scots pine */
    TREE_THORN, /* the flat-crowned tree of dry country */
    TREE_WILLOW,
    TREE_MAPLE,
    TREE_CHERRY,
    TREE_REDWOOD,
    TREE_PALM,
    TREE_BAOBAB,
    TREE_UMBRELLA, /* the umbrella pine of warm, dry hills */
    TREE_MOUNTAIN_PINE,
    TREE_KINDS
} tree_kind;

/* Bark comes in windows one block tall, cut from one strip TREE_BARK_WINDOWS
 * blocks tall that tiles end to end: block y of a trunk shows window
 * (y + the trunk's phase) % TREE_BARK_WINDOWS, so the furrows run on
 * unbroken up the trunk and no two trunks line up. */
#define TREE_BARK_WINDOWS 4

/* Sentinel returned for positions in chunks that are not loaded. It is
 * solid for collisions (nothing falls into unloaded terrain) and anchors
 * structures. It is never stored. */
#define B_UNLOADED 0xFF

enum {
    BF_SOLID = 1 << 0, /* collides with bodies */
    BF_OPAQUE = 1 << 1, /* hides neighbouring faces */
    BF_TRANSLUCENT = 1 << 2, /* drawn in the blended pass */
    BF_GRANULAR = 1 << 3, /* only supported from directly below (sand) */
    BF_FLUID = 1 << 4,
    BF_ANCHOR = 1 << 5, /* never falls (bedrock) */
    BF_BRITTLE = 1 << 6, /* shatters instead of landing when it hits hard */
    BF_BURNING = 1 << 7, /* a heat source, drawn at full brightness */
};

/* Texture layers in the block texture array. */
typedef enum { // NOLINT(cert-int09-c): the trees' runs are sized from TREE_KINDS
    T_STONE,
    T_DIRT,
    T_GRASS_TOP,
    T_GRASS_SIDE,
    T_SAND,
    T_GRAVEL,
    T_PLANKS,
    T_GLASS,
    T_BRICK,
    T_SNOW,
    T_ICE,
    T_WATER,
    T_BEDROCK,
    /* Skin, and the undyed linen of the players' base layer (avatar.c). */
    T_SKIN,
    T_LINEN,
    /* Break progress overlay, four stages (transparent between cracks). */
    T_CRACK0,
    T_CRACK1,
    T_CRACK2,
    T_CRACK3,
    /* Flames, four frames the block shader cycles through. */
    T_FIRE0,
    T_FIRE1,
    T_FIRE2,
    T_FIRE3,
    T_CAMPFIRE_BASE,
    T_ASH,
    /* Layers added later go last, so the ones the shaders animate keep
     * their numbers. */
    T_COAL_ORE,
    T_COPPER_ORE,
    T_IRON_ORE,
    T_FURNACE_FRONT,
    T_FURNACE_TOP,
    /* The creatures' pixel art (mobskin.c), one layer per kind of face. */
    T_WOLF_BODY_SIDE,
    T_WOLF_BODY_TOP,
    T_WOLF_BELLY,
    T_WOLF_MANE_SIDE,
    T_WOLF_MANE_FRONT,
    T_WOLF_HEAD_SIDE,
    T_WOLF_HEAD_FRONT,
    T_WOLF_HEAD_TOP,
    T_WOLF_SNOUT_SIDE,
    T_WOLF_SNOUT_FRONT,
    T_WOLF_EAR,
    T_WOLF_TAIL_SIDE,
    T_WOLF_TAIL_TOP,
    T_WOLF_LEG_UPPER,
    T_WOLF_LEG_LOWER,
    T_WOLF_FUR,
    T_BOAR_BODY_SIDE,
    T_BOAR_BODY_TOP,
    T_BOAR_BELLY,
    T_BOAR_RIDGE,
    T_BOAR_HEAD_SIDE,
    T_BOAR_HEAD_FRONT,
    T_BOAR_HEAD_TOP,
    T_BOAR_SNOUT_SIDE,
    T_BOAR_SNOUT_FRONT,
    T_BOAR_EAR,
    T_BOAR_TAIL,
    T_BOAR_LEG_UPPER,
    T_BOAR_LEG_LOWER,
    T_BOAR_FUR,
    T_MOB_SOLE, /* paw and hoof undersides */
    T_GRANITE,
    T_BASALT,
    T_LIMESTONE,
    T_SANDSTONE,
    T_SHALE,
    T_MARBLE,
    T_OBSIDIAN,
    T_CLAY,
    T_MUD,
    T_PEAT,
    T_PODZOL,
    T_TIN_ORE, /* then one per ore block, in block order, to T_GRAPHITE_ORE */
    T_ZINC_ORE,
    T_LEAD_ORE,
    T_NICKEL_ORE,
    T_SILVER_ORE,
    T_GOLD_ORE,
    T_BAUXITE,
    T_TITANIUM_ORE,
    T_URANIUM_ORE,
    T_LITHIUM_ORE,
    T_SULFUR_ORE,
    T_SALTPETER_ORE,
    T_ROCK_SALT,
    T_QUARTZ_ORE,
    T_GRAPHITE_ORE,
    /* The players' figure (avatar.c): the shorts' heavier linen, hair, and
     * the face, which is a front layer and so stays under 128. */
    T_LINEN_DARK,
    T_HAIR,
    T_FACE,
    T_LAVA,
    T_CRUDE_OIL,
    T_BRINE,
    T_ITEM_TORCH,
    T_LAMP,
    T_FOXFIRE,
    /* Item model materials (itemmodel.h): greyscale and near white, so the
     * box's tint is its colour, and stretched over each face. */
    T_MAT_WOOD,
    T_MAT_METAL,
    T_MAT_CAST,
    T_MAT_ROCK,
    T_MAT_CLOTH,
    T_MAT_GLASS,
    T_MAT_PLAIN,
    T_MAT_FLESH,
    T_MAT_GRAIN,
    /* Powder smoke in three steps of thinning (fx.c), and the muzzle flash
     * and tracers' burning white. */
    T_SMOKE0,
    T_SMOKE1,
    T_SMOKE2,
    T_FLASH,
    /* The trees, every kind in tree_kind order in each run: the bark's
     * windows, the bark turned on its side (on a fallen trunk), the bark
     * under moss, the end grain, then the leaves. One run of layers, so the
     * shaders tint it as one range, and the leaves together, so they sway
     * as one. */
    T_BARK_FIRST,
    T_SIDEWAYS_FIRST = T_BARK_FIRST + (int)TREE_KINDS * TREE_BARK_WINDOWS,
    T_MOSSY_FIRST = T_SIDEWAYS_FIRST + (int)TREE_KINDS,
    T_END_FIRST = T_MOSSY_FIRST + (int)TREE_KINDS,
    T_TREE_LEAVES_FIRST = T_END_FIRST + (int)TREE_KINDS,
    T_TREE_LEAVES_LAST = T_TREE_LEAVES_FIRST + (int)TREE_KINDS - 1,
    /* The ground plants, in their blocks' order, and the vines: together,
     * so the shaders sway them as one range. */
    T_GRASS_TUFT,
    T_TALL_GRASS,
    T_FERN,
    T_REEDS,
    T_HEATHER,
    T_DRY_SHRUB,
    T_BUSH,
    T_POPPY,
    T_DANDELION,
    T_CORNFLOWER,
    T_OXEYE,
    T_VINES,
    T_COUNT
} tex_id;

_Static_assert(T_COUNT <= 256, "a texture layer is one byte in block_def and the vertex");
#define T_MOB_FIRST T_WOLF_BODY_SIDE
#define T_MOB_LAST T_MOB_SOLE

#define T_FIRE_FRAMES 4
#define T_TREE_FIRST T_BARK_FIRST
#define T_TREE_LAST T_TREE_LEAVES_LAST
#define T_PLANT_FIRST T_GRASS_TUFT
#define T_PLANT_LAST T_VINES

static inline int tree_tex_bark(int kind, int window) { return T_BARK_FIRST + kind * TREE_BARK_WINDOWS + window; }
static inline int tree_tex_sideways(int kind) { return T_SIDEWAYS_FIRST + kind; }
static inline int tree_tex_mossy(int kind) { return T_MOSSY_FIRST + kind; }
static inline int tree_tex_end(int kind) { return T_END_FIRST + kind; }
static inline int tree_tex_leaves(int kind) { return T_TREE_LEAVES_FIRST + kind; }

/* What a block is made of, as far as tools and sound care: the tool that
 * digs it and the noise it makes. */
typedef enum {
    BM_NONE,
    BM_GRASS,
    BM_DIRT,
    BM_STONE, /* dug with a pickaxe, which it needs a tier of */
    BM_WOOD,
    BM_SAND,
    BM_GRAVEL,
    BM_SNOW,
    BM_LEAVES,
    BM_GLASS,
    BM_ICE,
    BM_WATER,
} block_mat;

typedef struct {
    const char *name;
    uint8_t flags;
    uint8_t tex[6];     /* per face: +X -X +Y -Y +Z -Z */
    int8_t span;        /* max horizontal cantilever in blocks */
    float density;      /* kg/m^3 */
    float friction;     /* Coulomb friction coefficient against a shoe sole */
    float restitution;  /* fraction of impact speed returned in a bounce */
    float hardness;     /* s to break by hand; < 0: unbreakable */
    float heat_capacity;/* J/(kg K) */
    float conductivity; /* W/(m K) */
    uint8_t mat;        /* block_mat */
    float cushion;      /* share of a landing's speed that counts; soft ground is < 1 */
    float grit;         /* how much grit it grinds into scraped skin, 0..1 */
    uint8_t loose;      /* rock in place: the block it breaks into; 0 if it comes away as itself */
    uint8_t tier;       /* the pickaxe tier (item.h TIER_*) rock and ore need to yield anything */
} block_def;

extern const block_def g_blocks[B_COUNT];

/* Properties of a block. Unknown ids read as stone, so a damaged save can
 * never index past the table. */
static inline const block_def *block_get(uint8_t id)
{
    return &g_blocks[id < B_COUNT ? id : B_STONE];
}

/* Collides with the player and bodies. Unloaded space counts as solid, so
 * nothing falls out of the loaded world. */
static inline int block_solid(uint8_t id)
{
    return id == B_UNLOADED || (id < B_COUNT && (g_blocks[id].flags & BF_SOLID));
}

/* Hides the faces behind it (meshing, ambient occlusion). */
static inline int block_opaque(uint8_t id)
{
    return id < B_COUNT && (g_blocks[id].flags & BF_OPAQUE);
}

/* Liquids flow and keep their level in the cell's meta (world.h). */
static inline int block_fluid(uint8_t id) { return id < B_COUNT && (g_blocks[id].flags & BF_FLUID); }

extern const uint8_t g_tree_log[TREE_KINDS], g_tree_leaves[TREE_KINDS];
/* Per block: 1 + the kind of tree whose log or leaves it is, else 0. */
extern const uint8_t g_block_tree[B_COUNT];

/* The kind of tree a log or leaves belong to, or -1. */
static inline int block_tree(uint8_t id) { return id < B_COUNT ? (int)g_block_tree[id] - 1 : -1; }

/* A tree's trunk or branch, of any kind. */
static inline int block_log(uint8_t id) { return block_tree(id) >= 0 && g_blocks[id].mat == BM_WOOD; }

/* A ground plant: crossed quads, no collision, uprooted with its soil, or
 * vines, which go with what they hang from. */
static inline int block_plant(uint8_t id) { return id >= B_GRASS_TUFT && id <= B_VINES; }

/* A log's meta: its axis in the low bits, or LOG_BRANCH for a limb grown
 * on a tree, bark all round with no cut end; LOG_MOSS where moss grows. */
enum { LOG_UP, LOG_X, LOG_Z, LOG_BRANCH };
enum { LOG_MOSS = 4 }; /* a flag over the axis bits */

/* Leaves of any tree: drawn cut out, so the sky shows between them. */
static inline int block_leaves(uint8_t id) { return id < B_COUNT && g_blocks[id].mat == BM_LEAVES && !block_plant(id); }

/* Blocks that keep a 0..7 value in the column's meta array: liquids (their
 * level, 0 = full), campfires (fuel left, 0 = full) and logs (LOG_*). */
static inline int block_has_meta(uint8_t id) { return block_fluid(id) || id == B_CAMPFIRE || block_log(id); }

#define CAMPFIRE_FUEL_MAX 8 /* meta 0 = 8 units; one unit burns for a survival-clock hour */

#endif
