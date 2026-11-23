/* What a clouded mind does to the player's controls: the hands lag behind
 * what the player meant, some actions never happen, and walking feet
 * catch and stumble. The amounts come from health_get_senses; this module
 * only applies them to one frame's controls at a time, by elapsed time,
 * so it acts the same at any frame rate. It does not decide how impaired
 * the body is, and it does not touch the world. */
#ifndef MC_IMPAIR_H
#define MC_IMPAIR_H

#include "physics.h"

#include <stdint.h>

#define IMPAIR_RING 128 /* frames held back: 0.35 s at up to ~360 fps */

/* One frame of the player's controls. */
typedef struct {
    player_input move;
    float look_dx, look_dy;       /* radians turned this frame */
    int click_break, click_place; /* presses this frame */
} impair_frame;

typedef struct {
    impair_frame ring[IMPAIR_RING];
    double at[IMPAIR_RING]; /* s each held frame was taken */
    int head, count;
    player_input held;      /* the movement last let through, kept until a newer one is due */
    float stumble_t;        /* s left of a stumble */
    uint32_t rng;
} impair;

void impair_init(impair *m, uint32_t seed);

/* Takes the controls the player gave at time t and returns what the body
 * does this frame: lag s behind, each press lost with chance drop, and,
 * while walking, stumbles at stumble per second. */
impair_frame impair_step(impair *m, const impair_frame *now, float lag, float drop, float stumble, double t, float dt);

#endif
