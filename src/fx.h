/* Visual effects and the per-frame entity list: block-break debris and
 * splinters, shattered glass, splashes, then everything the renderer draws
 * as instanced cubes besides falling blocks (dropped items bobbing and
 * spinning, particles, the crack overlay on a block being broken), and a
 * gun's muzzle flash and powder smoke. */
#ifndef MC_FX_H
#define MC_FX_H

#include "entity.h"
#include "lightfx.h"
#include "physics.h"

#define FX_MAX_PARTICLES 1024

/* What a particle is: debris falls and bounces, smoke rides the wind and
 * spreads as it thins, and a glow is a spark of a muzzle flash, bright and
 * gone within a frame or two. */
enum { FXP_DEBRIS, FXP_SMOKE, FXP_GLOW };

typedef struct {
    dvec3 pos, prev_pos;
    dvec3 vel;
    float life, max_life;   /* s */
    float size;             /* m */
    float spin, spin_rate;  /* radians, rad/s */
    uint8_t tex;            /* texture layer */
    uint8_t bounce;         /* restitution in 1/255 */
    uint8_t kind;           /* FXP_* */
} particle;

typedef struct {
    particle p[FX_MAX_PARTICLES];
    int count;
    uint32_t rng;
    dvec3 wind;  /* m/s, what smoke drifts with; the caller keeps it current */
    float flash; /* a muzzle flash's light this frame, 0..1, which lights the smoke too */
} fx_state;

void fx_init(fx_state *f, uint32_t seed);

/* Debris from a block broken at cell (x, y, z): its own texture, flying
 * outward. `n` pieces; brittle blocks throw more, smaller shards. */
void fx_break(fx_state *f, uint8_t block, int x, int y, int z, int n);
/* A few chips knocked off while a block is being hit. */
void fx_chip(fx_state *f, uint8_t block, dvec3 at);
/* Spray where something hit water at `speed`. */
void fx_splash(fx_state *f, dvec3 at, double speed);

/* A shot leaving a muzzle at `at` along unit `dir`: a flash of burning gas
 * and a cloud of powder smoke, thick for black powder, which leaves about
 * half its charge as solids, and a wisp for smokeless. */
void fx_muzzle(fx_state *f, dvec3 at, dvec3 dir, int black);

/* Gravity, drag and bounces off solid blocks for debris, the wind for
 * smoke; expired particles go. */
void fx_step(fx_state *f, const world *w, double dt);

/* What is being broken right now, for the crack overlay. */
typedef struct {
    int active;
    int x, y, z;
    float progress;         /* 0..1 */
} fx_crack;

/* Fills out[] with dropped items and particles (opaque first, then
 * translucent) and the crack overlay, each in the light of its cell
 * (light_at with daylight). Positions are relative to eye; alpha
 * interpolates physics state between steps. */
void fx_build_entities(const fx_state *f, const physics *ph, const fx_crack *crack, dvec3 eye, double alpha, float time,
                       float daylight, const lightfx_frame *lf, entity_instance *out, int max, int *n_opaque,
                       int *n_trans);

#endif
