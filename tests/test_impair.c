/* What a clouded mind does to the controls (impair.c): a lag behind the
 * player's hands, dropped actions and stumbles, the same at any frame
 * rate. */
#include "impair.h"
#include "test_util.h"

#include <math.h>
#include <string.h>

/* Unimpaired, the controls pass through untouched, frame by frame. */
static void test_impair_clear(void)
{
    impair m;
    impair_init(&m, 1u);
    int same = 1;
    for (int i = 0; i < 600; i++) {
        impair_frame in = {0};
        in.move.forward = (float)((i / 30) % 3 - 1);
        in.move.jump = i % 7 == 0;
        in.look_dx = 0.001f * (float)(i % 11);
        in.click_break = i % 5 == 0;
        impair_frame out = impair_step(&m, &in, 0.0f, 0.0f, 0.0f, i / 60.0, 1.0f / 60.0f);
        same &= out.move.forward == in.move.forward && out.move.jump == in.move.jump && out.look_dx == in.look_dx &&
                out.click_break == in.click_break;
    }
    CHECK(same);
}

/* Fed at hz for seconds with a forward press at press_t and a steady turn
 * until then: returns when the press came through, and the turn delivered. */
static double lagged_press(int hz, float lag, double press_t, float *turned)
{
    impair m;
    impair_init(&m, 2u);
    const double dt = 1.0 / hz;
    double seen = -1.0;
    *turned = 0.0f;
    for (int i = 0; i < 3 * hz; i++) {
        const double t = i * dt;
        impair_frame in = {0};
        in.move.forward = t >= press_t ? 1.0f : 0.0f;
        in.look_dx = t < press_t ? 0.01f : 0.0f;
        impair_frame out = impair_step(&m, &in, lag, 0.0f, 0.0f, t, (float)dt);
        *turned += out.look_dx;
        if (seen < 0.0 && out.move.forward > 0.0f) seen = t;
    }
    return seen;
}

/* A 0.3 s lag: a press acts 0.3 s late, and every bit of the mouse's turn
 * still arrives, at 60 Hz as at 144. */
static void test_impair_lag(void)
{
    float turn60 = 0.0f, turn144 = 0.0f;
    double at60 = lagged_press(60, 0.3f, 0.5, &turn60), at144 = lagged_press(144, 0.3f, 0.5, &turn144);
    CHECK(fabs(at60 - 0.8) < 1.0 / 60.0 + 1e-9 && fabs(at144 - 0.8) < 1.0 / 144.0 + 1e-9);
    CHECK(fabsf(turn60 - 0.01f * 30.0f) < 1e-4f && fabsf(turn144 - 0.01f * 72.0f) < 1e-4f);
}

/* Clicks through a hand that misses some: the share lost is the rate. */
static int clicks_through(float drop)
{
    impair m;
    impair_init(&m, 3u);
    int n = 0;
    for (int i = 0; i < 2000; i++) {
        impair_frame in = {0};
        in.click_break = i % 2 == 0;
        n += impair_step(&m, &in, 0.0f, drop, 0.0f, i / 60.0, 1.0f / 60.0f).click_break;
    }
    return n;
}

static void test_impair_drop(void)
{
    CHECK(clicks_through(0.0f) == 1000);
    CHECK(clicks_through(1.0f) == 0);
    int half = clicks_through(0.5f);
    CHECK(half > 400 && half < 600);
}

/* Walking for ten minutes at a rate of stumbles: how many began, and
 * whether every stumble slowed the step. */
static int stumbles(int hz, float rate, float forward, int *slowed)
{
    impair m;
    impair_init(&m, 4u);
    const double dt = 1.0 / hz;
    int n = 0;
    float was = 0.0f;
    *slowed = 1;
    for (int i = 0; i < 600 * hz; i++) {
        impair_frame in = {0};
        in.move.forward = forward;
        impair_frame out = impair_step(&m, &in, 0.0f, 0.0f, rate, i * dt, (float)dt);
        if (m.stumble_t > 0.0f && was <= 0.0f) n++;
        if (m.stumble_t > 0.0f) *slowed &= out.move.forward < 0.3f;
        was = m.stumble_t;
    }
    return n;
}

static void test_impair_stumble(void)
{
    int slowed60 = 0, slowed144 = 0, still = 0;
    const int n60 = stumbles(60, 0.5f, 1.0f, &slowed60), n144 = stumbles(144, 0.5f, 1.0f, &slowed144);
    CHECK(n60 > 150 && n60 < 350 && slowed60 && slowed144);
    CHECK(fabs((double)n144 / (double)n60 - 1.0) < 0.25);
    CHECK(stumbles(60, 0.5f, 0.0f, &still) == 0); /* standing still, nothing to trip over */
}

/* The same seed and the same hands give the same stumbles and drops. */
static void test_impair_replay(void)
{
    impair a, b;
    impair_init(&a, 9u);
    impair_init(&b, 9u);
    int same = 1;
    for (int i = 0; i < 6000; i++) {
        impair_frame in = {0};
        in.move.forward = 1.0f;
        in.click_place = i % 3 == 0;
        in.look_dy = 0.002f;
        impair_frame oa = impair_step(&a, &in, 0.2f, 0.3f, 0.4f, i / 60.0, 1.0f / 60.0f);
        impair_frame ob = impair_step(&b, &in, 0.2f, 0.3f, 0.4f, i / 60.0, 1.0f / 60.0f);
        same &= memcmp(&oa, &ob, sizeof oa) == 0; /* NOLINT: both zero-initialised, padding too */
    }
    CHECK(same);
}

void test_impair_all(void)
{
    test_impair_clear();
    test_impair_lag();
    test_impair_drop();
    test_impair_stumble();
    test_impair_replay();
}
