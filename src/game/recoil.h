/* Recoil and the shooter's recovery. A shot's momentum (the bullet's and
 * the powder gas's) drives the rifle back into the shoulder; acting along
 * the bore, a few centimetres above the shoulder, it also climbs the
 * muzzle. The shooter's hold, a damped spring, brings the sights back
 * down, never quite to where they were. Works in view angles (radians)
 * the caller adds to the player's look; frame-rate independent. It does
 * not draw anything (viewmodel.c shows the kick) or move bullets. */
#ifndef MC_RECOIL_H
#define MC_RECOIL_H
#include "ballistics.h"

#include <stdint.h>

#define RIFLE_KG 4.0f /* a bolt-action service rifle, loaded */

typedef struct {
    float pitch, yaw;       /* current offset from where the shooter held, rad */
    float vp, vy;           /* their rates, rad/s */
    float rest_p, rest_y;   /* where the hold settles after this shot */
    float applied_p, applied_y; /* the offset already handed to the caller */
    uint32_t seed;
} recoil;

void recoil_init(recoil *r, uint32_t seed);
/* The momentum a shot of this round gives the rifle, N·s: the bullet's,
 * plus the powder gas leaving the muzzle. */
float recoil_impulse(const ammo_def *a);
/* Free recoil velocity (m/s) and energy (J) of a rifle of mass kg. */
float recoil_velocity(const ammo_def *a, float kg);
float recoil_energy(const ammo_def *a, float kg);
/* A shot goes off: the muzzle starts to climb. aim (0 hip .. 1 sights)
 * steadies it, the stock being in the shoulder. */
void recoil_fire(recoil *r, const ammo_def *a, float aim);
/* Advances dt seconds. Writes the change in pitch and yaw (rad) since the
 * last call, for the caller to add to the look. */
void recoil_step(recoil *r, float dt, float *dpitch, float *dyaw);
#endif
