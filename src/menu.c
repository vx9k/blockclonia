#include "menu.h"
#include "anim.h"
#include "health.h"
#include "palette.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define ROW_H 18.0f

/* Per-frame layout state. Items are numbered in drawing order; that number
 * is what keyboard focus and the hover animations refer to. */
typedef struct {
    menu *m;
    ui *u;
    const menu_input *in;
    int n;             /* next item number */
    int activate;      /* item activated by the keyboard this frame, -1 none */
    int adjust;        /* -1/+1 for the focused item (Left/Right) */
    int mouse_moved;
    menu_action action;
} ctx;

void menu_init(menu *m)
{
    memset(m, 0, sizeof *m);
    m->screen = SCREEN_TITLE;
    m->back_to = SCREEN_TITLE;
    m->drag = -1;
    m->last_mx = m->last_my = -1.0f;
}

void menu_open(menu *m, int screen)
{
    if (screen == SCREEN_SETTINGS || screen == SCREEN_CONTROLS) {
        if (m->screen == SCREEN_TITLE || m->screen == SCREEN_PAUSE) m->back_to = m->screen;
    }
    m->screen = screen;
    m->age = 0.0f;
    m->focus = screen == SCREEN_SETTINGS ? 3 : 0; /* past the three tabs, on the first setting */
    m->drag = -1;
    for (int i = 0; i < MENU_MAX_ITEMS; i++) m->hover[i] = 0.0f;
}

static int over(const ctx *c, float x, float y, float w, float h)
{ return c->in->mx >= x && c->in->mx < x + w && c->in->my >= y && c->in->my < y + h; }

/* Entry animation: items slide in from the left, one after another. */
static float entry(const ctx *c, int i, float *dx)
{
    float t = ease_out_cubic((c->m->age - 0.035f * (float)i) / 0.28f);
    *dx = -(1.0f - t) * 14.0f;
    return t;
}

/* Common item bookkeeping: hover moves the focus, the highlight eases in. */
static int item(ctx *c, float x, float y, float w, float h, int *hot)
{
    int i = c->n++;
    *hot = over(c, x, y, w, h);
    if (*hot && c->mouse_moved) c->m->focus = i;
    if (i < MENU_MAX_ITEMS)
        c->m->hover[i] = anim_approach(c->m->hover[i], c->m->focus == i ? 1.0f : 0.0f, 18.0f, c->in->dt);
    return i;
}

static float hover_of(const ctx *c, int i) { return i < MENU_MAX_ITEMS ? c->m->hover[i] : 0.0f; }

/* The item frame shared by buttons and setting rows: a dark cell whose
 * border warms to the monitor green, with an accent bar sliding in. */
static void cell(ui *u, float x, float y, float w, float h, float hv, float a)
{
    ui_rect(u, x, y, w, h, ui_alpha(pal_mix(ui_rgba(14, 16, 20, 215), ui_rgba(28, 36, 40, 235), hv), a));
    ui_frame(u, x, y, w, h, 1, ui_alpha(pal_mix(C_BORDER, C_ECG, hv * 0.85f), a));
    if (hv > 0.02f) ui_rect(u, x + 1, y + 1, 2.0f * hv + 0.01f, h - 2, ui_alpha(C_ECG, a * hv));
}

static int button(ctx *c, float x, float y, float w, const char *label)
{
    int hot;
    int i = item(c, x, y, w, ROW_H, &hot);
    float dx, a = entry(c, i, &dx);
    float hv = hover_of(c, i);
    cell(c->u, x + dx, y, w, ROW_H, hv, a);
    ui_text(c->u, x + dx + 8 + 3.0f * hv, y + 6, 1, ui_alpha(pal_mix(C_TEXT, C_HEAD, hv), a), label);
    int pressed = (hot && c->in->click) || c->activate == i;
    c->m->clicked |= pressed;
    return pressed;
}

static int slider(ctx *c, float x, float y, float w, const char *label, int *v, int lo, int hi, int step,
                  const char *unit)
{
    int hot;
    int i = item(c, x, y, w, ROW_H, &hot);
    float dx, a = entry(c, i, &dx);
    float hv = hover_of(c, i);
    int old = *v;
    const float tx = x + 8, tw = w - 16;
    if (hot && c->in->click) {
        c->m->drag = i;
        c->m->clicked = 1;
    }
    if (c->m->drag == i) {
        if (!c->in->mouse_down) {
            c->m->drag = -1;
        } else {
            float t = anim_clamp01((c->in->mx - tx) / tw);
            int nv = lo + (int)lroundf(t * (float)(hi - lo) / (float)step) * step;
            *v = nv < lo ? lo : (nv > hi ? hi : nv);
        }
    }
    if (c->m->focus == i && c->adjust) {
        int nv = *v + c->adjust * step;
        *v = nv < lo ? lo : (nv > hi ? hi : nv);
    }
    cell(c->u, x + dx, y, w, ROW_H, hv, a);
    ui_text(c->u, x + dx + 8 + 3.0f * hv, y + 4, 1, ui_alpha(pal_mix(C_TEXT, C_HEAD, hv), a), label);
    char b[32];
    snprintf(b, sizeof b, "%d%s", *v, unit);
    ui_text(c->u, x + dx + w - 8 - ui_text_width(b, 1), y + 4, 1, ui_alpha(C_ECG, a), b);
    float t = (float)(*v - lo) / (float)(hi - lo);
    ui_rect(c->u, tx + dx, y + ROW_H - 5, tw, 2, ui_alpha(C_BACK, a));
    ui_rect(c->u, tx + dx, y + ROW_H - 5, tw * t, 2, ui_alpha(pal_mix(C_DIM, C_ECG, 0.4f + 0.6f * hv), a));
    ui_rect(c->u, tx + dx + tw * t - 1, y + ROW_H - 7, 3, 6, ui_alpha(C_HEAD, a));
    return *v != old;
}

static int cycle(ctx *c, float x, float y, float w, const char *label, int *v, const char *const *names, int count)
{
    int hot;
    int i = item(c, x, y, w, ROW_H, &hot);
    float dx, a = entry(c, i, &dx);
    float hv = hover_of(c, i);
    int old = *v;
    if ((hot && c->in->click) || c->activate == i) {
        *v = (*v + 1) % count;
        c->m->clicked = 1;
    }
    if (c->m->focus == i && c->adjust) *v = (*v + c->adjust + count) % count;
    cell(c->u, x + dx, y, w, ROW_H, hv, a);
    ui_text(c->u, x + dx + 8 + 3.0f * hv, y + 6, 1, ui_alpha(pal_mix(C_TEXT, C_HEAD, hv), a), label);
    const char *name = names[*v < 0 || *v >= count ? 0 : *v];
    char b[48];
    snprintf(b, sizeof b, hv > 0.5f ? "< %s >" : "%s", name);
    ui_text(c->u, x + dx + w - 8 - ui_text_width(b, 1), y + 6, 1, ui_alpha(C_ECG, a), b);
    return *v != old;
}

/* ------------------------------------------------------------ title */

/* A lead II complex, for the trace under the logo: P, QRS, T. */
static float ecg_shape(float p)
{
    float v = 0.12f * expf(-powf((p - 0.12f) / 0.035f, 2.0f));
    v -= 0.15f * expf(-powf((p - 0.235f) / 0.012f, 2.0f));
    v += 1.0f * expf(-powf((p - 0.26f) / 0.014f, 2.0f));
    v -= 0.25f * expf(-powf((p - 0.285f) / 0.013f, 2.0f));
    v += 0.28f * expf(-powf((p - 0.5f) / 0.06f, 2.0f));
    return v;
}

static void ecg_sweep(ui *u, float x, float y, float w, float h, double time, float a)
{
    const float sweep = 2.4f, beat = 70.0f; /* s per sweep, px per beat */
    float cursor = (float)fmod(time / (double)sweep, 1.0) * w;
    int cols = (int)w;
    float prev = 0.0f;
    for (int cx = 0; cx < cols; cx++) {
        float behind = fmodf(cursor - (float)cx + w, w);
        if (behind > w - 14.0f) { /* the blank gap ahead of the cursor */
            prev = 0.0f;
            continue;
        }
        float v = ecg_shape(fmodf((float)cx, beat) / beat);
        float lo = v < prev ? v : prev, hi = v > prev ? v : prev;
        prev = v;
        float fade = 1.0f - 0.75f * behind / w;
        float y0 = y + h * 0.7f - hi * h * 0.7f, y1 = y + h * 0.7f - lo * h * 0.7f;
        ui_rect(u, x + (float)cx, y0, 1, y1 - y0 < 1.0f ? 1.0f : y1 - y0, ui_alpha(C_ECG, a * fade));
    }
    /* The bright writing head. */
    float v = ecg_shape(fmodf(cursor, beat) / beat);
    ui_rect(u, x + cursor - 1, y + h * 0.7f - v * h * 0.7f - 1, 3, 3, ui_alpha(C_HEAD, a));
}

static void title_screen(ctx *c)
{
    ui *u = c->u;
    menu *m = c->m;
    float fade = ease_out_cubic(m->age / 0.6f);
    /* Darken the left of the slowly turning world so the text reads. */
    uint32_t dark = ui_rgba(6, 8, 12, (int)(225.0f * fade)), clear = ui_rgba(6, 8, 12, 0);
    ui_rect(u, 0, 0, u->w * 0.2f, u->h, dark);
    ui_rect4(u, u->w * 0.2f, 0, u->w * 0.5f, u->h, dark, clear, clear, dark);
    ui_rect(u, 0, 0, u->w, u->h, ui_rgba(0, 0, 0, (int)(50.0f * fade)));

    const float x0 = 32.0f, y0 = floorf(u->h * 0.17f);
    const char *logo = "BLOCKCLONIA";
    float lx = x0;
    for (int i = 0; logo[i]; i++) {
        const char ch[2] = {logo[i], 0};
        float t = ease_out_back((m->age - 0.05f * (float)i) / 0.45f);
        float bob = sinf((float)m->time * 1.6f + (float)i * 0.55f) * 1.2f;
        float ly = y0 + bob - (1.0f - t) * 10.0f;
        uint32_t col = i < 5 ? C_HEAD : C_ECG;
        ui_text_shadow(u, lx, ly, 5, ui_alpha(col, anim_clamp01(t)), ch);
        lx += (float)(UI_CELL_W * 5);
    }
    float logo_w = ui_text_width(logo, 5);
    ecg_sweep(u, x0, y0 + 46, logo_w, 18, m->time, fade);
    ui_text_shadow(u, x0, y0 + 70, 1, ui_alpha(C_DIM, fade), "Real physics. A body that works like one.");

    float by = y0 + 96;
    if (button(c, x0, by, 150, "Play")) c->action = MENU_PLAY;
    if (button(c, x0, by + 22, 150, "Settings")) menu_open(m, SCREEN_SETTINGS);
    if (button(c, x0, by + 44, 150, "Controls")) menu_open(m, SCREEN_CONTROLS);
    if (button(c, x0, by + 66, 150, "Quit")) c->action = MENU_QUIT;

    ui_text_shadow(u, 6, u->h - 12, 1, ui_alpha(C_DIM, fade), m->footer);
    const char *keys = "Arrows + Enter or mouse   F2: screenshot";
    ui_text_shadow(u, u->w - 6 - ui_text_width(keys, 1), u->h - 12, 1, ui_alpha(C_DIM, fade * 0.8f), keys);
}

/* ------------------------------------------------------------ panels */

/* A centred panel that grows in from 96% with the screen's fade. */
static void panel(ui *u, float age, float w, float h, float *x, float *y, const char *title)
{
    float t = ease_out_cubic(age / 0.22f);
    float s = 0.96f + 0.04f * t;
    float pw = w * s, ph = h * s;
    *x = floorf((u->w - w) * 0.5f);
    *y = floorf((u->h - h) * 0.5f);
    float px = (u->w - pw) * 0.5f, py = (u->h - ph) * 0.5f;
    ui_rect(u, px, py, pw, ph, ui_alpha(C_PANEL, t));
    ui_frame(u, px, py, pw, ph, 1, ui_alpha(C_BORDER, t));
    ui_text(u, *x + 8, *y + 6, 1, ui_alpha(C_HEAD, t), title);
    ui_rect(u, px + 1, *y + 16, pw - 2, 1, ui_alpha(C_BORDER, t));
}

static void pause_screen(ctx *c)
{
    ui *u = c->u;
    menu *m = c->m;
    ui_rect(u, 0, 0, u->w, u->h, ui_rgba(0, 0, 0, (int)(150.0f * ease_out_cubic(m->age / 0.25f))));
    float x, y;
    const float w = 200, h = 162;
    panel(u, m->age, w, h, &x, &y, "PAUSED");
    if (m->status[0]) ui_text_wrap(u, x + 8, y + 22, w - 16, 2, 1, C_DIM, m->status);
    float by = y + 46, bw = w - 16;
    if (button(c, x + 8, by, bw, "Resume")) c->action = MENU_RESUME;
    if (button(c, x + 8, by + 22, bw, "Settings")) menu_open(m, SCREEN_SETTINGS);
    if (button(c, x + 8, by + 44, bw, "Controls")) menu_open(m, SCREEN_CONTROLS);
    if (button(c, x + 8, by + 66, bw, "Save and quit to title")) c->action = MENU_TO_TITLE;
    if (button(c, x + 8, by + 88, bw, "Quit game")) c->action = MENU_QUIT;
}

static void backdrop(ctx *c)
{
    /* Settings and controls sit over whatever opened them. */
    if (c->m->back_to == SCREEN_TITLE) {
        ui_rect(c->u, 0, 0, c->u->w, c->u->h, ui_rgba(6, 8, 12, 190));
    } else {
        ui_rect(c->u, 0, 0, c->u->w, c->u->h, ui_rgba(0, 0, 0, 150));
    }
}

/* A tab heading: highlighted when it is the open one. */
static int tab_button(ctx *c, float x, float y, float w, const char *label, int active)
{
    int hot;
    int i = item(c, x, y, w, ROW_H, &hot);
    float dx, a = entry(c, i, &dx);
    float hv = hover_of(c, i);
    ui *u = c->u;
    ui_rect(u, x, y, w, ROW_H, ui_alpha(active ? ui_rgba(30, 44, 40, 240) : ui_rgba(14, 16, 20, 200), a));
    ui_frame(u, x, y, w, ROW_H, 1, ui_alpha(pal_mix(C_BORDER, C_ECG, active ? 1.0f : hv * 0.7f), a));
    ui_text(u, x + (w - ui_text_width(label, 1)) * 0.5f, y + 6, 1, ui_alpha(active ? C_HEAD : C_TEXT, a), label);
    if (c->m->focus == i && c->adjust) c->m->tab = (c->m->tab + c->adjust + 3) % 3;
    return (hot && c->in->click) || c->activate == i;
}

static int settings_video(ctx *c, settings *s, float rx, float ry, float rw)
{
    static const char *const ONOFF[] = {"Off", "On"};
    static const char *const GUI[] = {"Auto", "1x", "2x", "3x", "4x"};
    int changed = 0;
    changed |= slider(c, rx, ry, rw, "Field of view", &s->fov, SETTINGS_FOV_MIN, SETTINGS_FOV_MAX, 1, UI_CH_DEGREE);
    changed |= cycle(c, rx, ry + 20, rw, "View bobbing", &s->view_bob, ONOFF, 2);
    changed |= cycle(c, rx, ry + 40, rw, "Sprint FOV effect", &s->fov_effects, ONOFF, 2);
    changed |= slider(c, rx, ry + 60, rw, "Render distance (restart)", &s->render_distance, SETTINGS_RD_MIN,
                      SETTINGS_RD_MAX, 1, "");
    changed |= cycle(c, rx, ry + 80, rw, "VSync", &s->vsync, ONOFF, 2);
    changed |= cycle(c, rx, ry + 100, rw, "Texture filtering (distance)", &s->filtering, ONOFF, 2);
    changed |= cycle(c, rx, ry + 120, rw, "GUI scale", &s->gui_scale, GUI, SETTINGS_GUI_MAX + 1);
    return changed;
}

static int settings_controls(ctx *c, settings *s, float rx, float ry, float rw)
{
    static const char *const ONOFF[] = {"Off", "On"};
    static const char *const SPRINT[] = {"Hold", "Toggle"};
    int changed = 0;
    changed |= slider(c, rx, ry, rw, "Mouse sensitivity", &s->sensitivity, SETTINGS_SENS_MIN, SETTINGS_SENS_MAX, 5,
                      "%");
    changed |= cycle(c, rx, ry + 20, rw, "Invert mouse", &s->invert_y, ONOFF, 2);
    changed |= cycle(c, rx, ry + 40, rw, "Sprint", &s->sprint_toggle, SPRINT, 2);
    changed |= cycle(c, rx, ry + 60, rw, "Key hints", &s->show_hints, ONOFF, 2);
    return changed;
}

static int settings_audio(ctx *c, settings *s, float rx, float ry, float rw)
{
    int changed = 0;
    changed |= slider(c, rx, ry, rw, "Master volume", &s->volume_master, 0, 100, 5, "%");
    changed |= slider(c, rx, ry + 20, rw, "Effects", &s->volume_effects, 0, 100, 5, "%");
    changed |= slider(c, rx, ry + 40, rw, "Ambience (wind, water, fire)", &s->volume_ambient, 0, 100, 5, "%");
    return changed;
}

static void settings_screen(ctx *c, settings *s)
{
    ui *u = c->u;
    menu *m = c->m;
    backdrop(c);
    float x, y;
    const float w = 300, h = 222;
    panel(u, m->age, w, h, &x, &y, "SETTINGS");
    static const char *const TABS[3] = {"Video", "Controls", "Audio"};
    float tw = (w - 16 - 8) / 3.0f;
    for (int t = 0; t < 3; t++)
        if (tab_button(c, x + 8 + (float)t * (tw + 4), y + 22, tw, TABS[t], m->tab == t)) m->tab = t;
    float rx = x + 8, rw = w - 16, ry = y + 48;
    int changed;
    if (m->tab == 0) changed = settings_video(c, s, rx, ry, rw);
    else if (m->tab == 1) changed = settings_controls(c, s, rx, ry, rw);
    else changed = settings_audio(c, s, rx, ry, rw);
    if (button(c, x + w - 8 - 90, y + h - 8 - ROW_H, 90, "Done")) menu_open(m, m->back_to);
    if (changed) c->action = MENU_SETTINGS;
}

static void controls_screen(ctx *c)
{
    ui *u = c->u;
    menu *m = c->m;
    backdrop(c);
    float x, y;
    const float w = 420, h = 262;
    panel(u, m->age, w, h, &x, &y, "CONTROLS");
    static const char *const KEYS[][2] = {
        {"W A S D", "Walk"},
        {"Ctrl / double-tap W", "Sprint"},
        {"Shift", "Sneak (won't walk off edges)"},
        {"Space", "Jump / swim up"},
        {"F", "Fly (no injuries)"},
        {"Left click (hold)", "Break block"},
        {"Right click", "Place / use"},
        {"Middle click", "Pick block"},
        {"1-9 / scroll", "Hotbar slot"},
        {"Q", "Drop item (Ctrl: stack)"},
        {"Tab / I", "Inventory and crafting"},
        {"H", "Health panel"},
        {"E / R", "Eat / drink"},
        {"F2", "Screenshot"},
        {"F3", "Debug overlay (K: injury menu)"},
        {"Esc", "Pause"},
    };
    const int n = (int)(sizeof KEYS / sizeof KEYS[0]);
    for (int i = 0; i < n; i++) {
        int col = i / 8, row = i % 8;
        float kx = x + 8 + (float)col * 206, ky = y + 24 + (float)row * 24;
        float t = ease_out_cubic((m->age - 0.02f * (float)i) / 0.3f);
        ui_rect(u, kx, ky, 198, 20, ui_alpha(ui_rgba(255, 255, 255, 10), t));
        ui_text(u, kx + 4, ky + 3, 1, ui_alpha(C_ECG, t), KEYS[i][0]);
        ui_text(u, kx + 4, ky + 11, 1, ui_alpha(C_TEXT, t), KEYS[i][1]);
    }
    if (button(c, x + w - 8 - 90, y + h - 8 - ROW_H, 90, "Back")) menu_open(m, m->back_to);
}

/* Trigger any injury the body model supports, on a chosen part, plus a
 * heal/reset: for testing bleeding, fractures, burns and the rest without
 * having to get hurt first. Opened straight from gameplay (K, with the F3
 * overlay up), so it has no back_to to return to: Back and Esc both just
 * resume. Doesn't touch health.h's health struct: it only hands the
 * caller an index into HEALTH_DEBUG_KINDS through m->debug_kind. */
static void debug_health_screen(ctx *c)
{
    ui *u = c->u;
    menu *m = c->m;
    ui_rect(u, 0, 0, u->w, u->h, ui_rgba(0, 0, 0, (int)(150.0f * ease_out_cubic(m->age / 0.25f))));
    float x, y;
    const float w = 460, h = 296;
    panel(u, m->age, w, h, &x, &y, "DEBUG: HEALTH");
    static const char *const PARTS[BP_COUNT] = {"Head",      "Chest",    "Abdomen",  "Left arm",
                                                "Right arm", "Left leg", "Right leg"};
    float rx = x + 8, rw = w - 16, ry = y + 22;
    cycle(c, rx, ry, rw, "Part (for entries that need one)", &m->debug_part, PARTS, BP_COUNT);
    ry += 22;
    const float bw = (rw - 6) * 0.5f;
    for (int i = 0; i < HEALTH_DEBUG_COUNT; i++) {
        int row = i / 2; /* two buttons per row: integer grid position, not a fraction */
        float bx = rx + (float)(i % 2) * (bw + 6), by = ry + (float)row * ROW_H;
        if (button(c, bx, by, bw, HEALTH_DEBUG_KINDS[i].label)) {
            m->debug_kind = i;
            c->action = MENU_DEBUG_INJURE;
        }
    }
    int rows = (HEALTH_DEBUG_COUNT + 1) / 2; /* rows used above, rounded up */
    float by = ry + (float)rows * ROW_H + 6;
    if (button(c, rx, by, bw, "Heal (reset to new)")) c->action = MENU_DEBUG_RESET;
    if (button(c, rx + bw + 6, by, bw, "Back")) c->action = MENU_RESUME;
}

menu_action menu_frame(menu *m, ui *u, const menu_input *in, settings *s)
{
    ctx c = {m, u, in, 0, -1, 0, 0, MENU_NONE};
    int focus0 = m->focus, screen0 = m->screen, tab0 = m->tab;
    m->clicked = m->hovered = 0;
    m->age += in->dt;
    m->time += (double)in->dt;
    c.mouse_moved = in->mx >= 0.0f && (fabsf(in->mx - m->last_mx) + fabsf(in->my - m->last_my) > 0.5f);
    m->last_mx = in->mx;
    m->last_my = in->my;

    if (m->items > 0) {
        if (in->up) m->focus = (m->focus + m->items - 1) % m->items;
        if (in->down) m->focus = (m->focus + 1) % m->items;
        if (m->focus >= m->items) m->focus = 0;
    }
    if (in->enter) c.activate = m->focus;
    c.adjust = in->right - in->left;

    int screen = m->screen;
    switch (screen) {
    case SCREEN_TITLE: title_screen(&c); break;
    case SCREEN_PAUSE: pause_screen(&c); break;
    case SCREEN_SETTINGS: settings_screen(&c, s); break;
    case SCREEN_CONTROLS: controls_screen(&c); break;
    case SCREEN_DEBUG_HEALTH: debug_health_screen(&c); break;
    default: break;
    }
    /* The screen functions can switch screens through c.m. */
    /* cppcheck-suppress knownConditionTrueFalse */
    if (m->screen == screen) m->items = c.n;
    else m->items = 0; /* switched: the new screen lays out next frame */

    if (in->back && c.action == MENU_NONE && m->screen == screen) {
        if (screen == SCREEN_PAUSE || screen == SCREEN_DEBUG_HEALTH) c.action = MENU_RESUME;
        else if (screen == SCREEN_SETTINGS || screen == SCREEN_CONTROLS) menu_open(m, m->back_to);
    }
    m->hovered = m->focus != focus0 && m->screen == screen0 && m->items > 0;
    m->clicked |= m->screen != screen0 || m->tab != tab0;
    return c.action;
}
