#include "debug.h"
#include "block.h"
#include "mathlib.h"
#include "palette.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void debug_frames_push(debug_frames *f, float ms)
{
    f->ms[f->pos] = ms;
    f->pos = (f->pos + 1) % DEBUG_HIST;
    float fps = ms > 0.01f ? 1000.0f / ms : 0.0f;
    f->fps = f->fps > 0.0f ? f->fps + (fps - f->fps) * 0.05f : fps;
}

const char *debug_facing(float yaw)
{
    static const char *const DIR[8] = {"north", "north-east", "east", "south-east",
                                       "south", "south-west", "west", "north-west"};
    float t = yaw / (float)(MC_PI / 4.0);
    int i = (int)lroundf(t);
    return DIR[((i % 8) + 8) % 8];
}

/* Minecraft-style lines: each on its own translucent strip. */
typedef struct {
    ui *u;
    float y;
    int right;
} column_ctx;

static void line(column_ctx *c, uint32_t col, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

static void line(column_ctx *c, uint32_t col, const char *fmt, ...)
{
    char b[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (b[0]) {
        float w = ui_text_width(b, 1);
        float x = c->right ? c->u->w - 4 - w : 4;
        ui_rect(c->u, x - 2, c->y - 1, w + 4, 10, ui_rgba(0, 0, 0, 130));
        ui_text(c->u, x, c->y, 1, col, b);
    }
    c->y += 10;
}

static void gap(column_ctx *c) { c->y += 4; }

static void frame_graph(ui *u, const debug_frames *f, float x, float y, float w, float h)
{
    ui_rect(u, x, y, w, h, ui_rgba(0, 0, 0, 150));
    /* 60 and 30 fps lines. */
    const float full = 50.0f; /* ms at the top */
    float y60 = y + h - h * (16.67f / full), y30 = y + h - h * (33.3f / full);
    ui_rect(u, x, y60, w, 1, ui_alpha(C_ECG, 0.35f));
    ui_rect(u, x, y30, w, 1, ui_alpha(C_CO2, 0.35f));
    int cols = (int)w;
    float worst = 0.0f, sum = 0.0f;
    for (int c = 0; c < cols; c++) {
        int k = (f->pos - cols + c + DEBUG_HIST * 4) % DEBUG_HIST;
        float ms = f->ms[k];
        worst = ms > worst ? ms : worst;
        sum += ms;
        float bh = h * fminf(ms / full, 1.0f);
        uint32_t col = ms <= 17.5f ? C_ECG : (ms <= 34.0f ? C_CO2 : C_ART);
        ui_rect(u, x + (float)c, y + h - bh, 1, bh, ui_alpha(col, 0.85f));
    }
    char b[64];
    snprintf(b, sizeof b, "avg %.1f  max %.1f ms", (double)(sum / (float)cols), (double)worst);
    ui_text(u, x + 2, y + 2, 1, C_TEXT, b);
    ui_text(u, x + w - 2 - ui_text_width("60", 1), y60 - 9, 1, ui_alpha(C_ECG, 0.8f), "60");
    ui_text(u, x + w - 2 - ui_text_width("30", 1), y30 - 9, 1, ui_alpha(C_CO2, 0.8f), "30");
}

/* World axes seen from the camera, drawn at the crosshair. */
static void axes(ui *u, float yaw, float pitch)
{
    mat4 v = m4_view_rot(yaw, pitch);
    float cx = floorf(u->w * 0.5f), cy = floorf(u->h * 0.5f);
    const uint32_t col[3] = {ui_rgba(235, 70, 70, 255), ui_rgba(70, 225, 90, 255), ui_rgba(80, 140, 255, 255)};
    for (int a = 0; a < 3; a++) {
        /* Column a of the rotation is where world axis a lands in view space. */
        float vx = v.m[a * 4 + 0], vy = v.m[a * 4 + 1];
        ui_line(u, cx, cy, cx + vx * 12.0f, cy - vy * 12.0f, 1, col[a]);
    }
}

void debug_draw(ui *u, const debug_info *d)
{
    column_ctx lc = {u, 4, 0};
    const debug_frames *f = d->frames;
    line(&lc, C_HEAD, "blockclonia  %.0f fps  (%.2f ms, physics %.2f ms)", f ? (double)f->fps : 0.0,
         f && f->fps > 0.0f ? 1000.0 / (double)f->fps : 0.0, (double)d->phys_ms);
    gap(&lc);
    line(&lc, C_TEXT, "XYZ %.3f / %.3f / %.3f", d->x, d->y, d->z);
    int bx = (int)floor(d->x), by = (int)floor(d->y), bz = (int)floor(d->z);
    line(&lc, C_TEXT, "Block %d %d %d   chunk %d %d  in %d %d", bx, by, bz, bx >> 4, bz >> 4, bx & 15, bz & 15);
    line(&lc, C_TEXT, "Facing %s  (yaw %.1f" UI_CH_DEGREE ", pitch %.1f" UI_CH_DEGREE ")", debug_facing(d->yaw),
         (double)d->yaw * 180.0 / MC_PI, (double)d->pitch * 180.0 / MC_PI);
    double hs = sqrt(d->vx * d->vx + d->vz * d->vz);
    line(&lc, C_TEXT, "Velocity %.2f %.2f %.2f m/s  (%.2f horizontal)", d->vx, d->vy, d->vz, hs);
    line(&lc, C_TEXT, "%s%s%s  submerged %.0f%%", d->flying ? "flying" : (d->on_ground ? "on ground" : "airborne"),
         d->sprinting ? ", sprinting" : "", d->sneaking ? ", sneaking" : "", d->submerged * 100.0);
    int mins = (int)(d->day_time * 24.0 * 60.0);
    line(&lc, C_TEXT,
         "Time %02d:%02d   air %.1f" UI_CH_DEGREE "C   skin %.1f" UI_CH_DEGREE "C   core %.2f" UI_CH_DEGREE "C",
         mins / 60, mins % 60, (double)d->air_temp, (double)d->feels_like, (double)d->body_temp);
    gap(&lc);
    if (d->has_target) {
        const block_def *b = block_get(d->target_id);
        line(&lc, C_ECG, "Target %s at %d %d %d", b->name, d->tx, d->ty, d->tz);
        line(&lc, C_TEXT, "  density %.0f kg/m3  friction %.2f  span %d", (double)b->density, (double)b->friction,
             (int)b->span);
        line(&lc, C_TEXT, "  restitution %.2f  hardness %.1f s  c %.0f J/kgK  k %.2f W/mK", (double)b->restitution,
             (double)b->hardness, (double)b->heat_capacity, (double)b->conductivity);
        line(&lc, C_TEXT, "  temperature %.1f" UI_CH_DEGREE "C", (double)d->target_temp);
        if (d->target_level) line(&lc, C_TEXT, "  water level %d/8", d->target_level);
        if (d->break_progress > 0.0f) line(&lc, C_CO2, "  breaking %.0f%%", (double)d->break_progress * 100.0);
    } else {
        line(&lc, C_DIM, "Target: none");
    }

    column_ctx rc = {u, 4, 1};
    line(&rc, C_TEXT, "%s", d->versions ? d->versions : "");
    line(&rc, C_TEXT, "GPU %s", d->gpu ? d->gpu : "?");
    if (d->api) line(&rc, C_DIM, "%s", d->api);
    if (d->gpu_ms > 0.0f) line(&rc, C_TEXT, "GPU frame %.2f ms", (double)d->gpu_ms);
    if (d->vram_budget_mb)
        line(&rc, C_TEXT, "VRAM %u / %u MB (budget)", d->vram_used_mb, d->vram_budget_mb);
    line(&rc, C_TEXT, "Display %dx%d  UI scale %.0f  quads %d", d->width, d->height, (double)u->scale, d->ui_quads);
    gap(&rc);
    if (d->rss || d->commit)
        line(&rc, C_TEXT, "Memory rss %.1f MB  committed %.1f MB", (double)d->rss / 1048576.0,
             (double)d->commit / 1048576.0);
    line(&rc, C_TEXT, "Mesh pool %.1f / %.0f MB", (double)d->pool_used_kb / 1024.0, (double)d->pool_total_kb / 1024.0);
    line(&rc, C_TEXT, "Draws %d  sections %d  quads %u", d->draw_calls, d->sections, d->quads);
    gap(&rc);
    line(&rc, C_TEXT, "Seed %u  radius %d  workers %d", d->seed, d->radius, d->threads);
    line(&rc, C_TEXT, "Bodies %d  items %d  particles %d", d->bodies, d->items, d->particles);
    line(&rc, C_TEXT, "Fluid cells %d/tick  heat cells %d  fires %d", d->fluid_updates, d->heat_cells, d->fires);
    if (d->last_collapse) line(&rc, C_TEXT, "Last collapse %d blocks", d->last_collapse);
    gap(&rc);
    line(&rc, C_TEXT, "Sound %s  voices %d  mixer %.1f%%", d->audio ? d->audio : "off", d->snd_voices,
         (double)d->snd_load * 100.0);

    if (f) frame_graph(u, f, u->w - 184, u->h - 52, 180, 48);
    axes(u, d->yaw, d->pitch);
}
