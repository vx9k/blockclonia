#include "hud.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define C_TEXT ui_rgba(232, 232, 232, 255)
#define C_DIM ui_rgba(150, 156, 165, 255)
#define C_HEAD ui_rgba(255, 255, 255, 255)
#define C_PANEL ui_rgba(14, 16, 20, 232)
#define C_BORDER ui_rgba(72, 82, 98, 255)
#define C_BACK ui_rgba(40, 44, 52, 255)
#define C_ECG ui_rgba(40, 235, 100, 255)
#define C_ART ui_rgba(245, 70, 70, 255)
#define C_PLETH ui_rgba(70, 200, 245, 255)
#define C_CO2 ui_rgba(245, 220, 70, 255)

static const uint32_t SEV[4] = {0xff5ac85au, 0xff3cc8e6u, 0xff2882f0u, 0xff3232e6u};
/* green, yellow, orange, red (packed little-endian RGBA) */

static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

static int blink(const health *h, double hz) { return fmod(h->t * hz, 1.0) < 0.5; }

/* The heart glyph flashes on each beat. */
static uint32_t beat_color(const health *h)
{
    int beat = h->rhythm == RHYTHM_SINUS && h->t - h->last_r < 0.12;
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
        prev = buf[k1 - 1];
        have_prev = 1;
        float t0 = clamp01((mx - lo) / (hi - lo)), t1 = clamp01((mn - lo) / (hi - lo));
        float y0 = y + h - t0 * h, y1 = y + h - t1 * h;
        ui_rect(u, x + (float)c, y0, 1.0f, y1 - y0 < 1.0f ? 1.0f : y1 - y0, col);
    }
}

/* Scrolling strip of the last `samples` samples, newest on the right. */
static void trace_scroll(ui *u, const float *buf, int pos, int samples, float x, float y, float w, float h,
                         float lo, float hi, uint32_t col)
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
    int sev;
} alert;

static int collect_alerts(const health *h, alert *a, int max)
{
    int n = 0;
#define ADD(sv, ...) do { if (n < max) { snprintf(a[n].text, sizeof a[n].text, __VA_ARGS__); a[n].sev = (sv); n++; } } while (0)
    float bleed = h->bleed_ext + h->bleed_int;
    if (h->rhythm == RHYTHM_VF) ADD(3, "CARDIAC ARREST (VF)");
    else if (h->rhythm == RHYTHM_ASYSTOLE) ADD(3, "CARDIAC ARREST");
    if (bleed >= 5.0f) ADD(bleed > 30.0f ? 3 : 2, "Bleeding %.0f mL/min", bleed);
    if (!h->breathing && h->conscious != CONS_UNCONSCIOUS && h->rhythm == RHYTHM_SINUS)
        ADD(h->sao2 < 0.85f || h->lung_water > 0.02f ? 3 : 1, h->lung_water > 0.02f ? "Drowning" : "Holding breath");
    if (h->sao2 < 0.9f && h->rhythm == RHYTHM_SINUS) ADD(h->sao2 < 0.8f ? 3 : 2, "Low oxygen");
    for (int i = 0; i < BP_COUNT; i++)
        if (h->part[i].fracture)
            ADD(2, "Broken %s%s", health_bone_name(i), h->part[i].splinted ? " (splinted)" : "");
    float hyd = health_hydration(h), hun = health_hunger(h);
    if (hyd < 0.35f) ADD(hyd < 0.15f ? 3 : 1, hyd < 0.15f ? "Severely dehydrated" : "Thirsty");
    if (hun > 0.7f) ADD(hun > 0.95f ? 3 : 1, hun > 0.95f ? "Starving" : "Hungry");
    if (h->temp < 35.0f) ADD(h->temp < 32.0f ? 3 : 2, "Hypothermia %.1f" UI_CH_DEGREE "C", h->temp);
    else if (h->temp > 38.3f) ADD(h->temp > 40.0f ? 3 : 1, "Fever %.1f" UI_CH_DEGREE "C", h->temp);
    float inf = 0.0f;
    for (int i = 0; i < h->wound_count; i++) inf = inf > h->wounds[i].infection ? inf : h->wounds[i].infection;
    if (h->sepsis > 0.2f) ADD(3, "Sepsis");
    else if (inf > 0.3f) ADD(2, "Infected wound");
    if (h->pain > 6.0f) ADD(2, "Severe pain");
    if (health_stamina(h) < 0.1f) ADD(1, "Exhausted");
    if (h->conscious == CONS_CONFUSED) ADD(2, "Confused");
#undef ADD
    return n;
}

static void hud(ui *u, const health *h, const hud_state *s)
{
    const float x = 4, y = 4, w = 152;
    ui_rect(u, x, y, w, 46, ui_rgba(0, 0, 0, 150));
    int arrest = h->rhythm != RHYTHM_SINUS;
    ui_text(u, x + 3, y + 3, 1, beat_color(h), UI_CH_HEART);
    uint32_t hrc = arrest || h->hr > 140.0f || h->hr < 45.0f ? (blink(h, 2) ? C_ART : C_TEXT) : C_ECG;
    ui_textf(u, x + 11, y + 3, 1, hrc, "%3.0f", arrest ? 0.0 : (double)h->hr);
    ui_textf(u, x + 36, y + 3, 1, h->sbp < 90.0f ? C_ART : C_TEXT, "BP %3.0f/%-3.0f", (double)h->sbp, (double)h->dbp);
    int sp = health_spo2_reading(h);
    if (sp < 0) ui_text(u, x + 104, y + 3, 1, C_DIM, "SpO2 --");
    else ui_textf(u, x + 104, y + 3, 1, sp < 90 ? C_ART : C_PLETH, "SpO2 %d%%", sp);
    ui_rect(u, x + 3, y + 13, w - 6, 13, ui_rgba(0, 20, 8, 200));
    trace_scroll(u, h->ecg, h->wave_pos, HEALTH_WAVE_LEN / 2, x + 4, y + 14, w - 8, 11, -0.6f, 1.6f, C_ECG);

    /* Needs: blood, water, food, stamina. */
    struct { const char *l; float v; uint32_t c; } bars[4] = {
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

    alert al[8];
    int n = collect_alerts(h, al, 6);
    float ay = y + 50;
    for (int i = 0; i < n; i++, ay += 10) {
        uint32_t c = SEV[al[i].sev];
        if (al[i].sev == 3 && !blink(h, 1.5)) c = C_TEXT;
        ui_rect(u, x, ay - 1, ui_text_width(al[i].text, 1) + 6, 10, ui_rgba(0, 0, 0, 175));
        ui_text(u, x + 3, ay, 1, c, al[i].text);
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

    float by = u->h - 14;
    if (s->held) ui_text(u, (u->w - ui_text_width(s->held, 1)) * 0.5f, by, 1, C_TEXT, s->held);
    if (s->flying) {
        const char *t = "Flying: no injuries";
        ui_text(u, (u->w - ui_text_width(t, 1)) * 0.5f, by - 10, 1, C_DIM, t);
    }
    const char *hint = "H: health";
    ui_text(u, u->w - ui_text_width(hint, 1) - 4, by, 1, ui_alpha(C_DIM, 0.8f), hint);
}

/* ----------------------------------------------------------- panel */

typedef struct { float x, y, w, h; } rectf;

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
        ui_rect(u, r.x, r.y, r.w, r.h, ui_alpha(c, 0.35f + 0.4f * (1.0f - bp->integrity)));
        ui_frame(u, r.x, r.y, r.w, r.h, 1, ui_alpha(c, 0.9f));
        if (bp->internal > 1.0f) ui_rect(u, r.x + 3, r.y + r.h * 0.35f, r.w - 6, r.h * 0.3f, ui_rgba(120, 0, 0, 200));
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
        int bandaged = 0;
        for (int i = 0; i < h->wound_count; i++) {
            const wound *w = &h->wounds[i];
            if (w->part != p) continue;
            bleed += w->bleed;
            inf = inf > w->infection ? inf : w->infection;
            bandaged |= w->bandage != BANDAGE_NONE;
        }
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
        char num[2] = {(char)('1' + p), 0};
        ui_text(u, r.x + (r.w - 5) * 0.5f, r.y + 2, 1, ui_rgba(10, 10, 10, 220), num);
        if (p == sel) ui_frame(u, r.x - 1, r.y - 1, r.w + 2, r.h + 2, 1, blink(h, 2) ? C_HEAD : ui_rgba(255, 255, 255, 120));
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
    const float row = 46, tw = w - 86; /* 84 px of numbers fits "200/120" */
    ui_rect(u, x, y, w, row * 4 + 4, ui_rgba(0, 0, 0, 255));
    ui_frame(u, x, y, w, row * 4 + 4, 1, C_BORDER);
    int arrest = h->rhythm != RHYTHM_SINUS;
    char b[48];

    /* ECG, lead II. */
    float ry = y + 2;
    ui_text(u, x + 3, ry + 1, 1, ui_alpha(C_ECG, 0.7f), "II");
    trace_sweep(u, h->ecg, h->wave_pos, x + 3, ry + 9, tw, row - 12, -0.6f, 1.6f, C_ECG);
    int hr_alarm = arrest || h->hr > 130.0f || h->hr < 45.0f;
    uint32_t c = hr_alarm && blink(h, 2) ? C_ART : C_ECG;
    ui_text(u, x + tw + 8, ry + 1, 1, ui_alpha(C_ECG, 0.7f), "HR");
    ui_text(u, x + tw + 22, ry + 1, 1, beat_color(h), UI_CH_HEART);
    snprintf(b, sizeof b, "%.0f", arrest ? 0.0 : (double)h->hr);
    ui_text(u, x + tw + 8, ry + 12, 3, c, b);
    if (arrest) ui_text(u, x + tw + 8, ry + 38, 1, C_ART, h->rhythm == RHYTHM_VF ? "VF" : "ASYSTOLE");
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
    float bleed = 0.0f, inf = 0.0f;
    int wounds = 0, dressed = 1, arterial = 0;
    for (int i = 0; i < h->wound_count; i++) {
        const wound *w = &h->wounds[i];
        if (w->part != part) continue;
        wounds++;
        bleed += w->bleed;
        inf = inf > w->infection ? inf : w->infection;
        dressed &= w->bandage != BANDAGE_NONE;
        arterial |= w->arterial && w->bleed > 20.0f;
    }
    if (arterial) snprintf(buf, n, "B: pressure dressing now. An artery will not clot by itself; keep still.");
    else if (wounds && !dressed && bleed > 1.0f) snprintf(buf, n, "B: dress the wound to stop the bleeding.");
    else if (p->fracture && !p->splinted && part != BP_HEAD && part != BP_CHEST)
        snprintf(buf, n, "S: splint it (a splint, or 2 sticks + 1 fibre). Walking on it bleeds inside.");
    else if (inf > 0.3f) snprintf(buf, n, "A: antibiotics. Too deep for antiseptic now.");
    else if (inf > 0.05f) snprintf(buf, n, "D: clean the wound (antiseptic, or water nearby).");
    else if (p->internal > 5.0f) snprintf(buf, n, "Internal bleeding: rest, keep warm, drink.");
    else if (part == BP_HEAD && (h->concussion > 0.0f || h->confusion > 0.0f)) snprintf(buf, n, "Concussed: rest.");
    else if (h->pain > 5.0f) snprintf(buf, n, "P: a painkiller eases the pain for 6 h.");
    else return 0;
    return 1;
}

static void panel(ui *u, const health *h, const hud_state *s)
{
    ui_rect(u, 0, 0, u->w, u->h, ui_rgba(0, 0, 0, 140));
    const float pw = 628, ph = 348;
    const float x0 = floorf((u->w - pw) * 0.5f), y0 = floorf((u->h - ph) * 0.5f);
    ui_rect(u, x0, y0, pw, ph, C_PANEL);
    ui_frame(u, x0, y0, pw, ph, 1, C_BORDER);
    char buf[200];

    ui_text(u, x0 + 8, y0 + 6, 1, C_HEAD, "HEALTH");
    int mins = (int)(h->t / 60.0);
    const char *cons = h->conscious == CONS_ALERT ? "Alert" : (h->conscious == CONS_CONFUSED ? "Confused" : "Unconscious");
    snprintf(buf, sizeof buf, "%s   core %.1f" UI_CH_DEGREE "C   alive %d:%02d", cons, (double)h->temp, mins / 60,
             mins % 60);
    ui_text(u, x0 + pw - 8 - ui_text_width(buf, 1), y0 + 6, 1,
            h->conscious == CONS_ALERT ? C_DIM : SEV[2], buf);
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
    ui_textf(u, cx, cy + 176, 1, C_HEAD, "%s:", health_part_name(s->sel));
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
    labelled_bar(u, ox, sy, "Blood", clamp01((vol - 0.5f) / 0.5f), vol < 0.7f ? SEV[3] : ui_rgba(210, 40, 40, 255), buf);
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
    const survival_items *it = &h->items;
    ui_text(u, x0 + 8, by, 1, C_HEAD, "SUPPLIES");
    struct { const char *name; int n; } sup[9] = {
        {"Bandages", it->bandages}, {"Splints", it->splints}, {"Antiseptic", it->antiseptic},
        {"Plant fibre", it->fibre}, {"Sticks", it->sticks}, {"Painkillers", it->painkillers},
        {"Apples", it->apples}, {NULL, 0}, {"Antibiotics", it->antibiotics},
    };
    for (int i = 0; i < 9; i++) {
        if (!sup[i].name) continue;
        ui_textf(u, x0 + 8 + (float)(i % 3) * 96, by + 11 + (float)(i / 3) * 10, 1, sup[i].n ? C_TEXT : C_DIM,
                 "%s %d", sup[i].name, sup[i].n);
    }
    ui_text(u, x0 + 8, by + 43, 1, C_DIM, "Leaves give fibre and apples; logs give sticks.");

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
            "1-7 select part   B bandage   S splint   D disinfect   P painkiller   A antibiotics");
    ui_text(u, x0 + 8, y0 + ph - 11, 1, C_DIM, "E eat   R drink (in or facing water)   H close");
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
        ui_text(u, (u->w - ui_text_width(h->log[i], 1)) * 0.5f, u->h * 0.3f + 90 + (float)n * 10, 1, C_DIM,
                h->log[i]);
        n++;
    }
    const char *r = "Press Enter to respawn";
    if (blink(h, 1)) ui_text(u, (u->w - ui_text_width(r, 1)) * 0.5f, u->h * 0.3f + 130, 1, C_HEAD, r);
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
