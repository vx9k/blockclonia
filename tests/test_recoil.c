/* Recoil from a shot's momentum, the hold bringing the sights back, and
 * the bolt being worked between shots. */
#include "test_util.h"

#include "combat.h"
#include "itemmodel.h"
#include "recoil.h"
#include "viewmodel.h"

#include <math.h>

/* The momentum and free-recoil energy of published loads in a 4 kg rifle:
 * a 7.62 mm service round gives about 17 J, the heavy black-powder bullet
 * a little more. */
static void test_recoil_energy(void)
{
    const ammo_def *ball = ammo_get(AMMO_SMOKELESS), *lead = ammo_get(AMMO_BLACK_LEAD);
    float p = recoil_impulse(ball);
    CHECK(p > 11.4f && p < 11.9f);
    CHECK(fabsf(recoil_velocity(ball, RIFLE_KG) - p / RIFLE_KG) < 1e-5f);
    float e = recoil_energy(ball, RIFLE_KG), el = recoil_energy(lead, RIFLE_KG);
    CHECK(e > 15.0f && e < 19.0f && el > 20.0f && el < 24.0f);
    /* The gas counts: without the charge the push is smaller. */
    ammo_def bare = *ball;
    bare.charge = 0.0f;
    CHECK(recoil_impulse(&bare) < p - 3.0f);
    CHECK(recoil_impulse(NULL) == 0.0f && recoil_velocity(ball, 0.0f) == 0.0f);
}

/* Fires once and follows the look for t seconds at fps; returns the
 * highest climb and writes where the look ends up. */
static float follow(float aim, float fps, float t, float *end_p, float *end_y)
{
    recoil r = {0};
    recoil_init(&r, 99);
    recoil_fire(&r, ammo_get(AMMO_SMOKELESS), aim);
    float sum_p = 0.0f, sum_y = 0.0f, top = 0.0f;
    int frames = (int)lroundf(t * fps);
    for (int i = 0; i < frames; i++) {
        float dp = 0.0f, dy = 0.0f;
        recoil_step(&r, 1.0f / fps, &dp, &dy);
        sum_p += dp;
        sum_y += dy;
        top = fmaxf(top, sum_p);
    }
    *end_p = sum_p;
    *end_y = sum_y;
    return top;
}

/* The muzzle climbs a few degrees, more from the hip, then the hold brings
 * the sights back within a few milliradians of the mark. */
static void test_recoil_kick(void)
{
    float p = 0.0f, y = 0.0f, hp = 0.0f, hy = 0.0f;
    float top = follow(1.0f, 120.0f, 2.0f, &p, &y), hip = follow(0.0f, 120.0f, 2.0f, &hp, &hy);
    CHECK(top > 0.017f && top < 0.105f && hip > top * 1.3f);
    CHECK(fabsf(p) <= 0.0031f && fabsf(y) <= 0.0031f);
    CHECK(fabsf(hp) <= 0.0151f && fabsf(hy) <= 0.0151f);
    /* The same climb at 30 and 144 fps. */
    float a = 0.0f, b = 0.0f, ay = 0.0f, by = 0.0f;
    float ta = follow(1.0f, 30.0f, 1.0f / 3.0f, &a, &ay), tb = follow(1.0f, 144.0f, 1.0f / 3.0f, &b, &by);
    CHECK(fabsf(a - b) < 0.02f * fabsf(b) + 1e-4f && fabsf(ay - by) < 0.02f * fabsf(by) + 1e-4f);
    (void)ta;
    (void)tb;

    /* Bad frame times hand over nothing; once settled it starts from zero. */
    recoil r = {0};
    recoil_init(&r, 5);
    recoil_fire(&r, ammo_get(AMMO_SMOKELESS), 1.0f);
    float dp = 1.0f, dy = 1.0f;
    recoil_step(&r, NAN, &dp, &dy);
    CHECK(dp == 0.0f && dy == 0.0f);
    for (int i = 0; i < 600; i++) recoil_step(&r, 1.0f / 60.0f, &dp, &dy);
    CHECK(r.pitch == 0.0f && r.vp == 0.0f && r.applied_p == 0.0f && r.seed != 5u);
}

/* The bolt is worked after the recoil: hand to the knob, up, back, home,
 * down, hand back to the wrist. */
static void test_bolt_cycle(void)
{
    float reach = 1.0f, lift = 1.0f, slide = 1.0f;
    viewmodel_bolt(0.0f, &reach, &lift, &slide);
    CHECK(reach == 0.0f && lift == 0.0f && slide == 0.0f);
    viewmodel_bolt(0.1f, &reach, &lift, &slide);
    CHECK(reach == 0.0f && lift == 0.0f && slide == 0.0f);
    viewmodel_bolt(0.36f, &reach, &lift, &slide);
    CHECK(reach == 1.0f && lift > 0.2f && lift < 0.8f && slide == 0.0f);
    viewmodel_bolt(0.58f, &reach, &lift, &slide);
    CHECK(reach == 1.0f && lift == 1.0f && slide == 1.0f);
    viewmodel_bolt(0.8f, &reach, &lift, &slide);
    CHECK(reach == 1.0f && lift > 0.0f && lift < 1.0f && slide == 0.0f);
    viewmodel_bolt(1.0f, &reach, &lift, &slide);
    CHECK(reach == 0.0f && lift == 0.0f && slide == 0.0f);
    viewmodel_bolt(NAN, &reach, &lift, &slide);
    CHECK(reach == 0.0f && lift == 0.0f && slide == 0.0f);
    CHECK(COMBAT_CYCLE_S > 1.0f && COMBAT_CYCLE_S < 2.0f);

    /* Drawn back, the handle and the right hand move; the rest stays. */
    viewmodel still, worked;
    viewmodel_init(&still);
    viewmodel_input in = {0};
    in.held_id = I_RIFLE;
    in.aim = 1.0f;
    in.light = 1.0f;
    in.dt = 1.0f / 60.0f;
    entity_instance a[VIEWMODEL_MAX] = {0}, b[VIEWMODEL_MAX] = {0};
    for (int i = 0; i < 120; i++) viewmodel_build(&still, &in, a, VIEWMODEL_MAX);
    worked = still;
    int n = viewmodel_build(&still, &in, a, VIEWMODEL_MAX);
    in.bolt = 0.58f;
    int m = viewmodel_build(&worked, &in, b, VIEWMODEL_MAX);
    /* The rifle comes down off the cheek, and the right palm travels to
     * the knob relative to the left one on the fore-end. */
    const int r = VIEWMODEL_PALM, l = VIEWMODEL_ARM_PARTS + VIEWMODEL_PALM;
    float rel = 0.0f;
    for (int k = 0; k < 3; k++) rel += fabsf((b[r].pos[k] - a[r].pos[k]) - (b[l].pos[k] - a[l].pos[k]));
    CHECK(n == m && n > 2 * VIEWMODEL_ARM_PARTS && b[n - 1].pos[1] < a[n - 1].pos[1] - 0.05f && rel > 0.05f);
}

/* Every round carries a real charge and pushes back; the slow, heavy
 * black-powder bullet pushes hardest. */
static void test_recoil_rounds(void)
{
    float most = 0.0f;
    int hardest = -1;
    for (int k = 0; k < AMMO_COUNT; k++) {
        const ammo_def *a = ammo_get(k);
        CHECK(a->charge > 0.002f && a->charge < 0.005f);
        float p = recoil_impulse(a);
        CHECK(p > 8.0f && p < 15.0f);
        if (p > most) {
            most = p;
            hardest = k;
        }
    }
    CHECK(hardest == AMMO_BLACK_LEAD);
}

/* The same seed follows the same path; a hitch or a quick second shot
 * stays bounded. */
static void test_recoil_edges(void)
{
    recoil a = {0}, b = {0}, c = {0};
    recoil_init(&a, 3);
    recoil_init(&b, 3);
    recoil_init(&c, 4);
    recoil_fire(&a, ammo_get(AMMO_SMOKELESS), 1.0f);
    recoil_fire(&b, ammo_get(AMMO_SMOKELESS), 1.0f);
    recoil_fire(&c, ammo_get(AMMO_SMOKELESS), 1.0f);
    CHECK(a.rest_p == b.rest_p && a.rest_y == b.rest_y && a.rest_p != c.rest_p);
    float dp = 0.0f, dy = 0.0f;
    recoil_step(&a, 1.0f, &dp, &dy);
    CHECK(isfinite(dp) && fabsf(dp) < 0.1f && fabsf(dy) < 0.1f);
    recoil_fire(&b, ammo_get(AMMO_SMOKELESS), 1.0f);
    float top = 0.0f, sum = 0.0f;
    for (int i = 0; i < 120; i++) {
        recoil_step(&b, 1.0f / 60.0f, &dp, &dy);
        sum += dp;
        top = fmaxf(top, sum);
    }
    CHECK(top > 0.02f && top < 0.2f);
}

/* Only the rifle has a bolt, and it marks the handle; the phases blend
 * smoothly, with no jump between frames. */
static void test_bolt_model(void)
{
    item_model m = {0};
    CHECK(itemmodel_build(I_RIFLE, &m) && m.bolt > 0 && m.bolt + 1 < m.n);
    CHECK(fabsf(m.box[m.bolt].pos[1] - 0.40f) < 1e-4f && m.box[m.bolt + 1].pos[2] > m.box[m.bolt].pos[2]);
    item_model pick = {0};
    CHECK(itemmodel_build(I_BANDAGE, &pick) && pick.bolt == 0);
    float prev[3] = {0}, cur[3] = {0}, jump = 0.0f;
    for (int i = 0; i <= 200; i++) {
        viewmodel_bolt((float)i / 200.0f, &cur[0], &cur[1], &cur[2]);
        for (int k = 0; k < 3; k++) jump = fmaxf(jump, fabsf(cur[k] - prev[k]));
        for (int k = 0; k < 3; k++) prev[k] = cur[k];
    }
    CHECK(jump < 0.1f);
}

void test_recoil_all(void)
{
    test_recoil_rounds();
    test_recoil_edges();
    test_bolt_model();
    test_recoil_energy();
    test_recoil_kick();
    test_bolt_cycle();
}
