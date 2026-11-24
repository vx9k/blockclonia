#include "camera.h"
#include "anim.h"

#include <math.h>
#include <string.h>

void camera_init(camera_anim *c) { memset(c, 0, sizeof *c); }

camera_pose camera_update(camera_anim *c, const player *p, float landing, float hurt, int effects, int fov_fx, float dt)
{
    camera_pose o = {0, 0, 0, 0, 1.0f, 0};
    c->t += (double)dt;
    float hs = (float)hypot(p->vel.x, p->vel.z);
    int walking = p->on_ground && !p->flying && hs > 0.3f;

    /* The stride: one step per ~0.7-1.2 m depending on pace, so the bob
     * follows the feet instead of a clock. */
    if (walking) {
        float step = 0.7f + 0.08f * hs;
        c->stride = fmodf(c->stride + hs * dt / step * (float)MC_PI, 2.0f * (float)MC_PI);
    }
    c->bob = anim_approach(c->bob, walking ? anim_clamp01(hs / (float)SPRINT_SPEED * 1.25f) : 0.0f, 8.0f, dt);
    c->sprint = anim_approach(c->sprint, p->sprinting ? 1.0f : 0.0f, 6.0f, dt);
    c->crouch = anim_approach(c->crouch, p->sneaking ? 0.3f : 0.0f, 12.0f, dt);
    c->swim = anim_approach(c->swim, p->submerged > 0.6 && !p->flying ? 1.0f : 0.0f, 3.0f, dt);

    /* Landing: an impulse into a critically damped spring, bigger for a
     * harder landing, so the head sinks and recovers without wobbling. */
    if (landing > 2.0f) c->dip_vel -= fminf(landing, 20.0f) * 0.3f; /* 10 m/s: ~11 cm */
    c->dip = anim_spring(c->dip, &c->dip_vel, 0.0f, 10.0f, dt);
    if (c->dip < -0.45f) c->dip = -0.45f;

    /* Hurt: a roll jolt, alternating sides. */
    if (hurt > c->last_hurt + 0.05f) {
        float side = fmod(c->t * 7.3, 2.0) < 1.0 ? 1.0f : -1.0f;
        c->roll_vel += side * (hurt - c->last_hurt) * 2.2f;
    }
    c->last_hurt = hurt;
    c->roll = anim_spring(c->roll, &c->roll_vel, 0.0f, 8.0f, dt);

    float s = sinf(c->stride);
    float amp = (0.05f + 0.035f * c->sprint) * c->bob;
    float lateral = amp * 0.9f * s; /* sways toward the stance foot, one side per step */
    float rx = cosf(p->yaw), rz = sinf(p->yaw); /* camera right */

    o.dy = -c->crouch;
    if (effects) {
        float vertical = -amp * s * s; /* lowest as each foot lands */
        o.dx = rx * lateral;
        o.dz = rz * lateral;
        o.dy += vertical + c->dip;
        o.roll = c->roll + 0.012f * c->bob * s + 0.02f * c->swim * sinf((float)c->t * 1.3f);
        o.pitch_add = -0.006f * c->bob * s * s;
        o.dy += 0.04f * c->swim * sinf((float)c->t * 0.9f);
    }
    if (fov_fx) o.fov_scale = 1.0f + 0.12f * c->sprint;
    return o;
}

camera_mode camera_next(camera_mode m, int debug)
{
    switch (m) {
    case CAM_FIRST: return CAM_BEHIND;
    case CAM_BEHIND: return CAM_FRONT;
    case CAM_FRONT: return debug ? CAM_FREE : CAM_FIRST;
    default: return CAM_FIRST;
    }
}

int camera_mode_parse(const char *s)
{
    static const char *const NAMES[] = {"first", "back", "front", "free"};
    for (int i = 0; i < 4; i++)
        if (strcmp(s, NAMES[i]) == 0) return i;
    return -1;
}

#define ARM_GAP 0.25f  /* m the camera keeps from a block: the near plane sits just in front of it */
#define ARM_STEP 0.05f /* m between the samples along the arm */
#define ARM_EASE 0.25f /* s for the arm to win back most of its length */

/* The free length along dir from head, sampled along the centre line and
 * four lines ARM_GAP to its sides so the view's corners stay clear too. A
 * side line that starts inside a block (a ceiling just above the head)
 * says nothing about the arm and is skipped. */
static float arm_room(const world *w, dvec3 head, vec3 dir, float want)
{
    vec3 side = v3(-dir.z, 0.0f, dir.x);
    float sl = sqrtf(side.x * side.x + side.z * side.z);
    side = sl > 1e-4f ? v3(side.x / sl, 0.0f, side.z / sl) : v3(1.0f, 0.0f, 0.0f);
    vec3 up = v3(side.y * dir.z - side.z * dir.y, side.z * dir.x - side.x * dir.z, side.x * dir.y - side.y * dir.x);
    float room = want + ARM_GAP;
    for (int k = 0; k < 5; k++) {
        float ox = 0.0f, oy = 0.0f, oz = 0.0f;
        if (k > 0) {
            vec3 o = k < 3 ? side : up;
            float sgn = (k & 1) ? 1.0f : -1.0f;
            ox = o.x * ARM_GAP * sgn;
            oy = o.y * ARM_GAP * sgn;
            oz = o.z * ARM_GAP * sgn;
        }
        for (int i = 0; (float)i * ARM_STEP < room; i++) {
            float d = (float)i * ARM_STEP;
            double x = head.x + (double)(ox + dir.x * d), y = head.y + (double)(oy + dir.y * d);
            double z = head.z + (double)(oz + dir.z * d);
            if (!block_solid(world_get(w, (int)floor(x), (int)floor(y), (int)floor(z)))) continue;
            if (k == 0 || d > 0.0f) room = d;
            break;
        }
    }
    return fmaxf(0.0f, room - ARM_GAP);
}

camera_view camera_third(const world *w, float *arm, dvec3 head, float yaw, float pitch, int front, float want,
                         float dt)
{
    camera_view v = {head, yaw, pitch};
    if (front) {
        v.yaw = fmodf(yaw + (float)MC_PI, 2.0f * (float)MC_PI);
        v.pitch = -pitch;
    }
    vec3 look = look_dir(v.yaw, v.pitch);
    vec3 out = v3(-look.x, -look.y, -look.z); /* from the head toward the camera */
    float room = arm_room(w, head, out, want);
    if (room < *arm) *arm = room;
    else *arm += (room - *arm) * (1.0f - expf(-dt / ARM_EASE));
    v.eye = dv3(head.x + (double)(out.x * *arm), head.y + (double)(out.y * *arm), head.z + (double)(out.z * *arm));
    return v;
}

dvec3 camera_body_pivot(const world *w, dvec3 feet, float yaw)
{
    vec3 f = look_dir(yaw, 0.0f);
    dvec3 p = dv3(feet.x, feet.y + 0.5, feet.z);
    for (int i = 8; i > 0; i--) {
        float d = 0.1f * (float)i;
        dvec3 q = dv3(feet.x + (double)(f.x * d), feet.y + 0.5, feet.z + (double)(f.z * d));
        if (!block_solid(world_get(w, (int)floor(q.x), (int)floor(q.y), (int)floor(q.z)))) return q;
    }
    return p; /* the feet's own cell, where the body stood */
}

dvec3 camera_fly(dvec3 at, float yaw, float pitch, float fwd, float right, float up, float speed, float dt)
{
    vec3 look = look_dir(yaw, pitch);
    float s = speed * dt;
    return dv3(at.x + (double)((look.x * fwd + cosf(yaw) * right) * s), at.y + (double)((look.y * fwd + up) * s),
               at.z + (double)((look.z * fwd + sinf(yaw) * right) * s));
}
