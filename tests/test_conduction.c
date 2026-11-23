/* The heart's electrical timing (conduction.c): when the atria and the
 * ventricles fire in each rhythm, how the AV node passes or blocks an
 * impulse, and the ectopic beats an irritable ventricle adds. */
#include "conduction.h"
#include "test_util.h"

#include <math.h>
#include <string.h>

#define TICK (1.0 / 60.0)

typedef struct {
    int beats, ectopic, pr_bad;
    double rr[2048];
    int n_rr;
    double t_beat[2048];
    int ect[2048];
} cond_log;

/* Runs seconds of conduction, logging every ventricular beat. */
static void run(conduction *k, const conduction_input *in, double seconds, uint32_t seed, cond_log *log)
{
    memset(log, 0, sizeof *log);
    uint32_t rng = seed;
    double t = 0.0, last = -1.0;
    int ticks = (int)lround(seconds / TICK);
    for (int i = 0; i < ticks; i++) {
        int n = cond_step(k, in, t, TICK, &rng);
        t += TICK;
        if (n <= 0) continue;
        log->beats += n;
        if (k->last_ectopic) log->ectopic++;
        if (!k->last_ectopic && (k->t_vent - k->t_atria < 0.12 || k->t_vent - k->t_atria > 0.20)) log->pr_bad++;
        if (log->n_rr < 2048) {
            log->t_beat[log->n_rr] = k->t_vent;
            log->ect[log->n_rr] = k->last_ectopic;
            log->rr[log->n_rr] = last >= 0.0 ? k->t_vent - last : 0.0;
            log->n_rr++;
        }
        last = k->t_vent;
    }
}

static conduction_input sinus_input(float rate)
{
    conduction_input in = {0};
    in.sa_rate = rate;
    in.av_delay = 0.16f;
    in.rhythm = RHYTHM_SINUS;
    return in;
}

static cond_log g_log, g_log2;

static void test_cond_sinus(void)
{
    conduction k;
    cond_init(&k);
    conduction_input in = sinus_input(70.0f);
    run(&k, &in, 60.0, 1u, &g_log);
    CHECK(g_log.beats >= 69 && g_log.beats <= 71);
    CHECK(g_log.pr_bad == 0);
    CHECK(g_log.ectopic == 0);
}

/* The PR interval follows the delay the nerves set. */
static void test_cond_autonomic_av(void)
{
    const float delays[2] = {0.12f, 0.20f};
    for (int d = 0; d < 2; d++) {
        conduction k;
        cond_init(&k);
        conduction_input in = sinus_input(70.0f);
        in.av_delay = delays[d];
        uint32_t rng = 7u;
        double t = 0.0, worst = 0.0;
        for (int i = 0; i < 1800; i++) {
            if (cond_step(&k, &in, t, TICK, &rng) > 0)
                worst = fmax(worst, fabs(k.t_vent - k.t_atria - (double)delays[d]));
            t += TICK;
        }
        CHECK(worst < 0.005);
    }
}

/* Complete heart block: the atria keep the sinus rate, the ventricles
 * fall back on their own slow escape rhythm. */
static void test_cond_avb3(void)
{
    conduction k;
    cond_init(&k);
    conduction_input in = sinus_input(80.0f);
    in.rhythm = RHYTHM_AVB3;
    uint32_t rng = 3u;
    double t = 0.0, last_a = -1.0;
    int atrial = 0, vent = 0;
    for (int i = 0; i < 3600; i++) {
        vent += cond_step(&k, &in, t, TICK, &rng);
        if (k.t_atria != last_a) atrial++;
        last_a = k.t_atria;
        t += TICK;
    }
    CHECK(vent >= 30 && vent <= 40);
    CHECK(atrial >= 78 && atrial <= 82);
}

/* Atrial fibrillation: an irregularly irregular ventricle at 90-150. */
static void test_cond_af(void)
{
    conduction k;
    cond_init(&k);
    conduction_input in = sinus_input(70.0f);
    in.rhythm = RHYTHM_AF;
    run(&k, &in, 120.0, 11u, &g_log);
    double mean = 0.0, var = 0.0;
    int n = 0;
    for (int i = 1; i < g_log.n_rr; i++) {
        mean += g_log.rr[i];
        n++;
    }
    mean /= n;
    for (int i = 1; i < g_log.n_rr; i++) var += (g_log.rr[i] - mean) * (g_log.rr[i] - mean);
    double cv = sqrt(var / n) / mean;
    CHECK(cv > 0.12);
    CHECK(60.0 / mean > 90.0 && 60.0 / mean < 150.0);
}

/* Fast AF under full sympathetic drive: the ventricles' refractory time
 * shortens with the rate, so the response runs well past 240 a minute,
 * but never faster than a beat every 0.19 s (~315 a minute). */
static void test_cond_af_fast(void)
{
    conduction k;
    cond_init(&k);
    conduction_input in = sinus_input(220.0f);
    in.rhythm = RHYTHM_AF;
    run(&k, &in, 60.0, 13u, &g_log);
    int over = 0;
    double rr_min = 1.0;
    for (int i = 1; i < g_log.n_rr; i++) {
        over += g_log.rr[i] < 60.0 / 240.0;
        rr_min = fmin(rr_min, g_log.rr[i]);
    }
    CHECK(over > 10);
    CHECK(rr_min >= 0.19 - 1e-9);
}

static void test_cond_fast_slow(void)
{
    conduction k;
    cond_init(&k);
    conduction_input in = sinus_input(70.0f);
    in.rhythm = RHYTHM_VT;
    run(&k, &in, 60.0, 5u, &g_log);
    CHECK(fabs(g_log.beats - COND_VT_RATE) <= 5.0);
    cond_init(&k);
    in.rhythm = RHYTHM_PVT;
    run(&k, &in, 60.0, 5u, &g_log);
    CHECK(fabs(g_log.beats - COND_PVT_RATE) <= 5.0);
    cond_init(&k);
    in.rhythm = RHYTHM_ASYSTOLE;
    run(&k, &in, 60.0, 5u, &g_log);
    CHECK(g_log.beats == 0);
    /* A rhythm switched mid-run takes over at once. */
    cond_init(&k);
    in.rhythm = RHYTHM_SINUS;
    run(&k, &in, 10.0, 5u, &g_log);
    in.rhythm = RHYTHM_VT;
    run(&k, &in, 10.0, 5u, &g_log);
    CHECK(fabs(g_log.beats - COND_VT_RATE / 6.0) <= 2.0);
}

/* Ectopic beats come at the rate asked, each early and followed by a
 * compensatory pause: the sinus beat after it is blocked, so the beat
 * after that lands two sinus cycles after the one before the ectopic. */
static void test_cond_ectopic_rate(void)
{
    conduction k;
    cond_init(&k);
    conduction_input in = sinus_input(70.0f);
    in.irritability = 0.05f;
    run(&k, &in, 600.0, 21u, &g_log);
    CHECK(g_log.ectopic <= 2);
    cond_init(&k);
    in.irritability = 6.0f;
    run(&k, &in, 600.0, 21u, &g_log);
    CHECK(g_log.ectopic >= 40 && g_log.ectopic <= 80);
    double rr = 60.0 / 70.0;
    int early = 1, pause = 1, checked = 0;
    for (int i = 1; i + 1 < g_log.n_rr; i++) {
        if (!g_log.ect[i] || g_log.ect[i - 1] || g_log.ect[i + 1]) continue;
        early &= g_log.t_beat[i] - g_log.t_beat[i - 1] < rr - 0.05;
        pause &= fabs(g_log.t_beat[i + 1] - g_log.t_beat[i - 1] - 2.0 * rr) < 0.03;
        checked++;
    }
    CHECK(checked > 20);
    CHECK(early);
    CHECK(pause);
}

/* Beats are timed exactly inside a step, not snapped to it: the same
 * sinus rhythm stepped at 60 Hz and at 144 Hz fires its ventricles at the
 * same moments, to well under a millisecond, and keeps the PR interval. */
static void test_cond_beat_timing(void)
{
    conduction a, b;
    cond_init(&a);
    cond_init(&b);
    const conduction_input in = sinus_input(73.0f);
    double t_a[128] = {0}, t_b[128] = {0};
    int n_a = 0, n_b = 0;
    uint32_t rng_a = 5u, rng_b = 5u;
    for (int i = 0; i < 60 * 60; i++)
        if (cond_step(&a, &in, i / 60.0, 1.0 / 60.0, &rng_a) > 0 && n_a < 128) t_a[n_a++] = a.t_vent;
    for (int i = 0; i < 60 * 144; i++)
        if (cond_step(&b, &in, i / 144.0, 1.0 / 144.0, &rng_b) > 0 && n_b < 128) t_b[n_b++] = b.t_vent;
    double worst = 0.0;
    for (int i = 0; i < n_a && i < n_b; i++) worst = fmax(worst, fabs(t_a[i] - t_b[i]));
    CHECK(n_a == n_b && n_a > 60);
    CHECK(worst < 1e-4);
    CHECK(fabs(a.t_vent - a.t_atria - 0.16) < 1e-3);
}

static void test_cond_replay(void)
{
    conduction a, b;
    cond_init(&a);
    cond_init(&b);
    conduction_input in = sinus_input(70.0f);
    in.rhythm = RHYTHM_AF;
    in.irritability = 3.0f;
    run(&a, &in, 120.0, 99u, &g_log);
    run(&b, &in, 120.0, 99u, &g_log2);
    int same = g_log.n_rr == g_log2.n_rr;
    for (size_t i = 0; i < sizeof g_log.t_beat / sizeof g_log.t_beat[0]; i++)
        same &= g_log.t_beat[i] == g_log2.t_beat[i];
    CHECK(same);
}

void test_conduction_all(void)
{
    test_cond_sinus();
    test_cond_autonomic_av();
    test_cond_avb3();
    test_cond_af();
    test_cond_af_fast();
    test_cond_fast_slow();
    test_cond_ectopic_rate();
    test_cond_beat_timing();
    test_cond_replay();
}
