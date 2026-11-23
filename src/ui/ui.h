/* Immediate-mode 2D UI: coloured quads and a built-in 5x7 pixel font,
 * written straight into a mapped vertex buffer and drawn in one call.
 *
 * Coordinates are logical pixels. `scale` screen pixels make one logical
 * pixel, chosen so the screen is at least 640x360 logical pixels; at that
 * scale the font is one texel per logical pixel and stays crisp. Every
 * quad is textured from a small R8 atlas: solid fills sample its white
 * corner. No Vulkan here, so layout code is testable. */
#ifndef MC_UI_H
#define MC_UI_H

#include <stdint.h>

#define UI_ATLAS_W 128
#define UI_ATLAS_H 64
#define UI_GLYPH_W 5
#define UI_GLYPH_H 8     /* 7 rows above the baseline, 1 descender row */
#define UI_CELL_W 6      /* advance */
#define UI_LINE_H 10

/* Extra glyphs past ASCII. */
#define UI_CH_DEGREE "\x7f"
#define UI_CH_HEART "\x80"
#define UI_CH_DROP "\x81"

typedef struct {
    float x, y;          /* screen pixels */
    uint16_t u, v;       /* atlas, UNORM */
    uint32_t rgba;       /* R8G8B8A8_UNORM, sRGB-encoded */
} ui_vertex;

typedef struct {
    ui_vertex *v;
    int quads, max_quads;
    int overflow;        /* quads dropped because the buffer was full */
    float scale;         /* screen pixels per logical pixel */
    float w, h;          /* logical size of the screen */
} ui;

static inline uint32_t ui_rgba(int r, int g, int b, int a)
{
    return (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16 | (uint32_t)a << 24;
}

/* Replaces the alpha of a packed colour, a in 0..1. */
static inline uint32_t ui_alpha(uint32_t c, float a)
{
    int ia = (int)(a * (float)(c >> 24) + 0.5f);
    ia = ia < 0 ? 0 : (ia > 255 ? 255 : ia);
    return (c & 0x00ffffffu) | (uint32_t)ia << 24;
}

/* Fills an UI_ATLAS_W x UI_ATLAS_H single-channel atlas. */
void ui_font_build(uint8_t *atlas);

/* The integer scale for a framebuffer: at least 640x360 logical pixels. */
int ui_scale_for(int fb_w, int fb_h);

void ui_begin(ui *u, ui_vertex *mem, int max_quads, int fb_w, int fb_h);
/* Overrides the automatic scale (the GUI scale setting). Clamped so the
 * screen stays at least 480x270 logical pixels; 0 keeps the automatic one. */
void ui_set_scale(ui *u, int scale, int fb_w, int fb_h);
/* Lowers the scale in whole steps, never below 1, until the screen is at
 * least w x h logical pixels: for a view that must fit whole, drawn
 * between saving and restoring scale, w and h. */
void ui_fit(ui *u, float w, float h);

void ui_rect(ui *u, float x, float y, float w, float h, uint32_t rgba);
/* Corner colours clockwise from the top left: gradients for free. */
void ui_rect4(ui *u, float x, float y, float w, float h, uint32_t tl, uint32_t tr, uint32_t br, uint32_t bl);
void ui_frame(ui *u, float x, float y, float w, float h, float t, uint32_t rgba);
void ui_line(ui *u, float x0, float y0, float x1, float y1, float thick, uint32_t rgba);
/* Any convex quad, corners clockwise from the top left (isometric icons). */
void ui_quad(ui *u, const float xy[8], uint32_t rgba);

/* Draws text at integer size `size` (1 = 5x7). '\n' starts a new line.
 * Returns the width of the widest line. */
float ui_text(ui *u, float x, float y, int size, uint32_t rgba, const char *s);
float ui_textf(ui *u, float x, float y, int size, uint32_t rgba, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 6, 7)))
#endif
    ;
float ui_text_width(const char *s, int size);
/* Text with a one-pixel drop shadow, for text over the 3D view. */
float ui_text_shadow(ui *u, float x, float y, int size, uint32_t rgba, const char *s);
/* Word-wraps text into lines at most `width` wide; returns lines drawn. */
int ui_text_wrap(ui *u, float x, float y, float width, int max_lines, int size, uint32_t rgba, const char *s);

/* A horizontal bar: `frac` of it filled. */
void ui_bar(ui *u, float x, float y, float w, float h, float frac, uint32_t fill, uint32_t back);

#endif
