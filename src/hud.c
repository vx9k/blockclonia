#include "hud.h"
#include "debug.h"
#include "palette.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const uint32_t SEV[4] = {PAL_SEV0, PAL_SEV1, PAL_SEV2, PAL_SEV3};

static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

static int blink(const health *h, double hz) { return fmod(h->t * hz, 1.0) < 0.5; }

/* The heart glyph flashes on each beat. */
static uint32_t beat_color(const health *h)
{
    int beat = health_rhythm_perfusing(h->rhythm) && h->t - h->last_r < 0.12;
    return beat ? ui_rgba(255, 90, 90, 255) : ui_rgba(170, 40, 40, 255);
}

/* ------------------------------------------------------------ traces */

/* Monitor sweep: the ring buffer maps straight onto the trace width, the
 * newest sample sits at the cursor and a short gap ahead of it is blank,
 * like a bedside monitor erasing the previous sweep. Each column draws
 * the min..max of its samples joined to its neighbour, so narrow QRS
 * spikes survive the decimation. */
static void trace_sweep(ui *u, const float *buf, int pos, float x, float y, float w, float h, float lo, float hi,
                        uint32_t col)
{
    int cols = (int)w;
    if (cols < 2) return;
    const int n = HEALTH_WAVE_LEN;
    int cursor = (pos - 1 + n) % n;
    int gap = n / 24; /* 0.25 s */
    float prev = 0.0f;
    int have_prev = 0;
    for (int c = 0; c < cols; c++) {
        int k0 = (int)((long)c * n / cols), k1 = (int)((long)(c + 1) * n / cols);
        int ahead = (k0 - cursor + n) % n;
        if (ahead > 0 && ahead <= gap) {
            have_prev = 0;
            continue;
        }
        float mn = 1e9f, mx = -1e9f;
        for (int k = k0; k < k1; k++) {
            float v = buf[k];
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        if (have_prev) {
            if (prev < mn) mn = prev;
            if (prev > mx) mx = prev;
        }
        prev = buf[k1 > k0 ? k1 - 1 : k0];
        have_prev = 1;
        float t0 = clamp01((mx - lo) / (hi - lo)), t1 = clamp01((mn - lo) / (hi - lo));
        float y0 = y + h - t0 * h, y1 = y + h - t1 * h;
        ui_rect(u, x + (float)c, y0, 1.0f, y1 - y0 < 1.0f ? 1.0f : y1 - y0, col);
    }
}

/* Scrolling strip of the last `samples` samples, newest on the right. */
static void trace_scroll(ui *u, const float *buf, int pos, int samples, float x, float y, float w, float h, float lo,
                         float hi, uint32_t col)
{
    int cols = (int)w;
    if (cols < 2) return;
    const int n = HEALTH_WAVE_LEN;
    int start = (pos - samples + n) % n;
    float prev = 0.0f;
    for (int c = 0; c < cols; c++) {
        int a = c * samples / cols, b = (c + 1) * samples / cols;
        float mn = 1e9f, mx = -1e9f;
        for (int k = a; k < b; k++) {
            float v = buf[(start + k) % n];
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        if (c) {
            if (prev < mn) mn = prev;
            if (prev > mx) mx = prev;
        }
        prev = buf[(start + b - 1) % n];
        float t0 = clamp01((mx - lo) / (hi - lo)), t1 = clamp01((mn - lo) / (hi - lo));
        float y0 = y + h - t0 * h, y1 = y + h - t1 * h;
        ui_rect(u, x + (float)c, y0, 1.0f, y1 - y0 < 1.0f ? 1.0f : y1 - y0, col);
    }
}

/* --------------------------------------------------------- effects */

static void vignette(ui *u, float strength, uint32_t rgb)
{
    if (strength <= 0.01f) return;
    float e = (0.12f + 0.28f * strength) * (u->h < u->w ? u->h : u->w);
    uint32_t c = ui_alpha(rgb | 0xff000000u, 0.9f * strength), z = ui_alpha(rgb, 0.0f);
    ui_rect4(u, 0, 0, u->w, e, c, c, z, z);
    ui_rect4(u, 0, u->h - e, u->w, e, z, z, c, c);
    ui_rect4(u, 0, 0, e, u->h, c, z, z, c);
    ui_rect4(u, u->w - e, 0, e, u->h, z, c, c, z);
}

static void effects(ui *u, const health *h)
{
    if (h->dead) return;
    if (h->conscious == CONS_UNCONSCIOUS) {
        ui_rect(u, 0, 0, u->w, u->h, ui_rgba(0, 0, 0, 240));
        const char *t = "You are unconscious";
        ui_text(u, (u->w - ui_text_width(t, 1)) * 0.5f, u->h * 0.45f, 1, ui_rgba(120, 120, 130, 255), t);
        return;
    }
    /* Failing brain oxygen narrows vision to a tunnel before a faint. */
    float dim = clamp01((0.85f - h->brain_o2) / 0.35f);
    if (h->conscious == CONS_CONFUSED) dim = dim > 0.35f ? dim : 0.35f;
    vignette(u, dim, 0);
    if (h->hurt_flash > 0.0f) vignette(u, 0.6f * h->hurt_flash, ui_rgba(200, 0, 0, 0));
}

/* ------------------------------------------------------------- HUD */

typedef struct {
    char text[64];
    uint32_t key;
    int sev;
} alert;

static const char *side_name(int part) { return part == BP_LARM || part == BP_LLEG ? "left" : "right"; }

typedef struct {
    alert *a;
    int n, max;
} alert_list;

static void add_alert(alert_list *l, uint32_t key, int sev, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;

static void add_alert(alert_list *l, uint32_t key, int sev, const char *fmt, ...)
{
    if (l->n >= l->max) return;
    alert *al = &l->a[l->n++];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(al->text, sizeof al->text, fmt, ap);
    va_end(ap);
    al->key = key;
    al->sev = sev;
}

/* Keys stay the same while an alert's text changes (bleeding rate,
 * degrees), so the animation can follow it; the line number makes them
 * unique. */
#define KEY ((uint32_t)__LINE__ * 16u)

/* What the defibrillator is doing, as its voice prompts put it. */
static int defib_prompt(const health *h, char *buf, size_t n)
{
    const defib_state *d = &h->defib;
    int arrest = !health_rhythm_perfusing(h->rhythm);
    switch (d->phase) {
    case DEFIB_ANALYSE: snprintf(buf, n, "Defibrillator: analysing rhythm"); return arrest ? 3 : 1;
    case DEFIB_CHARGE: snprintf(buf, n, "Defibrillator: shock advised, charging %.0f J", (double)d->joules); return 3;
    case DEFIB_CLEAR: snprintf(buf, n, "Defibrillator: stand clear, shock in %.0f", ceil((double)d->timer)); return 3;
    default: break;
    }
    if (d->shocks && d->since_shock < 5.0f) snprintf(buf, n, "Defibrillator: shock %d delivered", d->shocks);
    else if (arrest) snprintf(buf, n, "Defibrillator: no shock advised");
    else snprintf(buf, n, "Defibrillator pads on");
    return arrest || d->since_shock < 5.0f ? 3 : 0;
}

/* Heart, breathing, blood, fire and weight on the body. */
static void alerts_vital(const health *h, alert_list *l)
{
    float bleed = h->bleed_ext + h->bleed_int;
    int arrest = !health_rhythm_perfusing(h->rhythm);
    if (h->rhythm == RHYTHM_ASYSTOLE) add_alert(l, KEY, 3, "CARDIAC ARREST");
    else if (arrest) add_alert(l, KEY, 3, "CARDIAC ARREST (%s)", health_rhythm_short(h->rhythm));
    else if (h->rhythm == RHYTHM_VT) add_alert(l, KEY, 3, "Racing heartbeat (VT)");
    else if (h->rhythm == RHYTHM_AVB3) add_alert(l, KEY, 3, "Slow heartbeat (heart block)");
    else if (h->rhythm == RHYTHM_AF) add_alert(l, KEY, 2, "Irregular heartbeat (AF)");
    if (h->defib.phase != DEFIB_OFF) {
        char t[64];
        int sev = defib_prompt(h, t, sizeof t);
        add_alert(l, KEY, sev, "%s", t);
    }
    if (h->in_fire) add_alert(l, KEY, 3, "ON FIRE");
    float pinned = 0.0f;
    for (int i = 0; i < BP_COUNT; i++) pinned += h->part[i].crush_load;
    if (pinned > 0.0f) add_alert(l, KEY, 3, "Crushed: pinned under %.0f kg", (double)pinned);
    if (bleed >= 5.0f) add_alert(l, KEY, bleed > 30.0f ? 3 : 2, "Bleeding %.0f mL/min", (double)bleed);
    if (!h->breathing && h->conscious != CONS_UNCONSCIOUS && !arrest)
        add_alert(l, KEY, h->sao2 < 0.85f || h->lung_water > 0.02f ? 3 : 1, "%s",
                  h->lung_water > 0.02f ? "Drowning" : (pinned > 0.0f ? "Can't breathe" : "Holding breath"));
    if (h->sao2 < 0.9f && !arrest) add_alert(l, KEY, h->sao2 < 0.8f ? 3 : 2, "Low oxygen");
    /* A crisis by the ACC/AHA line (180/120); dizziness when the systolic
     * no longer keeps the head perfused; angina from an ischaemic heart. */
    if (!arrest && (h->sbp >= 180.0f || h->dbp >= 120.0f))
        add_alert(l, KEY, 3, "Hypertensive crisis %.0f/%.0f", (double)h->sbp, (double)h->dbp);
    else if (!arrest && h->sbp < 90.0f && h->conscious != CONS_UNCONSCIOUS)
        add_alert(l, KEY, h->sbp < 75.0f ? 3 : 2, "Dizzy: low blood pressure");
    if (!arrest && h->ischemia > 1.0f && h->conscious != CONS_UNCONSCIOUS) add_alert(l, KEY, 3, "Chest pain");
}

/* Burns, broken bones, joints, frostbite and crushed muscle. */
static void alerts_injury(const health *h, alert_list *l)
{
    int burn = 0;
    float burned = 0.0f;
    for (int i = 0; i < BP_COUNT; i++) {
        const body_part *p = &h->part[i];
        if (p->burn > burn) burn = p->burn;
        if (p->burn) burned += p->burn_area * health_part_area(i) * 100.0f;
    }
    if (burn)
        add_alert(l, KEY, burn >= 2 && burned > 15.0f ? 3 : burn, "Burned: %s degree, %.0f%% of body",
                  burn == 1 ? "1st" : (burn == 2 ? "2nd" : "3rd"), (double)(burned < 1.0f ? 1.0f : burned));
    if (h->radiant > 2500.0f && !h->in_fire) add_alert(l, KEY, 2, "Scorching heat");
    for (int i = 0; i < BP_COUNT; i++)
        if (h->part[i].fracture)
            add_alert(l, KEY + (uint32_t)i, 2, "Broken %s%s", health_bone_name(i),
                      h->part[i].splinted ? " (splinted)" : "");
    for (int i = BP_LARM; i <= BP_RARM; i++)
        if (h->part[i].dislocated)
            add_alert(l, KEY + (uint32_t)i, 2, "Dislocated %s %s", side_name(i),
                      h->part[i].dislocated == DISLOC_ELBOW ? "elbow" : "shoulder");
    int frost = 0, frost_part = 0;
    for (int i = 0; i < BP_COUNT; i++)
        if (h->part[i].frost > frost) {
            frost = h->part[i].frost;
            frost_part = i;
        }
    if (frost)
        add_alert(l, KEY, frost, "%s: %s",
                  frost == FROST_NIP ? "Frostnip" : (frost == FROST_DEEP ? "Deep frostbite" : "Frostbite"),
                  health_part_name(frost_part));
    if (h->myoglobin > 5.0f || h->potassium > 6.0f) {
        add_alert(l, KEY, 3, "Crush syndrome: drink");
        return;
    }
    for (int i = 0; i < BP_COUNT; i++)
        if (h->part[i].crush > 0.3f && h->part[i].crush_load <= 0.0f) {
            add_alert(l, KEY, 2, "Crushed %s", health_part_name(i));
            break;
        }
}

/* Water, food, temperature, infection, pain and fatigue. */
static void alerts_needs(const health *h, alert_list *l)
{
    float hyd = health_hydration(h), hun = health_hunger(h);
    if (hyd < 0.35f) add_alert(l, KEY, hyd < 0.15f ? 3 : 1, "%s", hyd < 0.15f ? "Severely dehydrated" : "Thirsty");
    if (hun > 0.7f) add_alert(l, KEY, hun > 0.95f ? 3 : 1, "%s", hun > 0.95f ? "Starving" : "Hungry");
    if (h->temp < 35.0f)
        add_alert(l, KEY, h->temp < 32.0f ? 3 : 2, "Hypothermia %.1f" UI_CH_DEGREE "C", (double)h->temp);
    else if (h->temp > 38.3f)
        add_alert(l, KEY, h->temp > 40.0f ? 3 : 1, "%s %.1f" UI_CH_DEGREE "C",
                  h->sepsis > 0.1f ? "Fever" : "Overheating", (double)h->temp);
    float cold = 99.0f;
    for (int i = 0; i < BP_COUNT; i++) cold = h->part[i].skin < cold ? h->part[i].skin : cold;
    if (h->wet > 0.5f)
        add_alert(l, KEY, h->air_temp < 10.0f ? 2 : 1, "%s", h->air_temp < 10.0f ? "Soaked and freezing" : "Soaked");
    else if (cold < 8.0f || (h->air_temp < 5.0f && health_skin_mean(h) < 26.0f))
        add_alert(l, KEY, cold < 2.0f ? 2 : 1, "Freezing");
    float inf = 0.0f;
    for (int i = 0; i < h->wound_count; i++) inf = inf > h->wounds[i].infection ? inf : h->wounds[i].infection;
    if (h->sepsis > 0.2f) add_alert(l, KEY, 3, "Sepsis");
    else if (inf > 0.3f) add_alert(l, KEY, 2, "Infected wound");
    if (h->pain > 6.0f) add_alert(l, KEY, 2, "Severe pain");
    if (health_stamina(h) < 0.1f) add_alert(l, KEY, 1, "Exhausted");
    if (h->conscious == CONS_CONFUSED) add_alert(l, KEY, 2, "Confused");
}
#undef KEY

/* The conditions worth a line on the HUD, most urgent first. */
static int collect_alerts(const health *h, alert *a, int max)
{
    alert_list l = {a, 0, max};
    alerts_vital(h, &l);
    alerts_injury(h, &l);
    alerts_needs(h, &l);
    return l.n;
}

static float approach(float cur, float target, float dt, float tau)
{ return cur + (target - cur) * (1.0f - expf(-dt / tau)); }

void hud_update(hud_state *s, const health *h, float dt)
{
    alert cur[HUD_ALERTS];
    int n = h->dead ? 0 : collect_alerts(h, cur, s->debug ? 3 : 6);
    hud_alert next[HUD_ALERTS];
    int m = 0;
    /* Current conditions in order, carrying over the state of ones already
     * shown; a new one starts off-screen at its row. */
    for (int i = 0; i < n; i++) {
        hud_alert a = {.slide = 1.0f, .alpha = 1.0f, .row = (float)i};
        for (int k = 0; k < s->alert_count; k++)
            if (s->alerts[k].key == cur[i].key) a = s->alerts[k];
        memcpy(a.text, cur[i].text, sizeof a.text);
        a.key = cur[i].key;
        a.sev = cur[i].sev;
        a.live = 1;
        next[m++] = a;
    }
    /* Cleared conditions stay below while they fade. */
    for (int k = 0; k < s->alert_count && m < HUD_ALERTS; k++) {
        int found = 0;
        for (int i = 0; i < n; i++) found |= s->alerts[k].key == cur[i].key;
        if (found) continue;
        next[m] = s->alerts[k];
        next[m++].live = 0;
    }
    s->alert_count = 0;
    for (int k = 0; k < m; k++) {
        hud_alert *a = &next[k];
        a->row = approach(a->row, (float)k, dt, 0.1f);
        if (a->live) {
            a->slide = approach(a->slide, 0.0f, dt, 0.09f);
            if (a->slide < 0.002f) a->slide = 0.0f;
            a->alpha = approach(a->alpha, 1.0f, dt, 0.05f);
        } else {
            a->alpha -= dt * 2.0f; /* half a second */
        }
        if (a->live || a->alpha > 0.0f) s->alerts[s->alert_count++] = *a;
    }
    s->animated = 1;
}

/* Heart rate, pressure, SpO2, a live ECG strip, the four needs bars, and
 * the air around the body with how wet the clothes are. */
static void vitals(ui *u, const health *h, float x, float y)
{
    const float w = 152;
    ui_rect(u, x, y, w, 56, ui_rgba(0, 0, 0, 150));
    int arrest = !health_rhythm_perfusing(h->rhythm);
    ui_text(u, x + 3, y + 3, 1, beat_color(h), UI_CH_HEART);
    uint32_t hrc = arrest || h->hr > 140.0f || h->hr < 45.0f ? (blink(h, 2) ? C_ART : C_TEXT) : C_ECG;
    ui_textf(u, x + 11, y + 3, 1, hrc, "%3.0f", arrest ? 0.0 : (double)h->hr);
    ui_textf(u, x + 34, y + 3, 1, h->sbp < 90.0f ? C_ART : C_TEXT, "BP %3.0f/%-3.0f", (double)h->sbp, (double)h->dbp);
    /* The right-hand readouts end at the box's edge whatever their width. */
    char b[24];
    int sp = health_spo2_reading(h);
    if (sp < 0) snprintf(b, sizeof b, "SpO2 --");
    else snprintf(b, sizeof b, "SpO2 %d%%", sp);
    ui_text(u, x + w - 3 - ui_text_width(b, 1), y + 3, 1, sp < 0 ? C_DIM : (sp < 90 ? C_ART : C_PLETH), b);
    ui_rect(u, x + 3, y + 13, w - 6, 13, ui_rgba(0, 20, 8, 200));
    trace_scroll(u, h->ecg, h->wave_pos, HEALTH_WAVE_LEN / 2, x + 4, y + 14, w - 8, 11, -0.6f, 1.6f, C_ECG);

    /* Needs: blood, water, food, stamina. */
    struct {
        const char *l;
        float v;
        uint32_t c;
    } bars[4] = {
        {UI_CH_DROP, h->blood / BLOOD_NORMAL, ui_rgba(210, 40, 40, 255)},
        {"W", health_hydration(h), ui_rgba(60, 140, 240, 255)},
        {"F", 1.0f - health_hunger(h), ui_rgba(235, 150, 50, 255)},
        {"S", health_stamina(h), ui_rgba(235, 215, 60, 255)},
    };
    for (int i = 0; i < 4; i++) {
        float bx = x + 3 + (float)i * 37;
        ui_text(u, bx, y + 30, 1, bars[i].c, bars[i].l);
        float v = i == 0 ? clamp01((bars[i].v - 0.5f) / 0.5f) : bars[i].v; /* blood: 50% is fatal */
        ui_bar(u, bx + 7, y + 31, 27, 5, v, bars[i].c, C_BACK);
    }

    /* The air (blue when freezing, orange near a fire) and the clothes. */
    float ta = h->air_temp;
    uint32_t tc = h->radiant > 1000.0f ? ui_rgba(255, 150, 60, 255)
                                       : (ta < 0.0f ? ui_rgba(150, 200, 255, 255) : (ta > 30.0f ? SEV[1] : C_TEXT));
    ui_textf(u, x + 3, y + 43, 1, tc, "Air %.0f" UI_CH_DEGREE "C", (double)ta);
    if (h->radiant > 1000.0f) ui_textf(u, x + 58, y + 43, 1, tc, "+%.1fkW", (double)(h->radiant / 1000.0f));
    if (h->wet >= 0.05f) snprintf(b, sizeof b, "Wet %d%%", (int)lroundf(h->wet * 100.0f));
    else snprintf(b, sizeof b, "Dry");
    ui_text(u, x + w - 3 - ui_text_width(b, 1), y + 43, 1, h->wet >= 0.05f ? ui_rgba(90, 160, 255, 255) : C_DIM, b);
}

static void alert_line(ui *u, float x, float y, const char *text, int sev, float alpha, const health *h)
{
    uint32_t c = SEV[sev];
    if (sev == 3 && !blink(h, 1.5)) c = C_TEXT;
    ui_rect(u, x, y - 1, ui_text_width(text, 1) + 6, 10, ui_rgba(0, 0, 0, (int)(175.0f * alpha)));
    ui_rect(u, x, y - 1, 2, 10, ui_alpha(SEV[sev], alpha)); /* severity tab */
    ui_text(u, x + 4, y, 1, ui_alpha(c, alpha), text);
}

static void hud(ui *u, const health *h, const hud_state *s)
{
    /* With the F3 overlay up, the vitals move below its left column. */
    const float x = 4, y = s->debug ? DEBUG_LEFT_BOTTOM : 4;
    vitals(u, h, x, y);
    float ay = y + 60;
    if (s->animated) {
        /* Slide in with an ease-out, fade when cleared. */
        float rows = 0.0f;
        for (int i = 0; i < s->alert_count; i++) {
            const hud_alert *a = &s->alerts[i];
            float w = ui_text_width(a->text, 1) + 12;
            float e = a->slide * a->slide;
            alert_line(u, x - w * e, ay + a->row * 10.0f, a->text, a->sev, clamp01(a->alpha) * (1.0f - 0.6f * e), h);
            if (a->row + 1.0f > rows) rows = a->row + 1.0f;
        }
        ay += floorf(rows * 10.0f + 0.5f);
    } else {
        alert al[8];
        int n = collect_alerts(h, al, s->debug ? 3 : 6);
        for (int i = 0; i < n; i++) {
            alert_line(u, x, ay, al[i].text, al[i].sev, 1.0f, h);
            ay += 10;
        }
    }
    /* Fresh events fade out. */
    for (int i = HEALTH_LOG - 1; i >= 0; i--) {
        float age = h->log_age[i];
        if (!h->log[i][0] || age > 6.0f) continue;
        float a = age < 5.0f ? 1.0f : 6.0f - age;
        ui_rect(u, x, ay - 1, ui_text_width(h->log[i], 1) + 6, 10, ui_rgba(0, 0, 0, (int)(110.0f * a)));
        ui_text(u, x + 3, ay, 1, ui_alpha(C_HEAD, a), h->log[i]);
        ay += 10;
    }
    if (s->msg[0] && s->msg_age < 4.0f) {
        ui_text(u, (u->w - ui_text_width(s->msg, 1)) * 0.5f, u->h * 0.62f, 1,
                ui_alpha(C_HEAD, clamp01(4.0f - s->msg_age)), s->msg);
    }

    /* The hotbar owns the bottom 28 px. */
    float by = u->h - 14;
    if (s->held) ui_text(u, (u->w - ui_text_width(s->held, 1)) * 0.5f, by - 30, 1, C_TEXT, s->held);
    if (s->flying) {
        const char *t = "Flying: no injuries, instant building";
        ui_text_shadow(u, (u->w - ui_text_width(t, 1)) * 0.5f, by - 42, 1, C_DIM, t);
    }
    if (!s->hide_hints) {
        const char *hint = "H: health   Tab: inventory";
        ui_text_shadow(u, u->w - ui_text_width(hint, 1) - 4, by, 1, ui_alpha(C_DIM, 0.8f), hint);
    }
}

/* ----------------------------------------------------------- panel */

typedef struct {
    float x, y, w, h;
} rectf;

static int is_limb_part(int part) { return part >= BP_LARM; }

/* Back view, so the player's left is on the left. */
static rectf part_rect(int part, float bx, float by)
{
    switch (part) {
    case BP_HEAD: return (rectf){bx + 17, by, 16, 16};
    case BP_CHEST: return (rectf){bx + 12, by + 19, 26, 30};
    case BP_ABDOMEN: return (rectf){bx + 12, by + 51, 26, 22};
    case BP_LARM: return (rectf){bx + 1, by + 19, 9, 56};
    case BP_RARM: return (rectf){bx + 40, by + 19, 9, 56};
    case BP_LLEG: return (rectf){bx + 12, by + 75, 12, 72};
    default: return (rectf){bx + 26, by + 75, 12, 72};
    }
}

static void body_diagram(ui *u, const health *h, int sel, float bx, float by)
{
    char buf[160];
    for (int p = 0; p < BP_COUNT; p++) {
        rectf r = part_rect(p, bx, by);
        int sev = health_part_status(h, p, buf, sizeof buf);
        const body_part *bp = &h->part[p];
        uint32_t c = SEV[sev];
        if (bp->dislocated) {
            /* Out of its socket: the arm hangs low and away, leaving a gap. */
            ui_rect(u, p == BP_LARM ? r.x + r.w - 2 : r.x, r.y, 2, 6, ui_rgba(255, 90, 90, 220));
            r.x += p == BP_LARM ? -3.0f : 3.0f;
            r.y += 5.0f;
        }
        ui_rect(u, r.x, r.y, r.w, r.h, ui_alpha(c, 0.35f + 0.4f * (1.0f - bp->integrity)));
        ui_frame(u, r.x, r.y, r.w, r.h, 1, ui_alpha(c, 0.9f));
        if (bp->internal > 1.0f) ui_rect(u, r.x + 3, r.y + r.h * 0.35f, r.w - 6, r.h * 0.3f, ui_rgba(120, 0, 0, 200));
        if (bp->burn) {
            /* Orange over the burned share, from below (flames climb); a
             * charred core for full thickness. */
            float bh = floorf(r.h * (0.25f + 0.75f * bp->burn_area));
            ui_rect(u, r.x + 1, r.y + r.h - 1 - bh, r.w - 2, bh, ui_rgba(255, 130, 30, 90 + 50 * bp->burn));
            if (bp->burn == 3)
                ui_rect(u, r.x + 3, r.y + r.h - 1 - bh * 0.6f, r.w - 6, bh * 0.4f, ui_rgba(50, 25, 15, 230));
        }
        if (bp->frost) {
            /* Pale blue at the fingers, toes or face. */
            float fh = floorf(r.h * (is_limb_part(p) ? 0.22f : 0.3f));
            float fy = is_limb_part(p) ? r.y + r.h - 1 - fh : r.y + 1;
            ui_rect(u, r.x + 1, fy, r.w - 2, fh, ui_rgba(190, 225, 255, 110 + 45 * bp->frost));
        }
        if (bp->crush_load > 0.0f || bp->crush > 0.05f) {
            int pinned = bp->crush_load > 0.0f;
            uint32_t col = ui_rgba(120, 90, 160, pinned ? 240 : 170);
            ui_rect(u, r.x - 1, r.y + r.h * 0.4f, r.w + 2, pinned ? 6.0f : 3.0f, col);
        }
        if (bp->fracture) {
            /* A jagged break line across the middle. */
            float my = r.y + r.h * 0.5f, x0 = r.x + 1, x1 = r.x + r.w - 1, st = (x1 - x0) / 4;
            for (int k = 0; k < 4; k++)
                ui_line(u, x0 + st * (float)k, my + ((k & 1) ? 2.0f : -2.0f), x0 + st * (float)(k + 1),
                        my + ((k & 1) ? -2.0f : 2.0f), 1, bp->fracture == FX_OPEN ? C_ART : C_HEAD);
        }
        if (bp->splinted) {
            ui_rect(u, r.x - 2, r.y + 4, 2, r.h - 8, ui_rgba(150, 100, 50, 255));
            ui_rect(u, r.x + r.w, r.y + 4, 2, r.h - 8, ui_rgba(150, 100, 50, 255));
        }
        float bleed = 0.0f, inf = 0.0f;
        int bandaged = 0, grazed = 0;
        for (int i = 0; i < h->wound_count; i++) {
            const wound *w = &h->wounds[i];
            if (w->part != p) continue;
            bleed += w->bleed;
            inf = inf > w->infection ? inf : w->infection;
            bandaged |= w->bandage != BANDAGE_NONE;
            grazed |= w->kind == WOUND_ABRASION && w->closure < 0.8f;
        }
        if (grazed) /* road rash: raw scratches */
            for (int k = 0; k < 3; k++)
                ui_rect(u, r.x + 2, r.y + r.h * 0.72f + (float)k * 3, r.w - 4, 1, ui_rgba(200, 70, 60, 200));
        if (bandaged) ui_rect(u, r.x, r.y + r.h * 0.62f, r.w, 3, ui_rgba(235, 235, 225, 230));
        if (inf > 0.1f)
            for (int k = 0; k < 3; k++)
                ui_rect(u, r.x + 2 + (float)k * (r.w - 5) / 2, r.y + r.h * 0.25f + (float)(k & 1) * 3, 2, 2,
                        ui_rgba(170, 200, 40, (int)(120 + 135 * inf)));
        if (bleed >= 1.0f) {
            /* Drops on the outer side, clear of the neighbouring part. */
            int drops = bleed > 100.0f ? 3 : (bleed > 20.0f ? 2 : 1);
            float dx = p == BP_LARM || p == BP_LLEG ? r.x - 7 : r.x + r.w + 2;
            for (int k = 0; k < drops; k++) ui_text(u, dx, r.y + 2 + (float)k * 8, 1, C_ART, UI_CH_DROP);
        }
        const char num[2] = {(char)('1' + p), 0};
        ui_text(u, r.x + (r.w - 5) * 0.5f, r.y + 2, 1, ui_rgba(10, 10, 10, 220), num);
        if (p == sel)
            ui_frame(u, r.x - 1, r.y - 1, r.w + 2, r.h + 2, 1, blink(h, 2) ? C_HEAD : ui_rgba(255, 255, 255, 120));
    }
}

static void labelled_bar(ui *u, float x, float y, const char *label, float frac, uint32_t col, const char *value)
{
    ui_text(u, x, y, 1, C_DIM, label);
    ui_bar(u, x + 58, y + 1, 46, 5, frac, col, C_BACK);
    ui_text(u, x + 108, y, 1, C_TEXT, value);
}

static void monitor(ui *u, const health *h, float x, float y, float w)
{
    const float row = 46, tw = w - 92; /* 84 px of numbers fits "200/120" at size 2 */
    ui_rect(u, x, y, w, row * 4 + 4, ui_rgba(0, 0, 0, 255));
    ui_frame(u, x, y, w, row * 4 + 4, 1, C_BORDER);
    int arrest = !health_rhythm_perfusing(h->rhythm);
    char b[48];

    /* ECG, lead II. */
    float ry = y + 2;
    /* Through the defibrillator's pads, as a monitor labels that lead. */
    ui_text(u, x + 3, ry + 1, 1, ui_alpha(C_ECG, 0.7f), h->defib.phase != DEFIB_OFF ? "Pads" : "II");
    trace_sweep(u, h->ecg, h->wave_pos, x + 3, ry + 9, tw, row - 12, -0.6f, 1.6f, C_ECG);
    int hr_alarm = arrest || h->hr > 130.0f || h->hr < 45.0f;
    uint32_t c = hr_alarm && blink(h, 2) ? C_ART : C_ECG;
    ui_text(u, x + tw + 8, ry + 1, 1, ui_alpha(C_ECG, 0.7f), "HR");
    ui_text(u, x + tw + 22, ry + 1, 1, beat_color(h), UI_CH_HEART);
    snprintf(b, sizeof b, "%.0f", arrest ? 0.0 : (double)h->hr);
    ui_text(u, x + tw + 8, ry + 12, 3, c, b);
    if (h->rhythm != RHYTHM_SINUS)
        ui_text(u, x + tw + 8, ry + 38, 1, arrest ? C_ART : C_CO2, health_rhythm_short(h->rhythm));
    else if (h->last_beat_pvc) ui_text(u, x + tw + 8, ry + 38, 1, C_CO2, "PVC");

    /* Invasive arterial pressure, scaled to the systolic like a monitor. */
    ry += row;
    float top = h->sbp > 150.0f ? 40.0f * ceilf((h->sbp + 20.0f) / 40.0f) : 160.0f;
    ui_textf(u, x + 3, ry + 1, 1, ui_alpha(C_ART, 0.7f), "ART 0-%.0f", (double)top);
    trace_sweep(u, h->art, h->wave_pos, x + 3, ry + 9, tw, row - 12, 0.0f, top, C_ART);
    uint32_t ca = (h->sbp < 90.0f || h->sbp > 180.0f) && blink(h, 2) ? C_TEXT : C_ART;
    snprintf(b, sizeof b, "%.0f/%.0f", (double)h->sbp, (double)h->dbp);
    ui_text(u, x + tw + 8, ry + 12, 2, ca, b);
    ui_textf(u, x + tw + 8, ry + 32, 1, C_ART, "(%.0f)", (double)h->map);

    /* Pulse oximetry. */
    ry += row;
    ui_text(u, x + 3, ry + 1, 1, ui_alpha(C_PLETH, 0.7f), "Pleth");
    trace_sweep(u, h->pleth, h->wave_pos, x + 3, ry + 9, tw, row - 12, -0.05f, 1.1f, C_PLETH);
    ui_text(u, x + tw + 8, ry + 1, 1, ui_alpha(C_PLETH, 0.7f), "SpO2");
    int sp = health_spo2_reading(h);
    if (sp < 0) {
        ui_text(u, x + tw + 8, ry + 12, 3, C_PLETH, "--");
        ui_text(u, x + tw + 8, ry + 38, 1, C_PLETH, "no pulse");
    } else {
        snprintf(b, sizeof b, "%d", sp);
        ui_text(u, x + tw + 8, ry + 12, 3, sp < 90 && blink(h, 2) ? C_ART : C_PLETH, b);
    }

    /* Capnography. */
    ry += row;
    ui_text(u, x + 3, ry + 1, 1, ui_alpha(C_CO2, 0.7f), "CO2");
    trace_sweep(u, h->capno, h->wave_pos, x + 3, ry + 9, tw, row - 12, 0.0f, 50.0f, C_CO2);
    ui_text(u, x + tw + 8, ry + 1, 1, ui_alpha(C_CO2, 0.7f), "EtCO2");
    snprintf(b, sizeof b, "%.0f", (double)h->etco2);
    ui_text(u, x + tw + 8, ry + 12, 2, C_CO2, b);
    ui_textf(u, x + tw + 8, ry + 32, 1, C_CO2, "RR %.0f", (double)h->rr);
}

/* What would help the selected part most, as a key hint. */
static int advice(const health *h, int part, char *buf, size_t n)
{
    const body_part *p = &h->part[part];
    float bleed = 0.0f, inf = 0.0f, grit = 0.0f;
    int wounds = 0, dressed = 1, arterial = 0, burn_open = 0;
    for (int i = 0; i < h->wound_count; i++) {
        const wound *w = &h->wounds[i];
        if (w->part != part) continue;
        wounds++;
        bleed += w->bleed;
        inf = inf > w->infection ? inf : w->infection;
        dressed &= w->bandage != BANDAGE_NONE;
        arterial |= w->arterial && w->bleed > 20.0f;
        burn_open |= w->kind == WOUND_BURN && w->bandage == BANDAGE_NONE;
        if (w->kind == WOUND_ABRASION && w->contamination > grit) grit = w->contamination;
    }
    int fresh_burn = (p->burn || p->surface > 5.0f) && !p->burn_cooled && p->burn_age < 3.0f && p->cool_time <= 0.0f;
    if (arterial) snprintf(buf, n, "B: pressure dressing now. An artery will not clot by itself; keep still.");
    else if (wounds && !dressed && bleed > 1.0f) snprintf(buf, n, "B: dress the wound to stop the bleeding.");
    else if (p->crush_load > 0.0f)
        snprintf(buf, n, "Pinned: get the weight off. Freed crushed muscle poisons the kidneys: drink.");
    else if (fresh_burn) snprintf(buf, n, "C: cool the burn under water now: early, it stops it deepening.");
    else if (p->dislocated && !p->fracture)
        snprintf(buf, n, "X: pull the joint back in. It hurts; a painkiller first makes it easier.");
    else if (burn_open) snprintf(buf, n, "B: dress the burn: it leaks plasma and gets infected. Drink to make up.");
    else if (p->frost == FROST_NIP) snprintf(buf, n, "Frostnip: warm it by a fire now and it recovers.");
    else if (p->frost) snprintf(buf, n, "Frostbite: rewarm gently by a fire. Don't rub it or let it refreeze.");
    else if (p->fracture && !p->splinted && part != BP_HEAD && part != BP_CHEST)
        snprintf(buf, n, "S: splint it (a splint, or 2 sticks + 1 fibre). Walking on it bleeds inside.");
    else if (inf > 0.3f) snprintf(buf, n, "A: antibiotics. Too deep for antiseptic now.");
    else if (inf > 0.05f) snprintf(buf, n, "D: clean the wound (antiseptic, or water nearby).");
    else if (grit > 0.3f) snprintf(buf, n, "D: scrub the grit out of the graze (antiseptic, or water).");
    else if (p->crush > 0.1f || h->myoglobin > 2.0f)
        snprintf(buf, n, "Crushed muscle: drink plenty to spare the kidneys.");
    else if (p->skin < 12.0f) snprintf(buf, n, "Numb with cold: get out of the wind, warm up by a fire.");
    else if (p->internal > 5.0f) snprintf(buf, n, "Internal bleeding: rest, keep warm, drink.");
    else if (part == BP_HEAD && (h->concussion > 0.0f || h->confusion > 0.0f)) snprintf(buf, n, "Concussed: rest.");
    else if (h->pain > 5.0f) snprintf(buf, n, "P: a painkiller eases pain for 6 h.");
    else return 0;
    return 1;
}

static void panel(ui *u, const health *h, const hud_state *s)
{
    ui_rect(u, 0, 0, u->w, u->h, ui_rgba(0, 0, 0, 140));
    const float pw = 628, ph = 348;
    /* Drop the GUI scale until the fixed-size panel fits whole: a high
     * override (or a small window) otherwise leaves logical pixels short
     * and runs the panel off the edge. Restored below, since the cursor
     * and anything drawn after read u->scale at its usual value. */
    const float saved_scale = u->scale, saved_w = u->w, saved_h = u->h;
    ui_fit(u, pw + 8, ph + 8);
    const float x0 = floorf((u->w - pw) * 0.5f), y0 = floorf((u->h - ph) * 0.5f);
    ui_rect(u, x0, y0, pw, ph, C_PANEL);
    ui_frame(u, x0, y0, pw, ph, 1, C_BORDER);
    char buf[200];

    ui_text(u, x0 + 8, y0 + 6, 1, C_HEAD, "HEALTH");
    int mins = (int)(h->t / 60.0);
    const char *cons = h->conscious == CONS_ALERT ? "Alert"
                                                  : (h->conscious == CONS_CONFUSED ? "Confused" : "Unconscious");
    snprintf(buf, sizeof buf, "%s   core %.1f" UI_CH_DEGREE "C   air %.0f" UI_CH_DEGREE "C   alive %d:%02d", cons,
             (double)h->temp, (double)h->air_temp, mins / 60, mins % 60);
    ui_text(u, x0 + pw - 8 - ui_text_width(buf, 1), y0 + 6, 1, h->conscious == CONS_ALERT ? C_DIM : SEV[2], buf);
    ui_rect(u, x0 + 1, y0 + 16, pw - 2, 1, C_BORDER);

    /* Column 1: body diagram and per-part status; the selected part gets
     * its full status and what would help, below. */
    float cx = x0 + 8, cy = y0 + 22;
    body_diagram(u, h, s->sel, cx + 9, cy + 6);
    float lx = cx + 74;
    for (int p = 0; p < BP_COUNT; p++) {
        float py = cy + (float)p * 25;
        int sev = health_part_status(h, p, buf, sizeof buf);
        if (p == s->sel) ui_rect(u, lx - 3, py - 2, 154, 24, ui_rgba(255, 255, 255, 22));
        ui_textf(u, lx, py, 1, p == s->sel ? C_HEAD : SEV[sev], "%d %s", p + 1, health_part_name(p));
        ui_text_wrap(u, lx + 6, py + 10, 144, 1, 1, sev ? SEV[sev] : C_DIM, buf);
    }
    int sel_sev = health_part_status(h, s->sel, buf, sizeof buf);
    ui_textf(u, cx, cy + 176, 1, C_HEAD, "%s (skin %.0f" UI_CH_DEGREE "C):", health_part_name(s->sel),
             (double)h->part[s->sel].skin);
    int nl = ui_text_wrap(u, cx, cy + 186, 222, 2, 1, sel_sev ? SEV[sel_sev] : C_DIM, buf);
    if (advice(h, s->sel, buf, sizeof buf)) /* three lines in all, above the supplies */
        ui_text_wrap(u, cx, cy + 188 + (float)nl * 10, 222, 3 - nl, 1, ui_rgba(250, 235, 150, 255), buf);

    /* Column 2: organs and body state. */
    float ox = x0 + 236, oy = y0 + 22;
    ui_text(u, ox, oy, 1, C_HEAD, "ORGANS");
    for (int o = 0; o < ORG_COUNT; o++) {
        int sev = health_organ_status(h, o, buf, sizeof buf);
        float ry = oy + 11 + (float)o * 10;
        ui_text(u, ox, ry, 1, C_DIM, health_organ_name(o));
        ui_text(u, ox + 50, ry, 1, SEV[sev], buf);
    }
    float sy = oy + 80;
    ui_text(u, ox, sy, 1, C_HEAD, "BODY");
    sy += 11;
    float vol = h->blood / BLOOD_NORMAL;
    snprintf(buf, sizeof buf, "%.2f L", (double)h->blood);
    labelled_bar(u, ox, sy, "Blood", clamp01((vol - 0.5f) / 0.5f), vol < 0.7f ? SEV[3] : ui_rgba(210, 40, 40, 255),
                 buf);
    sy += 10;
    float hyd = health_hydration(h);
    snprintf(buf, sizeof buf, "%.0f%%", (double)(hyd * 100.0f));
    labelled_bar(u, ox, sy, "Hydration", hyd, ui_rgba(60, 140, 240, 255), buf);
    sy += 10;
    float sat = 1.0f - health_hunger(h);
    snprintf(buf, sizeof buf, "%.0f%%", (double)(sat * 100.0f));
    labelled_bar(u, ox, sy, "Food", sat, ui_rgba(235, 150, 50, 255), buf);
    sy += 10;
    snprintf(buf, sizeof buf, "%.0f kcal", (double)h->glycogen);
    labelled_bar(u, ox, sy, "Glycogen", h->glycogen / GLYCOGEN_MAX, ui_rgba(200, 120, 220, 255), buf);
    sy += 10;
    snprintf(buf, sizeof buf, "%.0f%%", (double)(health_stamina(h) * 100.0f));
    labelled_bar(u, ox, sy, "Stamina", health_stamina(h), ui_rgba(235, 215, 60, 255), buf);
    sy += 10;
    snprintf(buf, sizeof buf, "%.1f/10", (double)h->pain);
    labelled_bar(u, ox, sy, "Pain", h->pain / 10.0f, h->pain > 6.0f ? SEV[3] : SEV[1], buf);
    sy += 10;
    snprintf(buf, sizeof buf, "%.1f" UI_CH_DEGREE "C", (double)h->temp);
    labelled_bar(u, ox, sy, "Core temp", clamp01((h->temp - 30.0f) / 12.0f),
                 h->temp < 35.0f ? ui_rgba(90, 160, 255, 255) : (h->temp > 38.3f ? SEV[2] : SEV[0]), buf);
    sy += 13;
    ui_textf(u, ox, sy, 1, C_DIM, "Hb %.1f g/dL   lactate %.1f", (double)health_hb(h), (double)h->lactate);
    sy += 10;
    ui_textf(u, ox, sy, 1, C_DIM, "CO %.1f L/min   SV %.0f mL", (double)h->co, (double)h->sv);
    sy += 10;
    ui_textf(u, ox, sy, 1, C_DIM, "PaCO2 %.0f   PaO2 %.0f mmHg", (double)h->paco2, (double)h->pao2);
    sy += 10;
    float loss = h->bleed_ext + h->bleed_int;
    ui_textf(u, ox, sy, 1, loss >= 5.0f ? SEV[3] : C_DIM, "Blood loss %.0f mL/min", (double)loss);

    /* Column 3: the bedside monitor. */
    monitor(u, h, x0 + pw - 8 - 210, y0 + 22, 210);

    /* Bottom: supplies, events, last treatment, keys. */
    float by = y0 + 248;
    ui_rect(u, x0 + 1, by - 6, pw - 2, 1, C_BORDER);
    ui_text(u, x0 + 8, by, 1, C_HEAD, "SUPPLIES");
    struct {
        const char *name;
        int id;
    } sup[9] = {
        {"Bandages", I_BANDAGE},       {"Splints", I_SPLINT},
        {"Antiseptic", I_ANTISEPTIC},  {"Plant fibre", I_FIBRE},
        {"Sticks", I_STICK},           {"Painkillers", I_PAINKILLER},
        {"Apples", I_APPLE},           {"Water bucket", I_WATER_BUCKET},
        {"Antibiotics", I_ANTIBIOTIC},
    };
    for (int i = 0; i < 9; i++) {
        const int col = i % 3, row = i / 3;
        int n = s->inv ? inv_count(s->inv, sup[i].id) : 0;
        ui_textf(u, x0 + 8 + (float)col * 96, by + 11 + (float)row * 10, 1, n ? C_TEXT : C_DIM, "%s %d", sup[i].name,
                 n);
    }
    ui_text(u, x0 + 8, by + 43, 1, C_DIM, "Leaves give fibre and apples. Craft in the inventory (Tab).");

    float ex = x0 + 330;
    ui_text(u, ex, by, 1, C_HEAD, "EVENTS");
    for (int i = 0, n = 0; i < HEALTH_LOG; i++) {
        if (!h->log[i][0]) continue;
        ui_text_wrap(u, ex, by + 11 + (float)n * 10, pw - (ex - x0) - 8, 1, 1, i == 0 ? C_TEXT : C_DIM, h->log[i]);
        n++;
    }
    if (s->msg[0] && s->msg_age < 10.0f)
        ui_text_wrap(u, x0 + 8, by + 55, pw - 16, 1, 1, ui_alpha(SEV[1], clamp01(10.0f - s->msg_age)), s->msg);
    ui_rect(u, x0 + 1, y0 + ph - 26, pw - 2, 1, C_BORDER);
    ui_text(u, x0 + 8, y0 + ph - 21, 1, C_DIM,
            "1-7 select part   B bandage   S splint   D disinfect   C cool burn   X reduce joint");
    ui_text(u, x0 + 8, y0 + ph - 11, 1, C_DIM,
            "P painkiller   A antibiotics   E eat   R drink (in or facing water)   H close");
    u->scale = saved_scale;
    u->w = saved_w;
    u->h = saved_h;
}

/* ------------------------------------------------------------ death */

static void death_screen(ui *u, const health *h)
{
    ui_rect(u, 0, 0, u->w, u->h, ui_rgba(70, 0, 0, 205));
    const char *t = "YOU DIED";
    ui_text(u, (u->w - ui_text_width(t, 4)) * 0.5f, u->h * 0.3f, 4, ui_rgba(255, 220, 220, 255), t);
    const char *c = health_death_text(h->cause);
    ui_text(u, (u->w - ui_text_width(c, 2)) * 0.5f, u->h * 0.3f + 44, 2, C_TEXT, c);
    char b[96];
    int secs = (int)h->t;
    snprintf(b, sizeof b, "Survived %d min %02d s", secs / 60, secs % 60);
    ui_text(u, (u->w - ui_text_width(b, 1)) * 0.5f, u->h * 0.3f + 72, 1, C_DIM, b);
    for (int i = 0, n = 0; i < HEALTH_LOG && n < 3; i++) {
        if (!h->log[i][0]) continue;
        ui_text(u, (u->w - ui_text_width(h->log[i], 1)) * 0.5f, u->h * 0.3f + 90 + (float)n * 10, 1, C_DIM, h->log[i]);
        n++;
    }
    if (blink(h, 1)) {
        const char *r = "Press Enter to respawn";
        ui_text(u, (u->w - ui_text_width(r, 1)) * 0.5f, u->h * 0.3f + 130, 1, C_HEAD, r);
    }
}

void hud_draw(ui *u, const health *h, const hud_state *s)
{
    if (h->dead) {
        death_screen(u, h);
        return;
    }
    effects(u, h);
    if (s->panel) panel(u, h, s);
    else hud(u, h, s);
}
