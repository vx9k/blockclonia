/* The first-person view model: the right arm and whatever it holds, drawn
 * as a few instanced cubes in view space (x right, y up, -z forward) by
 * the renderer's view-model pass. Animated: it sways with the stride,
 * lags the mouse a little, swings when hitting or placing, dips out and
 * back in when the held item changes, lifts food to the mouth when eating,
 * and sinks when sprinting. No window or GPU. */
#ifndef MC_VIEWMODEL_H
#define MC_VIEWMODEL_H

#include "entity.h"

#define VIEWMODEL_MAX 8

typedef struct {
    int held_id;          /* item in hand, 0 for the bare hand */
    float swing;          /* interact.swing: 1 right after an action, easing to 0 */
    int swinging;         /* the attack button is held on a block (repeat swings) */
    float stride;         /* camera_anim.stride: step phase, radians */
    float bob;            /* camera_anim.bob: 0..1 */
    float sprint;         /* camera_anim.sprint: 0..1 */
    float look_dx, look_dy; /* mouse motion this frame, radians (for lag) */
    float eating;         /* 0..1 while an eat action plays, else 0 */
    float light;          /* brightness (daylight) */
    float dt;
} viewmodel_input;

typedef struct {
    float equip;          /* 0 lowered .. 1 raised */
    int shown_id;         /* the item drawn (changes while lowered) */
    float lag_x, lag_y;   /* smoothed mouse lag */
    float swing_t;        /* 0..1 progress of the current swing */
    float time;
} viewmodel;

void viewmodel_init(viewmodel *vm);

/* Advances the animation and writes up to max instances; returns how many. */
int viewmodel_build(viewmodel *vm, const viewmodel_input *in, entity_instance *out, int max);

#endif
