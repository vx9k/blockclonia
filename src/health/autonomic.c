/* Autonomic control (autonomic.h). The baroreflex follows Ursino (1998):
 * its afferent sigmoid, efferent curves, delays, gains and time constants.
 * Exercise follows Magosso and Ursino (2002); dynamic cerebral
 * autoregulation Aaslid et al. (1989). See CREDITS.md. */
#include "autonomic.h"

#include <math.h>
#include <string.h>

#define RHO_G 78.0f /* mmHg per metre of blood */

/* Afferent: carotid sinus (Ursino 1998, Table 1). */
#define TAU_Z 6.37f
#define TAU_P 2.076f
#define F_MIN 2.52f
#define F_MAX 47.78f
#define P_N 92.0f
#define K_A 11.758f
/* Efferent sympathetic and vagal curves. */
#define FES_INF 2.10f
#define FES_0 16.11f
#define K_ES 0.0675f
#define FES_MIN 2.66f
#define FEV_0 3.2f
#define FEV_INF 6.3f
#define K_EV 7.06f
#define FCS_0 25.0f

/* Effectors: gain per unit of ln(f_s - FES_MIN + 1) (vagal: per spike/s),
 * delay and time constant (Ursino 1998, Table 2). Gains on contractility,
 * resistance and venous volume are relative to his resting values: 0.475
 * of 2.392 mmHg/mL, 0.695 of 2.49 mmHg s/mL, -265 of ~2900 mL. */
#define G_EMAX 0.3f
#define D_EMAX 2.0f
#define T_EMAX 8.0f
#define G_R 0.6f
#define D_R 2.0f
#define T_R 6.0f
#define G_V0 (-0.12f)
#define D_V0 5.0f
#define T_V0 20.0f
#define G_TS (-0.2f)
#define D_TS 2.0f
#define T_TS 2.0f
#define G_TV 0.09f
#define D_TV 0.2f
#define T_TV 1.5f

static float afferent(float p) { return (F_MIN + F_MAX * expf((p - P_N) / K_A)) / (1.0f + expf((p - P_N) / K_A)); }
static float symp_curve(float fcs) { return FES_INF + (FES_0 - FES_INF) * expf(-K_ES * fcs); }
static float vagal_curve(float fcs)
{
    float e = expf((fcs - FCS_0) / K_EV);
    return (FEV_0 + FEV_INF * e) / (1.0f + e);
}
static float log_symp(float fs) { return logf(fmaxf(fs - FES_MIN, 0.0f) + 1.0f); }
static float lagf(float cur, float target, double dt, float tau)
{
    return cur + (target - cur) * (float)(1.0 - exp(-dt / (double)tau));
}

/* A normal draw from four uniforms (Irwin-Hall), deterministic per state. */
static float gauss(uint32_t *s)
{
    float sum = 0.0f;
    for (int i = 0; i < 4; i++) {
        uint32_t x = *s;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        *s = x;
        sum += (float)(x >> 8) * (1.0f / 16777216.0f);
    }
    return (sum - 2.0f) * 1.7320508f;
}

/* Resting traffic, with the resting cardiopulmonary term (RA ~3 mmHg). */
#define P_RA_REST 3.0f
static float cardiopulm(float p_ra) { return 0.3f * fmaxf(0.0f, 5.0f - p_ra); }

void auto_init(autonomic *a, float hr_rest)
{
    memset(a, 0, sizeof *a);
    float fcs = afferent(93.0f);
    a->p_cs = a->p_prev = 93.0f;
    a->f_cs = fcs;
    a->f_s = symp_curve(fcs) + cardiopulm(P_RA_REST);
    a->f_v = vagal_curve(fcs);
    a->ls0 = log_symp(a->f_s);
    /* Breathing gates the vagus by 1 - 0.4 x 0.4 on average. */
    a->fv0 = a->f_v * 0.84f;
    for (int i = 0; i < AUTO_HIST; i++) {
        a->hist_s[i] = a->f_s;
        a->hist_v[i] = a->fv0;
    }
    a->t0 = 60.0f / (hr_rest > 20.0f ? hr_rest : 64.0f);
    a->autoreg = 1.0f;
}

static float delayed(const autonomic *a, const float *hist, float d, double dt)
{
    int back = (int)lround((double)d / dt);
    if (back > a->hist_n - 1) back = a->hist_n - 1;
    if (back > AUTO_HIST - 1) back = AUTO_HIST - 1;
    if (back < 0) back = 0;
    return hist[(a->hist_pos - 1 - back + 2 * AUTO_HIST) % AUTO_HIST];
}

void auto_step(autonomic *a, const autonomic_input *in, double dt, uint32_t *vrng, autonomic_out *out)
{
    if (dt <= 0.0) return;
    /* Afferent: the sinus sits above the heart, and senses the rate of
     * change as well as the level. */
    float p = in->p_ao_mean - RHO_G * in->h_neck;
    float dp = (float)((double)(p - a->p_prev) / dt);
    a->p_prev = p;
    a->p_cs += (float)(dt / (double)TAU_P) * (p + TAU_Z * dp - a->p_cs);
    /* Exercise resets the reflex upward, so pressure may rise while running. */
    a->command = lagf(a->command, in->effort, dt, 3.0f);
    /* The vagus lets go of the heart within a beat or two of starting to
     * work, but takes it back slowly: the heart rate's fall after stopping
     * runs over a minute or more, slowly at first (squared below). */
    a->vagal_off = lagf(a->vagal_off, in->effort, dt, in->effort > a->vagal_off ? 2.0f : 40.0f);
    a->ex_drive = lagf(a->ex_drive, in->effort, dt, in->effort > a->ex_drive ? 40.0f : 15.0f);
    /* Working muscle's vessels open in two phases: within the first
     * contractions (rapid onset vasodilation, ~40% of the rise), then by
     * its own metabolites over half a minute, which wash out over minutes
     * after it stops (the hyperaemia behind the pressure dip after
     * exercise). */
    a->metab_fast = lagf(a->metab_fast, in->effort, dt, 2.0f);
    a->metab = lagf(a->metab, in->effort, dt, in->effort > a->metab ? 30.0f : 120.0f);
    a->metabo = lagf(a->metabo, in->muscle_o2_debt, dt, 60.0f);
    /* Central command resets the reflex while working, and the reset eases
     * off over a minute after stopping, as the vagus returns; the oxygen-starved
     * muscles' own afferents (the metaboreflex) hold the reset after. */
    a->f_cs = afferent(a->p_cs - 25.0f * fmaxf(a->vagal_off, a->metabo));

    /* Random sympathetic traffic, an Ornstein-Uhlenbeck process (tau 2 s);
     * the reflex loop's own delay turns it into ~0.1 Hz Mayer waves. */
    float decay = (float)exp(-dt / 2.0);
    float sd = 0.3f * in->noise_amp;
    a->noise = a->noise * decay + sd * sqrtf(1.0f - decay * decay) * gauss(vrng);

    a->fail = lagf(a->fail, in->brain_ok ? 0.0f : 1.0f, dt, 5.0f);
    /* Fear's own push is small: the defence reaction's rise of a few tens
     * of mmHg comes mostly from the adrenaline it releases, and a bigger
     * term here outruns the whole baroreflex (300/245 when hunted). */
    float fs = symp_curve(a->f_cs) + cardiopulm(in->p_ra_mean) + 8.0f * in->chemo + 4.0f * in->pain + 1.0f * in->fear +
               10.0f * a->ex_drive + 6.0f * a->metabo + 3.0f * in->diving + a->noise;
    fs *= (1.0f - 0.8f * in->vagal_surge) * (1.0f - a->fail);
    float fv = vagal_curve(a->f_cs) + 10.0f * in->vagal_surge + 3.0f * in->diving;
    /* Fear's quick pulse is mostly the vagus letting go of the heart:
     * +20-40 bpm with no squeeze on the vessels. */
    fv *= (1.0f - 0.5f * in->fear) * (in->breath_phase < 0.4f ? 0.6f : 1.0f) * (1.0f - a->vagal_off) *
          (1.0f - a->vagal_off) * (1.0f - a->fail);
    a->f_s = fmaxf(fs, 0.0f);
    a->f_v = fmaxf(fv, 0.0f);
    a->hist_s[a->hist_pos] = a->f_s;
    a->hist_v[a->hist_pos] = a->f_v;
    a->hist_pos = (a->hist_pos + 1) % AUTO_HIST;
    if (a->hist_n < AUTO_HIST) a->hist_n++;

    float g = in->gain > 0.0f ? in->gain : 1.0f;
    float ls_emax = log_symp(delayed(a, a->hist_s, D_EMAX, dt)) - a->ls0;
    float ls_r = log_symp(delayed(a, a->hist_s, D_R, dt)) - a->ls0;
    float ls_v0 = log_symp(delayed(a, a->hist_s, D_V0, dt)) - a->ls0;
    float ls_ts = log_symp(delayed(a, a->hist_s, D_TS, dt)) - a->ls0;
    float fv_d = delayed(a, a->hist_v, D_TV, dt) - a->fv0;
    a->eff_emax = lagf(a->eff_emax, g * G_EMAX * ls_emax, dt, T_EMAX);
    a->eff_r = lagf(a->eff_r, g * G_R * ls_r, dt, T_R);
    a->eff_v0 = lagf(a->eff_v0, g * G_V0 * ls_v0, dt, T_V0);
    a->eff_ts = lagf(a->eff_ts, g * G_TS * ls_ts, dt, T_TS);
    a->eff_tv = lagf(a->eff_tv, g * G_TV * fv_d, dt, T_TV);

    /* The brain's arterioles widen or narrow to hold its flow (tau ~5 s),
     * within the limits of how far they can go. The hold is not perfect:
     * the leak leaves about a fifth of a sustained change in pressure
     * showing in the flow, as static autoregulation does. */
    float flow = in->brain_flow > 0.0f ? in->brain_flow : 1.0f;
    a->autoreg *= (float)exp(((double)(flow - 1.0f) - 0.2 * log((double)a->autoreg)) * dt / 5.0);
    a->autoreg = fminf(fmaxf(a->autoreg, 0.4f), 2.5f);

    float period = fmaxf(a->t0 + a->eff_ts + a->eff_tv, 60.0f / 220.0f);
    float rate = 60.0f / period;
    /* A failing brainstem leaves a hypoxic sinus node slowing toward 40. */
    out->sa_rate = rate + (fminf(rate, 40.0f) - rate) * a->fail;
    out->av_delay = fminf(fmaxf(0.16f + 0.10f * a->eff_ts + 0.15f * a->eff_tv, 0.10f), 0.24f);
    out->emax = fmaxf(1.0f + a->eff_emax, 0.3f);
    out->v0_scale = fminf(fmaxf(1.0f + a->eff_v0, 0.6f), 1.2f) + 0.08f * in->vagal_surge;
    /* Silencing the nerves only removes resting tone, about a quarter of
     * the resistance; the gain that tightens the beds after a bleed is far
     * larger than what the reflex can take away. */
    float r = fmaxf(1.0f + a->eff_r, 0.75f);
    /* A vasovagal surge does more than silence the nerves: muscle vessels
     * actively open and the veins slacken, the drop that empties the head. */
    r *= 1.0f - 0.45f * in->vagal_surge;
    for (int b = 0; b < CB_COUNT; b++) out->r_bed[b] = r;
    /* Cerebral vessels barely follow the sympathetic nerves; the brain
     * regulates itself. */
    out->r_bed[CB_BRAIN] = (1.0f + 0.2f * a->eff_r) * a->autoreg;
    /* Working muscle opens its vessels and overrides the nerves that would
     * close them. */
    float m = fmaxf(a->metab, 0.25f * a->metab_fast + 0.75f * a->metab);
    out->r_bed[CB_LEGS] = (1.0f + a->eff_r * (1.0f - m)) / (1.0f + 20.0f * m);
    out->symp = fminf(fmaxf((a->f_s - FES_MIN) / 30.0f, 0.0f), 1.0f);
}
