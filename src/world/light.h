/* Light: how much sky and coloured block light reaches every cell.
 *
 * A cell's light is 16 bits: a sky level and red, green and blue block
 * levels, 0..15 each. Sky light is 15 wherever the open sky is straight
 * above; everything else spreads from cell to cell through blocks that are
 * not opaque, losing one level per block, so light reaches at most 15
 * blocks from where it starts. Each channel spreads on its own, which
 * mixes colours the way overlapping lamps do: the brighter one wins per
 * channel.
 *
 * light_region computes a column's light from a snapshot of the column and
 * its eight neighbours (48 x 48 blocks). Nothing farther than 15 blocks
 * can reach the column or the one-block border kept around it, so the
 * result is exact, and a worker can compute it from the snapshot alone.
 * The time of day doesn't enter: the shader scales sky light by daylight.
 *
 * Light is derived from blocks: never saved or sent, and recomputed on
 * every machine. No window, GPU or world access here. */
#ifndef MC_LIGHT_H
#define MC_LIGHT_H

#include "world.h"
#include <stdint.h>

/* Packed as the GPU texel (B4G4R4A4): sky in bits 0-3, red 4-7, green
 * 8-11, blue 12-15, so the shader reads sky as alpha. */
#define LIGHT_PACK(sky, r, g, b) \
    ((uint16_t)((unsigned)(sky) | (unsigned)(r) << 4 | (unsigned)(g) << 8 | (unsigned)(b) << 12))
#define LIGHT_SKY(l) ((unsigned)(l) & 15u)
#define LIGHT_R(l) (((unsigned)(l) >> 4) & 15u)
#define LIGHT_G(l) (((unsigned)(l) >> 8) & 15u)
#define LIGHT_B(l) (((unsigned)(l) >> 12) & 15u)
#define LIGHT_MAX 15
#define LIGHT_OPEN_SKY LIGHT_PACK(15, 0, 0, 0)

/* The snapshot: three columns a side. */
#define LIGHT_REGION (3 * CHUNK_W)
/* What is kept per column: the column plus a one-block border, so a
 * section's padded texels (light_texels) need no neighbour column. */
#define LIGHT_FOOT (CHUNK_W + 2)
#define LIGHT_SEC_VOL (LIGHT_FOOT * LIGHT_FOOT * SECTION_H)
/* A section's texels for the GPU: 16^3 plus a one-cell border all round. */
#define LIGHT_TEX 18
#define LIGHT_TEX_VOL (LIGHT_TEX * LIGHT_TEX * LIGHT_TEX)

/* Index of x, z in -1..16 and y in 0..15 of one section's footprint. */
static inline int light_fidx(int x, int y, int z) { return (y * LIGHT_FOOT + (z + 1)) * LIGHT_FOOT + (x + 1); }

/* column_light, a column's light by section, is in world.h. */

/* A light source that is not a block of its own: a hot cell of the heat
 * field. Coordinates are in the snapshot (0..LIGHT_REGION-1, 0..WORLD_H-1). */
typedef struct light_emitter {
    int16_t x, y, z;
    uint16_t light; /* LIGHT_PACK(0, r, g, b) */
} light_emitter;

/* The block light a block gives off itself (sky bits 0). */
uint16_t light_emission(uint8_t id);
/* The glow of something at temp_c degrees C: dull red from about 500 C
 * (the Draper point), through orange to yellow-white; 0 below. */
uint16_t light_glow(float temp_c);
/* 1 if light passes through the block. */
int light_passes(uint8_t id);

/* Computes the light of the middle column of a snapshot.
 * blocks: LIGHT_REGION^2 * top cells, index (y * LIGHT_REGION + z) *
 * LIGHT_REGION + x; nothing at y >= top blocks light or gives it off.
 * Writes every cell of the column's footprint, LIGHT_FOOT^2 * WORLD_H
 * cells indexed like light_fidx with y running over the whole height, to
 * foot. Uses about 6 bytes per snapshot cell of scratch (jobs_scratch on
 * a worker, the heap elsewhere). */
void light_region(const uint8_t *blocks, int top, const light_emitter *extra, int extra_count, uint16_t *foot);

/* Splits a computed footprint into sections, allocating an array only
 * where a section is not uniform. Frees what out held before. Returns a
 * bit per section whose light differs from what out held. */
uint64_t light_store(const uint16_t *foot, column_light *out);
void light_free(column_light *l);
/* The light of a cell of the column's footprint (x, z in -1..16). */
uint16_t light_get(const column_light *l, int x, int y, int z);

/* A section's texels, in two steps so the second can run on a worker:
 * light_copy_texels copies the footprint plus the cells above and below
 * (a few hundred row copies, on the thread that owns the column), then
 * light_fix_texels gives every opaque cell the light of its open
 * neighbours, the dimmest per channel, so the linear filter the shader
 * samples with neither darkens the edges of every lit face nor brings
 * light through a one-block wall. blocks is the mesher's padded copy
 * of the same section (mesh_input.blocks). light_fix_texels returns 1 if
 * all texels are equal (*uniform then holds the value). */
void light_copy_texels(const column_light *l, int sy, uint16_t *out);
int light_fix_texels(const uint8_t *blocks, uint16_t *out, uint16_t *uniform);

/* How bright a cell looks, 0..1, the same curve the shader uses: each
 * level is 80% of the one above, sky light scaled by daylight. Level l of
 * 0..1 is exp2(K l - K) with K = LIGHT_CURVE_K = -15 log2(0.8), block.frag's
 * LIGHT_K; daylight d scales it as a shift of log2(d) / K in l. */
#define LIGHT_CURVE_K 4.8289214f
float light_brightness(uint16_t l, float daylight);
/* The brightness at a world position (entities, which carry one value
 * each): the light of the cell it is in. */
float light_at(const world *w, double x, double y, double z, float daylight);

/* The GPU keeps every section's texels in one 3D image of slots, each
 * LIGHT_TEX cells a side. Slot coordinates reach the shader packed in one
 * uint as texel offsets, 11 bits for x and y and 10 for z, so the image
 * stays within 2047 x 2047 x 1023 texels as well as max_dim (the device's
 * maxImageDimension3D) a side. Chooses n[3] slots per axis for at least
 * want slots where that fits: a square of 7, 14, 28, ... slots a side
 * (126 texels and its doublings, which tile without padding), as deep as
 * it needs. Returns
 * n[0] * n[1] * n[2]. */
#define LIGHT_ATLAS_XY_MAX 2047
#define LIGHT_ATLAS_Z_MAX 1023
int light_atlas_layout(uint32_t max_dim, int want, int n[3]);
/* Slot index (0 .. n[0] * n[1] * n[2] - 1) to its texel offset in the
 * atlas: x | y << 11 | z << 22, the shader's i_light. */
uint32_t light_slot_base(int slot, const int n[3]);

#endif
