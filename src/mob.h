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
    float look;          /* head turned from the body, radians, in the sense of yaw (drawing) */
    float nod;           /* head lowered, radians: grazing, stalking (drawing) */
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

/* What a creature can tell about player i from the light around them
 * (lightfx_seen at the eyes): how lit they are, 0..1, and how much of it
 * is firelight, 0..1. */
typedef struct {
    float level;
    float fire;
} mob_sense;

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
 * or absent player). daylight is thermo_daylight. sense[i] is what the
 * light shows of player i (NULL: everyone in plain view, no fire): a
 * player in the dark is noticed from closer, one carrying a torch from as
 * far as by day, and a wolf that has not been hurt keeps out of
 * firelight. Bites are reported in ev for the caller to apply to the
 * right body. */
void mobs_step(mobs *s, physics *ph, player *const *players, const mob_sense *sense, int n, float daylight,
               mob_events *ev);

/* How far a creature of kind notices a player lit to level (mob_sense):
 * range in full light, less in the dark by how well its eyes cope. */
double mobs_sight(int kind, double range, float level);

/* The first mob along a ray within max_dist, -1 for none; *dist is set to
 * how far along the ray it was hit. */
int mobs_raycast(const mobs *s, dvec3 o, vec3 dir, double max_dist, double *dist);

/* Deals damage (hit points: a fist 1, a knife 3 to 5, a sword 6 to 9, a
 * bullet 14) to mob i from a blow or shot that came from `from`, knocking
 * it back. A killed mob drops what it leaves and is removed (indices past
 * i shift down). Returns 1 if it died. */
int mobs_hurt(mobs *s, physics *ph, int i, float damage, dvec3 from);

/* How frightening the creatures hunting near player p are, 0..1, for
 * health_env.fear: nothing beyond a wolf's hunting range, rising to 1 as
 * one closes to biting distance. daylight is thermo_daylight. */
float mobs_fear(const mobs *s, const player *p, float daylight);

/* MOB_BOAR or MOB_WOLF for "boar" or "wolf" (any case); -1 otherwise. */
int mobs_kind_named(const char *name);

/* Stands a mob on the ground `ahead` m in front of player p and `aside` m
 * to its right, facing the player (--mobs). Returns its index, -1 if full. */
int mobs_add_ahead(mobs *s, const world *w, const player *p, int kind, double ahead, double aside);

/* Adds a mob standing at pos (tests, and the spawner). Returns its index,
 * -1 if full. */
int mobs_add(mobs *s, int kind, dvec3 pos);

/* A mob as boxes covered in its pixel art (mobskin.c): body, shoulders,
 * head, snout, ears, tail and two-part legs, feet relative to the camera.
 * t is a clock in seconds for idle motion (a wagging tail). Writes up to
 * max instances and returns how many (MOB_FIGURE for a full figure). */
#define MOB_FIGURE 15
int mob_figure(const mob *m, const float feet[3], float light, float t, entity_instance *out, int max);

#endif
