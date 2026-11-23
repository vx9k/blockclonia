/* Lights that move: what other players carry, light-giving items lying
 * about, burning creatures and muzzle flashes. Each frame the game lists
 * them (lamps_add, then lamps_end); where one appears, moves to
 * another cell, changes or goes out, the columns it reaches relight, and
 * their light jobs pick the lamps up through the world's glow hook
 * (lamps_glow), after whatever that hook held before (the heat field's
 * hot cells). So a lamp is flood-filled like a block's light: walls stop
 * it and it takes its colour to the meshes and creatures alike.
 *
 * Deliberately not here: the local player's own torch, which block.frag
 * lights from the eye every frame without waiting for a light job, and
 * anything faster than a light job (a frame or two): a flash is held for
 * LAMPS_FLASH_S so the job can catch it. */
#ifndef MC_LAMPS_H
#define MC_LAMPS_H

#include "light.h"
#include "mathlib.h"
#include "world.h"

#define LAMPS_MAX 64
/* A flash lasts milliseconds; its lamp is held this long, so a light job
 * started the frame it fires still lights the walls around it. */
#define LAMPS_FLASH_S 0.1f
/* A burning creature: a campfire's colour a level down. */
#define LAMPS_FIRE LIGHT_PACK(0, 14, 8, 2)
/* A muzzle flash: burning powder, a warm white. */
#define LAMPS_FLASH LIGHT_PACK(0, 15, 13, 9)

typedef struct {
    int x, y, z;
    uint16_t light; /* LIGHT_PACK(0, r, g, b) */
} lamp;

typedef int (*lamps_glow_fn)(const void *user, int x0, int z0, int top, struct light_emitter *out, int cap);

typedef struct {
    lamp now[LAMPS_MAX], was[LAMPS_MAX];
    int n, was_n;
    /* The hook lamps_glow chains to first, or NULL. */
    lamps_glow_fn under;
    const void *under_user;
} lamps;

/* Starts empty; under/under_user are the glow hook the world had. */
void lamps_init(lamps *l, lamps_glow_fn under, const void *under_user);
/* Adds a light at p (block light, sky bits 0); none past LAMPS_MAX, and
 * none for a light of 0. */
void lamps_add(lamps *l, dvec3 p, uint16_t light);
/* Relights where this frame's lamps differ from the last frame's:
 * world_light_touch at every lamp that appeared, went out or changed.
 * Returns how many lamps differed. */
int lamps_end(lamps *l, world *w);
/* (lamps_end leaves the list empty for the next frame.) */
/* The world's glow hook (world.glow): the chained hook's emitters, then
 * the lamps inside the box, in the box's coordinates. */
int lamps_glow(const void *user, int x0, int z0, int top, struct light_emitter *out, int cap);

#endif
