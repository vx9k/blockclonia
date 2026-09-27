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
    float step = 0.7f + 0.08f * hs;
    if (walking) c->stride = fmodf(c->stride + hs * dt / step * (float)MC_PI, 2.0f * (float)MC_PI);
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
    float vertical = -amp * s * s;             /* lowest as each foot lands */
    float lateral = amp * 0.9f * s; /* sways toward the stance foot, one side per step */
    float rx = cosf(p->yaw), rz = sinf(p->yaw); /* camera right */

    o.dy = -c->crouch;
    if (effects) {
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
