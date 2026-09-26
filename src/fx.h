/* Visual effects and the per-frame entity list: block-break debris and
 * splinters, shattered glass, splashes, then everything the renderer draws
 * as instanced cubes besides falling blocks (dropped items bobbing and
 * spinning, particles, the crack overlay on a block being broken). */
#ifndef MC_FX_H
#define MC_FX_H

#include "entity.h"
#include "physics.h"

#define FX_MAX_PARTICLES 1024

typedef struct {
    dvec3 pos, prev_pos;
    dvec3 vel;
    float life, max_life;   /* s */
    float size;             /* m */
    float spin, spin_rate;  /* radians, rad/s */
    uint8_t tex;            /* texture layer */
    uint8_t bounce;         /* restitution in 1/255 */
} particle;

typedef struct {
    particle p[FX_MAX_PARTICLES];
    int count;
    uint32_t rng;
} fx_state;

void fx_init(fx_state *f, uint32_t seed);

/* Debris from a block broken at cell (x, y, z): its own texture, flying
 * outward. `n` pieces; brittle blocks throw more, smaller shards. */
void fx_break(fx_state *f, uint8_t block, int x, int y, int z, int n);
/* A few chips knocked off while a block is being hit. */
void fx_chip(fx_state *f, uint8_t block, dvec3 at);
/* Spray where something hit water at `speed`. */
void fx_splash(fx_state *f, dvec3 at, double speed);

/* Gravity, drag and bounces off solid blocks; expired particles go. */
void fx_step(fx_state *f, const world *w, double dt);

/* What is being broken right now, for the crack overlay. */
typedef struct {
    int active;
    int x, y, z;
    float progress;         /* 0..1 */
} fx_crack;

/* Fills out[] with dropped items and particles (opaque first, then
 * translucent) and the crack overlay. Positions are relative to eye;
 * alpha interpolates physics state between steps. */
void fx_build_entities(const fx_state *f, const physics *ph, const fx_crack *crack, dvec3 eye, double alpha,
                       float time, entity_instance *out, int max, int *n_opaque, int *n_trans);

#endif
