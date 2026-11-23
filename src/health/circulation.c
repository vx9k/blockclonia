/* The lumped circulation (circulation.h). Compartment layout and starting
 * constants after Heldt et al. (2002); chamber timing after Stergiopulos
 * et al. (1996); see CREDITS.md. */
#include "circulation.h"

#include <math.h>
#include <string.h>

/* mmHg per metre of blood column: 1060 kg/m^3 x 9.81 m/s^2 / 133.3 Pa/mmHg. */
#define RHO_G 78.0
#define PI_D 3.14159265358979323846

/* Stergiopulos et al. (1996)'s double Hill elastance curve, fitted to
 * human ventricles: rise and relaxation in units of the curve's period. */
#define HILL_A1 0.303
#define HILL_A2 0.508
#define HILL_N1 1.32
#define HILL_N2 21.9

/* How long each curve's period is for a cycle of rr seconds: systole
 * shortens with the square root of the cycle (Bazett's relation), so a
 * 70 bpm ventricle peaks ~0.28 s after it fires and a 200 bpm one still
 * relaxes before the next beat. */
#define T_VENT 0.82
#define T_ATRIA 0.45

enum { CH_RA, CH_RV, CH_LA, CH_LV };

/* Valve resistances (mmHg s/mL): open human valves drop a few mmHg at
 * peak flow (Heldt et al. 2002). */
#define R_AV_VALVE 0.003
#define R_SL_VALVE 0.006
#define R_PULM 0.08  /* pulmonary arterioles: PA mean ~15 to PV ~8 at 5 L/min */
#define R_PV_LA 0.01
#define R_AO_AA 0.08 /* the aorta's characteristic impedance, root to the compliant tree */
#define R_IVC_RA 0.015
#define R_SVC_RA 0.06
#define R_COR 20.0   /* coronary bed: ~4 % of output at rest */

static double hill(double x)
{
    if (x <= 0.0) return 0.0;
    double a = pow(x / HILL_A1, HILL_N1), b = pow(x / HILL_A2, HILL_N2);
    return a / (1.0 + a) / (1.0 + b);
}

/* 0..1 activation of a chamber fired at ta (or at ta_prev before ta). */
static double activation(const circ *c, double t, double ta, double ta_prev, double period)
{
    double s = (ta >= 0.0 && t >= ta) ? ta : ta_prev;
    if (s < 0.0 || t < s) return 0.0;
    return hill((t - s) / period) / c->hill_peak;
}

/* The bed's height and which arteries feed it and which veins drain it. */
static const int BED_ART[CB_COUNT] = {CC_AO, CC_AO, CC_AA, CC_AA, CC_AA};
static const int BED_VEIN_OUT[CB_COUNT] = {CC_SVC, CC_SVC, CC_IVC, CC_IVC, CC_IVC};

static double bed_height(const circ_input *in, int b)
{
    switch (b) {
    case CB_BRAIN: return (double)in->h_head;
    case CB_SPLANCH: return (double)in->h_splanch;
    case CB_RENAL: return 0.5 * (double)in->h_splanch; /* the kidneys sit high at the back */
    case CB_LEGS: return (double)in->h_legs;
    default: return 0.0;
    }
}

/* Pressure outside each compartment. */
static double outside(const circ_input *in, int i)
{
    switch (i) {
    case CC_RA:
    case CC_RV:
    case CC_PA:
    case CC_PV:
    case CC_LA:
    case CC_LV:
    case CC_AO:
    case CC_SVC: return (double)in->p_thorax;
    case CC_AA:
    case CC_SPLANCH:
    case CC_RENAL:
    case CC_IVC: return (double)in->p_abdomen;
    case CC_LEGS: return (double)in->leg_squeeze;
    default: return 0.0;
    }
}

/* Elastic pressure of a vessel holding v. Veins with a vmax stiffen as
 * they fill (Heldt et al. 2002's arctangent law), which is what stops the
 * legs taking the whole blood volume when standing. */
static double vessel_pressure(const circ *c, int i, double v, double v0)
{
    double s = v - v0;
    if (c->vmax[i] > 0.0 && s > 0.0) {
        double x = fmin(PI_D * s / (2.0 * c->vmax[i]), 1.55);
        return 2.0 * c->vmax[i] / (PI_D * c->c[i]) * tan(x);
    }
    return s / c->c[i];
}

typedef struct {
    double q_aov, q_bed[CB_COUNT], q_cor, q_finger, lost;
} flow_out;

/* Pressures from volumes, then every flow; dv gets each compartment's
 * net inflow (mL/s). */
static void derive(const circ *c, const circ_input *in, const double *v, double t, double *p, double *dv, flow_out *fo)
{
    double rr = in->rr > 0.2f ? (double)in->rr : 0.857;
    double sq = sqrt(rr);
    double fa = activation(c, t, in->t_atria, in->t_atria_prev, T_ATRIA * sq) * (double)in->atria_on;
    double fv = activation(c, t, in->t_vent, in->t_vent_prev, T_VENT * sq);
    double scale[4] = {(double)in->emax_r, (double)in->emax_r, (double)in->emax_l, (double)in->emax_l};
    double vent_on = (double)in->vent_on;

    double v_heart = v[CC_RA] + v[CC_RV] + v[CC_LA] + v[CC_LV];
    double peri = fmax(0.0, 0.4 * (exp((v_heart - 500.0) / 80.0) - 1.0));
    static const int CHAMBER[4] = {CC_RA, CC_RV, CC_LA, CC_LV};
    for (int k = 0; k < 4; k++) {
        double f = (k == CH_RA || k == CH_LA) ? fa : fv * vent_on;
        double peak = fmax(0.0, c->emax0[k] * scale[k] - c->emin0[k]);
        double e = c->emin0[k] + peak * f;
        int i = CHAMBER[k];
        p[i] = e * (v[i] - c->v0[i]) + peri + outside(in, i);
    }
    for (int i = CC_PA; i < CC_COUNT; i++) {
        if (i == CC_LA || i == CC_LV) continue;
        double v0 = c->v0[i];
        /* The gut's veins are the body's blood reserve: the nerves change
         * their unstressed volume twice as much as other veins' (Ursino
         * 1998's splanchnic against extrasplanchnic gains). */
        if (i == CC_SPLANCH) v0 *= fmax(1.0 + 2.0 * ((double)in->v0_scale - 1.0), 0.6);
        else if (i >= CC_BRAIN) v0 *= (double)in->v0_scale;
        p[i] = vessel_pressure(c, i, v[i], v0) + outside(in, i);
    }

    memset(dv, 0, sizeof(double) * CC_COUNT);
    memset(fo, 0, sizeof *fo);
#define MOVE(a, b, q)    \
    do {                 \
        double q_ = (q); \
        dv[a] -= q_;     \
        dv[b] += q_;     \
    } while (0)

    /* Right heart and lungs. */
    MOVE(CC_RA, CC_RV, fmax(0.0, p[CC_RA] - p[CC_RV]) / R_AV_VALVE);
    MOVE(CC_RV, CC_PA, fmax(0.0, p[CC_RV] - p[CC_PA]) / R_SL_VALVE);
    MOVE(CC_PA, CC_PV, (p[CC_PA] - p[CC_PV]) / R_PULM);
    MOVE(CC_PV, CC_LA, (p[CC_PV] - p[CC_LA]) / R_PV_LA);
    /* Left heart. */
    MOVE(CC_LA, CC_LV, fmax(0.0, p[CC_LA] - p[CC_LV]) / R_AV_VALVE);
    fo->q_aov = fmax(0.0, p[CC_LV] - p[CC_AO]) / R_SL_VALVE;
    MOVE(CC_LV, CC_AO, fo->q_aov);
    MOVE(CC_AO, CC_AA, (p[CC_AO] - p[CC_AA]) / R_AO_AA);
    /* The coronary arteries fill mostly in diastole: contracting muscle
     * squeezes the vessels running through it. */
    fo->q_cor = fmax(0.0, p[CC_AO] - p[CC_RA]) / (R_COR * (1.0 + 2.0 * fv * vent_on));
    MOVE(CC_AO, CC_RA, fo->q_cor);

    /* Beds: the artery feeds the bed's veins across its resistance, and the
     * veins drain to the great veins; the column of blood between heart
     * and bed adds to one and takes from the other. */
    for (int b = 0; b < CB_COUNT; b++) {
        int vein = CC_BRAIN + b, art = BED_ART[b], out = BED_VEIN_OUT[b];
        double col = RHO_G * bed_height(in, b);
        /* The skull's pressure (about zero standing) holds the brain's
         * veins open, and the neck veins collapse above the heart: no
         * siphon helps the head, so its inflow sees a waterfall. */
        double down = b == CB_BRAIN ? fmax(p[vein], 0.0) : p[vein];
        double q_in = fmax(0.0, p[art] - col - down) / (c->r_bed0[b] * (double)in->r_bed[b]);
        double q_out = (p[vein] + col - p[out]) / c->r_ven[b];
        /* Leg veins have valves: blood can't fall back down them. */
        if (b == CB_LEGS) q_out = fmax(0.0, q_out);
        MOVE(art, vein, q_in);
        MOVE(vein, out, q_out);
        fo->q_bed[b] = q_in;
        if (b == CB_UPPER) fo->q_finger = q_in;
        double d_art = (double)in->drain_art[b] * fmax(0.0, p[art] - col);
        dv[art] -= d_art;
        fo->lost += d_art;
    }
    MOVE(CC_IVC, CC_RA, (p[CC_IVC] - p[CC_RA]) / R_IVC_RA);
    MOVE(CC_SVC, CC_RA, (p[CC_SVC] - p[CC_RA]) / R_SVC_RA);
#undef MOVE

    for (int i = 0; i < CC_COUNT; i++) {
        double d = (double)in->drain[i] * fmax(0.0, p[i]);
        dv[i] -= d;
        fo->lost += d;
    }
    /* An empty compartment has nothing to give. */
    for (int i = 0; i < CC_COUNT; i++)
        if (v[i] <= 0.0 && dv[i] < 0.0) dv[i] = 0.0;
}

static void beat_reset(circ *c, double t)
{
    c->beat_t = t;
    c->b_vmax = -1e30;
    c->b_vmin = 1e30;
    c->b_pmax = -1e30;
    c->b_pmin = 1e30;
    c->b_cor = c->b_cor_dia = 0.0;
    c->b_pao = c->b_pra = c->b_qao = 0.0;
    memset(c->b_qbed, 0, sizeof c->b_qbed);
    c->b_n = 0;
    c->w_n = c->w_i = 0;
}

static void beat_finish(circ *c)
{
    if (c->b_vmax < c->b_vmin || c->b_n == 0) return;
    double n = (double)c->b_n;
    c->beat_len = (float)(n * CIRC_H);
    c->p_ao_mean = (float)(c->b_pao / n);
    c->p_ra_mean = (float)(c->b_pra / n);
    c->beat_q_ao = (float)(c->b_qao / n);
    c->beat_q_cor = (float)(c->b_cor / n);
    for (int b = 0; b < CB_COUNT; b++) c->beat_q_bed[b] = (float)(c->b_qbed[b] / n);
    c->edv = (float)c->b_vmax;
    c->esv = (float)c->b_vmin;
    c->p_ao_max = (float)c->b_pmax;
    c->p_ao_min = (float)c->b_pmin;
    c->cor_dia_frac = c->b_cor > 0.0 ? (float)(c->b_cor_dia / c->b_cor) : 0.0f;
    memcpy(c->pv_v, c->w_v, sizeof c->pv_v);
    memcpy(c->pv_p, c->w_p, sizeof c->pv_p);
    c->pv_n = c->w_n;
}

void circ_step(circ *c, const circ_input *in, double t0, double dt)
{
    double t_end = t0 + dt;
    if (fabs((double)c->steps * CIRC_H - t0) > 0.5) c->steps = (long long)floor(t0 / CIRC_H + 1e-6);
    double rr = in->rr > 0.2f ? (double)in->rr : 0.857;
    int pv_every = (int)fmax(1.0, rr / (CIRC_PV_N * CIRC_H));

    double p1[CC_COUNT] = {0}, p2[CC_COUNT] = {0}, k1[CC_COUNT] = {0}, k2[CC_COUNT] = {0}, vt[CC_COUNT] = {0};
    flow_out f1 = {0}, f2 = {0};
    double sum_bed[CB_COUNT] = {0}, sum_cor = 0.0, sum_ao = 0.0, sum_fin = 0.0, lost = 0.0;
    int n = 0;
    while ((double)(c->steps + 1) * CIRC_H <= t_end + 1e-9) {
        double t = (double)c->steps * CIRC_H;
        derive(c, in, c->v, t, p1, k1, &f1);

        /* A new ventricular activation closes the beat being measured. */
        double sv = (in->t_vent >= 0.0 && t >= in->t_vent) ? in->t_vent : in->t_vent_prev;
        if (sv >= 0.0 && sv != c->beat_t) {
            if (c->beat_t >= 0.0) beat_finish(c);
            beat_reset(c, sv);
        }
        if (c->beat_t >= 0.0) {
            c->b_vmax = fmax(c->b_vmax, c->v[CC_LV]);
            c->b_vmin = fmin(c->b_vmin, c->v[CC_LV]);
            c->b_pmax = fmax(c->b_pmax, p1[CC_AO]);
            c->b_pmin = fmin(c->b_pmin, p1[CC_AO]);
            c->b_cor += f1.q_cor;
            c->b_pao += p1[CC_AO];
            c->b_pra += p1[CC_RA];
            c->b_qao += f1.q_aov;
            for (int b = 0; b < CB_COUNT; b++) c->b_qbed[b] += f1.q_bed[b];
            c->b_n++;
            if (f1.q_aov <= 0.0) c->b_cor_dia += f1.q_cor;
            if (c->w_i++ % pv_every == 0 && c->w_n < CIRC_PV_N) {
                c->w_v[c->w_n] = (float)c->v[CC_LV];
                c->w_p[c->w_n] = (float)p1[CC_LV];
                c->w_n++;
            }
        }

        /* Heun's method: an Euler guess, then the average of both slopes. */
        for (int i = 0; i < CC_COUNT; i++) vt[i] = fmax(0.0, c->v[i] + CIRC_H * k1[i]);
        derive(c, in, vt, t + CIRC_H, p2, k2, &f2);
        for (int i = 0; i < CC_COUNT; i++) c->v[i] = fmax(0.0, c->v[i] + 0.5 * CIRC_H * (k1[i] + k2[i]));

        for (int b = 0; b < CB_COUNT; b++) sum_bed[b] += f1.q_bed[b];
        sum_cor += f1.q_cor;
        sum_ao += f1.q_aov;
        sum_fin += f1.q_finger;
        lost += 0.5 * CIRC_H * (f1.lost + f2.lost);
        c->steps++;
        n++;
    }
    c->lost = (float)lost;
    if (n == 0) return;
    for (int b = 0; b < CB_COUNT; b++) c->q_bed[b] = (float)(sum_bed[b] / n);
    c->q_cor = (float)(sum_cor / n);
    c->q_ao = (float)(sum_ao / n);
    c->q_finger = (float)(sum_fin / n);
    derive(c, in, c->v, (double)c->steps * CIRC_H, p1, k1, &f1);
    for (int i = 0; i < CC_COUNT; i++) c->p[i] = (float)p1[i];
    c->wave_ao = c->p[CC_AO];
    c->wave_finger = (float)f1.q_finger;
}

void circ_init(circ *c, float blood_ml, float heart_size, float art_stiff)
{
    memset(c, 0, sizeof *c);
    double hs = heart_size > 0.3f ? (double)heart_size : 1.0;
    double as = art_stiff > 0.3f ? (double)art_stiff : 1.0;
    /* Chambers (Heldt et al. 2002, tuned to an adult's resting volumes): a
     * bigger heart holds more and is less stiff per mL. */
    static const double EMAX[4] = {0.30, 0.90, 0.40, 3.00}; /* mmHg/mL */
    static const double EMIN[4] = {0.15, 0.08, 0.15, 0.08};
    static const double V0H[4] = {10.0, 40.0, 10.0, 15.0}; /* mL */
    static const int CHAMBER[4] = {CC_RA, CC_RV, CC_LA, CC_LV};
    for (int k = 0; k < 4; k++) {
        c->emax0[k] = EMAX[k] / hs;
        c->emin0[k] = EMIN[k] / hs;
        c->v0[CHAMBER[k]] = V0H[k] * hs;
        c->c[CHAMBER[k]] = hs / EMIN[k];
    }
    /* Vessels: compliance (mL/mmHg) and unstressed volume (mL). The
     * systemic veins hold ~2900 mL unstressed, most of it in the gut. */
    static const struct {
        int i;
        double c, v0, vmax;
    } V[] = {
        {CC_PA, 3.4, 160.0, 0.0},          {CC_PV, 9.0, 430.0, 0.0},    {CC_AO, 0.25, 500.0, 0.0},
        {CC_AA, 1.5, 200.0, 0.0},          {CC_BRAIN, 3.0, 120.0, 0.0}, {CC_UPPER, 8.0, 550.0, 0.0},
        {CC_SPLANCH, 55.0, 1300.0, 700.0}, {CC_RENAL, 8.0, 150.0, 0.0}, {CC_LEGS, 19.0, 300.0, 350.0},
        {CC_IVC, 2.0, 100.0, 0.0},         {CC_SVC, 15.0, 200.0, 0.0},
    };
    for (size_t k = 0; k < sizeof V / sizeof V[0]; k++) {
        c->c[V[k].i] = V[k].c;
        c->v0[V[k].i] = V[k].v0;
        c->vmax[V[k].i] = V[k].vmax;
    }
    c->c[CC_AO] /= as;
    c->c[CC_AA] /= as;
    /* Beds (mmHg s/mL): brain ~750, kidneys ~1100, gut ~1300, legs and
     * arms ~800 mL/min each at rest. */
    static const double RB[CB_COUNT] = {6.8, 6.5, 3.9, 4.7, 6.5};
    static const double RV[CB_COUNT] = {0.3, 0.2, 0.1, 0.1, 0.06};
    for (int b = 0; b < CB_COUNT; b++) {
        c->r_bed0[b] = RB[b];
        c->r_ven[b] = RV[b];
    }
    c->hill_peak = 1e-9;
    for (int k = 1; k < 2000; k++) c->hill_peak = fmax(c->hill_peak, hill(k * 0.0005));

    /* Fill at typical resting pressures, then shift every vessel's pressure
     * alike until the total is blood_ml. */
    static const double P_REF[CC_COUNT] = {4, 4, 15, 8, 7, 7, 95, 93, 2, 7, 8, 8, 8, 5, 4};
    double total = 0.0, c_sum = 0.0;
    for (int i = 0; i < CC_COUNT; i++) {
        c->v[i] = c->v0[i] + c->c[i] * P_REF[i];
        total += c->v[i];
        if (i >= CC_PA && i != CC_LA && i != CC_LV) c_sum += c->c[i];
    }
    double shift = ((double)blood_ml - total) / c_sum;
    for (int i = CC_PA; i < CC_COUNT; i++)
        if (i != CC_LA && i != CC_LV) c->v[i] = fmax(0.0, c->v[i] + c->c[i] * shift);
    c->beat_t = -1.0;
    for (int i = 0; i < CC_COUNT; i++) c->p[i] = (float)P_REF[i];
    c->p_ao_mean = 93.0f;
    c->p_ra_mean = 4.0f;
}

float circ_volume(const circ *c)
{
    double s = 0.0;
    for (int i = 0; i < CC_COUNT; i++) s += c->v[i];
    return (float)s;
}

void circ_add(circ *c, int comp, float ml)
{
    if (comp < 0 || comp >= CC_COUNT) return;
    c->v[comp] = fmax(0.0, c->v[comp] + (double)ml);
}
