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
#define SURVIVAL_AIR_TEMP 20.0   /* degrees C until the thermal surroundings are given */

/* The player's state before a physics step, to detect impacts after it. */
typedef struct {
    dvec3 vel;
    int on_ground;
    double submerged;
} survival_before;

survival_before survival_capture(const player *p);

/* Fills the environment for this step. `actions` counts blocks broken or
 * placed since the last step. The air and the ground start at
 * SURVIVAL_AIR_TEMP with no radiant heat; standing in a burning campfire
 * sets in_fire. */
void survival_env(const world *w, const player *p, const survival_before *b, int actions, health_env *e);

/* The thermal surroundings from the thermo module, after survival_env:
 * air temperature around the body (degrees C), radiant flux from fires
 * onto the skin facing them (W/m^2), the temperature of the surface under
 * the feet (degrees C), and whether the body stands in flames (OR'd with
 * what survival_env found). */
void survival_env_thermal(health_env *e, double air_temp, double radiant, double contact_temp, int in_fire);

/* Injuries from the step that just ran: landings, water entry, running
 * into walls, sliding landings on rough ground (abrasions), falling blocks
 * that hit the player (ph->hits) and blocks resting on the player
 * (ph->pinned_mass at ph->pinned_height: crush). */
void survival_impacts(health *h, const world *w, const physics *ph, const player *p, const survival_before *b);

/* Scales and filters movement by what the body can do. */
void survival_limit_input(const health *h, const player *p, player_input *in);

/* A block broken by hand: glass cuts the hand that smashed it. */
void survival_on_break(health *h, uint8_t id);

/* Water within reach to drink: standing in it or looking at it. */
int survival_water_nearby(const world *w, const player *p, vec3 dir);

/* The first water cell along a ray (buckets), stopping at solid blocks.
 * Returns 1 and the cell if found. */
int survival_find_water(const world *w, dvec3 eye, vec3 dir, double reach, ipos *out);

/* The player file: position, view, fly mode, the time of day (0..1) and
 * the inventory. decode
 * returns 0 on success and -1 (nothing changed) for malformed data. */
#define PLAYER_FILE_SIZE (4 + 4 + 3 * 8 + 2 * 4 + 4 + INV_ENCODED_SIZE)
size_t survival_encode_player(const player *p, const inventory *inv, double day, uint8_t *out, size_t cap);
int survival_decode_player(player *p, inventory *inv, double *day, const uint8_t *in, size_t len);

#endif
