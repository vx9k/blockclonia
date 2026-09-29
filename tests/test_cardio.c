/* Blood pressure out of range and the arrhythmias it brings (health.c):
 * the strain dose a crisis or hypotension builds and how it wears off,
 * what each new rhythm does to the circulation and the monitor, and the
 * replay guarantee through all of it. */
#include "hud.h"
#include "mem.h"
#include "test_util.h"

#include <math.h>
#include <string.h>

static float wave_span(const float *w)
{
    float lo = w[0], hi = w[0];
    for (int i = 1; i < HEALTH_WAVE_LEN; i++) {
        lo = fminf(lo, w[i]);
        hi = fmaxf(hi, w[i]);
    }
    return hi - lo;
}

static int has_alert(const health *h, const char *text)
{
    hud_state s;
    memset(&s, 0, sizeof s);
    hud_update(&s, h, 0.1f);
    for (int i = 0; i < s.alert_count; i++)
        if (strstr(s.alerts[i].text, text)) return 1;
    return 0;
}

/* A body at rest builds no strain and no arrhythmia, and neither does
 * hard exercise, whose high systolic is normal. */
static void test_calm_and_exercise(void)
{
    health *h = new_body(1);
    health_env e = calm_env();
    live(h, &e, 600.0);
    CHECK(h->strain == 0.0f && h->catechol == 0.0f && h->rhythm == RHYTHM_SINUS && h->ich == 0.0f);
    e.speed = 6.5;
    live(h, &e, 60.0);
    CHECK(h->sbp > 150.0f && h->hr > 150.0f);
    e.speed = 0.0;
    live(h, &e, 60.0);
    CHECK(h->strain < 0.05f && h->rhythm == RHYTHM_SINUS);
    mem_free(h);
}

/* A catecholamine surge: a crisis within seconds, with headache and a HUD
 * warning, a strain dose that grows while it lasts and wears off once the
 * pressure is back to normal. Two identical bodies replay exactly. */
static void test_crisis(void)
{
    health *a = new_body(21), *b = new_body(21);
    health_env e = calm_env();
    health *ab[2] = {a, b};
    for (int k = 0; k < 2; k++) {
        live(ab[k], &e, 5.0);
        health_injure(ab[k], health_debug_find("crisis"), -1);
    }
    live(a, &e, 30.0);
    CHECK(a->sbp > 180.0f && a->dbp > 110.0f && a->hr > 100.0f);
    CHECK(a->part[BP_HEAD].pain > 0.5f && has_alert(a, "Hypertensive crisis"));
    live(a, &e, 150.0);
    live(b, &e, 180.0);
    CHECK(a->rng == b->rng && a->strain == b->strain && a->rhythm == b->rhythm && a->map == b->map);
    CHECK(a->strain > 2.5f && a->rhythm == RHYTHM_SINUS);
    /* The surge ends: adrenaline clears in minutes, and with the pressure
     * back to normal the strain wears off. */
    float peak = a->strain;
    a->surge = 0.0f;
    live(a, &e, 900.0);
    CHECK(!a->dead && a->catechol < 0.1f && a->sbp < 150.0f && a->strain < peak * 0.5f);
    mem_free(a);
    mem_free(b);
}

/* A strained heart is irritable: among bodies left with ten minutes of
 * crisis behind them, some go into an arrhythmia within four minutes,
 * which a calm body never does. */
static void test_strain_risk(void)
{
    health_env e = calm_env();
    int irregular = 0;
    for (int i = 0; i < 20; i++) {
        health *h = new_body(300 + (uint32_t)i);
        health_injure(h, health_debug_find("strain"), -1);
        for (int s = 0; s < 240 && h->rhythm == RHYTHM_SINUS; s++) live(h, &e, 1.0);
        irregular += h->rhythm != RHYTHM_SINUS;
        mem_free(h);
    }
    CHECK(irregular >= 2 && irregular <= 12);
}

/* Bleeding out drops the diastolic below the coronaries' autoregulation
 * range, and the heart starts to take strain. */
static void test_hypotension_strain(void)
{
    health *h = new_body(5);
    health_env e = calm_env();
    health_cut(h, BP_LLEG, 0.7f, 1, 0.2f);
    for (int s = 0; s < 900 && h->dbp >= 40.0f && !h->dead; s++) live(h, &e, 1.0);
    CHECK(h->dbp < 40.0f && health_rhythm_perfusing(h->rhythm));
    float before = h->strain;
    live(h, &e, 30.0);
    CHECK(h->strain > before + 0.1f || !health_rhythm_perfusing(h->rhythm));
    CHECK(has_alert(h, "low blood pressure") || h->conscious == CONS_UNCONSCIOUS);
    mem_free(h);
}

/* Coefficient of variation of the R-R intervals over `seconds`. */
static float rr_variation(health *h, const health_env *e, double seconds)
{
    double sum = 0.0, sum2 = 0.0, last = h->last_r;
    int n = 0;
    for (int i = 0, steps = (int)(seconds * 60.0); i < steps; i++) {
        health_step(h, e, 1.0 / 60.0);
        if (h->last_r != last) {
            double rr = h->last_r - last;
            last = h->last_r;
            sum += rr;
            sum2 += rr * rr;
            n++;
        }
    }
    if (n < 3) return 0.0f;
    double mean = sum / n;
    return (float)(sqrt(fmax(0.0, sum2 / n - mean * mean)) / mean);
}

/* What each rhythm does: AF is irregular and loses the atrial kick, VT
 * runs fast with a weak pulse, complete block runs slow with a wide pulse
 * pressure, PEA and pulseless VT draw complexes without any pulse. */
static void test_rhythm_mechanics(void)
{
    health_env e = calm_env();
    health *s = new_body(40);
    live(s, &e, 20.0);
    CHECK(rr_variation(s, &e, 20.0) < 0.05f);
    float co_sinus = s->co;

    health *af = new_body(40);
    live(af, &e, 20.0);
    health_injure(af, health_debug_find("afib"), -1);
    live(af, &e, 10.0);
    CHECK(af->rhythm == RHYTHM_AF && rr_variation(af, &e, 20.0) > 0.12f);
    CHECK(af->hr > s->hr + 10.0f && health_spo2_reading(af) >= 90);

    health *vt = new_body(41);
    live(vt, &e, 20.0);
    health_injure(vt, health_debug_find("vtach"), -1);
    live(vt, &e, 5.0);
    CHECK(vt->rhythm == RHYTHM_VT);
    CHECK(fabsf(vt->hr - 165.0f) < 5.0f && vt->co < co_sinus && vt->sbp - vt->dbp < 25.0f);
    CHECK(health_spo2_reading(vt) >= 0 && has_alert(vt, "VT"));

    health *block = new_body(42);
    live(block, &e, 20.0);
    health_injure(block, health_debug_find("heart-block"), -1);
    live(block, &e, 10.0);
    CHECK(block->rhythm == RHYTHM_AVB3);
    CHECK(fabsf(block->hr - 36.0f) < 4.0f && block->sbp - block->dbp > 45.0f && block->co < co_sinus);
    CHECK(rr_variation(block, &e, 10.0) < 0.05f);

    health *pea = new_body(43), *asys = new_body(43), *pvt = new_body(43);
    health *arr[3] = {pea, asys, pvt};
    const int kinds[3] = {RHYTHM_PEA, RHYTHM_ASYSTOLE, RHYTHM_PVT};
    for (int k = 0; k < 3; k++) {
        live(arr[k], &e, 5.0);
        health_arrest(arr[k], kinds[k]);
        live(arr[k], &e, 8.0);
        CHECK(health_spo2_reading(arr[k]) == -1 && wave_span(arr[k]->pleth) < 1e-3f);
    }
    CHECK(wave_span(pea->ecg) > 0.8f && wave_span(asys->ecg) < 0.4f);
    CHECK(pvt->rhythm == RHYTHM_VF || wave_span(pvt->ecg) > 1.0f);
    CHECK(has_alert(pea, "CARDIAC ARREST (PEA)"));
    char buf[96];
    health_organ_status(pea, ORG_HEART, buf, sizeof buf);
    CHECK(strstr(buf, "PEA") != NULL);
    for (int k = 0; k < 3; k++) mem_free(arr[k]);
    mem_free(s);
    mem_free(af);
    mem_free(vt);
    mem_free(block);
}

/* A long crisis far above 200 systolic can bleed into the brain. Rare, so
 * bodies are tried until one does; and a normal pressure never does. */
static void test_stroke(void)
{
    health_env e = calm_env();
    int stroke = 0;
    for (int i = 0; i < 40 && !stroke; i++) {
        health *h = new_body(500 + (uint32_t)i);
        health_injure(h, health_debug_find("strain"), -1);
        for (int s = 0; s < 300 && !stroke && !h->dead; s++) {
            h->catechol = 20.0f; /* an adrenaline infusion held at a pressor level */
            live(h, &e, 1.0);
            stroke = h->ich_rate > 0.0f;
        }
        mem_free(h);
    }
    CHECK(stroke);
}

/* void test_cardio_all(void)
{
    test_calm_and_exercise();
    test_crisis();
    test_strain_risk();
    test_hypotension_strain();
    test_rhythm_mechanics();
    test_stroke();
} */
