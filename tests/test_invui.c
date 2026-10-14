/* The inventory screen driven by simulated mouse input: categories that
 * open and close, the "can make" filter, crafting from a row, slot clicks
 * reported for a multiplayer host, and every recipe in one category. */
#include "invui.h"
#include "test_util.h"

#include <math.h>
#include <string.h>

/* The screen's layout (invui.c): panel and crafting column origins. */
typedef struct {
    float x0, y0, cx, top, cw;
} layout;

static layout layout_of(const ui *u)
{
    const float grid_w = INV_HOTBAR * 20.0f + (INV_HOTBAR - 1) * 2.0f, cw = 224, ph = 254;
    const float pw = 8 + grid_w + 12 + cw + 8;
    layout l = {floorf((u->w - pw) * 0.5f), floorf((u->h - ph) * 0.5f), 0, 0, cw};
    l.cx = l.x0 + 8 + grid_w + 12;
    l.top = l.y0 + 24;
    return l;
}

/* One frame with the mouse at (x, y), clicking or not. */
static invui_result frame(ui *u, invui *s, inventory *inv, float x, float y, int click, int stations)
{
    ui_begin(u, NULL, 0, 1920, 1080);
    invui_input in = {x, y, click, 0, 0, stations, 0.0f, 1.0f};
    return invui_screen(u, s, inv, &in);
}

/* The centre of crafting row k. */
static void row_at(const layout *l, int k, float *x, float *y)
{
    *x = l->cx + 60.0f;
    *y = l->top + 14.0f + (float)k * 20.0f + 9.0f;
}

void test_invui_all(void)
{
    /* Every recipe sits in exactly one category; tools and blades where
     * they belong. */
    int per[RCAT_COUNT] = {0}, total = 0;
    for (int r = 0; r < g_recipe_count; r++) {
        int c = invui_recipe_category(r);
        CHECK(c >= 0 && c < RCAT_COUNT);
        if (c >= 0 && c < RCAT_COUNT) per[c]++;
        int kind = 0, tier = 0;
        if (item_tool(g_recipes[r].out.id, &kind, &tier))
            CHECK(c == (kind == TOOL_KNIFE || kind == TOOL_SWORD ? RCAT_WEAPONS : RCAT_TOOLS));
    }
    for (int c = 0; c < RCAT_COUNT; c++) total += per[c];
    CHECK(total == g_recipe_count && per[RCAT_TOOLS] == 12 && per[RCAT_BASICS] > 0);

    ui u = {0};
    invui s = {0};
    invui_init(&s);
    inventory inv = {0};
    inv_init(&inv);
    inv_add(&inv, B_LOG, 2);
    frame(&u, &s, &inv, 0, 0, 0, 0); /* opens: Basics expanded */
    layout l = layout_of(&u);
    CHECK(s.cat_open == 1u << RCAT_BASICS);

    /* Row 0 is the Basics header; row 1 its first recipe (planks from a
     * log): clicking it crafts. */
    float x = 0, y = 0;
    row_at(&l, 1, &x, &y);
    invui_result r = frame(&u, &s, &inv, x, y, 1, 0);
    CHECK(r.crafted >= 0 && g_recipes[r.crafted].out.id == B_PLANKS && inv_count(&inv, B_PLANKS) == 4);

    /* Clicking the header closes Basics; row 1 is then the next header, and
     * clicking it opens that category instead of crafting. */
    row_at(&l, 0, &x, &y);
    frame(&u, &s, &inv, x, y, 1, 0);
    CHECK(!(s.cat_open & (1u << RCAT_BASICS)));
    row_at(&l, 1, &x, &y);
    r = frame(&u, &s, &inv, x, y, 1, 0);
    CHECK(r.crafted < 0 && (s.cat_open & (1u << RCAT_BUILDING)));

    /* The filter: only categories with something to make now remain, and
     * Basics (sticks from planks) is one. */
    const char *chip = "[ ] can make";
    float chip_x = l.cx + l.cw - ui_text_width(chip, 1) - 2;
    frame(&u, &s, &inv, chip_x + 4, l.top + 3, 1, 0);
    CHECK(s.only_craftable == 1);
    s.cat_open = 1u << RCAT_BASICS;
    row_at(&l, 1, &x, &y);
    r = frame(&u, &s, &inv, x, y, 1, 0);
    CHECK(r.crafted >= 0); /* the first recipe shown can be made: clicking it crafts */

    /* A slot click is reported, for a host to repeat. */
    r = frame(&u, &s, &inv, l.x0 + 8 + 5, l.y0 + 24 + 3 * 22 + 6 + 5, 1, 0);
    CHECK(r.click_slot == 0 && r.changed);
}
