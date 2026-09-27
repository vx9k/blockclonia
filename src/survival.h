/* Glue between the world, player physics and the health model: what the
 * body is doing (environment), what just happened to it (landings,
 * collisions, falling blocks, broken glass), and what it can still do
 * (input limits). No window or GPU, so it runs in the unit tests. */
#ifndef MC_SURVIVAL_H
#define MC_SURVIVAL_H

#include "health.h"
#include "physics.h"
#include "world.h"

#define SURVIVAL_WATER_TEMP 14.0 /* degrees C: temperate lakes and sea */

/* The player's state before a physics step, to detect impacts after it. */
typedef struct {
    dvec3 vel;
    int on_ground;
    double submerged;
} survival_before;

survival_before survival_capture(const player *p);

/* Fills the environment for this step. `actions` counts blocks broken or
 * placed since the last step. */
void survival_env(const world *w, const player *p, const survival_before *b, int actions, health_env *e);

/* Injuries from the step that just ran: landings, water entry, running
 * into walls, and falling blocks that hit the player (ph->hits). */
void survival_impacts(health *h, const world *w, const physics *ph, const player *p, const survival_before *b);

/* Scales and filters movement by what the body can do. */
void survival_limit_input(const health *h, const player *p, player_input *in);

/* A block broken by hand: scavenged items, and cuts from glass. */
void survival_on_break(health *h, uint8_t id);

/* Water within reach to drink: standing in it or looking at it. */
int survival_water_nearby(const world *w, const player *p, vec3 dir);

#endif
