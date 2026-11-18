#include "recoil.h"

#include <math.h>

/* The powder gas leaves the muzzle faster than the bullet: SAAMI takes
 * 4,000 ft/s (1,219 m/s) for smokeless rifle powder; black powder's
 * heavier, slower gas is taken at 1.5 times the bullet's speed. */
#define GAS_SMOKELESS 1219.0f
#define GAS_BLACK 1.5f
/* The bore runs about 5 cm above where the butt meets the shoulder, so
 * the push back turns the rifle muzzle-up about the shoulder. The turning
 * inertia (rifle, arms and the shoulder giving way): about 0.4 kg·m² with
 * the stock in the shoulder and the cheek on it, 0.25 at the hip. */
#define BORE_OVER_SHOULDER 0.05f
#define INERTIA_SHOULDER 0.4f
#define INERTIA_HIP 0.25f
/* The hold that brings it back: 2 Hz, damping 0.55, settled in about half
 * a second, as a shooter's sights fall back onto the target. */
#define HOLD_HZ 2.0f
#define HOLD_DAMP 0.55f
/* Where the sights come to rest is off the old mark by a few milliradians
 * from the shoulder, more from the hip. */
#define REST_SHOULDER 0.003f
#define REST_HIP 0.015f
#define SUBSTEP (1.0f / 480.0f)

static float unit(recoil *r) /* -1 .. 1 */
{
    r->seed = r->seed * 1664525u + 1013904223u;
    return (float)(r->seed >> 8) / 8388608.0f - 1.0f;
}

static float finite01(float v) { return isfinite(v) ? fminf(fmaxf(v, 0.0f), 1.0f) : 0.0f; }

void recoil_init(recoil *r, uint32_t seed) { *r = (recoil){.seed = seed ? seed : 1u}; }

float recoil_impulse(const ammo_def *a)
{
    if (!a) return 0.0f;
    float gas = a->black ? GAS_BLACK * a->muzzle : GAS_SMOKELESS;
    return a->mass * a->muzzle + a->charge * gas;
}

float recoil_velocity(const ammo_def *a, float kg) { return kg > 0.0f ? recoil_impulse(a) / kg : 0.0f; }

float recoil_energy(const ammo_def *a, float kg)
{
    float v = recoil_velocity(a, kg);
    return 0.5f * kg * v * v;
}

void recoil_fire(recoil *r, const ammo_def *a, float aim)
{
    float s = finite01(aim);
    float inertia = INERTIA_HIP + (INERTIA_SHOULDER - INERTIA_HIP) * s;
    float w = recoil_impulse(a) * BORE_OVER_SHOULDER / inertia;
    /* Mostly up; a right-handed hold also throws it a little right. */
    r->vp += w;
    r->vy += w * (0.15f + 0.1f * unit(r));
    float rest = REST_HIP + (REST_SHOULDER - REST_HIP) * s;
    r->rest_p = r->pitch + rest * unit(r);
    r->rest_y = r->yaw + rest * unit(r);
}

void recoil_step(recoil *r, float dt, float *dpitch, float *dyaw)
{
    *dpitch = 0.0f;
    *dyaw = 0.0f;
    if (!isfinite(dt) || dt <= 0.0f) return;
    if (dt > 0.25f) dt = 0.25f; /* a hitch: the hold has settled meanwhile */
    /* Fixed substeps, so 30 and 144 fps follow the same curve. */
    int n = (int)ceilf(dt / SUBSTEP);
    float h = dt / (float)n, w = 2.0f * 3.14159265f * HOLD_HZ;
    for (int i = 0; i < n; i++) {
        r->vp += (-w * w * (r->pitch - r->rest_p) - 2.0f * HOLD_DAMP * w * r->vp) * h;
        r->vy += (-w * w * (r->yaw - r->rest_y) - 2.0f * HOLD_DAMP * w * r->vy) * h;
        r->pitch += r->vp * h;
        r->yaw += r->vy * h;
    }
    *dpitch = r->pitch - r->applied_p;
    *dyaw = r->yaw - r->applied_y;
    r->applied_p = r->pitch;
    r->applied_y = r->yaw;
    /* Settled: the look already holds it all, so start again from zero
     * and the offsets never grow. */
    if (fabsf(r->vp) + fabsf(r->vy) < 1e-4f && fabsf(r->pitch - r->rest_p) + fabsf(r->yaw - r->rest_y) < 1e-5f)
        *r = (recoil){.seed = r->seed};
}
