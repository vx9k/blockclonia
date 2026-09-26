#include "block.h"

#define ALL(t) {t, t, t, t, t, t}
#define SIDES_TOP_BOTTOM(s, t, b) {s, s, t, b, s, s}

/* Densities are real-world figures. Spans are how many blocks a material
 * can cantilever sideways before its own weight snaps it; they are a
 * stand-in for tensile strength (stone and timber strong, soil weak,
 * granular media zero). Friction coefficients are rubber-sole-on-material
 * estimates; ice is the notable outlier. */
const block_def g_blocks[B_COUNT] = {
    [B_AIR]     = {"air",     0, ALL(0), 0, 1.225f, 0.0f},
    [B_BEDROCK] = {"bedrock", BF_SOLID | BF_OPAQUE | BF_ANCHOR, ALL(T_BEDROCK), 127, 3000.0f, 0.7f},
    [B_STONE]   = {"stone",   BF_SOLID | BF_OPAQUE, ALL(T_STONE), 7, 2600.0f, 0.7f},
    [B_DIRT]    = {"dirt",    BF_SOLID | BF_OPAQUE, ALL(T_DIRT), 1, 1500.0f, 0.6f},
    [B_GRASS]   = {"grass",   BF_SOLID | BF_OPAQUE, SIDES_TOP_BOTTOM(T_GRASS_SIDE, T_GRASS_TOP, T_DIRT), 1, 1400.0f, 0.6f},
    [B_SAND]    = {"sand",    BF_SOLID | BF_OPAQUE | BF_GRANULAR, ALL(T_SAND), 0, 1600.0f, 0.5f},
    [B_GRAVEL]  = {"gravel",  BF_SOLID | BF_OPAQUE | BF_GRANULAR, ALL(T_GRAVEL), 0, 1800.0f, 0.6f},
    [B_LOG]     = {"log",     BF_SOLID | BF_OPAQUE, SIDES_TOP_BOTTOM(T_LOG_SIDE, T_LOG_TOP, T_LOG_TOP), 6, 600.0f, 0.6f},
    [B_LEAVES]  = {"leaves",  BF_SOLID | BF_OPAQUE, ALL(T_LEAVES), 3, 150.0f, 0.4f},
    [B_PLANKS]  = {"planks",  BF_SOLID | BF_OPAQUE, ALL(T_PLANKS), 6, 500.0f, 0.6f},
    [B_GLASS]   = {"glass",   BF_SOLID | BF_TRANSLUCENT, ALL(T_GLASS), 2, 2500.0f, 0.4f},
    [B_BRICK]   = {"brick",   BF_SOLID | BF_OPAQUE, ALL(T_BRICK), 5, 1900.0f, 0.7f},
    [B_SNOW]    = {"snow",    BF_SOLID | BF_OPAQUE, ALL(T_SNOW), 1, 400.0f, 0.3f},
    [B_ICE]     = {"ice",     BF_SOLID | BF_TRANSLUCENT, ALL(T_ICE), 3, 917.0f, 0.03f},
    [B_WATER]   = {"water",   BF_FLUID | BF_TRANSLUCENT, ALL(T_WATER), 0, 1000.0f, 0.0f},
};
