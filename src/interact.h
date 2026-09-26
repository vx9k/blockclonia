/* What the hands do each frame: breaking blocks (hold; it takes the
 * material's hardness in seconds, instant when flying), placing the held
 * block, buckets, picking a block into the hand, and dropping items. Also
 * drives the arm-swing animation state. No window or GPU. */
#ifndef MC_INTERACT_H
#define MC_INTERACT_H

#include "fx.h"
#include "inventory.h"
#include "physics.h"
#include "thermo.h"

typedef struct {
    int has_target;
    ipos target;
    uint8_t target_id;
    float progress;      /* 0..1 of breaking the target */
    float chip_t;        /* s until the next chip flies off */
    float repeat;        /* flying: s until held breaking takes the next block */
    float swing;         /* 1 right after an action, eases to 0 (the arm) */
    int swinging;        /* the attack button is held on something */
    uint32_t rng;
} interact;

typedef struct {
    int attack;          /* left button held */
    int attack_click;    /* left button pressed this frame */
    int use_click;       /* right button pressed this frame */
    int pick_click;      /* middle button */
    int drop;            /* 1: drop one of the held stack, 2: all of it */
    int can_act;         /* the body can use its hands */
    float speed;         /* breaking speed factor (hurt arms are slower) */
    float dt;
} interact_input;

typedef struct {
    int actions;         /* blocks broken or placed, for the health model */
    int broke;           /* a block was broken by hand (not flying) */
    uint8_t broken_id;
    int eat;             /* right-clicked food: eat it */
    const char *msg;     /* short feedback, or NULL */
} interact_out;

void interact_init(interact *s, uint32_t seed);

/* th may be NULL (no fires to feed). */
interact_out interact_frame(interact *s, world *w, physics *ph, thermo *th, const player *pl, inventory *inv,
                            fx_state *fx, dvec3 eye, vec3 dir, ray_hit hit, const interact_input *in);

/* True if an axis-aligned box overlaps cell (x, y, z). */
int interact_box_hits_cell(aabb b, int x, int y, int z);

#endif
