/* Fighting: blows with the hands, tools and blades, and rifle bullets in
 * flight. A blow lands on the nearest mob or player along the look ray, if
 * it is nearer than the block being looked at and within reach; a bullet
 * flies under gravity, drag and wind (ballistics.c), tested against
 * blocks, mobs and players along its path every physics step. Wounds to players go through the
 * health model (cuts, blunt hits); mobs lose hit points. Runs on the
 * authority (a single-player game or the multiplayer host). No window or
 * GPU. */
#ifndef MC_COMBAT_H
#define MC_COMBAT_H

#include "ballistics.h"
#include "entity.h"
#include "health.h"
#include "inventory.h"
#include "mob.h"
#include "physics.h"
#include "thermo.h"

#define COMBAT_SHOTS 32
#define COMBAT_REACH 3.0     /* m a blow reaches */
#define HIP_SPREAD 0.06f    /* extra spread fired from the hip (about +-1.7 degrees) */
#define CARTRIDGE_AMMO AMMO_BLACK_LEAD /* what I_CARTRIDGE fires */

typedef struct {
    dvec3 pos, vel;
    float life;          /* s left before it is dropped */
    int owner;           /* the player index that fired it */
    int ammo;            /* ammo_kind */
} bullet;

typedef struct {
    bullet b[COMBAT_SHOTS];
    int count;
    float cooldown[PHYS_MAX_PLAYERS]; /* s until each player can strike or fire again */
    uint32_t rng;
    uint32_t wind_seed;
    double time;         /* s simulated, for the wind */
    const thermo *th;    /* the air's temperature, or NULL for 15 C */
} combat;

/* The bodies that can be hit, by player index: pl[i] or hl[i] NULL for an
 * absent or dead player. */
typedef struct {
    player *const *pl;
    health *const *hl;
    int n;
} combat_targets;

typedef struct {
    int struck;          /* a blow landed (on a mob or a player) */
    int fired;           /* a shot went off */
    int killed;          /* a mob died this frame */
    item_id tool_broke;  /* item id that wore out, else 0 */
    dvec3 at;            /* where a blow landed or the shot left */
    const char *msg;     /* feedback, or NULL */
} combat_out;

void combat_init(combat *c, uint32_t seed);

/* 1 if a mob or another player is in reach along the ray and nearer than
 * block_dist: the attack button then strikes instead of digging. */
int combat_aim(const mobs *ms, const combat_targets *t, int who, dvec3 eye, vec3 dir, double block_dist);

/* Player `who`'s hands this frame: on attack_click a blow, or a shot when
 * holding the rifle with a cartridge in the inventory. aim (0 at the hip ..
 * 1 down the sights) sets the shot's spread. */
combat_out combat_frame(combat *c, mobs *ms, physics *ph, const combat_targets *t, int who, inventory *inv, dvec3 eye,
                        vec3 dir, double block_dist, int attack_click, float aim, float dt);

/* Moves the bullets one physics step and applies what they hit. Returns
 * the number that hit a body (for sound). */
int combat_step(combat *c, mobs *ms, physics *ph, const combat_targets *t);

/* One physics step of the living world on the authority: mobs move (and
 * spawn and despawn), their bites wound the players they reach, and
 * bullets fly. Returns the number of bites plus bullet hits (for sound). */
int combat_creatures(combat *c, mobs *ms, physics *ph, const combat_targets *t, float daylight);

/* The damage a blow does with this item in hand (hit points; a fist is 1). */
float combat_blow(item_id held);

/* Glowing streaks for the tracers in flight: each the stretch it covers in
 * one physics step, ending where it is `alpha` of the way into the next,
 * and never thinner than about a pixel however far away. A bullet's age is
 * BULLET_LIFE less its life. Writes up to max; returns how many. */
int combat_tracers(const combat *c, dvec3 eye, double alpha, entity_instance *out, int max);

#endif
