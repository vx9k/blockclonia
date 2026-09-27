/* Small animation helpers shared by the UI, the camera and the view model.
 * Everything is frame-rate independent: pass the frame's dt. */
#ifndef MC_ANIM_H
#define MC_ANIM_H

#include <math.h>

static inline float anim_clamp01(float t) { return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t); }

static inline float ease_out_cubic(float t)
{
    t = 1.0f - anim_clamp01(t);
    return 1.0f - t * t * t;
}

static inline float ease_in_out(float t)
{
    t = anim_clamp01(t);
    return t * t * (3.0f - 2.0f * t);
}

/* Overshoots a little before settling: pops UI elements into place. */
static inline float ease_out_back(float t)
{
    t = anim_clamp01(t) - 1.0f;
    const float c = 1.70158f;
    return 1.0f + t * t * ((c + 1.0f) * t + c);
}

/* Exponential approach of `cur` to `target`; `rate` is 1/time-constant. */
static inline float anim_approach(float cur, float target, float rate, float dt)
{ return target + (cur - target) * expf(-rate * dt); }

/* Critically damped spring, for camera offsets that should settle without
 * wobbling (landing dip). vel is the spring's own state. */
static inline float anim_spring(float cur, float *vel, float target, float omega, float dt)
{
    float x = cur - target;
    float e = expf(-omega * dt);
    float tmp = (*vel + omega * x) * dt;
    *vel = (*vel - omega * tmp) * e;
    return target + (x + tmp) * e;
}

#endif
