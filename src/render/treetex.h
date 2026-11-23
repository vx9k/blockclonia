/* The trees' textures: each kind's bark, its bark on a felled log and
 * under moss, its end grain, and its leaves (block.h has the layers).
 *
 * Bark is drawn from a field that repeats every 16 texels across and every
 * TREE_BARK_WINDOWS blocks up, so the windows a trunk shows one above
 * another join without a seam, and so do the sides of neighbouring logs.
 * The patterns follow each kind's real bark: an oak's deep furrows, a
 * birch's dark lenticels, a Scots pine's orange plates. It does not draw
 * the ground plants or anything else (texgen.c). */
#ifndef MC_TREETEX_H
#define MC_TREETEX_H

#include <stdint.h>

/* One texel of a tree layer, RGBA: returns 0 for a layer that is not a
 * tree's, leaving out untouched. x and y are 0..15, y downwards. */
int treetex_texel(int layer, int x, int y, uint8_t out[4]);

/* The bark field of a kind at texel (x, y) of a strip `period` texels
 * tall (16, or 16 * TREE_BARK_WINDOWS for the windows): x and y wrap. */
void treetex_bark(int kind, int x, int y, int period, uint8_t out[4]);

#endif
