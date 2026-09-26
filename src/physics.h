/* Physics: fixed-timestep rigid bodies against a voxel world.
 *
 * - Player: swept-AABB collision, gravity, quadratic air drag, friction-
 *   limited acceleration (ice is slippery because mu is low, not because
 *   of a special case), buoyancy and drag in water.
 * - Structural integrity: every solid block needs a load path to the
 *   ground. A material can stick out `span` blocks sideways (or hang
 *   below) from something grounded; granular blocks (sand, gravel) only
 *   rest on what is directly below. Unsupported blocks turn into falling
 *   rigid bodies.
 * - Falling blocks: gravity, drag and buoyancy from their real density
 *   (a log floats, stone sinks). They re-solidify when they land.
 * - Water: volume-conserving cellular automaton with 8 levels per block.
 *
 * Units: metres, seconds, kilograms. 1 block = 1 m. */
#ifndef MC_PHYSICS_H
#define MC_PHYSICS_H

#include <stdint.h>
#include "mathlib.h"
#include "world.h"

#define PHYS_HZ 60
#define PHYS_DT (1.0 / PHYS_HZ)
#define GRAVITY 9.81
#define FLUID_TICK_EVERY 6      /* 10 Hz fluid updates */
#define MAX_BODIES 4096
#define STRUCT_RADIUS 12
#define STRUCT_MAX_SPAN 15

#define PLAYER_HALF_W 0.3
#define PLAYER_HEIGHT 1.8
#define PLAYER_EYE 1.62
#define PLAYER_MASS 75.0
#define PLAYER_DENSITY 985.0

typedef struct { dvec3 min, max; } aabb;

typedef struct {
    dvec3 pos, prev_pos; /* min corner */
    dvec3 vel;
    float rest_time;
    uint8_t block;
} body;

typedef struct {
    dvec3 pos, prev_pos; /* centre of the feet */
    dvec3 vel;
    float yaw, pitch;
    int on_ground;
    int flying;
    double submerged;    /* 0..1 fraction of the body in water */
} player;

typedef struct {
    float forward, right; /* -1..1 */
    int jump, sprint, descend;
} player_input;

typedef struct { int x, y, z; } ipos;

typedef struct {
    ipos *items;
    int count, cap;
    uint32_t *set;       /* open-addressing dedupe of items (index + 1) */
    uint32_t set_cap;
} pos_queue;

typedef struct physics {
    world *w;
    body *bodies;
    int body_count;
    pos_queue fluid_now, fluid_next;
    ipos struct_queue[256];
    int struct_head, struct_count;
    int suppress_struct;
    uint64_t step_count;
    /* Scratch for structural checks. */
    int8_t *st_s;
    int32_t *st_bucket[STRUCT_MAX_SPAN + 2];
    int st_bucket_len[STRUCT_MAX_SPAN + 2];
    const player *pl;    /* for body-vs-player collisions */
    /* Stats for the HUD/bench. */
    int last_collapse;
    int fluid_updates;
} physics;

void physics_init(physics *ph, world *w);
void physics_destroy(physics *ph);

/* Hook for world->on_block_changed. */
void physics_on_block_changed(void *user, int x, int y, int z, uint8_t old_id, uint8_t new_id);

void physics_step(physics *ph, player *p, const player_input *in);

void player_spawn(player *p, const world *w, double x, double z);
aabb player_box(const player *p);

/* Voxel raycast (Amanatides-Woo). Returns 1 on hit, with the block and the
 * empty cell in front of the hit face. */
typedef struct { int hit; ipos block, before; uint8_t id; } ray_hit;
ray_hit physics_raycast(const world *w, dvec3 origin, vec3 dir, double max_dist);

/* Move `box` along one axis, stopping at solid blocks and body obstacles.
 * Returns the distance actually travelled. Exposed for tests. */
double physics_sweep(const physics *ph, aabb box, int axis, double delta, int skip_body,
                     int include_player);

/* Runs a structural check around (x,y,z) immediately. Returns the number of
 * blocks that became falling bodies. Exposed for tests. */
int physics_check_structure(physics *ph, int x, int y, int z);

void physics_fluid_tick(physics *ph);

#endif
