#include "invui.h"
#include "anim.h"
#include "palette.h"
#include "texgen.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define SLOT 20.0f       /* slot size, logical pixels */
#define GAP 2.0f

/* ------------------------------------------------------------ icons */

/* Packed RGBA per texel, built once from the texture generator. Blocks keep
 * three 4x4 faces (box-filtered), items their full 16x16 sprite. */
static uint32_t g_sprite[ITEM_END - ITEM_FIRST][16][16];
static uint32_t g_face[B_COUNT][3][4][4];
static int g_icons_built;

static uint32_t pack(const uint8_t c[4]) { return ui_rgba(c[0], c[1], c[2], c[3]); }

static void build_icons(void)
{
    for (int id = ITEM_FIRST; id < ITEM_END; id++)
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                uint8_t c[4];
                texgen_texel(item_get(id)->tex, x, y, c);
                g_sprite[id - ITEM_FIRST][y][x] = pack(c);
            }
    for (int b = 1; b < B_COUNT; b++) {
        const block_def *d = &g_blocks[b];
        const int layer[3] = {d->tex[2], d->tex[0], d->tex[4]}; /* top, left (+X side), right (+Z side) */
        for (int f = 0; f < 3; f++)
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++) {
                    /* Colour weighted by alpha, so transparent texels (flames,
                     * glass) do not darken the average. */
                    float sum[3] = {0, 0, 0}, wsum = 0.0f;
                    int alpha = 0;
                    for (int y = 0; y < 4; y++)
                        for (int x = 0; x < 4; x++) {
                            uint8_t c[4];
                            texgen_texel(layer[f], i * 4 + x, j * 4 + y, c);
                            float wgt = (float)c[3] / 255.0f;
                            for (int k = 0; k < 3; k++) sum[k] += (float)c[k] * wgt;
                            wsum += wgt;
                            alpha += c[3];
                        }
                    const float shade = (f == 0 ? 1.0f : (f == 1 ? 0.8f : 0.62f)) / (wsum > 0.0f ? wsum : 1.0f);
                    g_face[b][f][j][i] = ui_rgba((int)(sum[0] * shade), (int)(sum[1] * shade), (int)(sum[2] * shade),
                                                 alpha / 16);
                }
    }
    g_icons_built = 1;
}

/* A face as 4x4 parallelograms: corner o, edge vectors (ux, uy), (vx, vy). */
static void face(ui *u, const uint32_t *tex, float ox, float oy, float ux, float uy, float vx, float vy, float a)
{
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) {
            uint32_t c = tex[j * 4 + i];
            if ((c >> 24) < 8) continue;
            float s0 = (float)i / 4.0f, s1 = (float)(i + 1) / 4.0f, t0 = (float)j / 4.0f, t1 = (float)(j + 1) / 4.0f;
            float xy[8] = {ox + ux * s0 + vx * t0, oy + uy * s0 + vy * t0, ox + ux * s1 + vx * t0,
                           oy + uy * s1 + vy * t0, ox + ux * s1 + vx * t1, oy + uy * s1 + vy * t1,
                           ox + ux * s0 + vx * t1, oy + uy * s0 + vy * t1};
            ui_quad(u, xy, ui_alpha(c, a));
        }
}

void invui_icon(ui *u, int id, float x, float y, float size, float alpha)
{
    if (!item_valid(id)) return;
    if (!g_icons_built) build_icons();
    if (item_is_block(id)) {
        /* Isometric cube: top diamond, left and right faces. */
        float s = size, h = s * 0.5f, q = s * 0.25f;
        face(u, &g_face[id][0][0][0], x + h, y, h, q, -h, q, alpha); /* top: back corner, right edge, left edge */
        face(u, &g_face[id][1][0][0], x, y + q, h, q, 0.0f, h, alpha); /* left */
        face(u, &g_face[id][2][0][0], x + h, y + h, h, -q, 0.0f, h, alpha); /* right */
        return;
    }
    /* Items: the sprite, one rectangle per run of equal texels in a row. */
    if (id < ITEM_FIRST || id >= ITEM_END) return;
    const float px = size / 16.0f;
    const uint32_t *sp = &g_sprite[id - ITEM_FIRST][0][0];
    for (int ty = 0; ty < 16; ty++)
        for (int tx = 0; tx < 16;) {
            uint32_t c = sp[ty * 16 + tx];
            int run = 1;
            while (tx + run < 16 && sp[ty * 16 + tx + run] == c) run++;
            if ((c >> 24) >= 8)
                ui_rect(u, x + (float)tx * px, y + (float)ty * px, (float)run * px, px, ui_alpha(c, alpha));
            tx += run;
        }
}

/* The count in a slot's corner, with a shadow. */
static void count_label(ui *u, const item_stack *st, float x, float y, float a)
{
    if (st->count <= 1) return;
    char b[8];
    snprintf(b, sizeof b, "%d", st->count);
    float w = ui_text_width(b, 1);
    ui_text_shadow(u, x + SLOT - 2 - w, y + SLOT - 9, 1, ui_alpha(C_HEAD, a), b);
}

static void slot_cell(ui *u, float x, float y, int hot, float a)
{
    ui_rect(u, x, y, SLOT, SLOT, ui_alpha(hot ? ui_rgba(40, 48, 56, 235) : ui_rgba(22, 26, 32, 220), a));
    ui_frame(u, x, y, SLOT, SLOT, 1, ui_alpha(hot ? pal_mix(C_BORDER, C_ECG, 0.6f) : C_BORDER, a));
}

static void slot_item(ui *u, const item_stack *st, float x, float y, float scale, float a)
{
    if (!st->count) return;
    float s = 16.0f * scale;
    invui_icon(u, st->id, x + (SLOT - s) * 0.5f, y + (SLOT - s) * 0.5f, s, a);
    count_label(u, st, x, y, a);
}

void invui_init(invui *s)
{
    memset(s, 0, sizeof *s);
    s->last_sel = -1;
}

/* Pulses a slot whose stack grew since last frame (an item was picked up). */
static void track_pops(invui *s, const inventory *inv, float dt)
{
    for (int i = 0; i < INV_SLOTS; i++) {
        const item_stack *st = &inv->slot[i];
        if (st->count > s->last_count[i] && (st->id == s->last_slot_id[i] || !s->last_count[i])) s->pop[i] = 1.0f;
        s->last_count[i] = st->count;
        s->last_slot_id[i] = st->id;
        s->pop[i] = anim_approach(s->pop[i], 0.0f, 7.0f, dt);
    }
}

void invui_hotbar(ui *u, invui *s, const inventory *inv, float dt)
{
    track_pops(s, inv, dt);
    const float w = INV_HOTBAR * SLOT + (INV_HOTBAR - 1) * GAP;
    const float x0 = floorf((u->w - w) * 0.5f), y0 = u->h - SLOT - 4;
    ui_rect(u, x0 - 3, y0 - 3, w + 6, SLOT + 6, ui_rgba(0, 0, 0, 120));
    if (s->last_sel < 0) s->sel_x = (float)inv->selected;
    s->sel_x = anim_approach(s->sel_x, (float)inv->selected, 22.0f, dt);
    for (int i = 0; i < INV_HOTBAR; i++) {
        float x = x0 + (float)i * (SLOT + GAP);
        slot_cell(u, x, y0, 0, 0.85f);
        float pop = s->pop[i];
        slot_item(u, &inv->slot[i], x, y0 - pop * 2.0f, 1.0f + 0.15f * pop, 1.0f);
    }
    /* The selection frame glides between slots. */
    float sx = x0 + s->sel_x * (SLOT + GAP);
    ui_frame(u, sx - 2, y0 - 2, SLOT + 4, SLOT + 4, 2, C_HEAD);
    ui_rect(u, sx - 2, y0 + SLOT + 2, SLOT + 4, 1, C_ECG);

    const item_stack *held = &inv->slot[inv->selected];
    if (inv->selected != s->last_sel || held->id != s->last_id) {
        s->name_age = 0.0f;
        s->last_sel = inv->selected;
        s->last_id = held->id;
    }
    s->name_age += dt;
    if (held->count && s->name_age < 2.5f) {
        float a = s->name_age < 2.0f ? 1.0f : (2.5f - s->name_age) * 2.0f;
        const char *name = item_get(held->id)->name;
        ui_text_shadow(u, (u->w - ui_text_width(name, 1)) * 0.5f, y0 - 14, 1, ui_alpha(C_TEXT, a), name);
    }
}

/* ------------------------------------------------------------ screen */

static int inside(const invui_input *in, float x, float y, float w, float h)
{ return in->mx >= x && in->mx < x + w && in->my >= y && in->my < y + h; }

static void tooltip(ui *u, float mx, float my, const char *title, const char *line, uint32_t line_col)
{
    float w = ui_text_width(title, 1);
    float lw = line && line[0] ? ui_text_width(line, 1) : 0.0f;
    if (lw > w) w = lw;
    float h = line && line[0] ? 22.0f : 12.0f;
    float x = mx + 10, y = my - h - 2;
    if (x + w + 8 > u->w) x = u->w - w - 8;
    if (y < 2) y = my + 12;
    ui_rect(u, x, y, w + 8, h, ui_rgba(10, 12, 16, 245));
    ui_frame(u, x, y, w + 8, h, 1, pal_mix(C_BORDER, C_ECG, 0.5f));
    ui_text(u, x + 4, y + 3, 1, C_HEAD, title);
    if (line && line[0]) ui_text(u, x + 4, y + 13, 1, line_col, line);
}

invui_result invui_screen(ui *u, invui *s, inventory *inv, const invui_input *in)
{
    invui_result r = {0, {0, 0}, -1};
    s->open_t += in->dt;
    track_pops(s, inv, in->dt);
    float t = ease_out_back(s->open_t / 0.25f), fade = ease_out_cubic(s->open_t / 0.18f);
    ui_rect(u, 0, 0, u->w, u->h, ui_rgba(0, 0, 0, (int)(120.0f * fade)));

    const float grid_w = INV_HOTBAR * SLOT + (INV_HOTBAR - 1) * GAP;
    const float pw = 8 + grid_w + 12 + 190 + 8, ph = 170;
    const float x0 = floorf((u->w - pw) * 0.5f), y0 = floorf((u->h - ph) * 0.5f + (1.0f - t) * 10.0f);
    ui_rect(u, x0, y0, pw, ph, ui_alpha(C_PANEL, fade));
    ui_frame(u, x0, y0, pw, ph, 1, ui_alpha(C_BORDER, fade));
    ui_text(u, x0 + 8, y0 + 6, 1, ui_alpha(C_HEAD, fade), "INVENTORY");
    ui_rect(u, x0 + 1, y0 + 16, pw - 2, 1, ui_alpha(C_BORDER, fade));

    /* Slots: the 27 backpack slots in three rows, then the hotbar. */
    int hover = -1;
    for (int i = 0; i < INV_SLOTS; i++) {
        int row = i < INV_HOTBAR ? 3 : (i - INV_HOTBAR) / INV_HOTBAR;
        int col = i < INV_HOTBAR ? i : (i - INV_HOTBAR) % INV_HOTBAR;
        float x = x0 + 8 + (float)col * (SLOT + GAP);
        float y = y0 + 24 + (float)row * (SLOT + GAP) + (row == 3 ? 6.0f : 0.0f);
        float st = ease_out_cubic((s->open_t - 0.008f * (float)i) / 0.2f);
        int hot = inside(in, x, y, SLOT, SLOT);
        if (hot) hover = i;
        slot_cell(u, x, y, hot, st * fade);
        if (i == inv->selected) ui_rect(u, x + 1, y + SLOT - 2, SLOT - 2, 1, ui_alpha(C_ECG, st));
        slot_item(u, &inv->slot[i], x, y - s->pop[i] * 2.0f, 1.0f + 0.15f * s->pop[i], st);
    }
    ui_text(u, x0 + 8, y0 + 24 + 4 * (SLOT + GAP) + 10, 1, ui_alpha(C_DIM, fade), "Right: split  Shift: move");

    if (hover >= 0 && (in->click || in->rclick)) {
        inv_click(inv, hover, in->rclick ? 1 : 0, in->shift && in->click);
        r.changed = 1;
    } else if (inv->cursor.count && in->click && !inside(in, x0, y0, pw, ph)) {
        r.drop = inv->cursor; /* thrown out of the window */
        inv->cursor = (item_stack){0, 0};
        r.changed = 1;
    }

    /* Crafting: one row per recipe; greyed out when something is missing. */
    const float cx = x0 + 8 + grid_w + 12, cw = 190;
    ui_text(u, cx, y0 + 24, 1, ui_alpha(C_HEAD, fade), "CRAFTING");
    int hover_recipe = -1;
    for (int k = 0; k < g_recipe_count; k++) {
        const recipe *rc = &g_recipes[k];
        float ry = y0 + 36 + (float)k * 20;
        int ok = inv_can_craft(inv, k, in->near_fire);
        int hot = inside(in, cx, ry, cw, 18);
        if (hot) hover_recipe = k;
        float st = ease_out_cubic((s->open_t - 0.03f * (float)k) / 0.25f);
        ui_rect(u, cx, ry, cw, 18, ui_alpha(hot && ok ? ui_rgba(40, 52, 50, 235) : ui_rgba(22, 26, 32, 220), st));
        ui_frame(u, cx, ry, cw, 18, 1, ui_alpha(hot && ok ? C_ECG : C_BORDER, st));
        float a = st * (ok ? 1.0f : 0.45f);
        invui_icon(u, rc->out.id, cx + 2, ry + 1, 16, a);
        char b[48];
        snprintf(b, sizeof b, "%dx", rc->out.count);
        ui_text(u, cx + 20, ry + 5, 1, ui_alpha(C_TEXT, a), b);
        float ix = cx + 42;
        ui_text(u, ix - 8, ry + 5, 1, ui_alpha(C_DIM, a), "<");
        for (int m = 0; m < 3 && rc->in[m].id; m++) {
            invui_icon(u, rc->in[m].id, ix, ry + 1, 16, a);
            int have = inv_count(inv, rc->in[m].id);
            snprintf(b, sizeof b, "%d", rc->in[m].count);
            ui_text_shadow(u, ix + 12, ry + 9, 1, ui_alpha(have >= rc->in[m].count ? C_HEAD : C_ART, st), b);
            ix += 26;
        }
        if (rc->needs_fire)
            ui_text(u, cx + cw - 4 - ui_text_width("fire", 1), ry + 5, 1, ui_alpha(in->near_fire ? C_CO2 : C_DIM, st),
                    "fire");
        if (hot && in->click && ok) {
            int n = 0;
            while (inv_craft(inv, k, in->near_fire) && ++n < (in->shift ? 64 : 1)) {}
            if (n) {
                r.crafted = k;
                r.changed = 1;
            }
        }
    }

    /* Tooltips, then the stack riding the cursor on top of everything. */
    if (!inv->cursor.count) {
        if (hover >= 0 && inv->slot[hover].count) {
            const item_def *d = item_get(inv->slot[hover].id);
            tooltip(u, in->mx, in->my, d->name, d->desc, C_DIM);
        } else if (hover_recipe >= 0) {
            const recipe *rc = &g_recipes[hover_recipe];
            const item_def *d = item_get(rc->out.id);
            int ok = inv_can_craft(inv, hover_recipe, in->near_fire);
            const char *why = ok ? "Click to craft, Shift-click for all"
                                 : (rc->needs_fire && !in->near_fire ? "Needs a burning campfire nearby"
                                                                     : "Missing ingredients");
            tooltip(u, in->mx, in->my, d->name, why, ok ? C_ECG : C_ART);
        }
    }
    if (inv->cursor.count) {
        invui_icon(u, inv->cursor.id, in->mx - 8, in->my - 8, 16, 1.0f);
        item_stack c = inv->cursor;
        count_label(u, &c, in->mx - 10, in->my - 10, 1.0f);
    }
    return r;
}
