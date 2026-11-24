/* Camera motion: in first person, head bob that follows the stride, a
 * wider view while sprinting, the eyes dropping when sneaking, a dip that
 * springs back after a landing and a jolt when hurt; the third-person views
 * on a spring arm that keeps out of the blocks; and the debug free camera.
 * Pure state and maths, updated once per frame from the player; no window
 * or GPU. */
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

/* The views F5 cycles through. */
typedef enum { CAM_FIRST, CAM_BEHIND, CAM_FRONT, CAM_FREE } camera_mode;

/* The mode after m. The free camera, a debug tool, is offered only while
 * the debug view is on. */
camera_mode camera_next(camera_mode m, int debug);
/* "first", "back", "front" or "free" (--camera), or -1. */
int camera_mode_parse(const char *s);

typedef struct {
    dvec3 eye;
    float yaw, pitch; /* where the camera looks */
} camera_view;

/* A third-person view from head on an arm of up to want metres: behind the
 * head along the player's look (so the crosshair still points where the
 * player looks), or in front of it looking back at the face. *arm carries
 * the arm's length between frames (start it at 0): blocks in the way
 * shorten it at once, and it eases back out over dt when they clear. */
camera_view camera_third(const world *w, float *arm, dvec3 head, float yaw, float pitch, int front, float want,
                         float dt);
/* Which shoulder the view from behind looks over (settings camera_side). */
enum { CAM_SIDE_CENTRE, CAM_SIDE_RIGHT, CAM_SIDE_LEFT };
/* The other shoulder; from the centre, the right one (V). */
int camera_side_next(int side);
/* Metres to the right of the head for a CAM_SIDE_*: 0.6 one way or the
 * other, 0 centred or unknown. */
float camera_side_offset(int side);
/* The view from behind, side metres to the right of the head (negative:
 * left; walls bring it in) and want metres back. It turns in to look at
 * the point focus metres along the player's own look, which is where the
 * player aims, so the crosshair marks that point from off to the side. */
camera_view camera_shoulder(const world *w, float *arm, dvec3 head, float yaw, float pitch, float side, float focus,
                            float want, float dt);
/* Where the camera watches a body lying on the ground from: its middle,
 * 0.8 m ahead of feet along yaw and 0.5 m up, pulled back toward the feet
 * while that point is inside a block. */
dvec3 camera_body_pivot(const world *w, dvec3 feet, float yaw);
/* The free camera moved by dt along its look (fwd), its right and straight
 * up, each -1..1, at speed m/s. It passes through everything. */
dvec3 camera_fly(dvec3 at, float yaw, float pitch, float fwd, float right, float up, float speed, float dt);

#endif
