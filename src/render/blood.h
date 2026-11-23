/* Blood leaving the player's wounds: drops that fly and land, and the
 * stains they leave on whatever block face they hit, floor, wall or
 * ceiling. A venous wound oozes and drips at its bleed rate; an arterial
 * one spurts with each heartbeat, the jet as fast as the pressure behind
 * it drives it (Bernoulli), so the spurts shorten as the blood pressure
 * falls. Arterial blood is scarlet, darkening toward venous red as the
 * blood's oxygen falls; both dry to brown. A drop landing on a stain
 * grows it. Stains dry and clean themselves up after BLOOD_STAIN_S so the
 * world never fills with them, and water washes them off sooner.
 *
 * Visual only: it runs on the frame's dt with its own random state and
 * never feeds back into the deterministic simulation. The wounds and
 * their bleeding come from health.h; this does not decide how much a body
 * bleeds, only shows it. No window or GPU. */
#ifndef MC_BLOOD_H
#define MC_BLOOD_H

#include "entity.h"
#include "lightfx.h"
#include "mathlib.h"
#include "world.h"

#define BLOOD_MAX_DROPS 512
#define BLOOD_STAIN_S 30.0f   /* s a stain lasts on dry ground */
#define BLOOD_DROP_ML 0.05f   /* a drop of blood (about 4.6 mm across) */
#define BLOOD_JET 0.6f        /* of the Bernoulli speed a jet keeps through the wound */

/* A bleeding wound as the blood sees it. */
typedef struct {
    dvec3 at;     /* where it opens, world */
    dvec3 out;    /* unit: the way it faces */
    float ml_min; /* bleeding right now, mL/min */
    int arterial; /* spurts with the beat */
    float p_column; /* mmHg the blood above the wound adds to the heart's (less above the heart) */
} blood_source;

typedef struct {
    dvec3 pos, vel;
    float ml;
    uint8_t shade; /* 0 venous dark red .. 255 bright arterial */
} blood_drop;

typedef struct {
    dvec3 pos;     /* on the face, world */
    float ml;      /* blood in it, which sets its size */
    float age;     /* s */
    uint8_t face;  /* the face's outward normal: 0 -x, 1 +x, 2 -y, 3 +y, 4 -z, 5 +z */
    uint8_t shade;
    uint8_t live;
} blood_stain;

#define BLOOD_SOURCES 8 /* wounds followed at once (health.h's wounds, the worst first) */

typedef struct {
    blood_drop drop[BLOOD_MAX_DROPS];
    int drops;
    blood_stain *stain; /* a ring of max_stains (sys_plan's blood_stains) */
    int max_stains, next;
    float owed[BLOOD_SOURCES]; /* mL bled but not yet out as drops */
    uint32_t rng;
} blood_state;

void blood_init(blood_state *b, int max_stains, uint32_t seed);
void blood_free(blood_state *b);

/* This frame's bleeding from n sources (at most BLOOD_SOURCES; source i
 * keeps its place from frame to frame). beat is 1 if the heart beat since
 * the last call, p_sys that beat's systolic pressure in mmHg, hr the
 * heart rate, sat the arterial oxygen saturation (0..1), and body the
 * body's velocity, which the blood leaves with. Venous blood drips out as
 * it bleeds; arterial blood leaves in a jet at each beat. */
void blood_bleed(blood_state *b, const blood_source *s, int n, int beat, float p_sys, float hr, float sat, dvec3 body,
                 float dt);

/* Drops fly under gravity and air drag and stain the face they hit (a
 * drop in water is gone); stains age, and wash off under water. */
void blood_step(blood_state *b, const world *w, float dt);

/* The drops and stains as instanced boxes relative to eye, lit by their
 * cells (lf, or daylight alone without one), into out (at most max).
 * Returns how many; all opaque. */
int blood_build(const blood_state *b, const world *w, const lightfx_frame *lf, dvec3 eye, float daylight,
                entity_instance *out, int max);

/* A stain's colour (0xRRGGBB) for its shade and age: wet blood dries to
 * brown. */
uint32_t blood_colour(int shade, float age);

/* Where a wound on body part (health.h's BP_*) opens on a figure standing
 * at feet facing yaw (0 toward -z), and the way it faces; salt spreads
 * several wounds on one part apart. */
void blood_spot(int part, int salt, dvec3 feet, float yaw, dvec3 *at, dvec3 *out);

/* The sources for a body's wounds (health.h) on a figure standing at feet
 * facing yaw: wound i is source i, up to BLOOD_SOURCES of them, and a
 * bandaged wound bleeds into its bandage. Returns how many. */
struct health;
int blood_from_health(const struct health *h, dvec3 feet, float yaw, blood_source *out);

/* 1 if the heart has beaten since *seen, given the time of its last
 * ventricular activation (health's last_r), which it then remembers. */
int blood_beat(double *seen, double last_r);

#endif
