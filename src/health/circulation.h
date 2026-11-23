/* Circulation: the heart and blood vessels as a lumped (0-D) circuit of
 * elastic compartments, the way Guyton, Heldt et al. (2002) and Ursino
 * (1998) model them (CREDITS.md).
 *
 * Four heart chambers stiffen and relax on their own cycles (time-varying
 * elastance), valves let blood go one way, arteries and veins stretch, and
 * blood flows between them by pressure difference over resistance, with
 * gravity acting on every column. Pressures, pulses and stroke volume come
 * out of that: nothing here states what blood pressure should be.
 *
 * It does not decide when the chambers fire (conduction.c), how tight the
 * vessels are or how hard the heart squeezes (autonomic.c and health.c set
 * those as scales on the inputs), or what bleeds (health.c turns wounds
 * into drains). It knows nothing of oxygen.
 *
 * Units: mL, mmHg, mL/s, seconds, metres. Integrated in fixed CIRC_H
 * sub-steps of absolute time, so how the caller splits time never changes
 * the result. */
#ifndef MC_CIRCULATION_H
#define MC_CIRCULATION_H

#define CIRC_H 0.0005 /* s per sub-step */
#define CIRC_PV_N 64  /* points kept of the last left-ventricular pressure-volume loop */

/* Compartments. BRAIN .. LEGS are each bed's veins; the beds' arteries are
 * resistances from the aorta into them. IVC is the abdominal vena cava, SVC
 * the great veins inside the chest. */
typedef enum {
    CC_RA,
    CC_RV,
    CC_PA,
    CC_PV,
    CC_LA,
    CC_LV,
    CC_AO,
    CC_AA,
    CC_BRAIN,
    CC_UPPER,
    CC_SPLANCH,
    CC_RENAL,
    CC_LEGS,
    CC_IVC,
    CC_SVC,
    CC_COUNT
} circ_comp;

typedef enum { CB_BRAIN, CB_UPPER, CB_SPLANCH, CB_RENAL, CB_LEGS, CB_COUNT } circ_bed;

/* What drives the circuit over one call of circ_step. */
typedef struct {
    /* s, the last two activations of the atria and of the ventricles; -1
     * for never. A sub-step before the newest uses the one before it. */
    double t_atria, t_atria_prev, t_vent, t_vent_prev;
    float rr;                    /* s, the current cycle length: sets how long systole lasts */
    float atria_on, vent_on;     /* 0..1 how coordinated each contraction is */
    float emax_l, emax_r;        /* x peak elastance of each side (contractility) */
    float r_bed[CB_COUNT];       /* x each bed's arterial resistance */
    float v0_scale;              /* x the systemic veins' unstressed volume (< 1 venoconstricts) */
    float p_thorax, p_abdomen;   /* mmHg outside the chest and abdominal compartments */
    float h_head, h_legs, h_splanch; /* m above (+) or below (-) the heart */
    float leg_squeeze;           /* mmHg of muscle squeezing the leg veins now */
    float drain[CC_COUNT];       /* mL/s/mmHg out of a compartment through a wound (veins) */
    float drain_art[CB_COUNT];   /* mL/s/mmHg out of a bed's artery, at the bed's height */
} circ_input;

typedef struct {
    double v[CC_COUNT];          /* mL; double so 10^6 sub-steps don't leak blood to rounding */
    float p[CC_COUNT];           /* mmHg after the last sub-step */
    /* Means over the last call (mL/s). */
    float q_bed[CB_COUNT], q_cor, q_ao, q_finger;
    float lost;                  /* mL drained by wounds during the last call */
    /* The last completed beat (from one ventricular activation to the next). */
    float edv, esv;              /* mL, left ventricle */
    float p_ao_max, p_ao_min;    /* mmHg */
    float cor_dia_frac;          /* share of coronary flow that came with the aortic valve shut */
    float pv_v[CIRC_PV_N], pv_p[CIRC_PV_N];
    int pv_n;
    float wave_ao;               /* mmHg in the aortic root now */
    float wave_finger;           /* mL/s into the arm's bed now (a pulse oximeter's signal) */

    /* This body's constants (circ_init). */
    double c[CC_COUNT], v0[CC_COUNT];
    double vmax[CC_COUNT];       /* > 0: a vein that stiffens as it fills, holding at most this much above v0 */
    double r_bed0[CB_COUNT], r_ven[CB_COUNT];
    double emax0[4], emin0[4];   /* RA, RV, LA, LV */
    double hill_peak;

    /* Integration and the beat in progress. */
    long long steps;             /* sub-steps of absolute time done */
    double beat_t;               /* activation that started the beat being measured, -1 none */
    double b_vmax, b_vmin, b_pmax, b_pmin, b_cor, b_cor_dia;
    float w_v[CIRC_PV_N], w_p[CIRC_PV_N];
    int w_n, w_i;
} circ;

/* A healthy circuit holding blood_ml, at rest. heart_size scales the
 * chambers (1 average), art_stiff the arteries' stiffness (1 average). */
void circ_init(circ *c, float blood_ml, float heart_size, float art_stiff);

/* Advances the circuit from absolute time t0 to t0 + dt. */
void circ_step(circ *c, const circ_input *in, double t0, double dt);

/* mL of blood in the circuit. */
float circ_volume(const circ *c);

/* Puts ml into a compartment (or takes it out, never below empty):
 * infusions and fluid moving in from the tissues. */
void circ_add(circ *c, int comp, float ml);

#endif
