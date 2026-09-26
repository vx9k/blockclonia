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
 *   A hard landing bounces by the material's restitution, glass and ice
 *   shatter, a body sliding on the ground brakes by friction first, and a
 *   body that strikes the player shoves them. Bodies tumble as they fall;
 *   the rotation is only drawn, collisions stay axis-aligned.
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
    dvec3 pos, prev_pos; /* min corner of the cell it occupies */
    dvec3 vel;
    float rest_time;
    float rot[4];        /* drawn orientation (unit quaternion); collisions stay axis-aligned */
    float spin[3];       /* rad/s: tumbling from the collapse and from bounces */
    uint8_t block;
    uint8_t bounces;
} body;

#define WALK_SPEED 4.0      /* m/s, a brisk walk (game pace) */
#define SPRINT_SPEED 6.5    /* m/s */
#define SNEAK_SPEED 1.3     /* m/s */
#define SWIM_SPEED 1.2
#define SWIM_SPRINT_SPEED 1.8

typedef struct {
    dvec3 pos, prev_pos; /* centre of the feet */
    dvec3 vel;
    float yaw, pitch;
    int on_ground;
    int flying;
    double submerged;    /* 0..1 fraction of the body in water */
    int sprinting;       /* running flat out (see player_input.sprint) */
    int sneaking;
    int hit_wall;        /* horizontal movement was blocked last step */
    double fall_speed;   /* downward speed lost on the last landing, m/s */
} player;

typedef struct {
    float forward, right; /* -1..1 */
    int jump, descend;
    /* Sprint request. Sprinting starts only moving forward, on the ground
     * or swimming; it stops when forward is released, when a wall stops
     * the player, and while sneaking. */
    int sprint;
    int sneak;           /* slow walk that will not step off an edge */
} player_input;

typedef struct { int x, y, z; } ipos;

typedef struct {
    ipos *items;
    int count, cap;
    uint32_t *set;       /* open-addressing dedupe of items (index + 1) */
    uint32_t set_cap;
} pos_queue;

/* A falling block that struck the player during the last step. */
typedef struct {
    float mass;          /* kg */
    float speed;         /* m/s relative to the player */
    float height;        /* 0 feet .. 1 top of the head */
    uint8_t block;
} player_hit;

#define MAX_PLAYER_HITS 8

/* Per-step event lists for the effects: brittle blocks that shattered on
 * impact (their cell and material, for shards) and fast entries into water
 * by a body or the player (where the surface was crossed, and how fast). */
#define MAX_SHATTERS 16
#define MAX_SPLASHES 16

/* A dropped item: a small box that falls, bounces, slides, floats or
 * sinks, merges with identical stacks nearby and is pulled toward the
 * player when close. */
#define MAX_ITEMS 512
#define ITEM_HALF 0.125           /* 25 cm box */
#define ITEM_LIFETIME 300.0f      /* s before an unclaimed item despawns */
#define ITEM_PICKUP_RANGE 1.8

typedef struct {
    dvec3 pos, prev_pos;          /* centre */
    dvec3 vel;
    float age;                    /* s since dropped */
    float delay;                  /* s before it can be picked up */
    float spin;                   /* radians, for drawing */
    uint8_t id, count;
    uint8_t on_ground;
    uint8_t touching;             /* reached the player this step */
} item_ent;

typedef struct physics {
    world *w;
    body *bodies;
    int body_count;
    pos_queue fluid_now, fluid_next;
    int fluid_read, fluid_end;   /* this tick's cells: fluid_now.items[fluid_read, fluid_end) */
    int fluid_per_step;          /* a tick's work is spread over FLUID_TICK_EVERY steps */
    ipos struct_queue[256];
    int struct_head, struct_count;
    int suppress_struct;
    uint64_t step_count;
    /* Scratch for structural checks. */
    int8_t *st_s;
    uint8_t *st_id;              /* block ids of the region, read once per check */
    int32_t *st_bucket[STRUCT_MAX_SPAN + 2];
    int st_bucket_len[STRUCT_MAX_SPAN + 2];
    const player *pl;    /* for body-vs-player collisions */
    player_hit hits[MAX_PLAYER_HITS]; /* reset at the start of each step */
    int hit_count;
    item_ent *items;
    int item_count;
    /* Falling blocks resting on the player this step (crush injuries). */
    float pinned_mass;            /* kg */
    float pinned_height;          /* 0 feet .. 1 head, where the weight bears */
    /* Effects events of the last step, reset at the start of each step. */
    ipos shatter_pos[MAX_SHATTERS];
    uint8_t shatter_block[MAX_SHATTERS];
    int shatter_count;
    dvec3 splash_pos[MAX_SPLASHES];   /* on the surface, centred on what went in */
    float splash_speed[MAX_SPLASHES]; /* downward m/s when it crossed the surface */
    int splash_count;
    int magnet;                   /* pull nearby items to the player (alive, not in a menu) */
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
ray_hit physics_raycast(const world *w, dvec3 o, vec3 dir, double max_dist);

/* Runs a structural check around (x,y,z) immediately. Returns the number of
 * blocks that became falling bodies. Exposed for tests. */
int physics_check_structure(physics *ph, int x, int y, int z);

void physics_fluid_tick(physics *ph);

/* Drops an item stack at pos (centre) with an initial velocity. delay is
 * how long before it can be picked up (a thrown item flies clear first).
 * Returns 0 if there is no room for more items. */
int physics_drop_item(physics *ph, dvec3 pos, dvec3 vel, int id, int count, float delay);
/* Hands items that reached the player to `accept`, which returns how many
 * it took (the inventory may be full). Returns the number of stacks
 * picked up completely. */
int physics_pickup(physics *ph, int (*accept)(void *user, int id, int count), void *user);

/* Places falling bodies back into the world (all of them, or only those in
 * one column) so they survive a save. */
void physics_settle_bodies(physics *ph, const column *only);
/* Hook for world->on_column_unload. */
void physics_on_column_unload(void *user, const column *c);

#endif
