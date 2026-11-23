/* Autonomic control: the nerves and local reflexes that steer the
 * circulation. Stretch receptors in the carotid sinus fire with pressure;
 * the brainstem turns that into vagal and sympathetic traffic, which
 * reaches the heart and vessels after real delays and acts through real
 * time constants (Ursino 1998, CREDITS.md). Exercise adds central command
 * and the muscles' own signals; breathing gates the vagus; the brain keeps
 * its own blood flow steady; a band of random sympathetic traffic gives
 * the slow waves every real pressure trace shows.
 *
 * It reads pressures and drives and returns scales for circulation.c and
 * a rate and delay for conduction.c. It holds no blood and no hormones:
 * adrenaline from the adrenals is health.c's, which reads symp. */
#ifndef MC_AUTONOMIC_H
#define MC_AUTONOMIC_H

#include "circulation.h"

#include <stdint.h>

#define AUTO_HIST 360 /* steps of efferent history: 6 s at 60 Hz */

typedef struct {
    float p_cs;              /* mmHg as the carotid sinus senses it, rate term included */
    float p_prev;            /* last input pressure at the sinus */
    float f_cs;              /* afferent spikes/s */
    float f_s, f_v;          /* sympathetic and vagal efferent spikes/s now */
    float hist_s[AUTO_HIST], hist_v[AUTO_HIST];
    int hist_pos, hist_n;
    float eff_emax, eff_r, eff_v0, eff_ts, eff_tv; /* lagged effector changes from rest */
    float ls0, fv0;          /* resting log-sympathetic and vagal levels */
    float t0;                /* s, the heart period with every effector at rest */
    float command;           /* central command 0..1: fast, resets the baroreflex upward */
    float vagal_off;         /* 0..1 the vagus withdrawn by effort: in seconds, back over a minute */
    float ex_drive;          /* exercise sympathetic drive 0..1: slow */
    float metab;             /* the legs' metabolic dilation 0..1 */
    float metab_fast;        /* 0..1 their rapid dilation at the first contractions */
    float metabo;            /* muscle metaboreflex 0..1 */
    float noise;             /* band-limited sympathetic fluctuation, spikes/s */
    float autoreg;           /* x the brain's resistance, its own regulation */
    float fail;              /* 0..1 brainstem failure */
} autonomic;

typedef struct {
    float p_ao_mean, p_ra_mean; /* mmHg over the last step */
    float h_neck;               /* m, carotid sinus above the heart */
    float effort;               /* 0..1 of the body's aerobic capacity asked for */
    float muscle_o2_debt;       /* 0..1 working muscle short of oxygen */
    float chemo;                /* 0..1 chemoreceptor drive */
    float pain, fear, vagal_surge, diving; /* 0..1 */
    float breath_phase;         /* 0..1 through a breath; below 0.4 is breathing in */
    float brain_flow;           /* the brain's blood flow over its resting flow */
    float gain;                 /* x baroreflex gain (constitution) */
    float noise_amp;            /* x sympathetic fluctuation (constitution) */
    int brain_ok;               /* 0: the brainstem has failed */
} autonomic_input;

typedef struct {
    float sa_rate;           /* bpm */
    float av_delay;          /* s */
    float emax;              /* x contractility */
    float r_bed[CB_COUNT];   /* x each bed's resistance */
    float v0_scale;          /* x venous unstressed volume */
    float symp;              /* 0..1 overall sympathetic outflow */
} autonomic_out;

void auto_init(autonomic *a, float hr_rest);
void auto_step(autonomic *a, const autonomic_input *in, double dt, uint32_t *vrng, autonomic_out *out);

#endif
