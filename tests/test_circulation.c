/* The simulated circulation (circulation.c, and its use in health.c): the
 * four-chamber heart and its vessels driven open loop at a fixed rate, then
 * the whole body under its own control. Every value checked is a real
 * adult's normal range or a textbook response. */
#include "circulation.h"
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

void test_circulation_all(void)
{
    test_circ_rest();
    test_circ_conserves();
    test_circ_starling();
    test_circ_dt_split();
    test_circ_empty();
    test_circ_standing();
}
