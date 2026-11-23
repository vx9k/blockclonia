/* Conduction: when the heart's chambers fire. The sinus node sets the
 * atria going at the rate the nerves ask; the AV node passes each impulse
 * to the ventricles after a delay, unless it is still recovering; a slow
 * escape pacemaker below takes over when nothing comes through; and an
 * irritable ventricle adds early ectopic beats of its own. Each rhythm in
 * health.h is a mode of this timing.
 *
 * It only says when: how hard the chambers squeeze and what that does to
 * blood is circulation.c's, and the rate and delay the nerves ask for come
 * from autonomic.c. Events are timed exactly inside a step, not snapped to
 * it, so a PR interval is what it is at any frame rate. */
#ifndef MC_CONDUCTION_H
#define MC_CONDUCTION_H

#include <stdint.h>

#define COND_VT_RATE 165.0  /* bpm, monomorphic VT that still ejects */
#define COND_PVT_RATE 210.0 /* bpm: too fast to fill, so no pulse */
#define COND_ESCAPE_RATE 36.0 /* bpm, a ventricular escape pacemaker */

typedef struct {
    double t_atria, t_vent, prev_vent; /* s, last activations; -1 never */
    double pending_av;     /* s, when an impulse in the AV node reaches the ventricles; -1 none */
    double av_free;        /* s, when the AV node can conduct again */
    double next_af;        /* s, next fibrillating atrial wavelet */
    double next_own;       /* s, next beat of a ventricular focus (VT, VF) */
    double pending_ect;    /* s, when an ectopic focus became ready to fire; -1 none */
    double ect_coupling;   /* s after a beat that the waiting ectopic fires */
    double esc_ref;        /* s, where the escape pacemaker counts from */
    float sa_phase;        /* 0..1 to the next sinus firing */
    int last_ectopic;      /* the last ventricular beat was ectopic */
    int rhythm;            /* the mode last stepped, to restart foci on a change */
    int beats;             /* ventricular beats so far (wraps) */
} conduction;

typedef struct {
    float sa_rate;      /* bpm the nerves ask of the sinus node */
    float av_delay;     /* s, PR interval */
    float irritability; /* ectopic ventricular beats per minute expected now */
    int rhythm;         /* rhythm_kind (health.h) */
} conduction_input;

void cond_init(conduction *k);

/* Advances from t to t + dt, drawing chance from *rng. Returns how many
 * times the ventricles fired. */
int cond_step(conduction *k, const conduction_input *in, double t, double dt, uint32_t *rng);

/* How coordinated each contraction is in a rhythm (0..1). */
float cond_atria_on(int rhythm);
float cond_vent_on(int rhythm);

#endif
