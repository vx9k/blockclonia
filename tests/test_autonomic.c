/* The nerves that steer the circulation (autonomic.c), open loop: the
 * baroreflex's delays and time constants, exercise's fast and slow heart
 * rate rise, breathing's sway of the heart rate, the brain keeping its own
 * blood flow, and what happens when the brainstem fails. */
#include "autonomic.h"
#include "test_util.h"

#include <math.h>
#include <string.h>

#define TICK (1.0 / 60.0)

static autonomic_input calm_input(void)
{
    autonomic_input in = {0};
    in.p_ao_mean = 93.0f;
    in.p_ra_mean = 3.0f;
    in.h_neck = 0.0f;
    in.breath_phase = 0.5f;
    in.gain = 1.0f;
    in.noise_amp = 0.0f;
    in.brain_flow = 1.0f;
    in.brain_ok = 1;
    return in;
}

static void settle(autonomic *a, autonomic_input *in, double seconds, uint32_t *rng, autonomic_out *o)
{
    for (int i = 0; i < (int)lround(seconds / TICK); i++) auto_step(a, in, TICK, rng, o);
}

static void test_auto_rest(void)
{
    autonomic a;
    auto_init(&a, 64.0f);
    autonomic_input in = calm_input();
    autonomic_out o = {0};
    uint32_t rng = 1u;
    double hr = 0.0;
    int n = 0;
    for (int i = 0; i < 90 * 60; i++) {
        in.breath_phase = (float)fmod(i * TICK / 5.0, 1.0);
        auto_step(&a, &in, TICK, &rng, &o);
        if (i >= 60 * 60) {
            hr += (double)o.sa_rate;
            n++;
        }
    }
    CHECK(fabs(hr / n - 64.0) < 3.0);
    CHECK(fabsf(o.emax - 1.0f) < 0.02f && fabsf(o.v0_scale - 1.0f) < 0.02f);
    CHECK(fabsf(o.r_bed[CB_SPLANCH] - 1.0f) < 0.02f);
}

/* A sudden fall in pressure: the vagus answers within a beat, the
 * sympathetic nerves seconds later, the veins later still. */
static void test_auto_delays(void)
{
    autonomic a;
    auto_init(&a, 64.0f);
    autonomic_input in = calm_input();
    autonomic_out o = {0};
    uint32_t rng = 1u;
    settle(&a, &in, 60.0, &rng, &o);
    float hr0 = o.sa_rate, r0 = o.r_bed[CB_SPLANCH], v0 = o.v0_scale;
    in.p_ao_mean = 70.0f;
    int hr_early = 0, r_early = 0, v_early = 0;
    float hr_1s = 0.0f, hr_5s = 0.0f, r_10s = 0.0f;
    for (int i = 1; i <= 60 * 60; i++) {
        auto_step(&a, &in, TICK, &rng, &o);
        double t = i * TICK;
        if (t < 0.15 && fabsf(o.sa_rate - hr0) > 0.01f) hr_early = 1;
        if (t < 1.8 && fabsf(o.r_bed[CB_SPLANCH] - r0) > 1e-4f) r_early = 1;
        if (t < 4.5 && fabsf(o.v0_scale - v0) > 1e-4f) v_early = 1;
        if (i == 60) hr_1s = o.sa_rate;
        if (i == 300) hr_5s = o.sa_rate;
        if (i == 600) r_10s = o.r_bed[CB_SPLANCH];
    }
    CHECK(!hr_early && !r_early && !v_early);
    CHECK(hr_1s >= hr0 + 3.0f);
    CHECK(hr_5s >= hr0 + 8.0f);
    CHECK(o.r_bed[CB_SPLANCH] > r0 + 0.1f);
    CHECK(r_10s - r0 >= 0.6f * (o.r_bed[CB_SPLANCH] - r0));
    CHECK(o.v0_scale < v0 - 0.03f);
}

/* Running: the heart rate jumps as the vagus lets go, then keeps climbing
 * for minutes; the legs' vessels open over tens of seconds (their
 * conductance, which is what sets the flow). */
static void test_auto_exercise_kinetics(void)
{
    autonomic a;
    auto_init(&a, 64.0f);
    autonomic_input in = calm_input();
    autonomic_out o = {0};
    uint32_t rng = 1u;
    settle(&a, &in, 60.0, &rng, &o);
    float hr0 = o.sa_rate, legs0 = o.r_bed[CB_LEGS];
    in.effort = 0.8f;
    static float legs[300 * 60];
    float hr10 = 0.0f, hr90 = 0.0f;
    for (int i = 0; i < 300 * 60; i++) {
        auto_step(&a, &in, TICK, &rng, &o);
        legs[i] = 1.0f / o.r_bed[CB_LEGS];
        if (i == 600) hr10 = o.sa_rate;
        if (i == 5400) hr90 = o.sa_rate;
    }
    CHECK(hr10 >= hr0 + 15.0f);
    CHECK(hr90 >= hr10 + 15.0f);
    float g0 = 1.0f / legs0, fin = legs[300 * 60 - 1], target = g0 + 0.632f * (fin - g0);
    int tau_i = 0;
    while (tau_i < 300 * 60 && legs[tau_i] < target) tau_i++;
    CHECK(fin > 2.0f * g0);
    CHECK(tau_i >= 20 * 60 && tau_i <= 60 * 60);
}

/* Breathing in quiets the vagus, so the heart speeds on each breath. */
static void test_auto_rsa(void)
{
    autonomic a;
    auto_init(&a, 64.0f);
    autonomic_input in = calm_input();
    autonomic_out o = {0};
    uint32_t rng = 1u;
    double ins = 0.0, exs = 0.0;
    int ni = 0, ne = 0;
    for (int i = 0; i < 120 * 60; i++) {
        double t = i * TICK;
        in.breath_phase = (float)fmod(t / 5.0, 1.0);
        auto_step(&a, &in, TICK, &rng, &o);
        if (t < 30.0) continue;
        /* The heart answers the breath ~1 s late. */
        double lagged = fmod((t - 1.0) / 5.0, 1.0);
        if (lagged < 0.4) {
            ins += (double)o.sa_rate;
            ni++;
        } else {
            exs += (double)o.sa_rate;
            ne++;
        }
    }
    double d = ins / ni - exs / ne;
    CHECK(d >= 2.0 && d <= 8.0);
}

/* The brain's vessels widen when its flow falls short, within seconds,
 * but only so far. */
static void test_auto_cerebral(void)
{
    autonomic a;
    auto_init(&a, 64.0f);
    autonomic_input in = calm_input();
    autonomic_out o = {0};
    uint32_t rng = 1u;
    in.brain_flow = 0.8f;
    settle(&a, &in, 10.0, &rng, &o);
    CHECK(o.r_bed[CB_BRAIN] < 0.9f);
    settle(&a, &in, 300.0, &rng, &o);
    CHECK(o.r_bed[CB_BRAIN] >= 0.4f);
}

static void test_auto_failure(void)
{
    autonomic a;
    auto_init(&a, 64.0f);
    autonomic_input in = calm_input();
    autonomic_out o = {0};
    uint32_t rng = 1u;
    in.brain_ok = 0;
    in.p_ao_mean = 50.0f;
    settle(&a, &in, 30.0, &rng, &o);
    CHECK(o.sa_rate <= 50.0f);
    CHECK(o.symp <= 0.1f);
}

static void test_auto_replay(void)
{
    autonomic a, b;
    auto_init(&a, 64.0f);
    auto_init(&b, 64.0f);
    autonomic_input in = calm_input();
    in.noise_amp = 1.0f;
    autonomic_out oa = {0}, ob = {0};
    uint32_t ra = 5u, rb = 5u;
    settle(&a, &in, 120.0, &ra, &oa);
    settle(&b, &in, 120.0, &rb, &ob);
    CHECK(memcmp(&oa, &ob, sizeof oa) == 0 && memcmp(&a, &b, sizeof a) == 0);
    /* The noise is there: a quiet body and a noisy one differ. */
    autonomic q;
    auto_init(&q, 64.0f);
    autonomic_out oq = {0};
    in.noise_amp = 0.0f;
    uint32_t rq = 5u;
    settle(&q, &in, 120.0, &rq, &oq);
    CHECK(fabsf(oq.sa_rate - oa.sa_rate) > 0.01f);
}

void test_autonomic_all(void)
{
    test_auto_rest();
    test_auto_delays();
    test_auto_exercise_kinetics();
    test_auto_rsa();
    test_auto_cerebral();
    test_auto_failure();
    test_auto_replay();
}
