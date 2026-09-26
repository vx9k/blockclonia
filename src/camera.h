/* First-person camera motion: head bob that follows the stride, a wider
 * view while sprinting, the eyes dropping when sneaking, a dip that
 * springs back after a landing and a jolt when hurt. Pure state and maths,
 * updated once per frame from the player; no window or GPU. */
#ifndef MC_CAMERA_H
#define MC_CAMERA_H

#include "physics.h"

typedef struct {
    float stride;          /* step cycle phase, radians (one step = pi) */
    float bob;             /* 0..1 how much bob applies (walking on ground) */
    float sprint;          /* 0..1 eased sprint blend, drives the FOV */
    float crouch;          /* m the eyes are lowered */
    float dip, dip_vel;    /* landing dip, m (negative is down) */
    float roll;            /* radians, from being hurt */
    float roll_vel;
    float last_hurt;       /* health hurt_flash seen last frame */
    float swim;            /* 0..1 underwater sway blend */
    double t;
} camera_anim;

typedef struct {
    float dx, dy, dz;      /* eye offset in world space, m */
    float roll;            /* radians about the view axis */
    float fov_scale;       /* multiply the configured FOV */
    float pitch_add;       /* radians, tiny nod with each step */
} camera_pose;

void camera_init(camera_anim *c);

/* landing: the hardest landing speed since the last call (m/s); hurt is
 * health.hurt_flash (0..1); effects: 1 for bob/dip/roll (the view bobbing
 * setting); fov_fx: 1 to widen the view when sprinting. */
camera_pose camera_update(camera_anim *c, const player *p, float landing, float hurt, int effects, int fov_fx,
                          float dt);

#endif
