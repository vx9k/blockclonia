/* The inner voice (thoughts.c): what each feeling reads from the body,
 * when it speaks, when it keeps quiet, and that it looks the same at any
 * frame rate. */
#include "hud.h"
#include "mem.h"
#include "settings.h"
#include "test_util.h"
#include "thoughts.h"

#include <string.h>

/* Steps the voice for `seconds` at `fps`, counting the lines it says. */
static int listen(thoughts *t, const health *h, double seconds, double fps)
{
    int n = 0;
    int steps = (int)(seconds * fps + 0.5);
    for (int i = 0; i < steps; i++) n += thoughts_step(t, h, (float)(1.0 / fps));
    return n;
}

static int has_text(const thoughts *t, const char *s) { return strstr(t->now.text, s) != NULL; }

static void test_healthy_is_quiet(void)
{
    health *h = new_body(1);
    thoughts t;
    thoughts_init(&t, 7);
    for (int f = 0; f < FEEL_COUNT; f++) CHECK(thoughts_level(h, f) == 0);
    CHECK(listen(&t, h, 600.0, 30.0) == 0);
    CHECK(t.now.text[0] == '\0');
    CHECK(thoughts_alpha(&t) == 0.0f);
    CHECK(thoughts_level(h, -1) == 0 && thoughts_level(h, FEEL_COUNT) == 0);
    mem_free(h);
}

static void test_pain_levels(void)
{
    health *h = new_body(2);
    h->pain = 2.0f;
    CHECK(thoughts_level(h, FEEL_PAIN) == 0);
    h->pain = 3.0f;
    CHECK(thoughts_level(h, FEEL_PAIN) == 1);
    h->pain = 6.0f;
    CHECK(thoughts_level(h, FEEL_PAIN) == 2);
    h->pain = 9.0f;
    CHECK(thoughts_level(h, FEEL_PAIN) == 3);
    mem_free(h);
}

static void test_blood_levels(void)
{
    health *h = new_body(3);
    h->bleed_ext = 10.0f;
    CHECK(thoughts_level(h, FEEL_BLEEDING) == 1);
    h->bleed_ext = 150.0f;
    CHECK(thoughts_level(h, FEEL_BLEEDING) == 3);
    h->blood = BLOOD_NORMAL * 0.85f;
    CHECK(thoughts_level(h, FEEL_FAINT) == 1);
    h->blood = BLOOD_NORMAL * 0.6f;
    CHECK(thoughts_level(h, FEEL_FAINT) == 3);
    mem_free(h);
}

static void test_breath_levels(void)
{
    health *h = new_body(4);
    h->breathing = 1;
    h->sao2 = 0.80f;
    CHECK(thoughts_level(h, FEEL_BREATH) == 2);
    CHECK(thoughts_level(h, FEEL_AIR) == 0);
    h->sao2 = 0.97f;
    h->pneumothorax = 0.3f;
    CHECK(thoughts_level(h, FEEL_BREATH) == 2);
    /* Holding breath is its own feeling, driven by CO2 before oxygen. */
    h->pneumothorax = 0.0f;
    h->breathing = 0;
    h->paco2 = 48.0f;
    CHECK(thoughts_level(h, FEEL_BREATH) == 0);
    CHECK(thoughts_level(h, FEEL_AIR) == 1);
    h->paco2 = 53.0f;
    CHECK(thoughts_level(h, FEEL_AIR) == 2);
    h->sao2 = 0.8f;
    CHECK(thoughts_level(h, FEEL_AIR) == 3);
    mem_free(h);
}

static void test_heart_levels(void)
{
    health *h = new_body(5);
    h->rhythm = RHYTHM_AF;
    CHECK(thoughts_level(h, FEEL_HEART) == 1);
    h->rhythm = RHYTHM_VT;
    CHECK(thoughts_level(h, FEEL_HEART) == 2);
    h->rhythm = RHYTHM_SINUS;
    h->ischemia = 2.5f;
    CHECK(thoughts_level(h, FEEL_HEART) == 3);
    mem_free(h);
}

static void test_heat_levels(void)
{
    health *h = new_body(6);
    h->temp = 35.0f;
    CHECK(thoughts_level(h, FEEL_COLD) == 2);
    h->temp = 33.5f;
    CHECK(thoughts_level(h, FEEL_COLD) == 3);
    h->temp = 39.0f;
    CHECK(thoughts_level(h, FEEL_COLD) == 0);
    CHECK(thoughts_level(h, FEEL_HOT) == 2);
    /* The same temperature with sepsis is a fever, not the weather. */
    h->sepsis = 0.4f;
    CHECK(thoughts_level(h, FEEL_HOT) == 0);
    CHECK(thoughts_level(h, FEEL_FEVER) == 2);
    h->sepsis = 0.0f;
    h->temp = 37.0f;
    h->radiant = 3000.0f;
    CHECK(thoughts_level(h, FEEL_HOT) == 2);
    h->radiant = 0.0f;
    h->part[BP_RARM].surface = 10.0f;
    h->part[BP_RARM].skin = 36.0f;
    CHECK(thoughts_level(h, FEEL_BURN) == 2);
    h->in_fire = 1;
    CHECK(thoughts_level(h, FEEL_BURN) == 3);
    mem_free(h);
}

static void test_cold_skin_and_wet(void)
{
    health *h = new_body(7);
    for (int i = 0; i < BP_COUNT; i++) h->part[i].skin = 24.0f;
    CHECK(thoughts_level(h, FEEL_COLD) == 1);
    for (int i = 0; i < BP_COUNT; i++) h->part[i].skin = 33.0f;
    h->wet = 0.8f;
    h->air_temp = 5.0f;
    CHECK(thoughts_level(h, FEEL_COLD) == 1);
    h->air_temp = 20.0f;
    CHECK(thoughts_level(h, FEEL_COLD) == 0);
    mem_free(h);
}

static void test_numb_levels(void)
{
    health *h = new_body(8);
    h->part[BP_LLEG].skin = 5.0f;
    CHECK(thoughts_level(h, FEEL_NUMB) == 1);
    h->part[BP_RARM].frost = FROST_DEEP;
    CHECK(thoughts_level(h, FEEL_NUMB) == 3);
    /* Frostbite on the head is not numb fingers or toes. */
    h->part[BP_RARM].frost = FROST_NONE;
    h->part[BP_LLEG].skin = 30.0f;
    h->part[BP_HEAD].frost = FROST_SUPERFICIAL;
    CHECK(thoughts_level(h, FEEL_NUMB) == 0);
    mem_free(h);
}

static void test_inside_levels(void)
{
    health *h = new_body(9);
    h->wound_count = 1;
    h->wounds[0].infection = 0.5f;
    CHECK(thoughts_level(h, FEEL_FEVER) == 1);
    h->wound_count = 0;
    h->gut_upset = 2.0f;
    CHECK(thoughts_level(h, FEEL_SICK) == 1);
    h->gut_upset = 0.0f;
    h->hepatotox = 0.3f;
    CHECK(thoughts_level(h, FEEL_SICK) == 2);
    h->hepatotox = 0.0f;
    h->organ[ORG_LIVER] = 0.1f;
    CHECK(thoughts_level(h, FEEL_SICK) == 3);
    h->organ[ORG_LIVER] = 1.0f;
    h->potassium = 6.5f;
    CHECK(thoughts_level(h, FEEL_SICK) == 2);
    h->potassium = 4.0f;
    h->conscious = CONS_CONFUSED;
    CHECK(thoughts_level(h, FEEL_DAZED) == 2);
    h->conscious = CONS_ALERT;
    h->organ[ORG_BRAIN] = 0.25f;
    CHECK(thoughts_level(h, FEEL_DAZED) == 3);
    mem_free(h);
}

static void test_needs_levels(void)
{
    health *h = new_body(10);
    h->water = WATER_NORMAL * (1.0f - 0.15f * 0.6f); /* hydration 0.4 */
    CHECK(thoughts_level(h, FEEL_THIRST) == 1);
    h->water = WATER_NORMAL * (1.0f - 0.15f * 0.9f);
    CHECK(thoughts_level(h, FEEL_THIRST) == 3);
    h->water = WATER_NORMAL;
    h->glycogen = 0.0f;
    h->stomach_kcal = 0.0f;
    CHECK(thoughts_level(h, FEEL_HUNGER) == 3);
    h->wbal = 0.0f;
    CHECK(thoughts_level(h, FEEL_TIRED) == 2);
    mem_free(h);
}

static void test_adrenaline_levels(void)
{
    health *h = new_body(11);
    h->catechol = 1.0f;
    CHECK(thoughts_level(h, FEEL_RUSH) == 1);
    h->catechol = 3.0f;
    CHECK(thoughts_level(h, FEEL_RUSH) == 2);
    /* The crash is felt only once the rush is over. */
    h->crash = 0.8f;
    CHECK(thoughts_level(h, FEEL_CRASH) == 0);
    h->catechol = 0.1f;
    CHECK(thoughts_level(h, FEEL_CRASH) == 2);
    mem_free(h);
}

/* A feeling that sets in is voiced at once, at its level, naming the part
 * that hurts when the line has a place for it. */
static void test_voices_a_new_feeling(void)
{
    health *h = new_body(12);
    thoughts t;
    thoughts_init(&t, 3);
    h->pain = 6.0f;
    h->part[BP_LLEG].pain = 6.0f;
    CHECK(thoughts_step(&t, h, 1.0f / 60.0f) == 1);
    CHECK(t.now.feeling == FEEL_PAIN && t.now.level == 2);
    CHECK(t.now.text[0] != '\0' && !strchr(t.now.text, '{'));
    CHECK(!has_text(&t, "head") && !has_text(&t, "arm"));
    /* Only one line, however long it stays. */
    CHECK(listen(&t, h, 30.0, 60.0) == 0);
    mem_free(h);
}

/* Two feelings at once take turns: the more urgent first, the other after
 * the gap. */
static void test_gap_between_lines(void)
{
    health *h = new_body(13);
    thoughts t;
    thoughts_init(&t, 4);
    h->pain = 9.0f;
    h->water = WATER_NORMAL * (1.0f - 0.15f * 0.6f);
    CHECK(thoughts_step(&t, h, 0.02f) == 1);
    CHECK(t.now.feeling == FEEL_PAIN && t.now.level == 3);
    float gap = t.gap;
    CHECK(gap > t.now.life);
    CHECK(listen(&t, h, (double)gap - 0.1, 50.0) == 0);
    CHECK(listen(&t, h, 0.5, 50.0) == 1);
    CHECK(t.now.feeling == FEEL_THIRST && t.now.level == 1);
    mem_free(h);
}

/* A lasting feeling comes back: a mild one after minutes, a severe one
 * after half a minute. */
static void test_reminders(void)
{
    health *h = new_body(14);
    thoughts t;
    thoughts_init(&t, 5);
    h->pain = 3.0f;
    CHECK(listen(&t, h, 1.0, 30.0) == 1);
    CHECK(listen(&t, h, 140.0, 30.0) == 0);
    CHECK(listen(&t, h, 20.0, 30.0) == 1);
    CHECK(t.now.feeling == FEEL_PAIN);
    thoughts_init(&t, 5);
    h->pain = 9.0f;
    CHECK(listen(&t, h, 1.0, 30.0) == 1);
    CHECK(listen(&t, h, 120.0, 30.0) >= 3);
    mem_free(h);
}

/* A rise right after a line waits, unless the feeling turns severe. */
static void test_rise_hold(void)
{
    health *h = new_body(15);
    thoughts t;
    thoughts_init(&t, 6);
    h->pain = 3.0f;
    CHECK(listen(&t, h, 0.1, 30.0) == 1);
    t.gap = 0.0f;
    h->pain = 6.0f;
    CHECK(listen(&t, h, 2.0, 30.0) == 0);
    h->pain = 9.0f;
    t.gap = 0.0f;
    CHECK(listen(&t, h, 0.1, 30.0) == 1);
    CHECK(t.now.level == 3);
    mem_free(h);
}

/* A strong feeling that clears gets a line of relief; a mild one doesn't. */
static void test_relief(void)
{
    health *h = new_body(16);
    thoughts t;
    thoughts_init(&t, 8);
    h->temp = 34.5f;
    CHECK(listen(&t, h, 0.1, 30.0) == 1);
    CHECK(t.now.feeling == FEEL_COLD && t.now.level == 2);
    h->temp = 37.0f;
    CHECK(listen(&t, h, 20.0, 30.0) == 1);
    CHECK(t.now.feeling == FEEL_COLD && t.now.level == 0);
    CHECK(listen(&t, h, 60.0, 30.0) == 0);

    thoughts_init(&t, 8);
    h->pain = 3.0f;
    CHECK(listen(&t, h, 0.1, 30.0) == 1);
    h->pain = 0.0f;
    CHECK(listen(&t, h, 60.0, 30.0) == 0);
    /* Feelings without a relief line (being out of breath) clear silently. */
    thoughts_init(&t, 8);
    h->wbal = 0.0f;
    CHECK(listen(&t, h, 0.1, 30.0) == 1);
    h->wbal = 60000.0f;
    CHECK(listen(&t, h, 60.0, 30.0) == 0);
    mem_free(h);
}

/* The out cold think nothing; coming round is the first thing said. */
static void test_unconscious_and_waking(void)
{
    health *h = new_body(17);
    thoughts t;
    thoughts_init(&t, 9);
    h->pain = 9.0f;
    CHECK(listen(&t, h, 0.1, 30.0) == 1);
    h->conscious = CONS_UNCONSCIOUS;
    CHECK(thoughts_level(h, FEEL_PAIN) == 0);
    CHECK(listen(&t, h, 60.0, 30.0) == 0);
    CHECK(thoughts_alpha(&t) == 0.0f);
    h->conscious = CONS_ALERT;
    CHECK(thoughts_step(&t, h, 0.03f) == 1);
    CHECK(t.now.feeling == FEEL_WAKE);
    /* The pain is fresh again after the gap. */
    CHECK(listen(&t, h, 12.0, 30.0) == 1);
    CHECK(t.now.feeling == FEEL_PAIN && t.now.level == 3);
    mem_free(h);
}

static void test_dead_say_nothing(void)
{
    health *h = new_body(18);
    thoughts t;
    thoughts_init(&t, 10);
    h->dead = 1;
    h->pain = 9.0f;
    h->hurt_flash = 1.0f;
    CHECK(listen(&t, h, 30.0, 30.0) == 0);
    for (int f = 0; f < FEEL_COUNT; f++) CHECK(thoughts_level(h, f) == 0);
    mem_free(h);
}

/* A blow gets a cry at once, even mid-sentence, but one per blow. */
static void test_ouch(void)
{
    health *h = new_body(19);
    thoughts t;
    thoughts_init(&t, 11);
    h->pain = 6.0f;
    CHECK(listen(&t, h, 0.1, 30.0) == 1);
    CHECK(t.gap > 0.0f);
    h->hurt_flash = 0.5f;
    CHECK(thoughts_step(&t, h, 0.03f) == 1);
    CHECK(t.now.feeling == FEEL_OUCH && t.now.level == 2);
    h->hurt_flash = 0.1f;
    CHECK(thoughts_step(&t, h, 0.03f) == 0);
    h->hurt_flash = 0.9f;
    CHECK(thoughts_step(&t, h, 0.03f) == 0); /* too soon after the last cry */
    CHECK(listen(&t, h, 1.6, 30.0) == 0);
    h->hurt_flash = 0.1f;
    thoughts_step(&t, h, 0.03f);
    h->hurt_flash = 0.8f;
    CHECK(thoughts_step(&t, h, 0.03f) == 1);
    CHECK(t.now.feeling == FEEL_OUCH && t.now.level == 3);
    mem_free(h);
}

static void test_alpha_fades(void)
{
    health *h = new_body(20);
    thoughts t;
    thoughts_init(&t, 12);
    h->pain = 6.0f;
    thoughts_step(&t, h, 0.0f);
    CHECK(thoughts_alpha(&t) == 0.0f);
    thoughts_step(&t, h, 0.1f);
    CHECK(thoughts_alpha(&t) > 0.3f && thoughts_alpha(&t) < 0.5f);
    thoughts_step(&t, h, 1.0f);
    CHECK(thoughts_alpha(&t) == 1.0f);
    CHECK(t.now.life > 2.5f && t.now.life < 8.0f);
    thoughts_step(&t, h, t.now.life - t.now.age - 0.4f);
    CHECK(thoughts_alpha(&t) > 0.4f && thoughts_alpha(&t) < 0.6f);
    thoughts_step(&t, h, 0.5f);
    CHECK(thoughts_alpha(&t) == 0.0f);
    mem_free(h);
}

/* A muddled mind trails off; the body's own sensations don't. */
static void test_confused_trails_off(void)
{
    health *h = new_body(21);
    int inner = 0;
    for (uint32_t seed = 1; seed < 40; seed++) {
        thoughts t;
        thoughts_init(&t, seed);
        h->conscious = CONS_CONFUSED;
        h->pain = 6.0f;
        thoughts_step(&t, h, 0.03f);
        size_t n = strlen(t.now.text);
        CHECK(n > 0);
        if (t.now.voice == VOICE_INNER && n >= 3 && t.now.text[n - 3] == '.') inner++;
        if (t.now.voice == VOICE_BODY) CHECK(n < 3 || strcmp(t.now.text + n - 3, "...") != 0);
    }
    CHECK(inner > 0);
    mem_free(h);
}

/* Every feeling has something to say at every level it can reach, and no
 * placeholder is left in what comes out. */
static void test_every_line_fills_in(void)
{
    health *h = new_body(22);
    h->part[BP_RARM].frost = FROST_NIP;
    for (int f = FEEL_PAIN; f < FEEL_COUNT; f++) {
        int top = f == FEEL_RUSH || f == FEEL_CRASH || f == FEEL_TIRED ? 2 : 3;
        for (int lv = 1; lv <= top; lv++) CHECK(thoughts_lines(f, lv) >= 1);
        CHECK(thoughts_lines(f, top + 1) == 0);
    }
    CHECK(thoughts_lines(FEEL_OUCH, 1) >= 2 && thoughts_lines(FEEL_WAKE, 1) >= 2);
    /* Relief where a strong feeling can clear, none where it can't. */
    CHECK(thoughts_lines(FEEL_PAIN, 0) >= 2 && thoughts_lines(FEEL_COLD, 0) >= 2);
    CHECK(thoughts_lines(FEEL_BURN, 0) == 0 && thoughts_lines(FEEL_TIRED, 0) == 0);
    CHECK(thoughts_lines(-1, 1) == 0 && thoughts_lines(FEEL_COUNT, 1) == 0);
    /* Through the real path: numbness names the fingers, pain the part. */
    thoughts t;
    for (uint32_t seed = 1; seed < 30; seed++) {
        thoughts_init(&t, seed);
        CHECK(thoughts_step(&t, h, 0.03f) == 1);
        CHECK(t.now.feeling == FEEL_NUMB);
        CHECK(!strchr(t.now.text, '{'));
        CHECK(!has_text(&t, "toes"));
    }
    h->part[BP_RARM].frost = FROST_NONE;
    h->part[BP_LLEG].frost = FROST_SUPERFICIAL;
    for (uint32_t seed = 1; seed < 30; seed++) {
        thoughts_init(&t, seed);
        thoughts_step(&t, h, 0.03f);
        CHECK(!has_text(&t, "fingers"));
    }
    mem_free(h);
}

/* The same seed says the same things; another seed words them differently
 * at least some of the time. */
static void test_seeded(void)
{
    health *h = new_body(23);
    h->pain = 9.0f;
    char first[THOUGHT_LEN] = {0};
    int differ = 0;
    for (uint32_t seed = 1; seed < 20; seed++) {
        thoughts a, b;
        thoughts_init(&a, seed);
        thoughts_init(&b, seed);
        thoughts_step(&a, h, 0.03f);
        thoughts_step(&b, h, 0.03f);
        CHECK(!strcmp(a.now.text, b.now.text));
        if (seed == 1) memcpy(first, a.now.text, sizeof first);
        else differ |= strcmp(first, a.now.text) != 0;
    }
    CHECK(differ);
    /* A zeroed struct seeds itself. */
    thoughts z;
    memset(&z, 0, sizeof z);
    CHECK(thoughts_step(&z, h, 0.03f) == 1);
    mem_free(h);
}

/* The same line is not picked twice running when there is another. */
static void test_no_immediate_repeat(void)
{
    health *h = new_body(24);
    thoughts t;
    thoughts_init(&t, 25);
    h->pain = 9.0f;
    char prev[THOUGHT_LEN] = {0};
    int said = 0;
    for (int i = 0; i < 400 && said < 12; i++) {
        if (thoughts_step(&t, h, 1.0f)) {
            if (said) CHECK(strcmp(prev, t.now.text) != 0);
            memcpy(prev, t.now.text, sizeof prev);
            said++;
        }
    }
    CHECK(said >= 10);
    mem_free(h);
}

/* What is said, and when, does not depend on the frame rate. */
typedef struct {
    int feeling, level;
    double at;
} said_line;

static int script(double fps, said_line *out, int max)
{
    health *h = new_body(26);
    thoughts t;
    thoughts_init(&t, 31);
    int n = 0;
    int steps = (int)(240.0 * fps);
    for (int i = 0; i < steps; i++) {
        double now = (double)i / fps;
        h->pain = now < 60.0 ? 3.0f : (now < 120.0 ? 8.0f : 0.0f);
        h->temp = now > 100.0 && now < 180.0 ? 35.0f : 37.0f;
        h->hurt_flash = now > 59.5 && now < 60.5 ? 0.6f : 0.0f;
        if (thoughts_step(&t, h, (float)(1.0 / fps)) && n < max) {
            out[n].feeling = t.now.feeling;
            out[n].level = t.now.level;
            out[n].at = now;
            n++;
        }
    }
    mem_free(h);
    return n;
}

static void test_frame_rate_independent(void)
{
    said_line a[32] = {0}, b[32] = {0};
    int na = script(30.0, a, 32), nb = script(144.0, b, 32);
    CHECK(na == nb);
    CHECK(na >= 5);
    for (int i = 0; i < na && i < nb; i++) {
        CHECK(a[i].feeling == b[i].feeling && a[i].level == b[i].level);
        CHECK(a[i].at - b[i].at < 0.1 && b[i].at - a[i].at < 0.1);
    }
}

/* A real wound in the full model: the voice notices the bleeding. */
static void test_real_bleed(void)
{
    health *h = new_body(27);
    health_env e = calm_env();
    thoughts t;
    thoughts_init(&t, 13);
    health_cut(h, BP_LLEG, 0.9f, 1, 0.0f);
    int bleeding = 0, ouch_or_pain = 0;
    for (int i = 0; i < 60 * 40; i++) {
        health_step(h, &e, 1.0 / 60.0);
        if (thoughts_step(&t, h, 1.0f / 60.0f)) {
            bleeding |= t.now.feeling == FEEL_BLEEDING;
            ouch_or_pain |= t.now.feeling == FEEL_PAIN || t.now.feeling == FEEL_OUCH;
        }
    }
    CHECK(bleeding);
    CHECK(ouch_or_pain);
    mem_free(h);
}

/* The setting, and the HUD stepping and hiding the voice. */
static void test_setting_and_hud(void)
{
    settings s = {0};
    settings_default(&s);
    CHECK(s.inner_voice == 1);
    CHECK(settings_parse(&s, "inner_voice 0\n") == 1);
    CHECK(s.inner_voice == 0);
    CHECK(settings_parse(&s, "inner_voice 2\n") == 0);
    char buf[2048] = {0};
    settings_format(&s, buf, (int)sizeof buf);
    CHECK(strstr(buf, "inner_voice 0") != NULL);

    health *h = new_body(28);
    hud_state *hs = mem_calloc(1, sizeof *hs);
    h->pain = 6.0f;
    hud_update(hs, h, 0.0f); /* paused in a menu: nothing */
    CHECK(hs->voice.now.text[0] == '\0');
    hud_update(hs, h, 0.1f);
    CHECK(hs->voice.now.feeling == FEEL_PAIN && hs->voice.now.text[0]);
    mem_free(hs);
    mem_free(h);
}

void test_thoughts_all(void)
{
    test_healthy_is_quiet();
    test_pain_levels();
    test_blood_levels();
    test_breath_levels();
    test_heart_levels();
    test_heat_levels();
    test_cold_skin_and_wet();
    test_numb_levels();
    test_inside_levels();
    test_needs_levels();
    test_adrenaline_levels();
    test_voices_a_new_feeling();
    test_gap_between_lines();
    test_reminders();
    test_rise_hold();
    test_relief();
    test_unconscious_and_waking();
    test_dead_say_nothing();
    test_ouch();
    test_alpha_fades();
    test_confused_trails_off();
    test_every_line_fills_in();
    test_seeded();
    test_no_immediate_repeat();
    test_frame_rate_independent();
    test_real_bleed();
    test_setting_and_hud();
}
