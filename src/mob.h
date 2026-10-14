/* Creatures: boars that graze by day and run when hurt, wolves that hunt
 * players at night or when provoked. Each body is a player (physics_move_
 * player) driven by a small state machine instead of a keyboard, so it
 * walks, steps, swims and collides exactly as a player does. Hit points
 * stand in for a body: the human health model is far too heavy to run
 * for every animal. Mobs are not saved; they spawn around players and
 * despawn far from them. No window or GPU. */
#ifndef MC_MOB_H
#define MC_MOB_H

#include "entity.h"
#include "physics.h"

#define MOB_MAX 24

typedef enum { MOB_BOAR, MOB_WOLF, MOB_KINDS } mob_kind;

typedef struct {
    player body;
    dvec3 threat;        /* where a boar runs from */
    float hp;
    float hurt_t;        /* s left of the hurt flash */
    float attack_t;      /* s until a wolf can bite again */
    float think_t;       /* s until the next wander decision */
    float flee_t;        /* s left of running away (boar) */
    float angry_t;       /* s left of hunting by day after being hurt (wolf) */
    float walk;          /* 0 idle, 1 walking (wandering) */
    float stride;        /* leg swing phase, radians (drawing) */
    uint32_t rng;
    uint8_t kind;
} mob;

typedef struct {
    mob m[MOB_MAX];
    int count;
    uint32_t rng;
    float spawn_t;       /* s until the next spawn attempt */
    float age;           /* s since mobs_init */
} mobs;

/* A wolf bit player `who` this step. */
typedef struct {
    int who;
    int part;            /* body_part_id it bit */
    dvec3 from;          /* where the wolf was */
} mob_bite;

#define MOB_MAX_BITES 8

typedef struct {
    mob_bite bites[MOB_MAX_BITES];
    int bite_count;
} mob_events;

void mobs_init(mobs *s, uint32_t seed);

/* One fixed physics step (PHYS_DT): spawning around the players, despawning
 * far from all of them, thinking and moving. players[i] may be NULL (a dead
 * or absent player). daylight is thermo_daylight. Bites are reported in ev
 * for the caller to apply to the right body. */
void mobs_step(mobs *s, physics *ph, player *const *players, int n, float daylight, mob_events *ev);

/* The first mob along a ray within max_dist, -1 for none; *dist is set to
 * how far along the ray it was hit. */
int mobs_raycast(const mobs *s, dvec3 o, vec3 dir, double max_dist, double *dist);

/* Deals damage (hit points: a fist 1, a knife 3 to 5, a sword 6 to 9, a
 * bullet 14) to mob i from a blow or shot that came from `from`, knocking
 * it back. A killed mob drops what it leaves and is removed (indices past
 * i shift down). Returns 1 if it died. */
int mobs_hurt(mobs *s, physics *ph, int i, float damage, dvec3 from);

/* Adds a mob standing at pos (tests, and the spawner). Returns its index,
 * -1 if full. */
int mobs_add(mobs *s, int kind, dvec3 pos);

/* A mob as boxes: feet relative to the camera. Writes up to max instances
 * and returns how many (MOB_FIGURE at most). */
#define MOB_FIGURE 7
int mob_figure(const mob *m, const float feet[3], float light, entity_instance *out, int max);

#endif
