/* The first-person view model: the right arm and whatever it holds, drawn
 * as a few instanced cubes in view space (x right, y up, -z forward) by
 * the renderer's view-model pass. Animated: it sways with the stride,
 * lags the mouse a little, swings when hitting or placing, dips out and
 * back in when the held item changes, lifts food to the mouth when eating,
 * and sinks when sprinting. Picks, axes, shovels and the rifle take both
 * hands, each arm reaching its grip by two-bone IK; the rifle comes up to
 * the eye, its sights on the crosshair, as `aim` rises. No window or GPU. */
#ifndef MC_VIEWMODEL_H
#define MC_VIEWMODEL_H

#include "entity.h"
#include "item.h"

#define VIEWMODEL_ARM_PARTS 5 /* forearm, wrist, palm, fingers, thumb */
#define VIEWMODEL_PALM 2      /* the palm among an arm's parts: the hand's centre */
#define VIEWMODEL_MAX 26      /* both arms and an item model (itemmodel.h) */

typedef struct {
    item_id held_id;      /* item in hand, 0 for the bare hand */
    float swing;          /* interact.swing: 1 right after an action, easing to 0 */
    int swinging;         /* the attack button is held on a block (repeat swings) */
    float stride;         /* camera_anim.stride: step phase, radians */
    float bob;            /* camera_anim.bob: 0..1 */
    float sprint;         /* camera_anim.sprint: 0..1 */
    float look_dx, look_dy; /* mouse motion this frame, radians (for lag) */
    float eating;         /* 0..1 while an eat action plays, else 0 */
    float aim;            /* 0 rifle at the hip .. 1 down its sights */
    float bolt;           /* 0..1 through working the bolt after a shot (COMBAT_CYCLE_S), else 0 */
    float light;          /* brightness (daylight) */
    float dt;
} viewmodel_input;

typedef struct {
    float equip;          /* 0 lowered .. 1 raised */
    item_id shown_id;     /* the item drawn (changes while lowered) */
    float lag_x, lag_y;   /* smoothed mouse lag */
    float swing_t;        /* 0..1 progress of the current swing */
    float aim_x, aim_v;   /* the rifle's mount: a spring following the aim, and its rate */
    float bolt;           /* viewmodel_input.bolt, this frame */
    float time;
} viewmodel;

void viewmodel_init(viewmodel *vm);

/* The hands an item takes: 2 for picks, axes, shovels and the rifle, else
 * 1. The view model's instances are each arm's VIEWMODEL_ARM_PARTS (right
 * arm first), then the item. */
int viewmodel_hands(item_id id);

/* Advances the animation and writes up to max instances; returns how many. */
int viewmodel_build(viewmodel *vm, const viewmodel_input *in, entity_instance *out, int max);
/* The rifle's muzzle at aim (0 hip .. 1 sights), held still (no sway, lag
 * or recoil): pos is the barrel's end and dir the bore's unit axis, in
 * view space. Shots, flash and smoke leave from here, on the authority as
 * well as the client, so it depends on nothing but aim. */
void viewmodel_muzzle(float aim, float pos[3], float dir[3]);
/* Working a bolt action at phase (0..1 of the cycle): how far the right
 * hand has left the stock for the bolt's knob, how far the handle is
 * turned up (1 = fully, 60 degrees) and how far it is drawn back (1 = its
 * whole 9 cm). All 0 outside the cycle. */
void viewmodel_bolt(float phase, float *to_knob, float *lift, float *slide);

#endif
