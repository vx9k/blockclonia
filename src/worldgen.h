/* The climate and landscape of the third generator, as functions of a
 * block's (x, z): the mean air temperature and yearly rainfall, the ground
 * height and the biome they make. The terrain, the trees and the heat
 * model (thermo.c) all read this one field, so a tundra is cold to stand
 * in, not only white. It does not build columns (worldgen_column in
 * world.h does) or keep any state: every call starts from the seed. */
#ifndef MC_WORLDGEN_H
#define MC_WORLDGEN_H

#include <stdint.h>

/* Air cools by 0.11 K per metre of height above the sea: the real 6.5 K/km
 * squeezed (like thermo's older scale) so the 200 m the terrain rises
 * covers what a few kilometres would. A temperate 12 C at the sea leaves
 * the peaks of a range below 0 C, where snow lies. */
#define CLIMATE_LAPSE 0.11
/* The squeezed lapse holds up to here; above it the air cools at the real
 * rate (towers only: the terrain stays under 250 m). */
#define CLIMATE_TOP 256

/* Biomes, from temperature and rainfall (a Whittaker diagram) and height. */
typedef enum {
    BIOME_OCEAN,
    BIOME_BEACH,
    BIOME_TUNDRA,  /* too cold for trees: grass, snow in the far north */
    BIOME_TAIGA,   /* cold and wet: dense conifer forest */
    BIOME_FOREST,  /* temperate and wet: mixed forest */
    BIOME_MEADOW,  /* temperate: grass with scattered trees */
    BIOME_STEPPE,  /* dry grassland */
    BIOME_SCRUB,   /* warm and dry: patchy grass and bare soil */
    BIOME_DESERT,  /* hot and dry: sand and dunes */
    BIOME_WETLAND, /* very wet low ground */
    BIOME_ALPINE,  /* grass above the tree line */
    BIOME_ROCKY,   /* bare rock above the alpine meadows */
    BIOME_SNOW,    /* snowfields and glaciers */
    BIOME_COUNT
} biome;

typedef struct {
    int height;    /* the first block above the ground: air, or water up to level */
    int level;     /* the first air block above the sea, a river or a lake here; 0: dry */
    float temp;    /* mean air temperature at the ground, C */
    float rain;    /* yearly rainfall, 0 (desert) to 1 (wettest) */
    biome b;
} worldgen_point;

/* The ground, its climate and its biome at (x, z), exactly as generator 3
 * builds the column there. A few dozen noise samples: fine for spawn
 * searches and tests, not for filling columns. */
worldgen_point worldgen_sample(uint32_t seed, int x, int z);
/* Mean air temperature at sea level over (x, z), C: what thermo starts its
 * lapse from. Cheap (a few noise samples) and smooth across kilometres. */
float worldgen_sea_temp(uint32_t seed, int x, int z);
const char *biome_name(biome b);

#endif
