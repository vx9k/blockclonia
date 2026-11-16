/* The creatures' pixel art: one small hand-drawn picture per kind of face
 * (a wolf's muzzle, a boar's snout disc), written as palette-indexed rows
 * so the project still ships no image files. Each picture is exactly the
 * size of the face it covers at 16 texels per metre (ENTITY_TEX_METRIC),
 * first column at the front, first row at the top. texgen.c turns them
 * into texture layers; mob.c builds the boxes they cover. No window or
 * GPU, and no shapes: the boxes live in mob.c. */
#ifndef MC_MOBSKIN_H
#define MC_MOBSKIN_H

#include <stdint.h>

/* Writes the RGBA of texel (x, y) of a creature layer (T_MOB_FIRST ..
 * T_MOB_LAST); texels past the picture repeat its last column and row, so
 * smaller mips keep its colours. Returns 0 for any other layer. */
int mobskin_texel(int layer, int x, int y, uint8_t out[4]);

/* The picture's size in texels; 0 for a layer that is not a creature's. */
int mobskin_size(int layer, int *w, int *h);

/* 1 if every picture is well formed: rows of equal width within the
 * layer, only palette colours, a picture for every creature layer. */
int mobskin_check(void);

#endif
