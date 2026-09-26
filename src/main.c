/* blockclonia: entry point, input, and the fixed-timestep game loop. */
#include "anim.h"
#include "bench.h"
#include "camera.h"
#include "debug.h"
#include "fx.h"
#include "health.h"
#include "hud.h"
#include "interact.h"
#include "inventory.h"
#include "invui.h"
#include "jobs.h"
#include "log.h"
#include "mem.h"
#include "menu.h"
#include "mesher.h"
#include "physics.h"
#include "renderer.h"
#include "save.h"
#include "settings.h"
#include "survival.h"
#include "thermo.h"
#include "ui.h"
#include "viewmodel.h"
#include "world.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define REACH 5.0
#define MAX_STEPS_PER_FRAME 5
#define MESH_UPLOADS_PER_FRAME 128
#define AUTOSAVE_SECONDS 60.0

typedef struct {
    uint32_t seed;
    int have_seed;
    int radius, width, height, vsync, validate, gpu, threads;
    uint32_t pool_mb;
    const char *world_dir;
    int frames;
    const char *screenshot;
    int bench, demo, mem_stats;
    int open_panel;      /* start with the health panel open (screenshots) */
    const char *hurt;    /* injuries to start with (screenshots, testing) */
    int have_look, have_spawn;
    float look_yaw, look_pitch;
    int spawn_x, spawn_z;
    int have_radius, no_vsync;
    const char *config;  /* settings file */
    int play;            /* skip the title screen */
    const char *screen;  /* open this screen at start (screenshots) */
    int debug;           /* start with F3 on */
    const char *give;    /* items to start with: "stone:32,apple:3" */
    int have_time;       /* --time */
    double time_of_day;  /* 0..1 */
} options;

typedef struct {
    int keys[GLFW_KEY_LAST + 1];
    double mouse_dx, mouse_dy, last_x, last_y;
    int have_mouse, captured;
    int click_break, click_place, click_pick;
    int attack;         /* left button held in the game */
    int toggle_fly, want_shot, resized;
    int slot;           /* hotbar slot chosen with a number key, -1 none */
    int inv_open;       /* inventory screen (Tab / I) */
    int drop;           /* Q: 1 one item, 2 the whole stack */
    int ui_rclick;      /* right button pressed, for the inventory screen */
    double scroll;
    int panel;          /* health panel open */
    int sel_part;       /* body part selected in the panel */
    int treat;          /* treatment requested, -1 none */
    int respawn;
    int screen;         /* menu screen, SCREEN_NONE while playing */
    int debug;          /* F3 overlay */
    int lost_focus;     /* the window lost focus: pause */
    int sprint_latch;   /* toggle-sprint mode: Ctrl flips this */
    int sprint_tap;     /* W double-tapped: sprint until W is released */
    double last_w;      /* time of the last W press */
    int sprint_toggle;  /* copy of the setting, for the key callback */
    /* Menu input, gathered by the callbacks and consumed once per frame. */
    int ui_click, ui_down, ui_up, ui_dn, ui_left, ui_right, ui_enter, ui_back;
} input_state;

static input_state g_in;


/* ---------------------------------------------------------- arguments */

/* long long: `long` is 32-bit on 32-bit ARM, where (long)UINT32_MAX is -1. */
static int parse_int(const char *s, long long lo, long long hi, long long *out)
{
    char *end;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (errno || end == s || *end || v < lo || v > hi) return 0;
    *out = v;
    return 1;
}

/* "<a><sep><b>" with both integers in [lo, hi], e.g. 1280x720 or -40,12. */
static int parse_int_pair(const char *s, char sep, long long lo, long long hi, long long *a, long long *b)
{
    char *end;
    errno = 0;
    long long x = strtoll(s, &end, 10);
    if (errno || end == s || *end != sep || x < lo || x > hi) return 0;
    const char *t = end + 1;
    long long y = strtoll(t, &end, 10);
    if (errno || end == t || *end || y < lo || y > hi) return 0;
    *a = x;
    *b = y;
    return 1;
}

/* "<a>,<b>" with both finite floats of magnitude at most `lim`. */
static int parse_float_pair(const char *s, float lim, float *a, float *b)
{
    char *end;
    errno = 0;
    float x = strtof(s, &end);
    if (errno || end == s || *end != ',' || !(fabsf(x) <= lim)) return 0;
    const char *t = end + 1;
    float y = strtof(t, &end);
    if (errno || end == t || *end || !(fabsf(y) <= lim)) return 0;
    *a = x;
    *b = y;
    return 1;
}

static void usage(void)
{
    printf("usage: blockclonia [options]\n"
           "  --seed N          world seed (default: random, or the saved world's)\n"
           "  --radius N        render distance in chunks, 2-32 (default 8)\n"
           "  --size WxH        window size (default 1280x720)\n"
           "  --no-vsync        uncapped frame rate\n"
           "  --threads N       worker threads (default: cores - 1)\n"
           "  --pool-mb N       GPU vertex pool size (default: from radius)\n"
           "  --gpu N           Vulkan device index\n"
           "  --world DIR       save directory (default ./world)\n"
           "  --no-save         do not load or save the world\n"
           "  --validate        enable Vulkan validation layers\n"
           "  --frames N        quit after N frames\n"
           "  --screenshot F    write a PPM of the last frame to F\n"
           "  --look YAW,PITCH  initial view angles in degrees\n"
           "  --spawn X,Z       spawn position (default: nearest dry land)\n"
           "  --config FILE     settings file (default ./blockclonia.cfg)\n"
           "  --play            skip the title screen\n"
           "  --screen NAME     open title, pause, settings or controls at start\n"
           "  --debug           start with the F3 overlay on\n"
           "  --give LIST       start with items, e.g. log:8,sand:4 (names as shown in game)\n"
           "  --time HH         start at this hour of the day (0-23)\n"
           "  --demo            scripted structural-collapse demo\n"
           "  --health-panel    start with the health panel (H) open\n"
           "  --hurt LIST       start injured: comma list of bleed, artery, fracture,\n"
           "                    open-fracture, infection, concussion, burn, dislocation,\n"
           "                    abrasion, crush\n"
           "  --bench           run CPU benchmarks and exit (no window)\n"
           "  --mem-stats       print mimalloc statistics at exit\n");
}

static int parse_args(int argc, char **argv, options *o)
{
    memset(o, 0, sizeof *o);
    o->radius = 8;
    o->width = 1280;
    o->height = 720;
    o->vsync = 1;
    o->gpu = -1;
    o->threads = -1;
    o->world_dir = "world";
    o->config = "blockclonia.cfg";
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        long long n;
#define NEED_VALUE() do { if (!v) { fprintf(stderr, "%s needs a value\n", a); return 0; } i++; } while (0)
        if (!strcmp(a, "--seed")) {
            NEED_VALUE();
            if (!parse_int(v, 0, (long long)UINT32_MAX, &n)) goto bad;
            o->seed = (uint32_t)n;
            o->have_seed = 1;
        } else if (!strcmp(a, "--radius")) {
            NEED_VALUE();
            if (!parse_int(v, 2, 32, &n)) goto bad;
            o->radius = (int)n;
            o->have_radius = 1;
        } else if (!strcmp(a, "--size")) {
            NEED_VALUE();
            long long w, h;
            if (!parse_int_pair(v, 'x', 64, 16384, &w, &h)) goto bad;
            o->width = (int)w;
            o->height = (int)h;
        } else if (!strcmp(a, "--no-vsync")) {
            o->vsync = 0;
            o->no_vsync = 1;
        } else if (!strcmp(a, "--config")) {
            NEED_VALUE();
            if (strlen(v) >= 400) goto bad;
            o->config = v;
        } else if (!strcmp(a, "--play")) {
            o->play = 1;
        } else if (!strcmp(a, "--debug")) {
            o->debug = 1;
        } else if (!strcmp(a, "--time")) {
            NEED_VALUE();
            if (!parse_int(v, 0, 23, &n)) goto bad;
            o->time_of_day = (double)n / 24.0;
            o->have_time = 1;
        } else if (!strcmp(a, "--give")) {
            NEED_VALUE();
            o->give = v;
        } else if (!strcmp(a, "--screen")) {
            NEED_VALUE();
            if (strcmp(v, "title") && strcmp(v, "pause") && strcmp(v, "settings") && strcmp(v, "controls") &&
                strcmp(v, "game") && strcmp(v, "inventory"))
                goto bad;
            o->screen = v;
        } else if (!strcmp(a, "--threads")) {
            NEED_VALUE();
            if (!parse_int(v, 0, 64, &n)) goto bad;
            o->threads = (int)n;
        } else if (!strcmp(a, "--pool-mb")) {
            NEED_VALUE();
            if (!parse_int(v, 1, 512, &n)) goto bad;
            o->pool_mb = (uint32_t)n;
        } else if (!strcmp(a, "--gpu")) {
            NEED_VALUE();
            if (!parse_int(v, 0, 64, &n)) goto bad;
            o->gpu = (int)n;
        } else if (!strcmp(a, "--world")) {
            NEED_VALUE();
            if (strlen(v) >= 200) goto bad;
            o->world_dir = v;
        } else if (!strcmp(a, "--no-save")) {
            o->world_dir = NULL;
        } else if (!strcmp(a, "--validate")) {
            o->validate = 1;
        } else if (!strcmp(a, "--frames")) {
            NEED_VALUE();
            if (!parse_int(v, 1, INT_MAX, &n)) goto bad;
            o->frames = (int)n;
        } else if (!strcmp(a, "--screenshot")) {
            NEED_VALUE();
            o->screenshot = v;
        } else if (!strcmp(a, "--look")) {
            NEED_VALUE();
            float y, p;
            if (!parse_float_pair(v, 3600.0f, &y, &p)) goto bad;
            o->look_yaw = y;
            o->look_pitch = p;
            o->have_look = 1;
        } else if (!strcmp(a, "--spawn")) {
            NEED_VALUE();
            long long x, z;
            if (!parse_int_pair(v, ',', -(WORLD_LIMIT - 64), WORLD_LIMIT - 64, &x, &z)) goto bad;
            o->spawn_x = (int)x;
            o->spawn_z = (int)z;
            o->have_spawn = 1;
        } else if (!strcmp(a, "--demo")) {
            o->demo = 1;
        } else if (!strcmp(a, "--health-panel")) {
            o->open_panel = 1;
        } else if (!strcmp(a, "--hurt")) {
            NEED_VALUE();
            o->hurt = v;
        } else if (!strcmp(a, "--bench")) {
            o->bench = 1;
        } else if (!strcmp(a, "--mem-stats")) {
            o->mem_stats = 1;
        } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage();
            exit(0);
        } else {
            fprintf(stderr, "unknown option %s\n", a);
            usage();
            return 0;
        }
        continue;
    bad:
        fprintf(stderr, "invalid value for %s: %s\n", a, v);
        return 0;
#undef NEED_VALUE
    }
    return 1;
}

/* -------------------------------------------------------------- input */

static void set_captured(GLFWwindow *win, int on)
{
    if (on == g_in.captured) return;
    glfwSetInputMode(win, GLFW_CURSOR, on ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    g_in.captured = on;
    g_in.have_mouse = 0;
}

/* Keys while a menu is open: navigation only. */
static void menu_key(int key, int action)
{
    if (action != GLFW_PRESS && action != GLFW_REPEAT) return;
    switch (key) {
    case GLFW_KEY_UP: case GLFW_KEY_W: g_in.ui_up = 1; break;
    case GLFW_KEY_DOWN: case GLFW_KEY_S: case GLFW_KEY_TAB: g_in.ui_dn = 1; break;
    case GLFW_KEY_LEFT: case GLFW_KEY_A: g_in.ui_left = 1; break;
    case GLFW_KEY_RIGHT: case GLFW_KEY_D: g_in.ui_right = 1; break;
    case GLFW_KEY_ENTER: case GLFW_KEY_KP_ENTER: case GLFW_KEY_SPACE: g_in.ui_enter = 1; break;
    case GLFW_KEY_ESCAPE: g_in.ui_back = 1; break;
    default: break;
    }
}

static void key_cb(GLFWwindow *win, int key, int sc, int action, int mods)
{
    (void)sc; (void)win;
    if (key < 0 || key > GLFW_KEY_LAST) return;
    if (action == GLFW_PRESS) g_in.keys[key] = 1;
    else if (action == GLFW_RELEASE) g_in.keys[key] = 0;
    if (action == GLFW_PRESS && key == GLFW_KEY_F2) g_in.want_shot = 1;
    if (action == GLFW_PRESS && key == GLFW_KEY_F3 && g_in.screen != SCREEN_TITLE) g_in.debug = !g_in.debug;
    if (g_in.screen != SCREEN_NONE) {
        menu_key(key, action);
        return;
    }
    if (action != GLFW_PRESS) return;
    if (key == GLFW_KEY_W) {
        double t = glfwGetTime();
        if (t - g_in.last_w < 0.28) g_in.sprint_tap = 1;
        g_in.last_w = t;
    }
    if (key == GLFW_KEY_LEFT_CONTROL && g_in.sprint_toggle) g_in.sprint_latch = !g_in.sprint_latch;
    if (key == GLFW_KEY_ESCAPE) {
        /* Esc closes whatever is open first, and only then pauses. */
        if (g_in.inv_open) g_in.inv_open = 0;
        else if (g_in.panel) g_in.panel = 0;
        else g_in.screen = SCREEN_PAUSE;
    } else if (key == GLFW_KEY_TAB || key == GLFW_KEY_I) {
        g_in.inv_open = !g_in.inv_open;
        g_in.panel = 0;
    } else if (g_in.inv_open) {
        /* The inventory screen is mouse driven; number keys still pick a slot. */
        if (key >= GLFW_KEY_1 && key <= GLFW_KEY_9) g_in.slot = key - GLFW_KEY_1;
    } else if (key == GLFW_KEY_H) {
        g_in.panel = !g_in.panel;
    } else if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) {
        g_in.respawn = 1;
    } else if (key == GLFW_KEY_E) {
        g_in.treat = TREAT_EAT;
    } else if (key == GLFW_KEY_R) {
        g_in.treat = TREAT_DRINK;
    } else if (g_in.panel) {
        /* The panel is keyboard driven; movement is paused while it is open. */
        if (key >= GLFW_KEY_1 && key < GLFW_KEY_1 + BP_COUNT) g_in.sel_part = key - GLFW_KEY_1;
        else if (key == GLFW_KEY_UP) g_in.sel_part = (g_in.sel_part + BP_COUNT - 1) % BP_COUNT;
        else if (key == GLFW_KEY_DOWN) g_in.sel_part = (g_in.sel_part + 1) % BP_COUNT;
        else if (key == GLFW_KEY_B) g_in.treat = TREAT_BANDAGE;
        else if (key == GLFW_KEY_S) g_in.treat = TREAT_SPLINT;
        else if (key == GLFW_KEY_D) g_in.treat = TREAT_DISINFECT;
        else if (key == GLFW_KEY_P) g_in.treat = TREAT_PAINKILLER;
        else if (key == GLFW_KEY_A) g_in.treat = TREAT_ANTIBIOTIC;
        else if (key == GLFW_KEY_C) g_in.treat = TREAT_COOL;
        else if (key == GLFW_KEY_X) g_in.treat = TREAT_REDUCE;
    } else if (key == GLFW_KEY_F) {
        g_in.toggle_fly = 1;
    } else if (key == GLFW_KEY_Q) {
        g_in.drop = (mods & GLFW_MOD_CONTROL) ? 2 : 1;
    } else if (key >= GLFW_KEY_1 && key <= GLFW_KEY_9) {
        g_in.slot = key - GLFW_KEY_1;
    }
}

static void mouse_button_cb(GLFWwindow *win, int button, int action, int mods)
{
    (void)mods;
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        if (action == GLFW_PRESS) g_in.ui_click = 1;
        g_in.ui_down = action == GLFW_PRESS;
        if (action == GLFW_RELEASE) g_in.attack = 0;
    }
    if (button == GLFW_MOUSE_BUTTON_RIGHT && action == GLFW_PRESS) g_in.ui_rclick = 1;
    if (action != GLFW_PRESS || g_in.screen != SCREEN_NONE || g_in.inv_open) return;
    if (!g_in.captured) {
        set_captured(win, 1); /* this click only takes the mouse */
        return;
    }
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        g_in.click_break = 1;
        g_in.attack = 1;
    }
    else if (button == GLFW_MOUSE_BUTTON_RIGHT) g_in.click_place = 1;
    else if (button == GLFW_MOUSE_BUTTON_MIDDLE) g_in.click_pick = 1;
}

static void cursor_cb(GLFWwindow *win, double x, double y)
{
    (void)win;
    if (g_in.have_mouse && g_in.captured) {
        g_in.mouse_dx += x - g_in.last_x;
        g_in.mouse_dy += y - g_in.last_y;
    }
    g_in.last_x = x;
    g_in.last_y = y;
    g_in.have_mouse = 1;
}

static void scroll_cb(GLFWwindow *win, double dx, double dy)
{
    (void)win; (void)dx;
    g_in.scroll += dy;
}

static void resize_cb(GLFWwindow *win, int w, int h)
{
    (void)win; (void)w; (void)h;
    g_in.resized = 1;
}

static void focus_cb(GLFWwindow *win, int focused)
{
    (void)win;
    if (!focused) g_in.lost_focus = 1;
}

/* --------------------------------------------------------------- game */

typedef struct {
    world *w;
    physics *ph;
    const player *pl;
    const inventory *inv;
    const thermo *th;
    const char *dir;
} save_ctx;

static void save_player(const save_ctx *sc)
{
    if (!sc->dir) return;
    char path[512];
    uint8_t buf[PLAYER_FILE_SIZE];
    snprintf(path, sizeof path, "%s/player.dat", sc->dir);
    size_t n = survival_encode_player(sc->pl, sc->inv, thermo_day_time(sc->th), buf, sizeof buf);
    if (!n || save_write_file(path, buf, n) != 0) log_warn("could not save %s", path);
}

/* 1 if a saved player was loaded into pl and inv. */
static int load_player(const char *dir, player *pl, inventory *inv, double *day)
{
    if (!dir) return 0;
    char path[512];
    uint8_t buf[PLAYER_FILE_SIZE + 1];
    snprintf(path, sizeof path, "%s/player.dat", dir);
    long n = save_read_file(path, buf, sizeof buf);
    if (n < 0) return 0;
    if (survival_decode_player(pl, inv, day, buf, (size_t)n) != 0) {
        log_warn("%s is corrupt; starting fresh", path);
        return 0;
    }
    return 1;
}

/* A fatal error (device lost, surface lost) must not take the player's
 * edits with it: the world lives in CPU memory and is still intact. */
static void save_on_fatal(void *user)
{
    save_ctx *sc = user;
    physics_settle_bodies(sc->ph, NULL);
    world_save_all(sc->w);
    save_player(sc);
}

/* --give: "name:count" pairs; names match case-insensitively, spaces as
 * underscores ("plant_fibre:4"). */
static void give_items(inventory *inv, const char *list)
{
    char buf[256];
    snprintf(buf, sizeof buf, "%s", list);
    char *save = NULL;
    for (char *t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        char *colon = strchr(t, ':');
        long long n = 1;
        if (colon) {
            *colon = '\0';
            if (!parse_int(colon + 1, 1, 64 * INV_SLOTS, &n)) n = 1;
        }
        int found = 0;
        for (int id = 1; id < ITEM_END && !found; id++) {
            if (!item_valid(id)) continue;
            const char *name = item_get(id)->name;
            size_t k = 0;
            for (; name[k] && t[k]; k++) {
                char a = (char)tolower((unsigned char)name[k]), b = t[k] == '_' ? ' ' : (char)tolower((unsigned char)t[k]);
                if (a != b) break;
            }
            if (!name[k] && !t[k]) {
                inv_add(inv, id, (int)n);
                found = 1;
            }
        }
        if (!found) log_warn("--give: no item called '%s'", t);
    }
}

static int accept_item(void *user, int id, int count)
{
    inventory *inv = user;
    return count - inv_add(inv, id, count);
}

/* Everything carried falls where the player died. */
static void drop_everything(physics *ph, inventory *inv, const player *pl, uint32_t *rng)
{
    item_stack left = inv_return_cursor(inv);
    if (left.count) inv_add(inv, left.id, left.count);
    for (int i = 0; i < INV_SLOTS; i++) {
        item_stack *st = &inv->slot[i];
        if (!st->count) continue;
        *rng = *rng * 1664525u + 1013904223u;
        double a = (double)(*rng >> 8) / 16777216.0 * 2.0 * MC_PI;
        physics_drop_item(ph, dv3(pl->pos.x, pl->pos.y + 1.0, pl->pos.z), dv3(cos(a) * 2.0, 3.0, sin(a) * 2.0), st->id,
                          st->count, 1.0f);
        *st = (item_stack){0, 0};
    }
}

static void find_spawn(uint32_t seed, double *sx, double *sz)
{
    /* Walk outwards until we find dry land. */
    for (int r = 0; r < 4096; r += 16)
        for (int a = 0; a < 16; a++) {
            double ang = a * (2 * MC_PI / 16);
            int x = (int)(cos(ang) * r), z = (int)(sin(ang) * r);
            if (worldgen_height(seed, x, z) > SEA_LEVEL + 2) {
                *sx = x + 0.5;
                *sz = z + 0.5;
                return;
            }
        }
    *sx = *sz = 0.5;
}

/* Scripted demo: builds a stone tower with a long timber cantilever in
 * front of the player, then knocks out the tower's middle. */
typedef struct { int bx, by, bz, built; } demo_state;

/* The world has one block-change hook; physics and the heat field both
 * need it. */
typedef struct {
    physics *ph;
    thermo *th;
    const world *w;
} world_hooks;

static void on_block_changed(void *user, int x, int y, int z, uint8_t old_id, uint8_t new_id)
{
    world_hooks *h = user;
    physics_on_block_changed(h->ph, x, y, z, old_id, new_id);
    thermo_on_block_changed(h->th, h->w, x, y, z, old_id, new_id);
}

static void on_column_unload(void *user, const column *c)
{
    world_hooks *h = user;
    physics_on_column_unload(h->ph, c);
}

/* What the skin is exposed to: air, fires, the ground under the feet. */
static void thermal_env(const thermo *th, const world *w, const player *p, health_env *e)
{
    dvec3 mid = dv3(p->pos.x, p->pos.y + 0.9, p->pos.z);
    double air = (double)thermo_air(th, w, mid.x, mid.y, mid.z);
    double radiant = p->flying ? 0.0 : thermo_radiant(th, w, mid);
    int fx = (int)floor(p->pos.x), fz = (int)floor(p->pos.z), fy = (int)floor(p->pos.y - 0.05);
    double contact = block_solid(world_get(w, fx, fy, fz)) ? (double)thermo_block_temp(th, w, fx, fy, fz) : air;
    survival_env_thermal(e, air, radiant, contact, 0);
}

/* --hurt: start with a chosen set of injuries, for screenshots and for
 * trying treatments without having to get hurt first. */
static void apply_hurt(health *h, const char *list)
{
    char buf[256];
    snprintf(buf, sizeof buf, "%s", list);
    char *save = NULL;
    for (char *t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        if (!strcmp(t, "bleed")) health_cut(h, BP_LLEG, 0.6f, 0, 0.3f);
        else if (!strcmp(t, "artery")) health_cut(h, BP_LLEG, 0.7f, 1, 0.2f);
        else if (!strcmp(t, "fracture")) health_break_bone(h, BP_RARM, 0);
        else if (!strcmp(t, "open-fracture")) health_break_bone(h, BP_RLEG, 1);
        else if (!strcmp(t, "concussion")) health_blunt(h, BP_HEAD, 12.0);
        else if (!strcmp(t, "burn")) health_burn(h, BP_LARM, 5.0);
        else if (!strcmp(t, "dislocation")) health_dislocate(h, BP_RARM);
        else if (!strcmp(t, "abrasion")) health_abrasion(h, BP_LLEG, 0.3f, 0.7f);
        else if (!strcmp(t, "crush")) health_crush(h, BP_LLEG, 400.0, 600.0);
        else if (!strcmp(t, "infection")) {
            health_cut(h, BP_LARM, 0.5f, 0, 0.9f);
            h->wounds[h->wound_count - 1].infection = 0.45f;
        } else log_warn("--hurt: unknown injury '%s'", t);
    }
}

static void demo_step(world *w, physics *ph, fx_state *fx, const player *p, demo_state *d, int frame)
{
    if (frame == 20) {
        /* A scatter of dropped items and a burst of debris in view. */
        static const int IDS[] = {B_LOG, I_APPLE, I_BANDAGE, B_GLASS, I_STICK, B_SAND, I_WATER_BUCKET, B_BRICK};
        for (int i = 0; i < 8; i++) {
            double a = -0.9 + 0.25 * i;
            physics_drop_item(ph, dv3(p->pos.x + sin(a) * 3.0, p->pos.y + 1.5, p->pos.z - cos(a) * 3.0),
                              dv3(0, 1.0, 0), IDS[i], 1 + i, 10.0f);
        }
    }
    if (frame == 100) fx_break(fx, B_GRASS, (int)floor(p->pos.x), (int)floor(p->pos.y), (int)floor(p->pos.z) - 3, 14);
    if (frame == 25) {
        /* A campfire with a block of ice and one of snow beside it. */
        int x = (int)floor(p->pos.x) - 3, z = (int)floor(p->pos.z) - 4;
        int y = world_surface_y(w, x, z);
        if (y > 0 && y + 1 < WORLD_H) {
            world_set(w, x, y, z, B_CAMPFIRE, 0);
            world_set(w, x + 1, y, z, B_ICE, 0);
            world_set(w, x - 1, y, z, B_SNOW, 0);
        }
    }
    if (frame == 30) {
        d->bx = (int)floor(p->pos.x) + 4;
        d->bz = (int)floor(p->pos.z) - 10;
        d->by = world_surface_y(w, d->bx, d->bz);
        if (d->by < 0 || d->by + 8 >= WORLD_H) return;
        for (int y = d->by; y < d->by + 8; y++) world_set(w, d->bx, y, d->bz, B_STONE, 0);
        for (int i = 1; i <= 5; i++) world_set(w, d->bx - i, d->by + 7, d->bz, B_PLANKS, 0);
        for (int i = 1; i <= 3; i++) world_set(w, d->bx - 5, d->by + 7 - i, d->bz, B_GLASS, 0);
        d->built = 1;
        log_info("demo: built tower and cantilever at %d,%d,%d", d->bx, d->by, d->bz);
    } else if (frame == 90 && d->built) {
        world_set(w, d->bx, d->by + 3, d->bz, B_AIR, 0);
        log_info("demo: removed the tower's middle block");
    }
}

int main(int argc, char **argv)
{
    options o;
    if (!parse_args(argc, argv, &o)) return 2;
    mem_init();
    mesher_init();

    settings st;
    settings_default(&st);
    if (settings_load(&st, o.config) == 0) log_info("settings from %s", o.config);
    if (!o.have_radius) o.radius = st.render_distance;
    if (!o.no_vsync) o.vsync = st.vsync;
    else st.vsync = 0;

    if (o.bench) {
        int r = bench_run(o.have_seed ? o.seed : 1337u);
        if (o.mem_stats) mem_print_stats();
        return r;
    }

    uint32_t seed = o.have_seed ? o.seed : (uint32_t)time(NULL) * 2654435761u;
    if (o.world_dir) {
        if (save_ensure_dir(o.world_dir) != 0) {
            log_error("cannot create world directory '%s'; saving disabled", o.world_dir);
            o.world_dir = NULL;
        } else {
            uint32_t saved;
            if (save_read_seed(o.world_dir, &saved) == 0) {
                if (o.have_seed && saved != o.seed) log_warn("--seed ignored: world '%s' uses seed %u", o.world_dir, saved);
                seed = saved;
            } else if (save_write_seed(o.world_dir, seed) != 0) {
                log_warn("could not write level.dat");
            }
        }
    }
    log_info("seed %u", seed);

    if (!glfwInit()) log_fatal("glfwInit failed");
    if (!glfwVulkanSupported()) log_fatal("no Vulkan loader/driver found");
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow *win = glfwCreateWindow(o.width, o.height, "blockclonia", NULL, NULL);
    if (!win) log_fatal("could not create window");
    glfwSetKeyCallback(win, key_cb);
    glfwSetMouseButtonCallback(win, mouse_button_cb);
    glfwSetCursorPosCallback(win, cursor_cb);
    glfwSetScrollCallback(win, scroll_cb);
    glfwSetFramebufferSizeCallback(win, resize_cb);
    glfwSetWindowFocusCallback(win, focus_cb);
    if (glfwRawMouseMotionSupported()) glfwSetInputMode(win, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);

    int workers = o.threads >= 0 ? o.threads : cpu_count() - 1;
    if (workers < 1 && o.threads < 0) workers = 1;
    if (workers > 8 && o.threads < 0) workers = 8;
    jobs *js = jobs_create(workers);

    render_opts ro = {.vsync = o.vsync, .validate = o.validate, .render_radius = o.radius,
                      .gpu_index = o.gpu, .pool_mb = o.pool_mb, .screenshots = o.screenshot != NULL};
    renderer *rd = renderer_create(win, &ro);

    world w;
    world_init(&w, seed, o.radius, js, o.world_dir);
    physics ph;
    physics_init(&ph, &w);
    thermo *th = thermo_create(seed);
    world_hooks hooks = {&ph, th, &w};
    w.edit_user = &hooks;
    w.on_block_changed = on_block_changed;
    w.on_column_unload = on_column_unload;
    save_ctx sc = {&w, &ph, NULL, NULL, th, NULL};
    log_set_fatal_hook(save_on_fatal, &sc);
    renderer_bind_world(rd, &w);

    double sx, sz;
    if (o.have_spawn) {
        sx = o.spawn_x + 0.5;
        sz = o.spawn_z + 0.5;
    } else {
        find_spawn(seed, &sx, &sz);
    }
    world_load_blocking(&w, sx, sz, 1);
    player pl;
    player_spawn(&pl, &w, sx, sz);
    if (o.have_look) {
        pl.yaw = o.look_yaw * (float)(MC_PI / 180.0);
        pl.pitch = clampf(o.look_pitch, -89.0f, 89.0f) * (float)(MC_PI / 180.0);
    }
    inventory inv;
    inv_starting_kit(&inv);
    double day = THERMO_DAY_START;
    if (!o.have_spawn && !o.demo && load_player(o.world_dir, &pl, &inv, &day)) {
        thermo_set_day_time(th, day);
        world_load_blocking(&w, pl.pos.x, pl.pos.z, 1);
        log_info("player restored from %s/player.dat", o.world_dir);
    }
    if (o.give) give_items(&inv, o.give);
    if (o.have_time) thermo_set_day_time(th, o.time_of_day);
    sc.pl = &pl;
    sc.inv = &inv;
    sc.dir = o.world_dir;
    log_info("spawn at %.1f %.1f %.1f with %d worker threads", pl.pos.x, pl.pos.y, pl.pos.z,
             jobs_worker_count(js));
    fx_state *fx = mem_alloc(sizeof *fx);
    fx_init(fx, seed);
    interact ia;
    interact_init(&ia, seed ^ 0xA5A5u);
    invui iu;
    invui_init(&iu);
    entity_instance *ents = mem_alloc(sizeof(entity_instance) * RENDER_MAX_ENTS);
    fx_crack crack = {0};
    viewmodel vm;
    viewmodel_init(&vm);
    entity_instance vm_ents[VIEWMODEL_MAX];
    float eat_anim = 0.0f;
    uint32_t death_rng = seed;

    static health hl; /* 23 KB of waveform history: keep it off the stack */
    health_init(&hl, seed ^ 0x9E3779B9u);
    if (o.hurt) apply_hurt(&hl, o.hurt);
    hud_state hs;
    memset(&hs, 0, sizeof hs);
    g_in.treat = -1;
    g_in.slot = -1;
    g_in.panel = o.open_panel;
    g_in.debug = o.debug;
    int actions = 0; /* blocks broken or placed since the last physics step */

    /* Title screen first, unless a scripted start asks for the game. */
    menu mn;
    menu_init(&mn);
    int skip_title = o.play || o.demo || o.open_panel || o.hurt;
    g_in.screen = skip_title ? SCREEN_NONE : SCREEN_TITLE;
    if (o.screen) {
        if (!strcmp(o.screen, "title")) g_in.screen = SCREEN_TITLE;
        else if (!strcmp(o.screen, "pause")) g_in.screen = SCREEN_PAUSE;
        else if (!strcmp(o.screen, "settings")) g_in.screen = SCREEN_SETTINGS;
        else if (!strcmp(o.screen, "controls")) g_in.screen = SCREEN_CONTROLS;
        else g_in.screen = SCREEN_NONE;
        g_in.inv_open = !strcmp(o.screen, "inventory");
    }
    if (g_in.screen != SCREEN_NONE) {
        if (g_in.screen != SCREEN_TITLE) mn.screen = SCREEN_PAUSE; /* settings/controls return to pause */
        menu_open(&mn, g_in.screen);
    }
    int settings_dirty = 0;
    camera_anim cam;
    camera_init(&cam);
    debug_frames dframes;
    memset(&dframes, 0, sizeof dframes);
    float phys_ms = 0.0f;
    char versions[128];
    {
        char mi[48];
        mem_version_string(mi, sizeof mi);
        int gmaj, gmin, grev;
        glfwGetVersion(&gmaj, &gmin, &grev);
        snprintf(versions, sizeof versions, "%s  GLFW %d.%d.%d  Vulkan 1.0", mi, gmaj, gmin, grev);
        snprintf(mn.footer, sizeof mn.footer, "seed %u   %s   GLFW %d.%d.%d", seed, mi, gmaj, gmin, grev);
    }

    demo_state demo = {0};
    double prev = glfwGetTime(), acc = 0.0, title_t = prev, save_t = prev;
    int frame = 0, fps_frames = 0;
    char title[256];

    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();
        double now = glfwGetTime();
        double dt = now - prev;
        prev = now;
        debug_frames_push(&dframes, (float)(dt * 1000.0));
        if (dt > 0.25) dt = 0.25;

        if (g_in.resized) {
            renderer_on_resize(rd);
            g_in.resized = 0;
        }

        /* Menus: the simulation stands still and the cursor is free. */
        if (g_in.lost_focus) {
            if (g_in.screen == SCREEN_NONE && !o.frames) {
                g_in.screen = SCREEN_PAUSE;
            }
            g_in.lost_focus = 0;
        }
        if (g_in.screen != SCREEN_NONE && mn.screen != g_in.screen) menu_open(&mn, g_in.screen);
        int in_menu = g_in.screen != SCREEN_NONE;
        set_captured(win, !in_menu && g_in.captured);
        if (in_menu) {
            acc = 0.0;
            memset(g_in.keys, 0, sizeof g_in.keys); /* no keys stuck down on return */
            g_in.click_break = g_in.click_place = g_in.click_pick = 0;
            g_in.attack = 0;
        } else {
            acc += dt;
        }
        /* The inventory screen frees the cursor; closing it takes it back. */
        static int inv_was_open;
        if (g_in.inv_open && !inv_was_open) {
            set_captured(win, 0);
            iu.open_t = 0.0f;
        } else if (!g_in.inv_open && inv_was_open) {
            item_stack left = inv_return_cursor(&inv);
            if (left.count) {
                vec3 d = look_dir(pl.yaw, pl.pitch);
                physics_drop_item(&ph, dv3(pl.pos.x, pl.pos.y + 1.3, pl.pos.z),
                                  dv3((double)d.x * 3.0, 1.5, (double)d.z * 3.0), left.id, left.count, 1.2f);
            }
            if (!in_menu) set_captured(win, 1);
        }
        inv_was_open = g_in.inv_open;
        if (hl.dead) g_in.inv_open = 0;

        /* The panel, death and unconsciousness freeze the player's input. */
        int frozen = in_menu || g_in.panel || g_in.inv_open || hl.dead || hl.conscious == CONS_UNCONSCIOUS;
        if (frozen) g_in.mouse_dx = g_in.mouse_dy = 0;

        /* Look. */
        const float sens = 0.0022f * (float)st.sensitivity / 100.0f;
        float look_dx = (float)g_in.mouse_dx * sens, look_dy = (float)g_in.mouse_dy * sens;
        pl.yaw += (float)g_in.mouse_dx * sens;
        pl.pitch -= (float)g_in.mouse_dy * sens * (st.invert_y ? -1.0f : 1.0f);
        pl.pitch = clampf(pl.pitch, -1.55f, 1.55f);
        pl.yaw = fmodf(pl.yaw, (float)(2 * MC_PI));
        g_in.mouse_dx = g_in.mouse_dy = 0;
        if (g_in.scroll != 0) {
            if (!in_menu && !g_in.panel) {
                int s = inv.selected - (g_in.scroll > 0 ? 1 : -1);
                inv.selected = (s % INV_HOTBAR + INV_HOTBAR) % INV_HOTBAR;
            }
            g_in.scroll = 0;
        }
        if (g_in.slot >= 0 && g_in.slot < INV_HOTBAR) inv.selected = g_in.slot;
        g_in.slot = -1;
        if (g_in.toggle_fly) {
            pl.flying = !pl.flying;
            pl.vel = dv3(0, 0, 0);
            g_in.toggle_fly = 0;
        }

        player_input in = {0};
        in.forward = (float)(g_in.keys[GLFW_KEY_W] - g_in.keys[GLFW_KEY_S]);
        in.right = (float)(g_in.keys[GLFW_KEY_D] - g_in.keys[GLFW_KEY_A]);
        in.jump = g_in.keys[GLFW_KEY_SPACE];
        in.descend = g_in.keys[GLFW_KEY_LEFT_SHIFT];
        in.sneak = g_in.keys[GLFW_KEY_LEFT_SHIFT];
        g_in.sprint_toggle = st.sprint_toggle;
        if (!g_in.keys[GLFW_KEY_W]) g_in.sprint_tap = 0;
        if (!st.sprint_toggle) g_in.sprint_latch = 0;
        in.sprint = (st.sprint_toggle ? g_in.sprint_latch : g_in.keys[GLFW_KEY_LEFT_CONTROL]) || g_in.sprint_tap;
        if (frozen) memset(&in, 0, sizeof in);
        survival_limit_input(&hl, &pl, &in);

        ph.magnet = !hl.dead && !in_menu;
        int steps = 0;
        float landing = 0.0f; /* hardest landing this frame, for the camera */
        double phys_t0 = glfwGetTime();
        while (acc >= PHYS_DT && steps < MAX_STEPS_PER_FRAME) {
            survival_before sb = survival_capture(&pl);
            physics_step(&ph, &pl, &in);
            if ((float)pl.fall_speed > landing) landing = (float)pl.fall_speed;
            health_env env;
            survival_env(&w, &pl, &sb, actions, &env);
            thermal_env(th, &w, &pl, &env);
            actions = 0;
            survival_impacts(&hl, &w, &ph, &pl, &sb);
            /* Per-step physics events, cleared by the next step. */
            for (int k = 0; k < ph.shatter_count; k++)
                fx_break(fx, ph.shatter_block[k], ph.shatter_pos[k].x, ph.shatter_pos[k].y, ph.shatter_pos[k].z, 24);
            for (int k = 0; k < ph.splash_count; k++) fx_splash(fx, ph.splash_pos[k], (double)ph.splash_speed[k]);
            health_step(&hl, &env, PHYS_DT);
            acc -= PHYS_DT;
            steps++;
        }
        if (steps == MAX_STEPS_PER_FRAME) acc = 0.0; /* too slow to keep up: slow down time instead */
        if (steps) physics_pickup(&ph, accept_item, &inv);
        if (!in_menu) thermo_step(th, &w, dt, pl.pos);
        if (!in_menu) fx_step(fx, &w, dt);
        phys_ms = anim_approach(phys_ms, (float)((glfwGetTime() - phys_t0) * 1000.0), 5.0f, (float)dt);
        camera_pose pose = camera_update(&cam, &pl, landing, hl.hurt_flash, st.view_bob, st.fov_effects,
                                         in_menu ? 0.0f : (float)dt);

        /* Block interaction. */
        dvec3 eye = dv3(pl.pos.x, pl.pos.y + PLAYER_EYE - (double)cam.crouch, pl.pos.z);
        vec3 look = look_dir(pl.yaw, pl.pitch);
        ray_hit hit = physics_raycast(&w, eye, look, REACH);
        health_limits lim = health_get_limits(&hl);
        int hands = !frozen && (pl.flying || (lim.has_control && lim.can_act));
        {
            interact_input ii = {.attack = g_in.attack && !frozen, .attack_click = g_in.click_break,
                                 .use_click = g_in.click_place, .pick_click = g_in.click_pick && !frozen,
                                 .drop = frozen ? 0 : g_in.drop, .can_act = hands,
                                 .speed = pl.flying ? 1.0f : 0.4f + 0.6f * lim.move_scale, .dt = (float)dt};
            if (in_menu) ii.dt = 0.0f;
            interact_out io = interact_frame(&ia, &w, &ph, th, &pl, &inv, fx, eye, look, hit, &ii);
            actions += io.actions;
            if (io.broke) survival_on_break(&hl, io.broken_id);
            if (io.eat) g_in.treat = TREAT_EAT;
            if (io.msg) {
                snprintf(hs.msg, sizeof hs.msg, "%s", io.msg);
                hs.msg_age = 0.0f;
            }
            crack = (fx_crack){ia.has_target && !pl.flying, ia.target.x, ia.target.y, ia.target.z, ia.progress};
        }
        g_in.click_break = g_in.click_place = g_in.click_pick = g_in.drop = 0;

        /* Treatments, eating and drinking. */
        if (g_in.treat >= 0) {
            if (!hl.dead) {
                int water = survival_water_nearby(&w, &pl, look) || inv_held(&inv)->id == I_WATER_BUCKET;
                int ok = health_treat(&hl, &inv, g_in.sel_part, g_in.treat, water, hs.msg, sizeof hs.msg);
                if (ok && (g_in.treat == TREAT_EAT || g_in.treat == TREAT_DRINK)) eat_anim = 1.0f;
                hs.msg_age = 0.0f;
            }
            g_in.treat = -1;
        }
        if (g_in.respawn) {
            if (hl.dead) {
                drop_everything(&ph, &inv, &pl, &death_rng);
                world_load_blocking(&w, sx, sz, 1);
                player_spawn(&pl, &w, sx, sz);
                health_init(&hl, seed ^ (uint32_t)frame * 2654435761u);
                inv_starting_kit(&inv);
                hs.alert_count = 0;
                g_in.panel = 0;
                log_info("respawned");
            }
            g_in.respawn = 0;
        }
        hs.msg_age += (float)dt;

        if (o.demo) demo_step(&w, &ph, fx, &pl, &demo, frame);

        world_update(&w, pl.pos.x, pl.pos.y + PLAYER_EYE, pl.pos.z);

        if (g_in.want_shot) {
            renderer_request_screenshot(rd, "screenshot.ppm");
            g_in.want_shot = 0;
        }
        if (o.screenshot && o.frames && frame == o.frames - 1) renderer_request_screenshot(rd, o.screenshot);

        if (renderer_begin_frame(rd)) {
            jobs_poll(js, MESH_UPLOADS_PER_FRAME);
            world_schedule(&w);
            double alpha = acc / PHYS_DT;
            dvec3 ip = dv3_lerp(pl.prev_pos, pl.pos, alpha);
            render_view v = {.eye = dv3(ip.x, ip.y + PLAYER_EYE, ip.z), .yaw = pl.yaw, .pitch = pl.pitch,
                             .fov = (float)st.fov * (float)(MC_PI / 180.0), .has_selection = hit.hit && !in_menu,
                             .selection = hit.block};
            int on_title = g_in.screen == SCREEN_TITLE;
            v.eye.x += (double)pose.dx;
            v.eye.y += (double)pose.dy;
            v.eye.z += (double)pose.dz;
            v.roll = pose.roll;
            v.pitch = clampf(v.pitch + pose.pitch_add, -1.56f, 1.56f);
            v.fov *= pose.fov_scale;
            if (on_title) {
                v.roll = 0.0f;
                /* The title screen circles high above the spawn point. */
                double t = now * 0.035;
                int gy = world_surface_y(&w, (int)floor(sx), (int)floor(sz));
                double h = (gy > 0 ? gy : SEA_LEVEL) + 22.0;
                v.eye = dv3(sx + cos(t) * 28.0, h, sz + sin(t) * 28.0);
                v.yaw = (float)(t - MC_PI * 0.5);
                v.pitch = -0.32f;
            }
            v.underwater = world_get(&w, (int)floor(v.eye.x), (int)floor(v.eye.y), (int)floor(v.eye.z)) == B_WATER;
            v.time = (float)fmod(now, 3600.0);
            v.daylight = thermo_daylight(th);
            thermo_sky(th, v.sky);
            eat_anim = fmaxf(0.0f, eat_anim - (float)dt / 1.6f);
            if (!on_title && !hl.dead && !in_menu) {
                const item_stack *held_st = &inv.slot[inv.selected];
                viewmodel_input vi = {held_st->count ? held_st->id : 0, ia.swing, ia.swinging, cam.stride, cam.bob,
                                      cam.sprint, look_dx, look_dy, eat_anim > 0.0f ? 1.0f - eat_anim : 0.0f,
                                      v.daylight, (float)dt};
                v.view_model = vm_ents;
                v.view_model_count = viewmodel_build(&vm, &vi, vm_ents, VIEWMODEL_MAX);
            }
            int n_op = 0, n_tr = 0;
            fx_build_entities(fx, &ph, &crack, v.eye, alpha, v.time, ents, RENDER_MAX_ENTS, &n_op, &n_tr);
            v.ents = ents;
            v.ent_opaque = n_op;
            v.ent_trans = n_tr;

            /* Overlay: the game writes its quads straight into the frame's
             * mapped buffer. */
            int max_quads, fb_w, fb_h;
            ui_vertex *uv = renderer_ui_buffer(rd, &max_quads, &fb_w, &fb_h);
            ui u;
            ui_begin(&u, uv, max_quads, fb_w, fb_h);
            ui_set_scale(&u, st.gui_scale, fb_w, fb_h);
            hs.panel = g_in.panel;
            hs.sel = g_in.sel_part;
            hs.held = NULL;
            hs.flying = pl.flying;
            hs.debug = g_in.debug;
            hs.inv = &inv;
            hs.hide_hints = !st.show_hints;
            hud_update(&hs, &hl, in_menu ? 0.0f : (float)dt);
            if (!on_title) hud_draw(&u, &hl, &hs);
            if (!on_title && !hl.dead && !g_in.panel && !g_in.inv_open) invui_hotbar(&u, &iu, &inv, (float)dt);
            float mx = -1.0f, my = -1.0f;
            {
                double wx, wy;
                int ww, wh;
                glfwGetCursorPos(win, &wx, &wy);
                glfwGetWindowSize(win, &ww, &wh);
                if (ww > 0 && wh > 0) {
                    mx = (float)(wx * fb_w / ww) / u.scale;
                    my = (float)(wy * fb_h / wh) / u.scale;
                }
            }
            if (g_in.inv_open && !in_menu && !hl.dead) {
                invui_input ui_in = {mx, my, g_in.ui_click, g_in.ui_rclick,
                                     g_in.keys[GLFW_KEY_LEFT_SHIFT] || g_in.keys[GLFW_KEY_RIGHT_SHIFT],
                                     thermo_near_fire(th, &w, pl.pos, 3.0), (float)dt};
                invui_result ir = invui_screen(&u, &iu, &inv, &ui_in);
                if (ir.drop.count) {
                    vec3 d = look_dir(pl.yaw, pl.pitch);
                    physics_drop_item(&ph, dv3(pl.pos.x, pl.pos.y + 1.3, pl.pos.z),
                                      dv3((double)d.x * 3.0, 1.5, (double)d.z * 3.0), ir.drop.id, ir.drop.count, 1.2f);
                }
            }
            if (g_in.debug && !on_title) {
                render_stats rs = renderer_stats(rd);
                debug_info di;
                memset(&di, 0, sizeof di);
                di.frames = &dframes;
                di.phys_ms = phys_ms;
                di.x = ip.x;
                di.y = ip.y;
                di.z = ip.z;
                di.yaw = pl.yaw;
                di.pitch = pl.pitch;
                di.vx = pl.vel.x;
                di.vy = pl.vel.y;
                di.vz = pl.vel.z;
                di.on_ground = pl.on_ground;
                di.sprinting = pl.sprinting;
                di.sneaking = pl.sneaking;
                di.flying = pl.flying;
                di.submerged = pl.submerged;
                di.body_temp = hl.temp;
                di.air_temp = thermo_air(th, &w, eye.x, eye.y, eye.z);
                di.feels_like = health_skin_mean(&hl);
                di.day_time = thermo_day_time(th);
                di.heat_cells = thermo_cell_count(th);
                di.fires = thermo_fire_count(th);
                di.has_target = hit.hit;
                di.tx = hit.block.x;
                di.ty = hit.block.y;
                di.tz = hit.block.z;
                di.target_id = hit.id;
                di.target_temp = hit.hit ? thermo_block_temp(th, &w, hit.block.x, hit.block.y, hit.block.z) : 0.0f;
                di.seed = seed;
                di.radius = w.radius;
                di.threads = jobs_worker_count(js);
                di.bodies = ph.body_count;
                di.items = ph.item_count;
                di.particles = fx->count;
                di.break_progress = ia.has_target ? ia.progress : 0.0f;
                di.fluid_updates = ph.fluid_updates;
                di.last_collapse = ph.last_collapse;
                di.draw_calls = rs.draw_calls;
                di.sections = rs.sections_drawn;
                di.quads = rs.quads;
                di.pool_used_kb = rs.pool_used_kb;
                di.pool_total_kb = rs.pool_total_kb;
                di.ui_quads = u.quads;
                mem_process_info(&di.rss, &di.commit);
                di.gpu = renderer_device_name(rd);
                di.versions = versions;
                di.width = fb_w;
                di.height = fb_h;
                debug_draw(&u, &di);
            }
            if (in_menu) {
                menu_input mi = {.mouse_down = g_in.ui_down, .click = g_in.ui_click, .up = g_in.ui_up,
                                 .down = g_in.ui_dn, .left = g_in.ui_left, .right = g_in.ui_right,
                                 .enter = g_in.ui_enter, .back = g_in.ui_back, .dt = (float)dt};
                mi.mx = mx;
                mi.my = my;
                int mins = (int)(hl.t / 60.0);
                snprintf(mn.status, sizeof mn.status, "Alive %d:%02d   core %.1f" UI_CH_DEGREE "C   %s", mins / 60,
                         mins % 60, (double)hl.temp, hl.dead ? "dead" : "");
                menu_action act = menu_frame(&mn, &u, &mi, &st);
                switch (act) {
                case MENU_PLAY:
                case MENU_RESUME:
                    g_in.screen = SCREEN_NONE;
                    set_captured(win, 1);
                    break;
                case MENU_TO_TITLE:
                    physics_settle_bodies(&ph, NULL);
                    world_save_all(&w);
                    save_player(&sc);
                    g_in.inv_open = 0;
                    g_in.screen = SCREEN_TITLE;
                    g_in.panel = 0;
                    break;
                case MENU_QUIT: glfwSetWindowShouldClose(win, GLFW_TRUE); break;
                case MENU_SETTINGS:
                    renderer_set_vsync(rd, st.vsync);
                    settings_dirty = 1;
                    break;
                default: break;
                }
                if (g_in.screen != SCREEN_NONE) g_in.screen = mn.screen;
                if (settings_dirty && g_in.screen != SCREEN_SETTINGS) {
                    if (settings_save(&st, o.config) != 0) log_warn("could not save settings to %s", o.config);
                    settings_dirty = 0;
                }
            }
            v.ui_quads = u.quads;
            v.hide_crosshair = frozen || on_title;
            renderer_end_frame(rd, &w, &ph, &v, alpha);
        } else {
            jobs_poll(js, MESH_UPLOADS_PER_FRAME);
            world_schedule(&w);
            glfwWaitEventsTimeout(0.05);
        }
        g_in.ui_click = g_in.ui_rclick = g_in.ui_up = g_in.ui_dn = g_in.ui_left = g_in.ui_right = g_in.ui_enter =
            g_in.ui_back = 0;

        if (frame < INT_MAX) frame++; /* int overflow is UB; only --frames and the demo read it */
        fps_frames++;
        if (now - title_t >= 0.5) {
            double fps = fps_frames / (now - title_t);
            fps_frames = 0;
            title_t = now;
            render_stats rs = renderer_stats(rd);
            const item_stack *held = &inv.slot[inv.selected];
            snprintf(title, sizeof title,
                     "blockclonia | %.0f fps | %.1f %.1f %.1f | %s%s | draws %d | bodies %d | mesh %u/%u MB",
                     fps, pl.pos.x, pl.pos.y, pl.pos.z, held->count ? item_get(held->id)->name : "empty hand",
                     pl.flying ? " | fly" : "", rs.draw_calls, ph.body_count, rs.pool_used_kb / 1024,
                     rs.pool_total_kb / 1024);
            glfwSetWindowTitle(win, title);
        }
        if (now - save_t >= AUTOSAVE_SECONDS) {
            save_t = now;
            world_save_all(&w); /* only columns edited since the last save */
            save_player(&sc);
        }
        if (o.frames && frame >= o.frames) break;
    }

    if (settings_dirty && settings_save(&st, o.config) != 0) log_warn("could not save settings to %s", o.config);
    log_info("saving and shutting down");
    log_set_fatal_hook(NULL, NULL);
    physics_settle_bodies(&ph, NULL);
    world_save_all(&w);
    if (!hl.dead) save_player(&sc);
    jobs_wait_idle(js);
    mem_free(ents);
    mem_free(fx);
    world_destroy(&w);
    physics_destroy(&ph);
    thermo_destroy(th);
    jobs_destroy(js);
    renderer_destroy(rd);
    glfwDestroyWindow(win);
    glfwTerminate();
    if (o.mem_stats) mem_print_stats();
    return 0;
}
