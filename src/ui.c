#include "ui.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* 5x7 glyphs drawn for this game, rows top to bottom; an eighth row holds
 * descenders. '#' is ink. Covers ASCII 32..126 plus a degree sign, a heart
 * and a drop. */
#define GLYPH_FIRST 32
#define GLYPH_COUNT 98

static const char *const GLYPHS[GLYPH_COUNT][8] = {
    /*   */ {".....", ".....", ".....", ".....", ".....", ".....", ".....", "....."},
    /* ! */ {"..#..", "..#..", "..#..", "..#..", "..#..", ".....", "..#..", "....."},
    /* " */ {".#.#.", ".#.#.", ".....", ".....", ".....", ".....", ".....", "....."},
    /* # */ {".#.#.", ".#.#.", "#####", ".#.#.", "#####", ".#.#.", ".#.#.", "....."},
    /* $ */ {"..#..", ".####", "#.#..", ".###.", "..#.#", "####.", "..#..", "....."},
    /* % */ {"##...", "##..#", "...#.", "..#..", ".#...", "#..##", "...##", "....."},
    /* & */ {".##..", "#..#.", "#.#..", ".#...", "#.#.#", "#..#.", ".##.#", "....."},
    /* ' */ {"..#..", "..#..", ".....", ".....", ".....", ".....", ".....", "....."},
    /* ( */ {"...#.", "..#..", ".#...", ".#...", ".#...", "..#..", "...#.", "....."},
    /* ) */ {".#...", "..#..", "...#.", "...#.", "...#.", "..#..", ".#...", "....."},
    /* * */ {".....", "..#..", "#.#.#", ".###.", "#.#.#", "..#..", ".....", "....."},
    /* + */ {".....", "..#..", "..#..", "#####", "..#..", "..#..", ".....", "....."},
    /* , */ {".....", ".....", ".....", ".....", ".....", "..##.", "...#.", "..#.."},
    /* - */ {".....", ".....", ".....", "#####", ".....", ".....", ".....", "....."},
    /* . */ {".....", ".....", ".....", ".....", ".....", ".##..", ".##..", "....."},
    /* / */ {".....", "....#", "...#.", "..#..", ".#...", "#....", ".....", "....."},
    /* 0 */ {".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###.", "....."},
    /* 1 */ {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###.", "....."},
    /* 2 */ {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####", "....."},
    /* 3 */ {"#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###.", "....."},
    /* 4 */ {"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#.", "....."},
    /* 5 */ {"#####", "#....", "####.", "....#", "....#", "#...#", ".###.", "....."},
    /* 6 */ {"..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###.", "....."},
    /* 7 */ {"#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#...", "....."},
    /* 8 */ {".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###.", "....."},
    /* 9 */ {".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##..", "....."},
    /* : */ {".....", ".##..", ".##..", ".....", ".##..", ".##..", ".....", "....."},
    /* ; */ {".....", ".##..", ".##..", ".....", ".##..", "..#..", ".#...", "....."},
    /* < */ {"...#.", "..#..", ".#...", "#....", ".#...", "..#..", "...#.", "....."},
    /* = */ {".....", ".....", "#####", ".....", "#####", ".....", ".....", "....."},
    /* > */ {".#...", "..#..", "...#.", "....#", "...#.", "..#..", ".#...", "....."},
    /* ? */ {".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#..", "....."},
    /* @ */ {".###.", "#...#", "....#", ".##.#", "#.#.#", "#.#.#", ".###.", "....."},
    /* A */ {".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#", "....."},
    /* B */ {"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####.", "....."},
    /* C */ {".###.", "#...#", "#....", "#....", "#....", "#...#", ".###.", "....."},
    /* D */ {"###..", "#..#.", "#...#", "#...#", "#...#", "#..#.", "###..", "....."},
    /* E */ {"#####", "#....", "#....", "####.", "#....", "#....", "#####", "....."},
    /* F */ {"#####", "#....", "#....", "####.", "#....", "#....", "#....", "....."},
    /* G */ {".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".####", "....."},
    /* H */ {"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#", "....."},
    /* I */ {".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###.", "....."},
    /* J */ {"..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##..", "....."},
    /* K */ {"#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#", "....."},
    /* L */ {"#....", "#....", "#....", "#....", "#....", "#....", "#####", "....."},
    /* M */ {"#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#", "....."},
    /* N */ {"#...#", "#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "....."},
    /* O */ {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###.", "....."},
    /* P */ {"####.", "#...#", "#...#", "####.", "#....", "#....", "#....", "....."},
    /* Q */ {".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#", "....."},
    /* R */ {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#", "....."},
    /* S */ {".####", "#....", "#....", ".###.", "....#", "....#", "####.", "....."},
    /* T */ {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#..", "....."},
    /* U */ {"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###.", "....."},
    /* V */ {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#..", "....."},
    /* W */ {"#...#", "#...#", "#...#", "#.#.#", "#.#.#", "#.#.#", ".#.#.", "....."},
    /* X */ {"#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#", "....."},
    /* Y */ {"#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#..", "....."},
    /* Z */ {"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####", "....."},
    /* [ */ {".###.", ".#...", ".#...", ".#...", ".#...", ".#...", ".###.", "....."},
    /* \ */ {".....", "#....", ".#...", "..#..", "...#.", "....#", ".....", "....."},
    /* ] */ {".###.", "...#.", "...#.", "...#.", "...#.", "...#.", ".###.", "....."},
    /* ^ */ {"..#..", ".#.#.", "#...#", ".....", ".....", ".....", ".....", "....."},
    /* _ */ {".....", ".....", ".....", ".....", ".....", ".....", "#####", "....."},
    /* ` */ {".#...", "..#..", "...#.", ".....", ".....", ".....", ".....", "....."},
    /* a */ {".....", ".....", ".###.", "....#", ".####", "#...#", ".####", "....."},
    /* b */ {"#....", "#....", "#.##.", "##..#", "#...#", "#...#", "####.", "....."},
    /* c */ {".....", ".....", ".###.", "#....", "#....", "#...#", ".###.", "....."},
    /* d */ {"....#", "....#", ".##.#", "#..##", "#...#", "#...#", ".####", "....."},
    /* e */ {".....", ".....", ".###.", "#...#", "#####", "#....", ".###.", "....."},
    /* f */ {"..##.", ".#..#", ".#...", "###..", ".#...", ".#...", ".#...", "....."},
    /* g */ {".....", ".....", ".####", "#...#", "#...#", ".####", "....#", ".###."},
    /* h */ {"#....", "#....", "#.##.", "##..#", "#...#", "#...#", "#...#", "....."},
    /* i */ {"..#..", ".....", ".##..", "..#..", "..#..", "..#..", ".###.", "....."},
    /* j */ {"...#.", ".....", "..##.", "...#.", "...#.", "...#.", "#..#.", ".##.."},
    /* k */ {"#....", "#....", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "....."},
    /* l */ {".##..", "..#..", "..#..", "..#..", "..#..", "..#..", ".###.", "....."},
    /* m */ {".....", ".....", "##.#.", "#.#.#", "#.#.#", "#.#.#", "#.#.#", "....."},
    /* n */ {".....", ".....", "#.##.", "##..#", "#...#", "#...#", "#...#", "....."},
    /* o */ {".....", ".....", ".###.", "#...#", "#...#", "#...#", ".###.", "....."},
    /* p */ {".....", ".....", "####.", "#...#", "#...#", "####.", "#....", "#...."},
    /* q */ {".....", ".....", ".####", "#...#", "#...#", ".####", "....#", "....#"},
    /* r */ {".....", ".....", "#.##.", "##..#", "#....", "#....", "#....", "....."},
    /* s */ {".....", ".....", ".####", "#....", ".###.", "....#", "####.", "....."},
    /* t */ {".#...", ".#...", "###..", ".#...", ".#...", ".#..#", "..##.", "....."},
    /* u */ {".....", ".....", "#...#", "#...#", "#...#", "#..##", ".##.#", "....."},
    /* v */ {".....", ".....", "#...#", "#...#", "#...#", ".#.#.", "..#..", "....."},
    /* w */ {".....", ".....", "#...#", "#...#", "#.#.#", "#.#.#", ".#.#.", "....."},
    /* x */ {".....", ".....", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "....."},
    /* y */ {".....", ".....", "#...#", "#...#", "#...#", ".####", "....#", ".###."},
    /* z */ {".....", ".....", "#####", "...#.", "..#..", ".#...", "#####", "....."},
    /* { */ {"...#.", "..#..", "..#..", ".#...", "..#..", "..#..", "...#.", "....."},
    /* | */ {"..#..", "..#..", "..#..", "..#..", "..#..", "..#..", "..#..", "....."},
    /* } */ {".#...", "..#..", "..#..", "...#.", "..#..", "..#..", ".#...", "....."},
    /* ~ */ {".....", ".....", ".#...", "#.#.#", "...#.", ".....", ".....", "....."},
    /* deg */ {".##..", "#..#.", "#..#.", ".##..", ".....", ".....", ".....", "....."},
    /* heart */ {".....", ".#.#.", "#####", "#####", ".###.", "..#..", ".....", "....."},
    /* drop */ {"..#..", "..#..", ".###.", "#####", "#####", ".###.", ".....", "....."},
};

/* Atlas cells are 8x8, 16 to a row; the last cell is solid white. */
#define CELL 8
#define COLS (UI_ATLAS_W / CELL)
#define WHITE_CELL (COLS * (UI_ATLAS_H / CELL) - 1)
_Static_assert(GLYPH_COUNT <= WHITE_CELL, "font atlas too small");

void ui_font_build(uint8_t *atlas)
{
    memset(atlas, 0, UI_ATLAS_W * UI_ATLAS_H);
    for (int g = 0; g < GLYPH_COUNT; g++) {
        int ox = (g % COLS) * CELL, oy = (g / COLS) * CELL;
        for (int y = 0; y < UI_GLYPH_H; y++)
            for (int x = 0; x < UI_GLYPH_W; x++)
                if (GLYPHS[g][y][x] == '#') atlas[(oy + y) * UI_ATLAS_W + ox + x] = 255;
    }
    int ox = (WHITE_CELL % COLS) * CELL, oy = (WHITE_CELL / COLS) * CELL;
    for (int y = 0; y < CELL; y++) memset(atlas + (oy + y) * UI_ATLAS_W + ox, 255, CELL);
}

int ui_scale_for(int fb_w, int fb_h)
{
    int s = fb_h / 360 < fb_w / 640 ? fb_h / 360 : fb_w / 640;
    return s < 1 ? 1 : s;
}

void ui_begin(ui *u, ui_vertex *mem, int max_quads, int fb_w, int fb_h)
{
    u->v = mem;
    u->quads = 0;
    u->max_quads = mem ? max_quads : 0;
    u->overflow = 0;
    u->scale = (float)ui_scale_for(fb_w, fb_h);
    u->w = (float)fb_w / u->scale;
    u->h = (float)fb_h / u->scale;
}

static inline uint16_t unorm_u(float t) { return (uint16_t)(t * 65535.0f / UI_ATLAS_W + 0.5f); }
static inline uint16_t unorm_v(float t) { return (uint16_t)(t * 65535.0f / UI_ATLAS_H + 0.5f); }

/* Pushes one quad; corners clockwise from the top left. */
static void quad(ui *u, const float xy[8], uint16_t u0, uint16_t v0, uint16_t u1, uint16_t v1, const uint32_t col[4])
{
    if (u->quads >= u->max_quads) {
        u->overflow++;
        return;
    }
    ui_vertex *q = u->v + (size_t)u->quads * 4;
    const float s = u->scale;
    /* Built in a local and stored whole: the target is write-combined
     * GPU memory, where scattered small writes are slow. */
    ui_vertex t[4] = {
        {xy[0] * s, xy[1] * s, u0, v0, col[0]},
        {xy[2] * s, xy[3] * s, u1, v0, col[1]},
        {xy[4] * s, xy[5] * s, u1, v1, col[2]},
        {xy[6] * s, xy[7] * s, u0, v1, col[3]},
    };
    memcpy(q, t, sizeof t);
    u->quads++;
}

static void white_uv(uint16_t *uu, uint16_t *vv)
{
    const int col = WHITE_CELL % COLS, row = WHITE_CELL / COLS;
    float cx = (float)(col * CELL) + CELL * 0.5f;
    float cy = (float)(row * CELL) + CELL * 0.5f;
    *uu = unorm_u(cx);
    *vv = unorm_v(cy);
}

void ui_rect4(ui *u, float x, float y, float w, float h, uint32_t tl, uint32_t tr, uint32_t br, uint32_t bl)
{
    if (w <= 0.0f || h <= 0.0f) return;
    uint16_t wu, wv;
    white_uv(&wu, &wv);
    float xy[8] = {x, y, x + w, y, x + w, y + h, x, y + h};
    uint32_t c[4] = {tl, tr, br, bl};
    quad(u, xy, wu, wv, wu, wv, c);
}

void ui_rect(ui *u, float x, float y, float w, float h, uint32_t rgba)
{
    ui_rect4(u, x, y, w, h, rgba, rgba, rgba, rgba);
}

void ui_frame(ui *u, float x, float y, float w, float h, float t, uint32_t rgba)
{
    ui_rect(u, x, y, w, t, rgba);
    ui_rect(u, x, y + h - t, w, t, rgba);
    ui_rect(u, x, y + t, t, h - 2 * t, rgba);
    ui_rect(u, x + w - t, y + t, t, h - 2 * t, rgba);
}

void ui_line(ui *u, float x0, float y0, float x1, float y1, float thick, uint32_t rgba)
{
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-4f) {
        ui_rect(u, x0 - thick * 0.5f, y0 - thick * 0.5f, thick, thick, rgba);
        return;
    }
    /* Extend by half the thickness so joined segments leave no gaps. */
    float ex = dx / len * thick * 0.5f, ey = dy / len * thick * 0.5f;
    float nx = -ey, ny = ex;
    x0 -= ex;
    y0 -= ey;
    x1 += ex;
    y1 += ey;
    uint16_t wu, wv;
    white_uv(&wu, &wv);
    float xy[8] = {x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny};
    uint32_t c[4] = {rgba, rgba, rgba, rgba};
    quad(u, xy, wu, wv, wu, wv, c);
}

static int glyph_index(unsigned char ch)
{
    int g = (int)ch - GLYPH_FIRST;
    return g >= 0 && g < GLYPH_COUNT ? g : '?' - GLYPH_FIRST;
}

float ui_text_width(const char *s, int size)
{
    int best = 0, cur = 0;
    for (; *s; s++) {
        if (*s == '\n') {
            cur = 0;
            continue;
        }
        cur++;
        if (cur > best) best = cur;
    }
    return best ? (float)(best * UI_CELL_W - 1) * (float)size : 0.0f;
}

float ui_text(ui *u, float x, float y, int size, uint32_t rgba, const char *s)
{
    /* Snap to whole screen pixels so every texel lands on whole pixels. */
    x = floorf(x * u->scale + 0.5f) / u->scale;
    y = floorf(y * u->scale + 0.5f) / u->scale;
    const float gw = (float)(UI_GLYPH_W * size), gh = (float)(UI_GLYPH_H * size);
    const uint32_t c[4] = {rgba, rgba, rgba, rgba};
    float cx = x, cy = y, widest = 0.0f;
    for (; *s; s++) {
        if (*s == '\n') {
            if (cx - x > widest) widest = cx - x;
            cx = x;
            cy += (float)(UI_LINE_H * size);
            continue;
        }
        if (*s != ' ') {
            int g = glyph_index((unsigned char)*s);
            const int col = g % COLS, row = g / COLS;
            float tx = (float)(col * CELL), ty = (float)(row * CELL);
            float xy[8] = {cx, cy, cx + gw, cy, cx + gw, cy + gh, cx, cy + gh};
            quad(u, xy, unorm_u(tx), unorm_v(ty), unorm_u(tx + UI_GLYPH_W), unorm_v(ty + UI_GLYPH_H), c);
        }
        cx += (float)(UI_CELL_W * size);
    }
    if (cx - x > widest) widest = cx - x;
    return widest > 0.0f ? widest - (float)size : 0.0f;
}

float ui_textf(ui *u, float x, float y, int size, uint32_t rgba, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return ui_text(u, x, y, size, rgba, buf);
}

int ui_text_wrap(ui *u, float x, float y, float width, int max_lines, int size, uint32_t rgba, const char *s)
{
    int cols = (int)((width + (float)size) / (float)(UI_CELL_W * size));
    if (cols < 1) cols = 1;
    char line[256];
    int lines = 0;
    while (*s && lines < max_lines) {
        while (*s == ' ') s++;
        int n = (int)strlen(s);
        int take = n;
        if (n > cols) {
            take = cols;
            while (take > 0 && s[take] != ' ') take--; /* break at a space */
            if (take == 0) take = cols;                /* one long word */
        }
        if (lines == max_lines - 1 && n > take && take >= 2) {
            /* Last allowed line: truncate with an ellipsis. */
            take = cols < n ? cols - 2 : take;
            if (take < 0) take = 0;
            if (take > (int)sizeof line - 3) take = (int)sizeof line - 3;
            memcpy(line, s, (size_t)take);
            memcpy(line + take, "..", 3);
            ui_text(u, x, y + (float)(lines * UI_LINE_H * size), size, rgba, line);
            return lines + 1;
        }
        if (take > (int)sizeof line - 1) take = (int)sizeof line - 1;
        memcpy(line, s, (size_t)take);
        line[take] = '\0';
        ui_text(u, x, y + (float)(lines * UI_LINE_H * size), size, rgba, line);
        s += take;
        lines++;
    }
    return lines;
}

void ui_bar(ui *u, float x, float y, float w, float h, float frac, uint32_t fill, uint32_t back)
{
    frac = frac < 0.0f ? 0.0f : (frac > 1.0f ? 1.0f : frac);
    ui_rect(u, x, y, w, h, back);
    ui_rect(u, x, y, w * frac, h, fill);
}
