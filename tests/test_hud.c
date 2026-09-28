/* Layout regressions for the health HUD and the H panel: the readouts
 * that sit at a fixed distance from the right edge of their box, and the
 * H panel fitting the screen at every GUI scale. Drawing only reads the
 * body, so these set it to the widest readouts directly rather than
 * playing out a scenario. */
#include "debug.h"
#include "hud.h"
#include "mem.h"
#include "test_util.h"
#include "ui.h"

#include <math.h>
#include <string.h>

typedef struct {
    float x0, y0, x1, y1;
} box;

static const ui_vertex *quad_v(const ui *u, int q) { return u->v + (size_t)q * 4; }

static box quad_box(const ui *u, int q)
{
    const ui_vertex *v = quad_v(u, q);
    box b = {v[0].x, v[0].y, v[0].x, v[0].y};
    for (int k = 1; k < 4; k++) {
        b.x0 = fminf(b.x0, v[k].x);
        b.y0 = fminf(b.y0, v[k].y);
        b.x1 = fmaxf(b.x1, v[k].x);
        b.y1 = fmaxf(b.y1, v[k].y);
    }
    return b;
}

/* Text samples the font; fills sample the atlas's one white texel. */
static int is_glyph(const ui *u, int q) { return quad_v(u, q)[0].u != quad_v(u, q)[1].u; }

/* No quad of a glyph drawn with its baseline in [y0, y1) (screen pixels)
 * reaches past x1 (screen pixels): the fixed-position readouts that used
 * to run off the edge of their backdrop box at wide values. */
static int row_overflows(const ui *u, int mine, float y0, float y1, float x1)
{
    for (int q = 0; q < mine; q++) {
        if (!is_glyph(u, q)) continue;
        box b = quad_box(u, q);
        float cy = (b.y0 + b.y1) * 0.5f;
        if (cy >= y0 && cy < y1 && b.x1 > x1 + 0.5f) return 1;
    }
    return 0;
}

static int any_off_screen(const ui *u, int mine, int fb_w, int fb_h)
{
    for (int q = 0; q < mine; q++) {
        box b = quad_box(u, q);
        if (b.x0 < -0.5f || b.y0 < -0.5f || b.x1 > (float)fb_w + 0.5f || b.y1 > (float)fb_h + 0.5f) return 1;
    }
    return 0;
}

/* The widest the vitals strip ever gets: 3-digit blood pressure both
 * ways, SpO2 and wetness pinned at 100%, and a radiant heat readout. */
static health *widest_vitals(void)
{
    health *h = new_body(90);
    health_env e = calm_env();
    live(h, &e, 1.0);
    h->hr = 190.0f;
    h->sbp = 200.0f;
    h->dbp = 120.0f;
    h->spo2_shown = h->sao2 = 1.0f;
    h->wet = 1.0f;
    h->air_temp = -40.0f;
    h->radiant = 25000.0f;
    return h;
}

typedef struct {
    int w, h, gui;
} screen_size;

static const screen_size SIZES[] = {
    {640, 360, 0}, {800, 600, 0}, {1280, 720, 0}, {1920, 1080, 0}, {960, 540, 2}, {1920, 1080, 4},
};

/* Regression for the SpO2/wet readouts in the vitals strip: they draw
 * right-aligned to the box's own edge now, whatever their width, instead
 * of at a fixed offset that "SpO2 100%" and "Wet 100%" ran past. */
static void test_vitals_fits(void)
{
    health *h = widest_vitals();
    hud_state s;
    memset(&s, 0, sizeof s);
    ui_vertex *mem = mem_alloc(sizeof(ui_vertex) * 4 * 512);
    for (size_t i = 0; i < sizeof SIZES / sizeof SIZES[0]; i++) {
        const screen_size *sz = &SIZES[i];
        for (int debug = 0; debug < 2; debug++) {
            s.debug = debug;
            ui u;
            ui_begin(&u, mem, 512, sz->w, sz->h);
            ui_set_scale(&u, sz->gui, sz->w, sz->h);
            hud_draw(&u, h, &s);
            const float x = 4, y = debug ? DEBUG_LEFT_BOTTOM : 4, w = 152;
            float edge = (x + w) * u.scale;
            CHECK(!row_overflows(&u, u.quads, (y + 2) * u.scale, (y + 11) * u.scale, edge));  /* BP / SpO2 */
            CHECK(!row_overflows(&u, u.quads, (y + 42) * u.scale, (y + 51) * u.scale, edge)); /* Air / Wet */
            CHECK(u.overflow == 0);
        }
    }
    mem_free(mem);
    mem_free(h);
}

/* Regression for the bedside monitor's arterial pressure readout: it used
 * to be 5-6 px narrower than "200/120" needs at size 2, so three-digit
 * systolic and diastolic ran past the monitor's own frame. */
static void test_monitor_bp_fits(void)
{
    health *h = widest_vitals();
    hud_state s;
    memset(&s, 0, sizeof s);
    s.panel = 1;
    ui_vertex *mem = mem_alloc(sizeof(ui_vertex) * 4 * 4096);
    for (size_t i = 0; i < sizeof SIZES / sizeof SIZES[0]; i++) {
        const screen_size *sz = &SIZES[i];
        ui u;
        ui_begin(&u, mem, 4096, sz->w, sz->h);
        ui_set_scale(&u, sz->gui, sz->w, sz->h);
        /* Mirrors panel()'s own fit, to know where the monitor lands. */
        ui_fit(&u, 628.0f + 8.0f, 348.0f + 8.0f);
        float x0 = floorf((u.w - 628.0f) * 0.5f), y0 = floorf((u.h - 348.0f) * 0.5f);
        float mx = x0 + 628.0f - 8.0f - 210.0f, my = y0 + 22.0f;
        float edge = (mx + 210.0f) * u.scale;
        hud_draw(&u, h, &s);
        CHECK(!row_overflows(&u, u.quads, (my + 46.0f + 8.0f) * u.scale, (my + 46.0f + 30.0f) * u.scale, edge));
        CHECK(!any_off_screen(&u, u.quads, sz->w, sz->h));
        CHECK(u.overflow == 0);
    }
    mem_free(mem);
    mem_free(h);
}

/* The HUD starts under the F3 overlay's left column; it must stay above
 * DEBUG_LEFT_BOTTOM with every optional line showing. */
static void test_debug_left_bottom(void)
{
    ui_vertex *mem = mem_alloc(sizeof(ui_vertex) * 4 * 4096);
    debug_frames frames;
    memset(&frames, 0, sizeof frames);
    debug_info d;
    memset(&d, 0, sizeof d);
    d.frames = &frames;
    d.gpu = "GPU";
    d.versions = "versions";
    d.api = "api";
    d.audio = "audio";
    ui u;
    ui_begin(&u, mem, 4096, 1280, 720);
    debug_draw(&u, &d);
    float bottom = 0.0f;
    for (int q = 0; q < u.quads; q++) {
        box b = quad_box(&u, q);
        if (b.x0 < 8.0f * u.scale) bottom = fmaxf(bottom, b.y1 / u.scale); /* the left column's strips */
    }
    CHECK(bottom > 50.0f && bottom <= DEBUG_LEFT_BOTTOM);
    mem_free(mem);
}

/* ui_fit drops to the largest whole scale that holds a view. */
static void test_ui_fit(void)
{
    ui u;
    ui_begin(&u, NULL, 0, 960, 540);
    ui_set_scale(&u, 2, 960, 540);
    CHECK(u.w == 480.0f && u.h == 270.0f);
    ui_fit(&u, 636.0f, 356.0f);
    CHECK(u.scale == 1.0f && u.w == 960.0f && u.h == 540.0f);
    ui_begin(&u, NULL, 0, 1920, 1080);
    ui_set_scale(&u, 4, 1920, 1080);
    ui_fit(&u, 636.0f, 356.0f);
    CHECK(u.scale == 3.0f && u.w == 640.0f && u.h == 360.0f);
    ui_begin(&u, NULL, 0, 1280, 720);
    ui_fit(&u, 636.0f, 356.0f); /* already fits: unchanged */
    CHECK(u.scale == 2.0f && u.w == 640.0f);
    ui_begin(&u, NULL, 0, 500, 300);
    ui_fit(&u, 636.0f, 356.0f); /* too small even at 1: stays at 1 */
    CHECK(u.scale == 1.0f && u.w == 500.0f);
}

void test_hud_all(void)
{
    test_ui_fit();
    test_debug_left_bottom();
    test_vitals_fits();
    test_monitor_bp_fits();
}
