/* External ballistics: the rounds the game knows, and one integration
 * step of a bullet under gravity, drag and wind. Drag uses the standard
 * G1 and G7 reference curves scaled by each round's ballistic
 * coefficient, in the air atmos.c describes.
 *
 * It doesn't decide what a bullet hits or what that does (combat.c), and
 * it has no spin drift or Coriolis effect, which are centimetres at the
 * ranges a player can see. No window or GPU. */
#ifndef MC_BALLISTICS_H
#define MC_BALLISTICS_H

#include "atmos.h"

#include <stdint.h>

typedef enum { DRAG_G1, DRAG_G7 } drag_model;

/* How a bullet is made; sets how it penetrates and wounds. */
typedef enum { BUILD_LEAD, BUILD_FMJ, BUILD_SOFT_POINT, BUILD_HOLLOW_POINT, BUILD_AP } bullet_build;

typedef enum {
    AMMO_ROUND_BALL, /* a muzzle-loader's patched lead ball */
    AMMO_BLACK_LEAD, /* the black-powder cartridge's long lead bullet */
    AMMO_SMOKELESS,  /* a jacketed 7.62 mm service round */
    AMMO_MATCH,      /* a heavier 7.62 mm boat-tail for long range */
    AMMO_TRACER,     /* a 7.62 mm round whose base burns bright */
    AMMO_COUNT
} ammo_kind;

typedef struct {
    const char *name;
    float calibre; /* m */
    float mass;    /* kg */
    float muzzle;  /* m/s from the reference barrel */
    float bc;      /* ballistic coefficient, lb/in^2 as published */
    uint8_t drag;  /* drag_model the bc is given against */
    uint8_t build; /* bullet_build */
    uint8_t black; /* fired with black powder, which leaves a thick white cloud */
    float tracer;  /* s its base burns, from leaving the muzzle; 0 for none */
    float charge;  /* kg of powder, whose gas adds to the recoil */
} ammo_def;

const ammo_def *ammo_get(int kind); /* NULL past the table */

/* The reference drag coefficient of a model at a Mach number. */
double ballistics_cd(int model, double mach);

/* Drag deceleration, m/s^2, of round `a` moving through the air at v. */
double ballistics_drag(const ammo_def *a, const atmos_air *air, double v);

/* One step of dt: velocity under gravity and drag against the air moving
 * past it, then position. */
void ballistics_step(const ammo_def *a, const atmos_air *air, dvec3 *pos, dvec3 *vel, double dt);

#endif
