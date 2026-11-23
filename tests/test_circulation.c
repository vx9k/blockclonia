/* The simulated circulation (circulation.c, and its use in health.c): the
 * four-chamber heart and its vessels driven open loop at a fixed rate, then
 * the whole body under its own control. Every value checked is a real
 * adult's normal range or a textbook response. */
#include "circulation.h"
#include "health.h"
#include "mem.h"
#include "test_util.h"

#include <math.h>
#include <string.h>

#define TICK (1.0 / 60.0)

/* A resting, lying body's inputs: every scale at 1, quiet breathing. */
static circ_input rest_input(void)
{
    circ_input in = {0};
    in.t_atria = in.t_vent = in.t_atria_prev = in.t_vent_prev = -1.0;
    in.atria_on = in.vent_on = 1.0f;
    in.emax_l = in.emax_r = 1.0f;
    for (int b = 0; b < CB_COUNT; b++) in.r_bed[b] = 1.0f;
    in.v0_scale = 1.0f;
    in.p_thorax = -4.0f;
    return in;
}

/* Fires the atria every 60/hr s and the ventricles 0.16 s after. */
static void pace(circ_input *in, double t_end, float hr)
{
    double rr = 60.0 / (double)hr;
    double ka = floor(t_end / rr), kv = floor((t_end - 0.16) / rr);
    in->t_atria = ka * rr;
    in->t_atria_prev = ka >= 1.0 ? (ka - 1.0) * rr : -1.0;
    in->t_vent = kv >= 0.0 ? kv * rr + 0.16 : -1.0;
    in->t_vent_prev = kv >= 1.0 ? (kv - 1.0) * rr + 0.16 : -1.0;
    in->rr = (float)rr;
}

typedef struct {
    double co, cvp, pa_max, pa_min;
    int n;
} circ_stats;

/* Runs seconds of paced beats; the stats cover the last 10 s. */
static circ_stats drive(circ *c, double *t, double seconds, float hr, circ_input *in)
{
    circ_stats s = {0};
    s.pa_min = 1e9;
    int ticks = (int)lround(seconds / TICK);
    for (int i = 0; i < ticks; i++) {
        pace(in, *t + TICK, hr);
        circ_step(c, in, *t, TICK);
        *t += TICK;
        if (i >= ticks - 600) {
            s.co += (double)c->q_ao;
            s.cvp += (double)c->p[CC_RA];
            s.pa_max = fmax(s.pa_max, (double)c->p[CC_PA]);
            s.pa_min = fmin(s.pa_min, (double)c->p[CC_PA]);
            s.n++;
        }
    }
    s.co = s.co / s.n * 0.06; /* mL/s to L/min */
    s.cvp /= s.n;
    return s;
}

static circ *new_circ(void)
{
    circ *c = mem_calloc(1, sizeof *c);
    circ_init(c, 5250.0f, 1.0f, 1.0f);
    return c;
}

static void test_circ_rest(void)
{
    circ *c = new_circ();
    circ_input in = rest_input();
    double t = 0.0;
    circ_stats s = drive(c, &t, 60.0, 70.0f, &in);
    CHECK(c->p_ao_max > 110.0f && c->p_ao_max < 130.0f);
    CHECK(c->p_ao_min > 70.0f && c->p_ao_min < 85.0f);
    CHECK(s.co > 4.5 && s.co < 6.0);
    CHECK(c->edv > 110.0f && c->edv < 150.0f);
    float ef = (c->edv - c->esv) / c->edv;
    CHECK(ef > 0.55f && ef < 0.70f);
    CHECK(s.cvp > 2.0 && s.cvp < 8.0);
    CHECK(s.pa_max > 18.0 && s.pa_max < 30.0);
    CHECK(s.pa_min > 6.0 && s.pa_min < 14.0);
    CHECK(c->cor_dia_frac > 0.6f);
    mem_free(c);
}

static void test_circ_conserves(void)
{
    circ *c = new_circ();
    circ_input in = rest_input();
    double t = 0.0;
    float v0 = circ_volume(c);
    drive(c, &t, 600.0, 70.0f, &in);
    CHECK(fabsf(circ_volume(c) - v0) < 0.01f);
    mem_free(c);
}

/* Frank-Starling: more filling, a bigger stroke. */
static void test_circ_starling(void)
{
    circ *a = new_circ(), *b = new_circ();
    circ_input in = rest_input();
    double ta = 0.0, tb = 0.0;
    circ_add(b, CC_IVC, 500.0f);
    drive(a, &ta, 60.0, 70.0f, &in);
    drive(b, &tb, 60.0, 70.0f, &in);
    float sva = a->edv - a->esv, svb = b->edv - b->esv;
    CHECK(svb > 1.10f * sva && svb < 1.40f * sva);
    mem_free(a);
    mem_free(b);
}

/* The circuit steps in fixed sub-steps whatever the caller's dt, so one
 * long step and two short ones land on the same state. */
static void test_circ_dt_split(void)
{
    circ *a = new_circ(), *b = new_circ();
    circ_input in = rest_input();
    double t = 0.0;
    for (int i = 0; i < 300; i++) {
        pace(&in, t + 2.0 * TICK, 70.0f);
        circ_step(a, &in, t, 2.0 * TICK);
        circ_step(b, &in, t, TICK);
        circ_step(b, &in, t + TICK, TICK);
        t += 2.0 * TICK;
    }
    int same = 1;
    for (int i = 0; i < CC_COUNT; i++) same &= fabsf(a->p[i] - b->p[i]) < 1e-3f;
    CHECK(same);
    mem_free(a);
    mem_free(b);
}

/* Wounds in every vessel drain the circuit down to what it holds at zero
 * pressure, without a negative volume or a NaN anywhere. */
static void test_circ_empty(void)
{
    circ *c = new_circ();
    circ_input in = rest_input();
    for (int i = 0; i < CC_COUNT; i++) in.drain[i] = 50.0f;
    for (int b = 0; b < CB_COUNT; b++) in.drain_art[b] = 50.0f;
    double t = 0.0;
    drive(c, &t, 120.0, 70.0f, &in);
    int ok = 1;
    for (int i = 0; i < CC_COUNT; i++) ok &= c->v[i] >= 0.0 && isfinite(c->v[i]) && isfinite((double)c->p[i]);
    CHECK(ok);
    CHECK(circ_volume(c) < 4500.0f);
    mem_free(c);
}

/* Standing up, with no reflex to answer it: blood sinks into the legs and
 * the gut, and the heart has less to pump. */
static void test_circ_standing(void)
{
    circ *a = new_circ(), *b = new_circ();
    circ_input lie = rest_input(), stand = rest_input();
    stand.h_head = 0.3f;
    stand.h_legs = -0.6f;
    stand.h_splanch = -0.2f;
    double ta = 0.0, tb = 0.0;
    drive(a, &ta, 60.0, 70.0f, &lie);
    drive(b, &tb, 60.0, 70.0f, &stand);
    double pooled = (b->v[CC_LEGS] + b->v[CC_SPLANCH]) - (a->v[CC_LEGS] + a->v[CC_SPLANCH]);
    CHECK(pooled >= 300.0);
    CHECK(b->edv - b->esv <= 0.85f * (a->edv - a->esv));
    mem_free(a);
    mem_free(b);
}

/* Each beat's means: the mean aortic pressure sits between diastolic and
 * systolic, nearer diastolic, and the beat's mean outflow times its length
 * is its stroke volume. */
static void test_circ_beat_means(void)
{
    circ *c = new_circ();
    circ_input in = rest_input();
    double t = 0.0;
    drive(c, &t, 30.0, 70.0f, &in);
    CHECK(c->p_ao_mean > c->p_ao_min + 0.2f * (c->p_ao_max - c->p_ao_min));
    CHECK(c->p_ao_mean < c->p_ao_min + 0.5f * (c->p_ao_max - c->p_ao_min));
    float sv = c->edv - c->esv, from_flow = c->beat_q_ao * c->beat_len;
    CHECK(fabsf(from_flow - sv) < 0.05f * sv);
    CHECK(c->p_ra_mean > 1.0f && c->p_ra_mean < 8.0f);
    float beds = 0.0f;
    for (int b = 0; b < CB_COUNT; b++) beds += c->beat_q_bed[b];
    CHECK(beds > 0.85f * c->beat_q_ao && beds < 1.05f * c->beat_q_ao);
    CHECK(c->beat_q_cor > 0.02f * c->beat_q_ao && c->beat_q_cor < 0.08f * c->beat_q_ao);
    mem_free(c);
}

/* Atrial fibrillation at the same mean rate: irregular beats and no
 * atrial kick pump less than sinus rhythm. */
static void test_circ_af_kick(void)
{
    circ *a = new_circ(), *b = new_circ();
    circ_input sinus = rest_input(), af = rest_input();
    af.atria_on = 0.0f;
    const float hr = 100.0f;
    double t = 0.0, q_sinus = 0.0, q_af = 0.0, next = 0.5;
    uint32_t s = 99u;
    for (int i = 0; i < 90 * 60; i++) {
        pace(&sinus, t + TICK, hr);
        while (next <= t + TICK) {
            af.t_vent_prev = af.t_vent;
            af.t_vent = next;
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            double u = (double)(s >> 8) / 16777216.0;
            next += 60.0 / (double)hr * (0.6 + 0.8 * u);
        }
        af.rr = 60.0f / hr;
        circ_step(a, &sinus, t, TICK);
        circ_step(b, &af, t, TICK);
        t += TICK;
        if (i >= 30 * 60) {
            q_sinus += (double)a->q_ao;
            q_af += (double)b->q_ao;
        }
    }
    CHECK(q_af < 0.92 * q_sinus);
    mem_free(a);
    mem_free(b);
}

/* ------------------------------------------------- the whole body */

typedef struct {
    double sbp, dbp, map, co, hr, brain;
    int n;
} body_avg;

/* Lives seconds, averaging the vitals over the last `tail` of them. */
static body_avg body_live(health *h, const health_env *e, double seconds, double tail)
{
    body_avg a = {0};
    int ticks = (int)lround(seconds / TICK), from = ticks - (int)lround(tail / TICK);
    for (int i = 0; i < ticks; i++) {
        health_step(h, e, TICK);
        if (i < from) continue;
        a.sbp += (double)h->sbp;
        a.dbp += (double)h->dbp;
        a.map += (double)h->map;
        a.co += (double)h->co;
        a.hr += (double)h->hr;
        a.brain += (double)h->brain_flow;
        a.n++;
    }
    if (a.n) {
        a.sbp /= a.n;
        a.dbp /= a.n;
        a.map /= a.n;
        a.co /= a.n;
        a.hr /= a.n;
        a.brain /= a.n;
    }
    return a;
}

static int rest_ranges(const health *h, body_avg a)
{
    float ef = h->cv.edv > 1.0f ? (h->cv.edv - h->cv.esv) / h->cv.edv : 0.0f;
    return a.sbp > 105.0 && a.sbp < 135.0 && a.dbp > 65.0 && a.dbp < 88.0 && a.co > 4.0 && a.co < 6.5 && a.hr > 55.0 &&
           a.hr < 80.0 && ef > 0.5f && ef < 0.7f && a.brain > 0.9 && a.brain < 1.1;
}

/* Standing at rest, under the body's own control. */
static void test_body_rest(void)
{
    health *h = new_body(1);
    health_env e = calm_env();
    body_avg a = body_live(h, &e, 120.0, 20.0);
    CHECK(rest_ranges(h, a));
    CHECK(fabsf(h->blood - BLOOD_NORMAL) < 0.02f);
    CHECK(fabsf(h->svr - h->map / h->co) < 0.05f * h->svr);
    mem_free(h);
}

/* Breaking into a run: pressure doesn't jump, the heart rate climbs fast
 * then slowly; stopping leaves pressure up and the rate slow to fall. */
static void test_body_run_no_jump(void)
{
    health *h = new_body(2);
    health_env e = calm_env();
    body_avg a0 = body_live(h, &e, 90.0, 10.0);
    e.speed = 6.5;
    float sbp_1 = 0.0f, hr_10 = 0.0f, hr_90 = 0.0f;
    for (int i = 1; i <= 120 * 60; i++) {
        health_step(h, &e, TICK);
        if (i == 60) sbp_1 = h->sbp;
        if (i == 600) hr_10 = h->hr;
        if (i == 5400) hr_90 = h->hr;
    }
    float sbp_run = h->sbp, hr_run = h->hr;
    CHECK(sbp_1 - (float)a0.sbp < 10.0f);
    CHECK(hr_10 > (float)a0.hr + 15.0f);
    CHECK(hr_90 > hr_10 + 15.0f);
    CHECK(sbp_run > 160.0f && sbp_run < 190.0f);
    e.speed = 0.0;
    float sbp_after = 0.0f;
    int half_i = -1;
    for (int i = 1; i <= 120 * 60; i++) {
        health_step(h, &e, TICK);
        if (i == 600) sbp_after = h->sbp;
        if (half_i < 0 && h->hr < 0.5f * (hr_run + (float)a0.hr)) half_i = i;
    }
    CHECK(sbp_after > (float)a0.sbp + 20.0f);
    CHECK(half_i < 0 || half_i > 30 * 60);
    mem_free(h);
}

/* A steady arterial bleed walks through the classes of haemorrhage in
 * order: little at first, then a racing pulse and a narrowing pulse
 * pressure, then falling pressure. The classes count blood shed, which
 * plasma refilling from the tissues keeps ahead of the volume deficit. */
static void test_body_blood_loss_classes(void)
{
    health *h = new_body(3);
    health_env e = calm_env();
    body_avg a0 = body_live(h, &e, 90.0, 10.0);
    float b0 = h->blood, pp0 = (float)(a0.sbp - a0.dbp);
    health_cut(h, BP_LLEG, 0.3f, 1, 0.0f);
    int seen = 0, ok10 = 0, ok22 = 0, ok35 = 0, ok45 = 0;
    double shed = 0.0; /* L */
    /* Each beat's pressure rides the ~0.1 Hz Mayer waves, a few mmHg either
     * way, so the classes read a 3 s average, as a clinician would. */
    float hr = h->hr, sbp = h->sbp, dbp = h->dbp;
    const float k = 1.0f - expf(-(float)TICK / 3.0f);
    for (int i = 0; i < 40 * 60 * 60 && seen < 4; i++) {
        health_step(h, &e, TICK);
        hr += (h->hr - hr) * k;
        sbp += (h->sbp - sbp) * k;
        dbp += (h->dbp - dbp) * k;
        shed += (double)h->bleed_ext / 60.0 * TICK / 1000.0;
        float lost = (float)shed / b0;
        int arrested = !health_rhythm_perfusing(h->rhythm) || h->dead;
        if (seen == 0 && lost >= 0.10f) {
            ok10 = hr < 100.0f && sbp > (float)a0.sbp - 8.0f;
            seen = 1;
        } else if (seen == 1 && lost >= 0.22f) {
            ok22 = hr >= 100.0f && hr <= 130.0f && sbp - dbp < 0.85f * pp0;
            seen = 2;
        } else if (seen == 2 && lost >= 0.35f) {
            ok35 = (hr > 120.0f && sbp < 100.0f) || arrested;
            seen = 3;
        } else if (seen == 3 && lost >= 0.45f) {
            ok45 = sbp < 80.0f || arrested;
            seen = 4;
        }
        if (h->dead) break;
    }
    CHECK(seen == 4);
    CHECK(ok10);
    CHECK(ok22);
    CHECK(ok35);
    CHECK(ok45);
    mem_free(h);
}

/* A cut artery bleeds by pressure: as pressure falls, so does the flow. */
static void test_body_bleed_slows(void)
{
    health *h = new_body(4);
    health_env e = calm_env();
    live(h, &e, 60.0);
    health_cut(h, BP_LLEG, 0.8f, 1, 0.0f);
    live(h, &e, 2.0);
    float first = h->bleed_ext;
    int reached = 0;
    for (int i = 0; i < 20 * 60 * 60 && !h->dead; i++) {
        health_step(h, &e, TICK);
        if (h->map < 60.0f) {
            reached = 1;
            break;
        }
    }
    CHECK(first > 300.0f);
    CHECK(reached);
    CHECK(h->bleed_ext < 0.7f * first);
    mem_free(h);
}

/* Standing up from a crouch: the brain's flow dips for a few seconds, then
 * the reflexes and the brain's own vessels win it back; a bled body dips
 * deeper and recovers later. */
static void head_rush(health *h, float *min_flow, int *back_i)
{
    health_env e = calm_env();
    e.crouched = 1;
    live(h, &e, 60.0);
    e.crouched = 0;
    *min_flow = 9.0f;
    *back_i = -1;
    for (int i = 1; i <= 40 * 60; i++) {
        health_step(h, &e, TICK);
        if (i <= 10 * 60) *min_flow = fminf(*min_flow, h->brain_flow);
        if (*back_i < 0 && i > 2 * 60 && h->brain_flow > 0.96f && *min_flow < 0.96f) *back_i = i;
    }
}

static void test_body_head_rush(void)
{
    health *a = new_body(5), *b = new_body(5);
    health_env e = calm_env();
    live(a, &e, 60.0);
    live(b, &e, 60.0);
    b->blood *= 0.8f;
    b->rbc *= 0.8f;
    float min_a = 0.0f, min_b = 0.0f;
    int back_a = 0, back_b = 0;
    head_rush(a, &min_a, &back_a);
    head_rush(b, &min_b, &back_b);
    CHECK(min_a < 0.92f);
    CHECK(back_a > 0 && back_a <= 30 * 60);
    CHECK(min_b <= min_a - 0.03f);
    CHECK(back_b < 0 || back_b > back_a);
    mem_free(a);
    mem_free(b);
}

/* Bearing down against a closed throat: the four phases of the Valsalva
 * manoeuvre fall out of chest pressure and the reflex. */
static void test_body_valsalva(void)
{
    health *h = new_body(6);
    health_env e = calm_env();
    body_avg a0 = body_live(h, &e, 90.0, 10.0);
    float sbp0 = (float)a0.sbp, hr0 = (float)a0.hr;
    e.bearing_down = 1.0f;
    int ph1 = 0, ph2 = 0;
    float hr_max = 0.0f;
    for (int i = 1; i <= 15 * 60; i++) {
        health_step(h, &e, TICK);
        if (i <= 2 * 60 && h->sbp > sbp0 + 10.0f) ph1 = 1;
        if (i >= 6 * 60 && h->sbp < sbp0) ph2 = 1;
        hr_max = fmaxf(hr_max, h->hr);
    }
    float sbp_strain = h->sbp;
    e.bearing_down = 0.0f;
    int ph3 = 0, ph4 = 0;
    for (int i = 1; i <= 15 * 60; i++) {
        health_step(h, &e, TICK);
        if (i <= 2 * 60 && h->sbp < sbp_strain - 10.0f) ph3 = 1;
        if (i > 2 * 60 && h->sbp > sbp0 + 5.0f && h->hr < hr0) ph4 = 1;
    }
    CHECK(ph1);
    CHECK(ph2 && hr_max >= hr0 + 10.0f);
    CHECK(ph3);
    CHECK(ph4);
    mem_free(h);
}

/* Power of an unevenly sampled series at f Hz (a direct Fourier sum). */
static double power_at(const double *t, const double *x, int n, double f)
{
    double mean = 0.0, re = 0.0, im = 0.0;
    for (int i = 0; i < n; i++) mean += x[i];
    mean /= n;
    for (int i = 0; i < n; i++) {
        re += (x[i] - mean) * cos(6.283185307 * f * t[i]);
        im += (x[i] - mean) * sin(6.283185307 * f * t[i]);
    }
    return (re * re + im * im) / n;
}

/* A resting body's beat-to-beat variation: the heart quickens on each
 * breath in, and pressure rolls in ~10 s Mayer waves. */
static void test_body_rsa_mayer(void)
{
    health *h = mem_alloc(sizeof *h);
    health_init_varied(h, 11);
    health_env e = calm_env();
    live(h, &e, 60.0);
    static double bt[1000], bmap[1000];
    int n = 0, ni = 0, ne = 0;
    double ins = 0.0, exs = 0.0, last = h->last_r;
    for (int i = 0; i < 300 * 60 && n < 1000; i++) {
        health_step(h, &e, TICK);
        if (h->last_r == last) continue;
        double rr = h->last_r - h->prev_r;
        last = h->last_r;
        if (rr <= 0.2 || rr > 3.0) continue;
        bt[n] = h->last_r;
        bmap[n++] = (double)h->map;
        /* The heart answers the breath about a second late. */
        double ph = fmod((double)h->breath_phase - (double)h->rr / 60.0 + 1.0, 1.0);
        if (ph < 0.4) {
            ins += 60.0 / rr;
            ni++;
        } else {
            exs += 60.0 / rr;
            ne++;
        }
    }
    CHECK(ni > 20 && ne > 20);
    CHECK(ins / ni - exs / ne >= 2.0);
    double p10 = power_at(bt, bmap, n, 0.1), side = 0.5 * (power_at(bt, bmap, n, 0.03) + power_at(bt, bmap, n, 0.25));
    CHECK(p10 > 2.0 * side);
    mem_free(h);
}

/* Rhythms in a whole body: AF beats fast and irregularly, complete heart
 * block leaves the slow escape rhythm, and VF stops the output. */
static void test_body_rhythms(void)
{
    health *h = new_body(8);
    health_env e = calm_env();
    live(h, &e, 30.0);
    h->rhythm = RHYTHM_AF;
    double last = h->last_r, s = 0.0, s2 = 0.0;
    int n = 0;
    for (int i = 0; i < 60 * 60; i++) {
        health_step(h, &e, TICK);
        if (h->last_r == last) continue;
        double rr = h->last_r - last;
        last = h->last_r;
        if (i < 10 * 60) continue;
        s += rr;
        s2 += rr * rr;
        n++;
    }
    double mean = s / n, sd = sqrt(fmax(0.0, s2 / n - mean * mean));
    CHECK(60.0 / mean > 80.0 && 60.0 / mean < 160.0);
    CHECK(sd / mean > 0.1);
    mem_free(h);

    h = new_body(8);
    live(h, &e, 30.0);
    h->rhythm = RHYTHM_AVB3;
    live(h, &e, 15.0);
    CHECK(h->hr >= 30.0f && h->hr <= 40.0f);
    mem_free(h);

    h = new_body(8);
    live(h, &e, 30.0);
    health_arrest(h, RHYTHM_VF);
    live(h, &e, 5.0);
    CHECK(h->co < 0.2f);
    mem_free(h);
}

/* Bodies differ: resting rate and pressure spread like a population's,
 * all inside healthy limits. */
static void test_body_variation(void)
{
    double s_hr = 0.0, s2_hr = 0.0, s_sbp = 0.0, s2_sbp = 0.0;
    int inside = 1;
    health *h = mem_alloc(sizeof *h);
    for (uint32_t seed = 1; seed <= 50; seed++) {
        health_init_varied(h, seed);
        health_env e = calm_env();
        body_avg a = body_live(h, &e, 90.0, 20.0);
        s_hr += a.hr;
        s2_hr += a.hr * a.hr;
        s_sbp += a.sbp;
        s2_sbp += a.sbp * a.sbp;
        inside &= a.sbp > 90.0 && a.sbp < 145.0;
    }
    mem_free(h);
    double sd_hr = sqrt(s2_hr / 50.0 - (s_hr / 50.0) * (s_hr / 50.0));
    double sd_sbp = sqrt(s2_sbp / 50.0 - (s_sbp / 50.0) * (s_sbp / 50.0));
    CHECK(sd_hr >= 4.0);
    CHECK(sd_sbp >= 5.0);
    CHECK(inside);
}

/* Standing still in the heat: the skin's open vessels pool blood, and the
 * brain gets less of it. */
static void test_body_heat_pooling(void)
{
    health *a = new_body(9), *b = new_body(9);
    health_env cool = calm_env(), hot = calm_env();
    hot.air_temp = 38.0;
    body_avg ca = body_live(a, &cool, 1200.0, 60.0);
    body_avg hb = body_live(b, &hot, 1200.0, 60.0);
    CHECK(hb.brain <= ca.brain - 0.04);
    mem_free(a);
    mem_free(b);
}

/* Bleeding to death and starting again: nothing goes NaN while dead, and a
 * fresh body is a resting one. */
static void test_circ_dead_reset(void)
{
    health *h = new_body(10);
    health_env e = calm_env();
    health_cut(h, BP_LLEG, 1.0f, 1, 0.0f);
    health_cut(h, BP_RLEG, 1.0f, 1, 0.0f);
    int finite = 1;
    for (int i = 0; i < 30 * 60 * 60 && !h->dead; i++) {
        health_step(h, &e, TICK);
        finite &= isfinite(h->map) && isfinite(h->co) && isfinite(h->hr) && isfinite(h->blood);
    }
    CHECK(h->dead);
    live(h, &e, 10.0);
    finite &= isfinite(h->map) && isfinite(h->blood);
    CHECK(finite);
    health_init(h, 10);
    body_avg a = body_live(h, &e, 60.0, 15.0);
    CHECK(rest_ranges(h, a));
    mem_free(h);
}

/* The same body and the same injuries replay exactly. */
static void test_body_replay(void)
{
    health *a = mem_alloc(sizeof *a), *b = mem_alloc(sizeof *b);
    health_init_varied(a, 7);
    health_init_varied(b, 7);
    health_env e = calm_env();
    for (int k = 0; k < 2; k++) {
        health *h = k ? b : a;
        live(h, &e, 20.0);
        health_cut(h, BP_RARM, 0.6f, 1, 0.1f);
        e.speed = 3.0;
        live(h, &e, 100.0);
        e.speed = 0.0;
        live(h, &e, 180.0);
    }
    CHECK(memcmp(a, b, sizeof *a) == 0);
    mem_free(a);
    mem_free(b);
}

/* A plain body has a live variability stream: a zero xorshift state stays
 * zero, which fired an ectopic beat on every beat and pinned the
 * sympathetic noise at a constant. Different seeds wander differently. */
static void test_body_vrng_seeded(void)
{
    health *a = new_body(1), *b = new_body(2);
    CHECK(a->vrng != 0 && b->vrng != 0 && a->vrng != b->vrng);
    health_env e = calm_env();
    live(a, &e, 60.0);
    CHECK(a->vrng != 0 && a->rhythm == RHYTHM_SINUS && a->hr > 50.0f && a->hr < 90.0f);
    mem_free(a);
    mem_free(b);
}

void test_circulation_all(void)
{
    test_circ_rest();
    test_circ_conserves();
    test_circ_starling();
    test_circ_dt_split();
    test_circ_empty();
    test_circ_standing();
    test_circ_beat_means();
    test_circ_af_kick();
    test_body_rest();
    test_body_run_no_jump();
    test_body_blood_loss_classes();
    test_body_bleed_slows();
    test_body_head_rush();
    test_body_valsalva();
    test_body_rsa_mayer();
    test_body_rhythms();
    test_body_variation();
    test_body_heat_pooling();
    test_circ_dead_reset();
    test_body_replay();
    test_body_vrng_seeded();
}
