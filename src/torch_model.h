/* The torch as a model of entity cubes, the same in the world and in the
 * hand: a bark handle, a charred head, a glowing ember in it, a flame of
 * three layers that narrow upward, turn against each other and sway, and
 * two sparks rising off the tip. Only builds instances: drawing them is
 * the renderer's, lighting them the caller's. No GPU or world access. */
#ifndef MC_TORCH_MODEL_H
#define MC_TORCH_MODEL_H

#include "entity.h"
#include <stdint.h>

#define TORCH_PARTS 8
/* Height of the model at size 1, from the handle's foot to the flame's tip
 * (the block is 1 m). */
#define TORCH_HEIGHT 0.92f

/* Writes TORCH_PARTS instances standing on at (the handle's foot), turned
 * by rot and scaled by size. time drives the flame (s; the same at any
 * frame rate) and seed sets each torch apart. light is the brightness of
 * the handle and head; the flame and sparks are their own light (1).
 * Returns TORCH_PARTS. */
int torch_model(const float at[3], quat rot, float size, double time, uint32_t seed, float light,
                entity_instance out[TORCH_PARTS]);

#endif
