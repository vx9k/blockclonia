#include "invui.h"
#include "anim.h"
#include "combat.h"
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
                uint8_t c[4] = {0};
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
                            uint8_t c[4] = {0};
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
    char b[8] = {0};
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
    int dur = item_durability(st->id);
    if (dur && st->wear) {
        /* Uses left as a bar along the bottom, green to red. */
        float left = 1.0f - (float)st->wear / (float)dur;
        ui_rect(u, x + 3, y + SLOT - 4, SLOT - 6, 2, ui_alpha(ui_rgba(10, 10, 12, 230), a));
        ui_rect(u, x + 3, y + SLOT - 4, (SLOT - 6) * left, 2, ui_alpha(pal_mix(C_ART, C_ECG, left), a));
    }
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

/* The first slot's corner. */
static void hotbar_origin(const ui *u, float *x0, float *y0)
{
    *x0 = floorf((u->w - (INV_HOTBAR * SLOT + (INV_HOTBAR - 1) * GAP)) * 0.5f);
    *y0 = u->h - SLOT - 4;
}

void invui_hotbar(ui *u, invui *s, const inventory *inv, float dt)
{
    track_pops(s, inv, dt);
    const float w = INV_HOTBAR * SLOT + (INV_HOTBAR - 1) * GAP;
    float x0 = 0.0f, y0 = 0.0f;
    hotbar_origin(u, &x0, &y0);
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

static const char *const CAT_NAME[RCAT_COUNT] = {"Basics", "Building", "Smelting & cooking",
                                                 "Tools",  "Weapons",  "Medical"};

int invui_recipe_category(int r)
{
    if (r < 0 || r >= g_recipe_count) return RCAT_BASICS;
    int id = g_recipes[r].out.id, kind = 0, tier = 0;
    if (item_tool(id, &kind, &tier)) return kind == TOOL_KNIFE || kind == TOOL_SWORD ? RCAT_WEAPONS : RCAT_TOOLS;
    switch (id) {
    case B_GLASS:
    case B_BRICK:
    case B_FURNACE: return RCAT_BUILDING;
    case I_COPPER_INGOT:
    case I_IRON_INGOT:
    case I_COOKED_MEAT: return RCAT_SMELTING;
    case I_RIFLE:
    case I_CARTRIDGE:
    case I_GUNPOWDER: return RCAT_WEAPONS;
    case I_SPLINT:
    case I_BANDAGE: return RCAT_MEDICAL;
    default: return RCAT_BASICS;
    }
}

/* A small tooltip by the cursor: the name, and one line under it. */
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

/* The facts about an item worth knowing before using it, one per line. */
static int item_facts(const item_stack *st, char lines[4][48])
{
    int n = 0, id = st->id, kind = 0, tier = 0;
    float water = 0.0f, kcal = item_food(id, &water);
    int dur = item_durability(id);
    if (dur) snprintf(lines[n++], 48, "%d of %d uses left", dur - st->wear, dur);
    if (item_tool(id, &kind, &tier)) {
        static const uint8_t WORKS[3] = {B_ROCK, B_LOG, B_DIRT};
        static const char *const ON[3] = {"rock", "wood", "soil"};
        if (kind <= TOOL_SHOVEL)
            snprintf(lines[n++], 48, "Breaks %s %.1fx faster", ON[kind], (double)item_dig_speed(id, WORKS[kind]));
        snprintf(lines[n++], 48, "Blow: %.1f", (double)combat_blow(id));
    } else if (id == I_RIFLE) {
        snprintf(lines[n++], 48, "Bullet: 14 at 400 m/s");
    }
    if (kcal > 0.0f) snprintf(lines[n++], 48, "%.0f kcal", (double)kcal);
    if (item_is_block(id) && n < 4) {
        const block_def *b = block_get((uint8_t)id);
        snprintf(lines[n++], 48, "%.0f kg/m3, spans %d", (double)b->density, b->span);
    }
    return n;
}

/* The card under the slots: a big icon, the description and the facts of
 * the hovered slot or recipe (or the item in hand). */
static void detail_card(ui *u, float x, float y, float w, float h, const inventory *inv, const item_stack *st, int rec,
                        int stations, float a)
{
    ui_rect(u, x, y, w, h, ui_alpha(ui_rgba(20, 24, 30, 220), a));
    ui_frame(u, x, y, w, h, 1, ui_alpha(C_BORDER, a));
    if (!st->count) {
        ui_text(u, x + 6, y + 6, 1, ui_alpha(C_DIM, a), "Point at an item or a recipe.");
        ui_text(u, x + 6, y + 18, 1, ui_alpha(C_DIM, a), "Left: take/place  Right: split");
        ui_text(u, x + 6, y + 30, 1, ui_alpha(C_DIM, a), "Shift: move to the other bar");
        return;
    }
    const item_def *d = item_get(st->id);
    invui_icon(u, st->id, x + 6, y + 6, 32, a);
    ui_text(u, x + 44, y + 7, 1, ui_alpha(C_HEAD, a), d->name);
    ui_text_wrap(u, x + 44, y + 18, w - 50, 2, 1, ui_alpha(C_DIM, a), d->desc);
    float ly = y + 44;
    if (rec >= 0) {
        /* What it takes, have/need per ingredient, and where. */
        const recipe *rc = &g_recipes[rec];
        for (int m = 0; m < 3 && rc->in[m].id; m++) {
            int have = inv_count(inv, rc->in[m].id);
            invui_icon(u, rc->in[m].id, x + 6, ly - 1, 10, a);
            ui_textf(u, x + 20, ly, 1, ui_alpha(have >= rc->in[m].count ? C_TEXT : C_ART, a), "%s  %d/%d",
                     item_get(rc->in[m].id)->name, have < 99 ? have : 99, rc->in[m].count);
            ly += 11;
        }
        if (rc->station) {
            int ok = (stations & rc->station) == rc->station;
            const char *where = rc->station & STATION_FURNACE ? (ok ? "At a furnace" : "Needs a furnace nearby")
                                                              : (ok ? "By a fire" : "Needs a burning campfire nearby");
            ui_text(u, x + 6, ly, 1, ui_alpha(ok ? C_CO2 : C_ART, a), where);
        }
        return;
    }
    char lines[4][48] = {{0}};
    int n = item_facts(st, lines);
    for (int i = 0; i < n && ly + 11.0f * (float)i < y + h - 8; i++)
        ui_text(u, x + 6, ly + 11.0f * (float)i, 1, ui_alpha(C_TEXT, a), lines[i]);
    int dur = item_durability(st->id);
    if (dur) {
        float left = 1.0f - (float)st->wear / (float)dur;
        ui_bar(u, x + w - 56, y + 46, 50, 4, left, pal_mix(C_ART, C_ECG, left), ui_rgba(10, 10, 12, 230));
    }
}

/* One row of the crafting list: a category header or a recipe. */
typedef struct {
    int cat;             /* RCAT_*, for headers */
    int recipe;          /* g_recipes index, -1 for a header */
} craft_row;

/* How many recipes a category has, and how many can be made now. */
static int cat_counts(const inventory *inv, int cat, int stations, int *can)
{
    int total = 0;
    *can = 0;
    for (int k = 0; k < g_recipe_count; k++)
        if (invui_recipe_category(k) == cat) {
            total++;
            *can += inv_can_craft(inv, k, stations);
        }
    return total;
}

invui_result invui_screen(ui *u, invui *s, inventory *inv, const invui_input *in)
{
    invui_result r = {0, {0, 0, 0}, -1, 0, -1, 0, 0};
    if (!s->opened) {
        s->opened = 1;
        s->cat_open = 1u << RCAT_BASICS;
    }
    s->open_t += in->dt;
    track_pops(s, inv, in->dt);
    float t = ease_out_back(s->open_t / 0.25f), fade = ease_out_cubic(s->open_t / 0.18f);
    ui_rect(u, 0, 0, u->w, u->h, ui_rgba(0, 0, 0, (int)(120.0f * fade)));

    const float grid_w = INV_HOTBAR * SLOT + (INV_HOTBAR - 1) * GAP, cw = 224, ph = 254;
    const float pw = 8 + grid_w + 12 + cw + 8;
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

    if (hover >= 0 && (in->click || in->rclick)) {
        r.click_slot = hover;
        r.click_button = in->rclick ? 1 : 0;
        r.click_shift = in->shift && in->click;
        inv_click(inv, hover, r.click_button, r.click_shift);
        r.changed = 1;
    } else if (inv->cursor.count && in->click && !inside(in, x0, y0, pw, ph)) {
        r.drop = inv->cursor; /* thrown out of the window */
        inv->cursor = (item_stack){0, 0, 0};
        r.changed = 1;
    }

    /* Crafting: categories that open and close, each with its recipes; a
     * recipe that cannot be made now is greyed out, or hidden with the
     * filter on. The wheel scrolls the list. */
    const float cx = x0 + 8 + grid_w + 12, top = y0 + 24, row_h = 18;
    ui_text(u, cx, top, 1, ui_alpha(C_HEAD, fade), "CRAFTING");
    const char *chip = s->only_craftable ? "[x] can make" : "[ ] can make";
    float chip_w = ui_text_width(chip, 1), chip_x = cx + cw - chip_w - 2;
    int chip_hot = inside(in, chip_x - 2, top - 2, chip_w + 4, 11);
    ui_text(u, chip_x, top, 1, ui_alpha(chip_hot ? C_HEAD : (s->only_craftable ? C_ECG : C_DIM), fade), chip);
    if (chip_hot && in->click) {
        s->only_craftable = !s->only_craftable;
        s->recipe_top = 0;
    }

    craft_row rows[RCAT_COUNT + 64] = {{0}};
    int nrows = 0;
    const int max_rows = (int)(sizeof rows / sizeof rows[0]);
    for (int cat = 0; cat < RCAT_COUNT && nrows < max_rows; cat++) {
        int can = 0, total = cat_counts(inv, cat, in->stations, &can);
        if (!total || (s->only_craftable && !can)) continue;
        rows[nrows++] = (craft_row){cat, -1};
        if (!(s->cat_open & (1u << cat))) continue;
        for (int k = 0; k < g_recipe_count && nrows < max_rows; k++)
            if (invui_recipe_category(k) == cat && (!s->only_craftable || inv_can_craft(inv, k, in->stations)))
                rows[nrows++] = (craft_row){cat, k};
    }
    const int shown = (int)((ph - 46) / (row_h + 2));
    s->recipe_top -= (int)in->wheel;
    if (s->recipe_top > nrows - shown) s->recipe_top = nrows - shown;
    if (s->recipe_top < 0) s->recipe_top = 0;

    int hover_recipe = -1;
    for (int k = s->recipe_top; k < nrows && k < s->recipe_top + shown; k++) {
        const craft_row *cr = &rows[k];
        float ry = top + 14 + (float)(k - s->recipe_top) * (row_h + 2);
        int hot = inside(in, cx, ry, cw, row_h);
        float st = ease_out_cubic((s->open_t - 0.02f * (float)(k - s->recipe_top)) / 0.25f) * fade;
        if (cr->recipe < 0) {
            /* A category header: its name, how many can be made, a toggle. */
            int open = (s->cat_open & (1u << cr->cat)) != 0, can = 0;
            int total = cat_counts(inv, cr->cat, in->stations, &can);
            ui_rect(u, cx, ry, cw, row_h, ui_alpha(hot ? ui_rgba(36, 42, 52, 240) : ui_rgba(28, 32, 40, 230), st));
            ui_rect(u, cx, ry + row_h - 1, cw, 1, ui_alpha(C_BORDER, st));
            ui_textf(u, cx + 4, ry + 5, 1, ui_alpha(C_HEAD, st), "%s %s", open ? "-" : "+", CAT_NAME[cr->cat]);
            char cnt[16] = {0};
            snprintf(cnt, sizeof cnt, "%d/%d", can, total);
            ui_text(u, cx + cw - 4 - ui_text_width(cnt, 1), ry + 5, 1, ui_alpha(can ? C_ECG : C_DIM, st), cnt);
            if (hot && in->click) s->cat_open ^= 1u << cr->cat;
            continue;
        }
        const recipe *rc = &g_recipes[cr->recipe];
        int ok = inv_can_craft(inv, cr->recipe, in->stations);
        if (hot) hover_recipe = cr->recipe;
        ui_rect(u, cx + 6, ry, cw - 6, row_h,
                ui_alpha(hot && ok ? ui_rgba(40, 52, 50, 235) : ui_rgba(22, 26, 32, 220), st));
        ui_frame(u, cx + 6, ry, cw - 6, row_h, 1, ui_alpha(hot && ok ? C_ECG : (hot ? C_DIM : C_BORDER), st));
        float a = st * (ok ? 1.0f : 0.45f);
        invui_icon(u, rc->out.id, cx + 9, ry + 1, 16, a);
        const char *name = item_get(rc->out.id)->name;
        if (rc->out.count > 1) ui_textf(u, cx + 29, ry + 5, 1, ui_alpha(C_TEXT, a), "%dx %s", rc->out.count, name);
        else ui_text(u, cx + 29, ry + 5, 1, ui_alpha(C_TEXT, a), name);
        /* The ingredients at the right, red when short. */
        float ix = cx + cw - 2;
        for (int m = 2; m >= 0; m--) {
            if (!rc->in[m].id) continue;
            ix -= 22;
            invui_icon(u, rc->in[m].id, ix, ry + 3, 12, a);
            char b[8] = {0};
            snprintf(b, sizeof b, "%d", rc->in[m].count);
            int have = inv_count(inv, rc->in[m].id);
            ui_text_shadow(u, ix + 12, ry + 9, 1, ui_alpha(have >= rc->in[m].count ? C_HEAD : C_ART, st), b);
        }
        /* A stripe on the left for a station: yellow when near, red if not. */
        if (rc->station)
            ui_rect(u, cx + 6, ry + 2, 2, row_h - 4,
                    ui_alpha((in->stations & rc->station) == rc->station ? C_CO2 : C_ART, st));
        if (hot && in->click && ok) {
            int n = 0;
            while (inv_craft(inv, cr->recipe, in->stations) && ++n < (in->shift ? 64 : 1)) {}
            if (n) {
                r.crafted = cr->recipe;
                r.craft_all = in->shift;
                r.changed = 1;
            }
        }
    }
    if (s->recipe_top > 0) ui_text(u, cx + cw * 0.5f - 3, top + 7, 1, ui_alpha(C_DIM, fade), "^");
    if (s->recipe_top + shown < nrows) ui_text(u, cx + cw * 0.5f - 3, y0 + ph - 10, 1, ui_alpha(C_DIM, fade), "v");

    /* The card: the hovered slot or recipe, else what is in hand. */
    const float card_y = y0 + 24 + 4 * (SLOT + GAP) + 12;
    item_stack card = inv->slot[inv->selected];
    if (hover >= 0) card = inv->slot[hover];
    else if (hover_recipe >= 0) card = (item_stack){g_recipes[hover_recipe].out.id, 1, 0};
    detail_card(u, x0 + 8, card_y, grid_w, y0 + ph - 8 - card_y, inv, &card, hover >= 0 ? -1 : hover_recipe,
                in->stations, fade);

    /* A short tooltip at the cursor, then the stack riding it on top. */
    if (!inv->cursor.count) {
        if (hover >= 0 && inv->slot[hover].count) {
            tooltip(u, in->mx, in->my, item_get(inv->slot[hover].id)->name, NULL, C_DIM);
        } else if (hover_recipe >= 0) {
            const recipe *rc = &g_recipes[hover_recipe];
            int ok = inv_can_craft(inv, hover_recipe, in->stations);
            const char *why = "Missing ingredients";
            if (ok) why = "Click to craft, Shift-click for all";
            else if (rc->station & ~in->stations & STATION_FURNACE) why = "Needs a furnace nearby";
            else if (rc->station & ~in->stations & STATION_FIRE) why = "Needs a burning campfire nearby";
            tooltip(u, in->mx, in->my, item_get(rc->out.id)->name, why, ok ? C_ECG : C_ART);
        }
    }
    if (inv->cursor.count) {
        invui_icon(u, inv->cursor.id, in->mx - 8, in->my - 8, 16, 1.0f);
        item_stack c = inv->cursor;
        count_label(u, &c, in->mx - 10, in->my - 10, 1.0f);
    }
    return r;
}
