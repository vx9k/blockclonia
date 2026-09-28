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
#include "audio_device.h"
#include "settings.h"
#include "sound.h"
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
    int no_sound;
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

/* The --hurt names, comma-joined from HEALTH_DEBUG_KINDS so usage() can't
 * fall out of sync with what --hurt (and the debug menu) actually take. */
static const char *hurt_names(void)
{
    static char buf[512];
    size_t len = 0;
    for (int i = 0; i < HEALTH_DEBUG_COUNT && len < sizeof buf; i++) {
        int n = snprintf(buf + len, sizeof buf - len, "%s%s", i ? ", " : "", HEALTH_DEBUG_KINDS[i].name);
        if (n > 0) len += (size_t)n < sizeof buf - len ? (size_t)n : sizeof buf - len - 1;
    }
    return buf;
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
           "  --screen NAME     open title, pause, settings, controls or debug-health at start\n"
           "  --debug           start with the F3 overlay on\n"
           "  --give LIST       start with items, e.g. log:8,sand:4 (names as shown in game)\n"
           "  --time HH         start at this hour of the day (0-23)\n"
           "  --demo            scripted structural-collapse demo\n"
           "  --health-panel    start with the health panel (H) open\n"
           "  --hurt LIST       start injured: comma list of %s,\n"
           "                    pads (defibrillator pads on the chest)\n"
           "  --bench           run CPU benchmarks and exit (no window)\n"
           "  --mem-stats       print mimalloc statistics at exit\n"
           "  --no-sound        do not open an audio device\n",
           hurt_names());
}

/* Options that take no value. */
static int parse_flag(options *o, const char *a)
{
    const struct {
        const char *name;
        int *field;
    } flags[] = {
        {"--play", &o->play},
        {"--debug", &o->debug},
        {"--validate", &o->validate},
        {"--demo", &o->demo},
        {"--health-panel", &o->open_panel},
        {"--bench", &o->bench},
        {"--mem-stats", &o->mem_stats},
        {"--no-sound", &o->no_sound},
    };
    for (size_t i = 0; i < sizeof flags / sizeof flags[0]; i++)
        if (strcmp(a, flags[i].name) == 0) {
            *flags[i].field = 1;
            return 1;
        }
    if (strcmp(a, "--no-vsync") == 0) {
        o->vsync = 0;
        o->no_vsync = 1;
        return 1;
    }
    if (strcmp(a, "--no-save") == 0) {
        o->world_dir = NULL;
        return 1;
    }
    return 0;
}

/* Options with an integer value in a range. */
static int parse_int_option(options *o, const char *a, const char *v, int *ok)
{
    const struct {
        const char *name;
        int *field;
        int lo, hi;
        int *have;
    } ints[] = {
        {"--radius", &o->radius, 2, 32, &o->have_radius},
        {"--threads", &o->threads, 0, 64, NULL},
        {"--gpu", &o->gpu, 0, 64, NULL},
        {"--frames", &o->frames, 1, INT_MAX, NULL},
    };
    for (size_t i = 0; i < sizeof ints / sizeof ints[0]; i++) {
        if (strcmp(a, ints[i].name) != 0) continue;
        long long n;
        *ok = parse_int(v, ints[i].lo, ints[i].hi, &n);
        if (*ok) {
            *ints[i].field = (int)n;
            if (ints[i].have) *ints[i].have = 1;
        }
        return 1;
    }
    return 0;
}

static int valid_screen(const char *v)
{
    static const char *const NAMES[] = {"title", "pause", "settings", "controls", "debug-health", "game", "inventory"};
    for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++)
        if (strcmp(v, NAMES[i]) == 0) return 1;
    return 0;
}

/* Options with any other value. Returns 1 if a is one of them; *ok says
 * whether the value was acceptable. */
static int parse_value_option(options *o, const char *a, const char *v, int *ok)
{
    long long n = 0, x = 0, z = 0;
    *ok = 1;
    if (strcmp(a, "--seed") == 0) {
        *ok = parse_int(v, 0, (long long)UINT32_MAX, &n);
        o->seed = (uint32_t)n;
        o->have_seed = *ok;
    } else if (strcmp(a, "--size") == 0) {
        *ok = parse_int_pair(v, 'x', 64, 16384, &x, &z);
        o->width = (int)x;
        o->height = (int)z;
    } else if (strcmp(a, "--pool-mb") == 0) {
        *ok = parse_int(v, 1, 512, &n);
        o->pool_mb = (uint32_t)n;
    } else if (strcmp(a, "--time") == 0) {
        *ok = parse_int(v, 0, 23, &n);
        o->time_of_day = (double)n / 24.0;
        o->have_time = *ok;
    } else if (strcmp(a, "--look") == 0) {
        *ok = parse_float_pair(v, 3600.0f, &o->look_yaw, &o->look_pitch);
        o->have_look = *ok;
    } else if (strcmp(a, "--spawn") == 0) {
        *ok = parse_int_pair(v, ',', -(WORLD_LIMIT - 64), WORLD_LIMIT - 64, &x, &z);
        o->spawn_x = (int)x;
        o->spawn_z = (int)z;
        o->have_spawn = *ok;
    } else if (strcmp(a, "--config") == 0) {
        *ok = strlen(v) < 400;
        o->config = v;
    } else if (strcmp(a, "--world") == 0) {
        *ok = strlen(v) < 200;
        o->world_dir = v;
    } else if (strcmp(a, "--screen") == 0) {
        *ok = valid_screen(v);
        o->screen = v;
    } else if (strcmp(a, "--screenshot") == 0) {
        o->screenshot = v;
    } else if (strcmp(a, "--give") == 0) {
        o->give = v;
    } else if (strcmp(a, "--hurt") == 0) {
        o->hurt = v;
    } else {
        return parse_int_option(o, a, v, ok);
    }
    return 1;
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
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage();
            exit(0);
        }
        if (parse_flag(o, a)) continue;
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        int ok = 1;
        options probe = *o; /* is it a valued option at all? */
        if (!parse_value_option(&probe, a, v ? v : "", &ok)) {
            fprintf(stderr, "unknown option %s\n", a);
            usage();
            return 0;
        }
        if (!v) {
            fprintf(stderr, "%s needs a value\n", a);
            return 0;
        }
        parse_value_option(o, a, v, &ok);
        if (!ok) {
            fprintf(stderr, "invalid value for %s: %s\n", a, v);
            return 0;
        }
        i++;
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
    case GLFW_KEY_UP:
    case GLFW_KEY_W: g_in.ui_up = 1; break;
    case GLFW_KEY_DOWN:
    case GLFW_KEY_S:
    case GLFW_KEY_TAB: g_in.ui_dn = 1; break;
    case GLFW_KEY_LEFT:
    case GLFW_KEY_A: g_in.ui_left = 1; break;
    case GLFW_KEY_RIGHT:
    case GLFW_KEY_D: g_in.ui_right = 1; break;
    case GLFW_KEY_ENTER:
    case GLFW_KEY_KP_ENTER:
    case GLFW_KEY_SPACE: g_in.ui_enter = 1; break;
    case GLFW_KEY_ESCAPE: g_in.ui_back = 1; break;
    default: break;
    }
}

static void key_cb(GLFWwindow *win, int key, int sc, int action, int mods)
{
    (void)sc;
    (void)win;
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
    } else if (key == GLFW_KEY_K && g_in.debug) {
        g_in.screen = SCREEN_DEBUG_HEALTH; /* opens through the generic menu_open below */
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
    } else if (button == GLFW_MOUSE_BUTTON_RIGHT) g_in.click_place = 1;
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
                char a = (char)tolower((unsigned char)name[k]),
                     b = (char)(t[k] == '_' ? ' ' : tolower((unsigned char)t[k]));
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
        /* A stack that finds no room in the world stays in the inventory. */
        if (physics_drop_item(ph, dv3(pl->pos.x, pl->pos.y + 1.0, pl->pos.z), dv3(cos(a) * 2.0, 3.0, sin(a) * 2.0),
                              st->id, st->count, 1.0f))
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
 * trying treatments without having to get hurt first. The names and what
 * they do come from health.h's HEALTH_DEBUG_KINDS, the same table the F3
 * debug menu (K) uses, so the two can't drift apart. */
static void apply_hurt(health *h, const char *list)
{
    char buf[256];
    snprintf(buf, sizeof buf, "%s", list);
    char *save = NULL;
    for (char *t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        /* "pads" puts a defibrillator's pads on rather than causing an
         * injury, so it isn't in HEALTH_DEBUG_KINDS with the rest. */
        if (!strcmp(t, "pads")) {
            health_pads(h, 1);
            continue;
        }
        int kind = health_debug_find(t);
        if (kind < 0) log_warn("--hurt: unknown injury '%s'", t);
        else health_injure(h, kind, -1);
    }
}

static void demo_step(world *w, physics *ph, fx_state *fx, const player *p, demo_state *d, int frame)
{
    if (frame == 20) {
        /* A scatter of dropped items and a burst of debris in view. */
        static const int IDS[] = {B_LOG, I_APPLE, I_BANDAGE, B_GLASS, I_STICK, B_SAND, I_WATER_BUCKET, B_BRICK};
        for (int i = 0; i < 8; i++) {
            double a = -0.9 + 0.25 * i;
            physics_drop_item(ph, dv3(p->pos.x + sin(a) * 3.0, p->pos.y + 1.5, p->pos.z - cos(a) * 3.0), dv3(0, 1.0, 0),
                              IDS[i], 1 + i, 10.0f);
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

/* ---------------------------------------------------------------- game */

/* Everything the loop owns. One instance, static: several members are
 * large (the health model keeps seconds of waveforms). */
typedef struct {
    options o;
    settings st;
    int settings_dirty;
    int cfg_vsync;            /* vsync as the settings file has it (--no-vsync is not saved) */
    uint32_t seed;
    GLFWwindow *win;
    jobs *js;
    renderer *rd;
    world w;
    physics ph;
    thermo *th;
    world_hooks hooks;
    save_ctx sc;
    double sx, sz;            /* spawn point */
    player pl;
    inventory inv;
    health hl;
    hud_state hs;
    fx_state *fx;
    interact ia;
    invui iu;
    entity_instance *ents;
    fx_crack crack;
    viewmodel vm;
    entity_instance vm_ents[VIEWMODEL_MAX];
    float eat_anim;           /* 1 at the start of eating, easing to 0 */
    uint32_t death_rng;
    int actions;              /* blocks broken or placed since the last physics step */
    menu mn;
    camera_anim cam;
    debug_frames dframes;
    float phys_ms;
    char versions[128];
    demo_state demo;
    double acc;               /* unsimulated time, s */
    int frame;
    int inv_was_open;
    double title_t, save_t;
    int fps_frames;
    sound *snd;
    audio_device *audio;
    int last_step;            /* stride half-cycles, for footsteps */
    float landing;            /* hardest landing this frame, m/s */
    float swim_t, prev_hurt, prev_breath;
    double last_beat;         /* health.last_r of the last heartbeat heard */
    int snd_inv_open;

    /* This frame. */
    int in_menu, frozen;
    float look_dx, look_dy;   /* radians turned this frame, for the view model's lag */
    camera_pose pose;
    dvec3 eye;
    vec3 look;
    ray_hit hit;
} game;

static game g_game;

/* Writes the settings file. A --no-vsync override is not written back
 * unless the player changed VSync in the menu (which clears it). */
static int save_settings(const game *g)
{
    settings s = g->st;
    if (g->o.no_vsync) s.vsync = g->cfg_vsync;
    return settings_save(&s, g->o.config);
}

/* ---------------------------------------------------------------- sound */

static uint8_t block_under(const world *w, const player *p)
{
    return world_get(w, (int)floor(p->pos.x), (int)floor(p->pos.y - 0.05), (int)floor(p->pos.z));
}

/* Wind rises with height and with the air rushing past a falling or
 * flying body, and drops away under a roof. */
static float wind_level(const world *w, const player *p, dvec3 eye)
{
    float height = clampf((float)(eye.y - SEA_LEVEL) / 60.0f, 0.0f, 1.0f);
    float rush = clampf((float)dv3_len(p->vel) / 30.0f, 0.0f, 1.0f);
    float wind = 0.25f + 0.45f * height + 0.6f * rush;
    for (int dy = 1; dy <= 8; dy++)
        if (block_opaque(world_get(w, (int)floor(eye.x), (int)floor(eye.y) + dy, (int)floor(eye.z)))) {
            wind *= 0.2f;
            break;
        }
    return clampf(wind, 0.0f, 1.0f);
}

/* Water within a few metres (a coarse 5x3x5 sample), or all around. */
static float water_level(const world *w, const player *p, dvec3 eye)
{
    if (p->submerged > 0.6) return 1.0f;
    int n = 0;
    for (int dz = -4; dz <= 4; dz += 2)
        for (int dy = -2; dy <= 2; dy += 2)
            for (int dx = -4; dx <= 4; dx += 2)
                n += world_get(w, (int)floor(eye.x) + dx, (int)floor(eye.y) + dy, (int)floor(eye.z) + dz) == B_WATER;
    return clampf((float)n / 18.0f, 0.0f, 1.0f);
}

static void apply_volumes(game *g)
{
    sound_set_volumes(g->snd, (float)g->st.volume_master / 100.0f, (float)g->st.volume_effects / 100.0f,
                      (float)g->st.volume_ambient / 100.0f);
}

/* Per-step physics events worth hearing (they are cleared by the next step). */
static void sound_step_events(game *g)
{
    const physics *ph = &g->ph;
    for (int k = 0; k < ph->shatter_count; k++) {
        dvec3 p = dv3(ph->shatter_pos[k].x + 0.5, ph->shatter_pos[k].y + 0.5, ph->shatter_pos[k].z + 0.5);
        sound_play(g->snd, SND_SHATTER, &p, ph->shatter_block[k], 1.0f);
    }
    for (int k = 0; k < ph->splash_count; k++)
        sound_play(g->snd, SND_SPLASH, &ph->splash_pos[k], B_WATER, ph->splash_speed[k] / 10.0f);
    for (int k = 0; k < ph->thud_count; k++)
        sound_play(g->snd, SND_THUD, &ph->thud_pos[k], ph->thud_block[k], ph->thud_speed[k] / 10.0f);
}

static void sound_interact_events(game *g, const interact_out *io)
{
    static const int MAP[] = {[IE_DIG] = SND_DIG,         [IE_BREAK] = SND_BREAK,   [IE_PLACE] = SND_PLACE,
                              [IE_FILL] = SND_BUCKET_FILL, [IE_POUR] = SND_BUCKET_POUR, [IE_FEED] = SND_FIRE_FEED,
                              [IE_DROP] = SND_DROP};
    for (int i = 0; i < io->ev_count; i++) {
        const interact_event *e = &io->ev[i];
        if (e->kind >= 0 && e->kind <= IE_DROP) sound_play(g->snd, MAP[e->kind], &e->at, e->block, 0.8f);
    }
}

/* The body: footsteps with the stride, landings, swimming, being hurt, the
 * heart when it pounds and laboured breathing. */
static void sound_body(game *g, double dt)
{
    const player *pl = &g->pl;
    const health *h = &g->hl;
    dvec3 feet = pl->pos;
    int step = (int)floorf(g->cam.stride / (float)MC_PI);
    if (step != g->last_step && pl->on_ground && !pl->flying && g->cam.bob > 0.2f) {
        float pace = (float)hypot(pl->vel.x, pl->vel.z) / (float)SPRINT_SPEED;
        sound_play(g->snd, SND_STEP, &feet, block_under(&g->w, pl), pl->sneaking ? 0.25f : 0.35f + 0.65f * pace);
    }
    g->last_step = step;
    if (g->landing > 2.5f) sound_play(g->snd, SND_LAND, &feet, block_under(&g->w, pl), g->landing / 10.0f);
    g->swim_t -= (float)dt;
    if (pl->submerged > 0.3 && !pl->flying && g_in.keys[GLFW_KEY_SPACE] && g->swim_t <= 0.0f) {
        sound_play(g->snd, SND_SWIM, &feet, B_WATER, 0.6f);
        g->swim_t = 0.7f;
    }
    if (h->hurt_flash > g->prev_hurt + 0.1f && !h->dead)
        sound_play(g->snd, SND_HURT, NULL, 0, clampf(h->hurt_flash, 0.2f, 1.0f));
    g->prev_hurt = h->hurt_flash;
    /* The heart is heard when it races, stumbles or labours against low
     * pressure; each sound is one simulated beat. */
    if (h->last_r != g->last_beat) {
        g->last_beat = h->last_r;
        float felt = fmaxf((h->hr - 105.0f) / 60.0f, (95.0f - h->sbp) / 40.0f);
        if (felt > 0.0f && !h->dead) sound_play(g->snd, SND_HEARTBEAT, NULL, 0, clampf(0.25f + felt, 0.0f, 1.0f));
    }
    /* A breath at the start of each breathing cycle when out of breath. */
    int breath_start = h->breath_phase < g->prev_breath;
    g->prev_breath = h->breath_phase;
    float effort = fmaxf(1.0f - health_stamina(h) * 2.5f, (h->rr - 22.0f) / 18.0f);
    if (breath_start && effort > 0.0f && h->breathing && !h->dead && pl->submerged < 0.9)
        sound_play(g->snd, SND_BREATH, NULL, 0, clampf(effort, 0.0f, 1.0f));
}

/* Once a frame: the ears, the ambience and the body's own sounds. */
static void frame_sound(game *g, double dt, const render_view *v)
{
    sound_listener(g->snd, v->eye, v->yaw, v->pitch, v->underwater);
    int on_title = g_in.screen == SCREEN_TITLE;
    float wind = wind_level(&g->w, &g->pl, v->eye), water = water_level(&g->w, &g->pl, v->eye);
    sound_ambience(g->snd, wind * (on_title ? 0.6f : 1.0f), water);
    dvec3 fires[SOUND_MAX_FIRES];
    int nf = thermo_fires_near(g->th, &g->w, v->eye, 24.0, fires, SOUND_MAX_FIRES);
    sound_fires(g->snd, fires, nf);
    if (!g->in_menu && !on_title) sound_body(g, dt);
    /* The inventory screen opening and closing. */
    if (g_in.inv_open != g->snd_inv_open) sound_play(g->snd, g_in.inv_open ? SND_UI_OPEN : SND_UI_CLOSE, NULL, 0, 0.6f);
    g->snd_inv_open = g_in.inv_open;
    sound_update(g->snd, (float)dt);
}


/* Drops a stack in front of the player (inventory closed with a stack on
 * the cursor, or thrown out of the window). */
static void toss(game *g, item_stack st)
{
    if (!st.count) return;
    vec3 d = look_dir(g->pl.yaw, g->pl.pitch);
    if (!physics_drop_item(&g->ph, dv3(g->pl.pos.x, g->pl.pos.y + 1.3, g->pl.pos.z),
                           dv3((double)d.x * 3.0, 1.5, (double)d.z * 3.0), st.id, st.count, 1.2f)) {
        /* No room for more items in the world: back into the inventory, or
         * held on the cursor until there is. */
        int left = inv_add(&g->inv, st.id, st.count);
        if (left) g->inv.cursor = (item_stack){st.id, (uint8_t)left};
    }
}

static void open_start_screen(game *g)
{
    const options *o = &g->o;
    menu_init(&g->mn);
    int skip_title = o->play || o->demo || o->open_panel || o->hurt;
    g_in.screen = skip_title ? SCREEN_NONE : SCREEN_TITLE;
    if (o->screen) {
        if (!strcmp(o->screen, "title")) g_in.screen = SCREEN_TITLE;
        else if (!strcmp(o->screen, "pause")) g_in.screen = SCREEN_PAUSE;
        else if (!strcmp(o->screen, "settings")) g_in.screen = SCREEN_SETTINGS;
        else if (!strcmp(o->screen, "controls")) g_in.screen = SCREEN_CONTROLS;
        else if (!strcmp(o->screen, "debug-health")) g_in.screen = SCREEN_DEBUG_HEALTH;
        else g_in.screen = SCREEN_NONE;
        g_in.inv_open = !strcmp(o->screen, "inventory");
    }
    if (g_in.screen != SCREEN_NONE) {
        if (g_in.screen != SCREEN_TITLE) g->mn.screen = SCREEN_PAUSE; /* settings/controls return to pause */
        menu_open(&g->mn, g_in.screen);
    }
}

static uint32_t choose_seed(options *o)
{
    uint32_t seed = o->have_seed ? o->seed : (uint32_t)time(NULL) * 2654435761u;
    if (!o->world_dir) return seed;
    if (save_ensure_dir(o->world_dir) != 0) {
        log_error("cannot create world directory '%s'; saving disabled", o->world_dir);
        o->world_dir = NULL;
        return seed;
    }
    uint32_t saved;
    if (save_read_seed(o->world_dir, &saved) == 0) {
        if (o->have_seed && saved != o->seed) log_warn("--seed ignored: world '%s' uses seed %u", o->world_dir, saved);
        return saved;
    }
    if (save_write_seed(o->world_dir, seed) != 0) log_warn("could not write level.dat");
    return seed;
}

static GLFWwindow *open_window(const options *o)
{
    if (!glfwInit()) log_fatal("glfwInit failed");
    if (!glfwVulkanSupported()) log_fatal("no Vulkan loader/driver found");
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow *win = glfwCreateWindow(o->width, o->height, "blockclonia", NULL, NULL);
    if (!win) log_fatal("could not create window");
    glfwSetKeyCallback(win, key_cb);
    glfwSetMouseButtonCallback(win, mouse_button_cb);
    glfwSetCursorPosCallback(win, cursor_cb);
    glfwSetScrollCallback(win, scroll_cb);
    glfwSetFramebufferSizeCallback(win, resize_cb);
    glfwSetWindowFocusCallback(win, focus_cb);
    if (glfwRawMouseMotionSupported()) glfwSetInputMode(win, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    return win;
}

/* The world, physics, heat, the player and their things. */
static void game_init_world(game *g)
{
    const options *o = &g->o;
    int workers = o->threads >= 0 ? o->threads : cpu_count() - 1;
    if (workers < 1 && o->threads < 0) workers = 1;
    if (workers > 8 && o->threads < 0) workers = 8;
    g->js = jobs_create(workers);

    render_opts ro = {.vsync = o->vsync,
                      .validate = o->validate,
                      .render_radius = o->radius,
                      .gpu_index = o->gpu,
                      .pool_mb = o->pool_mb,
                      .screenshots = o->screenshot != NULL};
    g->rd = renderer_create(g->win, &ro);

    world_init(&g->w, g->seed, o->radius, g->js, o->world_dir);
    physics_init(&g->ph, &g->w);
    g->th = thermo_create(g->seed);
    g->hooks = (world_hooks){&g->ph, g->th, &g->w};
    g->w.edit_user = &g->hooks;
    g->w.on_block_changed = on_block_changed;
    g->w.on_column_unload = on_column_unload;
    g->sc = (save_ctx){&g->w, &g->ph, &g->pl, &g->inv, g->th, o->world_dir};
    log_set_fatal_hook(save_on_fatal, &g->sc);
    renderer_bind_world(g->rd, &g->w);

    if (o->have_spawn) {
        g->sx = o->spawn_x + 0.5;
        g->sz = o->spawn_z + 0.5;
    } else {
        find_spawn(g->seed, &g->sx, &g->sz);
    }
    world_load_blocking(&g->w, g->sx, g->sz, 1);
    player_spawn(&g->pl, &g->w, g->sx, g->sz);
    if (o->have_look) {
        g->pl.yaw = o->look_yaw * (float)(MC_PI / 180.0);
        g->pl.pitch = clampf(o->look_pitch, -89.0f, 89.0f) * (float)(MC_PI / 180.0);
    }
    inv_starting_kit(&g->inv);
    double day = THERMO_DAY_START;
    if (!o->have_spawn && !o->demo && load_player(o->world_dir, &g->pl, &g->inv, &day)) {
        thermo_set_day_time(g->th, day);
        world_load_blocking(&g->w, g->pl.pos.x, g->pl.pos.z, 1);
        log_info("player restored from %s/player.dat", o->world_dir);
    }
    if (o->give) give_items(&g->inv, o->give);
    if (o->have_time) thermo_set_day_time(g->th, o->time_of_day);
    log_info("spawn at %.1f %.1f %.1f with %d worker threads", g->pl.pos.x, g->pl.pos.y, g->pl.pos.z,
             jobs_worker_count(g->js));

    health_init_varied(&g->hl, g->seed ^ 0x9E3779B9u);
    if (o->hurt) apply_hurt(&g->hl, o->hurt);
}

static void game_init(game *g)
{
    g->seed = choose_seed(&g->o);
    log_info("seed %u", g->seed);
    g->win = open_window(&g->o);
    game_init_world(g);

    g->fx = mem_alloc(sizeof *g->fx);
    fx_init(g->fx, g->seed);
    interact_init(&g->ia, g->seed ^ 0xA5A5u);
    invui_init(&g->iu);
    g->ents = mem_alloc(sizeof(entity_instance) * RENDER_MAX_ENTS);
    viewmodel_init(&g->vm);
    camera_init(&g->cam);
    g->death_rng = g->seed;
    g->snd = sound_create(g->seed, SOUND_RATE, SOUND_CHANNELS);
    apply_volumes(g);
    g->audio = g->o.no_sound ? NULL : audio_device_open(sound_render, g->snd, SOUND_RATE, SOUND_CHANNELS);
    log_info("sound: %s", g->audio ? audio_device_name(g->audio) : "off");

    g_in.treat = -1;
    g_in.slot = -1;
    g_in.panel = g->o.open_panel;
    g_in.debug = g->o.debug;
    open_start_screen(g);

    char mi[48];
    mem_version_string(mi, sizeof mi);
    int gmaj, gmin, grev;
    glfwGetVersion(&gmaj, &gmin, &grev);
    snprintf(g->versions, sizeof g->versions, "%s  GLFW %d.%d.%d  %s", mi, gmaj, gmin, grev,
             renderer_api_string(g->rd));
    snprintf(g->mn.footer, sizeof g->mn.footer, "seed %u   %s   GLFW %d.%d.%d", g->seed, mi, gmaj, gmin, grev);
}

/* Menus, focus, the inventory screen opening and closing, and what that
 * freezes. */
static void frame_menus(game *g, double dt)
{
    if (g_in.lost_focus) {
        if (g_in.screen == SCREEN_NONE && !g->o.frames) {
            g_in.screen = SCREEN_PAUSE;
            g_in.inv_open = 0; /* closes below, returning any stack on the cursor */
        }
        g_in.lost_focus = 0;
    }
    if (g_in.screen != SCREEN_NONE && g->mn.screen != g_in.screen) menu_open(&g->mn, g_in.screen);
    g->in_menu = g_in.screen != SCREEN_NONE;
    set_captured(g->win, !g->in_menu && g_in.captured);
    if (g->in_menu) {
        g->acc = 0.0; /* the simulation stands still */
        memset(g_in.keys, 0, sizeof g_in.keys); /* no keys stuck down on return */
        g_in.click_break = g_in.click_place = g_in.click_pick = 0;
        g_in.attack = 0;
    } else {
        g->acc += dt;
    }
    /* The inventory screen frees the cursor; closing it takes it back. */
    if (g_in.inv_open && !g->inv_was_open) {
        set_captured(g->win, 0);
        g->iu.open_t = 0.0f;
    } else if (!g_in.inv_open && g->inv_was_open) {
        toss(g, inv_return_cursor(&g->inv));
        if (!g->in_menu) set_captured(g->win, 1);
    }
    g->inv_was_open = g_in.inv_open;
    if (g->hl.dead) g_in.inv_open = 0;
    /* The panel, death and unconsciousness freeze the player's input. */
    g->frozen = g->in_menu || g_in.panel || g_in.inv_open || g->hl.dead || g->hl.conscious == CONS_UNCONSCIOUS;
    if (g->frozen) g_in.mouse_dx = g_in.mouse_dy = 0;
}

/* Looking, the hotbar and the movement keys. */
static player_input frame_input(game *g)
{
    const settings *st = &g->st;
    player *pl = &g->pl;
    const float sens = 0.0022f * (float)st->sensitivity / 100.0f;
    g->look_dx = (float)g_in.mouse_dx * sens;
    g->look_dy = (float)g_in.mouse_dy * sens;
    pl->yaw += g->look_dx;
    pl->pitch -= g->look_dy * (st->invert_y ? -1.0f : 1.0f);
    pl->pitch = clampf(pl->pitch, -1.55f, 1.55f);
    pl->yaw = fmodf(pl->yaw, (float)(2 * MC_PI));
    g_in.mouse_dx = g_in.mouse_dy = 0;
    if (g_in.scroll != 0) {
        if (!g->in_menu && !g_in.panel) {
            int s = g->inv.selected - (g_in.scroll > 0 ? 1 : -1);
            g->inv.selected = (s % INV_HOTBAR + INV_HOTBAR) % INV_HOTBAR;
        }
        g_in.scroll = 0;
    }
    if (g_in.slot >= 0 && g_in.slot < INV_HOTBAR) g->inv.selected = g_in.slot;
    g_in.slot = -1;
    if (g_in.toggle_fly) {
        pl->flying = !pl->flying;
        pl->vel = dv3(0, 0, 0);
        g_in.toggle_fly = 0;
    }

    player_input in = {0};
    in.forward = (float)(g_in.keys[GLFW_KEY_W] - g_in.keys[GLFW_KEY_S]);
    in.right = (float)(g_in.keys[GLFW_KEY_D] - g_in.keys[GLFW_KEY_A]);
    in.jump = g_in.keys[GLFW_KEY_SPACE];
    in.descend = g_in.keys[GLFW_KEY_LEFT_SHIFT];
    in.sneak = g_in.keys[GLFW_KEY_LEFT_SHIFT];
    g_in.sprint_toggle = st->sprint_toggle;
    if (!g_in.keys[GLFW_KEY_W]) g_in.sprint_tap = 0;
    if (!st->sprint_toggle) g_in.sprint_latch = 0;
    in.sprint = (st->sprint_toggle ? g_in.sprint_latch : g_in.keys[GLFW_KEY_LEFT_CONTROL]) || g_in.sprint_tap;
    if (g->frozen) memset(&in, 0, sizeof in);
    survival_limit_input(&g->hl, pl, &in);
    return in;
}

/* Fixed 60 Hz steps: physics, what it did to the body, and the body. */
static void frame_simulate(game *g, const player_input *in, double dt)
{
    g->ph.magnet = !g->hl.dead && !g->in_menu;
    int steps = 0;
    float landing = 0.0f; /* hardest landing this frame, for the camera */
    double t0 = glfwGetTime();
    while (g->acc >= PHYS_DT && steps < MAX_STEPS_PER_FRAME) {
        survival_before sb = survival_capture(&g->pl);
        physics_step(&g->ph, &g->pl, in);
        if ((float)g->pl.fall_speed > landing) landing = (float)g->pl.fall_speed;
        health_env env;
        survival_env(&g->w, &g->pl, &sb, g->actions, &env);
        thermal_env(g->th, &g->w, &g->pl, &env);
        g->actions = 0;
        survival_impacts(&g->hl, &g->w, &g->ph, &g->pl, &sb);
        /* Per-step physics events, cleared by the next step. */
        for (int k = 0; k < g->ph.shatter_count; k++)
            fx_break(g->fx, g->ph.shatter_block[k], g->ph.shatter_pos[k].x, g->ph.shatter_pos[k].y,
                     g->ph.shatter_pos[k].z, 24);
        for (int k = 0; k < g->ph.splash_count; k++)
            fx_splash(g->fx, g->ph.splash_pos[k], (double)g->ph.splash_speed[k]);
        sound_step_events(g);
        health_step(&g->hl, &env, PHYS_DT);
        g->acc -= PHYS_DT;
        steps++;
    }
    if (steps == MAX_STEPS_PER_FRAME) g->acc = 0.0; /* too slow to keep up: slow down time instead */
    g->landing = landing;
    if (steps && physics_pickup(&g->ph, accept_item, &g->inv)) sound_play(g->snd, SND_PICKUP, NULL, 0, 0.7f);
    if (!g->in_menu) {
        thermo_step(g->th, &g->w, dt, g->pl.pos);
        fx_step(g->fx, &g->w, dt);
    }
    g->phys_ms = anim_approach(g->phys_ms, (float)((glfwGetTime() - t0) * 1000.0), 5.0f, (float)dt);
    g->pose = camera_update(&g->cam, &g->pl, landing, g->hl.hurt_flash, g->st.view_bob, g->st.fov_effects,
                            g->in_menu ? 0.0f : (float)dt);
}

/* The hands: breaking, placing, using; then treatments and respawning. */
static void frame_interact(game *g, double dt)
{
    player *pl = &g->pl;
    g->eye = dv3(pl->pos.x, pl->pos.y + PLAYER_EYE - (double)g->cam.crouch, pl->pos.z);
    g->look = look_dir(pl->yaw, pl->pitch);
    g->hit = physics_raycast(&g->w, g->eye, g->look, REACH);
    health_limits lim = health_get_limits(&g->hl);
    int hands = !g->frozen && (pl->flying || (lim.has_control && lim.can_act));
    interact_input ii = {.attack = g_in.attack && !g->frozen,
                         .attack_click = g_in.click_break,
                         .use_click = g_in.click_place,
                         .pick_click = g_in.click_pick && !g->frozen,
                         .drop = g->frozen ? 0 : g_in.drop,
                         .can_act = hands,
                         .speed = pl->flying ? 1.0f : 0.4f + 0.6f * lim.move_scale,
                         .dt = g->in_menu ? 0.0f : (float)dt};
    interact_out io = interact_frame(&g->ia, &g->w, &g->ph, g->th, pl, &g->inv, g->fx, g->eye, g->look, g->hit, &ii);
    g->actions += io.actions;
    sound_interact_events(g, &io);
    if (io.broke) survival_on_break(&g->hl, io.broken_id);
    if (io.eat) g_in.treat = TREAT_EAT;
    if (io.defib) g_in.treat = TREAT_DEFIB;
    if (io.msg) {
        snprintf(g->hs.msg, sizeof g->hs.msg, "%s", io.msg);
        g->hs.msg_age = 0.0f;
    }
    g->crack = (fx_crack){g->ia.has_target && !pl->flying, g->ia.target.x, g->ia.target.y, g->ia.target.z,
                          g->ia.progress};
    g_in.click_break = g_in.click_place = g_in.click_pick = g_in.drop = 0;

    if (g_in.treat >= 0) {
        if (!g->hl.dead) {
            int water = survival_water_nearby(&g->w, pl, g->look) || inv_held(&g->inv)->id == I_WATER_BUCKET;
            int ok = health_treat(&g->hl, &g->inv, g_in.sel_part, g_in.treat, water, g->hs.msg, sizeof g->hs.msg);
            if (ok && (g_in.treat == TREAT_EAT || g_in.treat == TREAT_DRINK)) {
                g->eat_anim = 1.0f;
                sound_play(g->snd, g_in.treat == TREAT_EAT ? SND_EAT : SND_DRINK, NULL, 0, 0.8f);
            }
            g->hs.msg_age = 0.0f;
        }
        g_in.treat = -1;
    }
    /* The pads are cabled to the device: they come off when it is dropped. */
    if (g->hl.defib.phase != DEFIB_OFF && !inv_count(&g->inv, I_DEFIBRILLATOR) && g->inv.cursor.id != I_DEFIBRILLATOR)
        health_pads(&g->hl, 0);
    if (g_in.respawn) {
        if (g->hl.dead) {
            drop_everything(&g->ph, &g->inv, pl, &g->death_rng);
            world_load_blocking(&g->w, g->sx, g->sz, 1);
            player_spawn(pl, &g->w, g->sx, g->sz);
            health_init_varied(&g->hl, g->seed ^ (uint32_t)g->frame * 2654435761u);
            inv_starting_kit(&g->inv);
            g->hs.alert_count = 0;
            g_in.panel = 0;
            log_info("respawned");
        }
        g_in.respawn = 0;
    }
    g->hs.msg_age += (float)dt;
}

/* Camera, sky, entities and the first-person arm for the renderer. */
static render_view build_view(game *g, double now, double dt, double alpha)
{
    const player *pl = &g->pl;
    dvec3 ip = dv3_lerp(pl->prev_pos, pl->pos, alpha);
    render_view v = {.eye = dv3(ip.x, ip.y + PLAYER_EYE, ip.z),
                     .yaw = pl->yaw,
                     .pitch = pl->pitch,
                     .fov = (float)g->st.fov * (float)(MC_PI / 180.0),
                     .has_selection = g->hit.hit && !g->in_menu,
                     .selection = g->hit.block};
    int on_title = g_in.screen == SCREEN_TITLE;
    v.eye.x += (double)g->pose.dx;
    v.eye.y += (double)g->pose.dy;
    v.eye.z += (double)g->pose.dz;
    v.roll = g->pose.roll;
    v.pitch = clampf(v.pitch + g->pose.pitch_add, -1.56f, 1.56f);
    v.fov *= g->pose.fov_scale;
    if (on_title) {
        /* The title screen circles high above the player, whose
         * surroundings are what the world keeps loaded. */
        double t = now * 0.035, cx = pl->pos.x, cz = pl->pos.z;
        int gy = world_surface_y(&g->w, (int)floor(cx), (int)floor(cz));
        double h = (gy > 0 ? gy : SEA_LEVEL) + 22.0;
        v.eye = dv3(cx + cos(t) * 28.0, h, cz + sin(t) * 28.0);
        v.yaw = (float)(t - MC_PI * 0.5);
        v.pitch = -0.32f;
        v.roll = 0.0f;
    }
    v.underwater = world_get(&g->w, (int)floor(v.eye.x), (int)floor(v.eye.y), (int)floor(v.eye.z)) == B_WATER;
    v.time = (float)fmod(now, 3600.0);
    v.daylight = thermo_daylight(g->th);
    thermo_sky(g->th, v.sky);

    g->eat_anim = fmaxf(0.0f, g->eat_anim - (float)dt / 1.6f);
    if (!on_title && !g->hl.dead && !g->in_menu) {
        const item_stack *held = &g->inv.slot[g->inv.selected];
        viewmodel_input vi = {held->count ? held->id : 0,
                              g->ia.swing,
                              g->ia.swinging,
                              g->cam.stride,
                              g->cam.bob,
                              g->cam.sprint,
                              g->look_dx,
                              g->look_dy,
                              g->eat_anim > 0.0f ? 1.0f - g->eat_anim : 0.0f,
                              v.daylight,
                              (float)dt};
        v.view_model = g->vm_ents;
        v.view_model_count = viewmodel_build(&g->vm, &vi, g->vm_ents, VIEWMODEL_MAX);
    }
    int n_op = 0, n_tr = 0;
    fx_build_entities(g->fx, &g->ph, &g->crack, v.eye, alpha, v.time, g->ents, RENDER_MAX_ENTS, &n_op, &n_tr);
    v.ents = g->ents;
    v.ent_opaque = n_op;
    v.ent_trans = n_tr;
    v.hide_crosshair = g->frozen || on_title;
    return v;
}

static void draw_debug(game *g, ui *u, int fb_w, int fb_h, double alpha)
{
    const player *pl = &g->pl;
    dvec3 ip = dv3_lerp(pl->prev_pos, pl->pos, alpha);
    render_stats rs = renderer_stats(g->rd);
    debug_info di;
    memset(&di, 0, sizeof di);
    di.frames = &g->dframes;
    di.phys_ms = g->phys_ms;
    di.x = ip.x;
    di.y = ip.y;
    di.z = ip.z;
    di.yaw = pl->yaw;
    di.pitch = pl->pitch;
    di.vx = pl->vel.x;
    di.vy = pl->vel.y;
    di.vz = pl->vel.z;
    di.on_ground = pl->on_ground;
    di.sprinting = pl->sprinting;
    di.sneaking = pl->sneaking;
    di.flying = pl->flying;
    di.submerged = pl->submerged;
    di.body_temp = g->hl.temp;
    di.air_temp = thermo_air(g->th, &g->w, g->eye.x, g->eye.y, g->eye.z);
    di.feels_like = health_skin_mean(&g->hl);
    di.day_time = thermo_day_time(g->th);
    di.heat_cells = thermo_cell_count(g->th);
    di.fires = thermo_fire_count(g->th);
    di.has_target = g->hit.hit;
    di.tx = g->hit.block.x;
    di.ty = g->hit.block.y;
    di.tz = g->hit.block.z;
    di.target_id = g->hit.id;
    di.target_temp = g->hit.hit ? thermo_block_temp(g->th, &g->w, g->hit.block.x, g->hit.block.y, g->hit.block.z)
                                : 0.0f;
    di.seed = g->seed;
    di.radius = g->w.radius;
    di.threads = jobs_worker_count(g->js);
    di.bodies = g->ph.body_count;
    di.items = g->ph.item_count;
    di.particles = g->fx->count;
    di.break_progress = g->ia.has_target ? g->ia.progress : 0.0f;
    di.fluid_updates = g->ph.fluid_updates;
    di.last_collapse = g->ph.last_collapse;
    di.draw_calls = rs.draw_calls;
    di.sections = rs.sections_drawn;
    di.quads = rs.quads;
    di.pool_used_kb = rs.pool_used_kb;
    di.pool_total_kb = rs.pool_total_kb;
    di.ui_quads = u->quads;
    mem_process_info(&di.rss, &di.commit);
    di.gpu = renderer_device_name(g->rd);
    di.versions = g->versions;
    di.api = renderer_api_string(g->rd);
    di.audio = g->audio ? audio_device_name(g->audio) : "off";
    sound_stats(g->snd, &di.snd_voices, &di.snd_load);
    di.width = fb_w;
    di.height = fb_h;
    debug_draw(u, &di);
}

/* Menus take the mouse: returns the action, applied here. */
static void run_menu(game *g, ui *u, float mx, float my, double dt)
{
    menu_input mi = {.mouse_down = g_in.ui_down,
                     .click = g_in.ui_click,
                     .up = g_in.ui_up,
                     .down = g_in.ui_dn,
                     .left = g_in.ui_left,
                     .right = g_in.ui_right,
                     .enter = g_in.ui_enter,
                     .back = g_in.ui_back,
                     .dt = (float)dt};
    mi.mx = mx;
    mi.my = my;
    int mins = (int)(g->hl.t / 60.0);
    snprintf(g->mn.status, sizeof g->mn.status, "Alive %d:%02d   core %.1f" UI_CH_DEGREE "C   %s", mins / 60, mins % 60,
             (double)g->hl.temp, g->hl.dead ? "dead" : "");
    switch (menu_frame(&g->mn, u, &mi, &g->st)) {
    case MENU_PLAY:
    case MENU_RESUME:
        g_in.screen = SCREEN_NONE;
        set_captured(g->win, 1);
        break;
    case MENU_TO_TITLE:
        physics_settle_bodies(&g->ph, NULL);
        world_save_all(&g->w);
        save_player(&g->sc);
        g_in.inv_open = 0;
        g_in.panel = 0;
        menu_open(&g->mn, SCREEN_TITLE); /* the menu's own screen too, or the copy below undoes it */
        g_in.screen = SCREEN_TITLE;
        break;
    case MENU_QUIT: glfwSetWindowShouldClose(g->win, GLFW_TRUE); break;
    case MENU_SETTINGS:
        if (g->o.no_vsync && g->st.vsync) g->o.no_vsync = 0; /* turned back on in the menu: a real choice */
        renderer_set_vsync(g->rd, g->st.vsync);
        apply_volumes(g);
        g->settings_dirty = 1;
        break;
    case MENU_DEBUG_INJURE: health_injure(&g->hl, g->mn.debug_kind, g->mn.debug_part); break;
    case MENU_DEBUG_RESET:
        health_init_varied(&g->hl, g->seed ^ (uint32_t)g->frame * 2654435761u);
        log_info("debug menu: health reset");
        break;
    default: break;
    }
    if (g->mn.clicked) sound_play(g->snd, SND_UI_CLICK, NULL, 0, 0.7f);
    else if (g->mn.hovered) sound_play(g->snd, SND_UI_HOVER, NULL, 0, 0.4f);
    if (g_in.screen != SCREEN_NONE) g_in.screen = g->mn.screen;
    if (g->settings_dirty && g_in.screen != SCREEN_SETTINGS) {
        if (save_settings(g) != 0) log_warn("could not save settings to %s", g->o.config);
        g->settings_dirty = 0;
    }
}

/* The mouse in UI pixels, or -1 when unknown. */
static void cursor_ui(const game *g, const ui *u, int fb_w, int fb_h, float *mx, float *my)
{
    double wx, wy;
    int ww, wh;
    glfwGetCursorPos(g->win, &wx, &wy);
    glfwGetWindowSize(g->win, &ww, &wh);
    *mx = *my = -1.0f;
    if (ww > 0 && wh > 0) {
        *mx = (float)(wx * fb_w / ww) / u->scale;
        *my = (float)(wy * fb_h / wh) / u->scale;
    }
}

/* The 2D overlay: HUD, hotbar, inventory, F3 and menus. Returns the quads. */
static int draw_overlay(game *g, double dt, double alpha)
{
    int max_quads, fb_w, fb_h;
    ui_vertex *uv = renderer_ui_buffer(g->rd, &max_quads, &fb_w, &fb_h);
    ui u;
    ui_begin(&u, uv, max_quads, fb_w, fb_h);
    ui_set_scale(&u, g->st.gui_scale, fb_w, fb_h);
    int on_title = g_in.screen == SCREEN_TITLE;
    hud_state *hs = &g->hs;
    hs->panel = g_in.panel;
    hs->sel = g_in.sel_part;
    hs->held = NULL;
    hs->flying = g->pl.flying;
    hs->debug = g_in.debug;
    hs->inv = &g->inv;
    hs->hide_hints = !g->st.show_hints;
    hud_update(hs, &g->hl, g->in_menu ? 0.0f : (float)dt);
    if (!on_title) hud_draw(&u, &g->hl, hs);
    if (!on_title && !g->hl.dead && !g_in.panel && !g_in.inv_open) invui_hotbar(&u, &g->iu, &g->inv, (float)dt);
    float mx, my;
    cursor_ui(g, &u, fb_w, fb_h, &mx, &my);
    if (g_in.inv_open && !g->in_menu && !g->hl.dead) {
        invui_input in = {mx,
                          my,
                          g_in.ui_click,
                          g_in.ui_rclick,
                          g_in.keys[GLFW_KEY_LEFT_SHIFT] || g_in.keys[GLFW_KEY_RIGHT_SHIFT],
                          thermo_near_fire(g->th, &g->w, g->pl.pos, 3.0),
                          (float)dt};
        invui_result ir = invui_screen(&u, &g->iu, &g->inv, &in);
        toss(g, ir.drop);
        if (ir.crafted >= 0) sound_play(g->snd, SND_CRAFT, NULL, 0, 0.8f);
        else if (ir.changed) sound_play(g->snd, SND_UI_CLICK, NULL, 0, 0.5f);
    }
    if (g_in.debug && !on_title) draw_debug(g, &u, fb_w, fb_h, alpha);
    if (g->in_menu) run_menu(g, &u, mx, my, dt);
    return u.quads;
}

static void frame_render(game *g, double now, double dt)
{
    if (g_in.want_shot) {
        renderer_request_screenshot(g->rd, "screenshot.ppm");
        g_in.want_shot = 0;
    }
    if (g->o.screenshot && g->o.frames && g->frame == g->o.frames - 1)
        renderer_request_screenshot(g->rd, g->o.screenshot);
    if (!renderer_begin_frame(g->rd)) {
        jobs_poll(g->js, MESH_UPLOADS_PER_FRAME);
        world_schedule(&g->w);
        glfwWaitEventsTimeout(0.05);
        return;
    }
    jobs_poll(g->js, MESH_UPLOADS_PER_FRAME);
    world_schedule(&g->w);
    double alpha = g->acc / PHYS_DT;
    render_view v = build_view(g, now, dt, alpha);
    frame_sound(g, dt, &v);
    v.ui_quads = draw_overlay(g, dt, alpha);
    renderer_end_frame(g->rd, &g->w, &g->ph, &v, alpha);
}

/* The window title (frame rate and a few counters) and the autosave. */
static void frame_bookkeeping(game *g, double now)
{
    if (g->frame < INT_MAX) g->frame++; /* int overflow is UB; only --frames and the demo read it */
    g->fps_frames++;
    if (now - g->title_t >= 0.5) {
        double fps = g->fps_frames / (now - g->title_t);
        g->fps_frames = 0;
        g->title_t = now;
        render_stats rs = renderer_stats(g->rd);
        const item_stack *held = &g->inv.slot[g->inv.selected];
        char title[256];
        snprintf(title, sizeof title,
                 "blockclonia | %.0f fps | %.1f %.1f %.1f | %s%s | draws %d | bodies %d | mesh %u/%u MB", fps,
                 g->pl.pos.x, g->pl.pos.y, g->pl.pos.z, held->count ? item_get(held->id)->name : "empty hand",
                 g->pl.flying ? " | fly" : "", rs.draw_calls, g->ph.body_count, rs.pool_used_kb / 1024,
                 rs.pool_total_kb / 1024);
        glfwSetWindowTitle(g->win, title);
    }
    if (now - g->save_t >= AUTOSAVE_SECONDS) {
        g->save_t = now;
        world_save_all(&g->w); /* only columns edited since the last save */
        save_player(&g->sc);
    }
}

static void game_shutdown(game *g)
{
    if (g->settings_dirty && save_settings(g) != 0)
        log_warn("could not save settings to %s", g->o.config);
    log_info("saving and shutting down");
    log_set_fatal_hook(NULL, NULL);
    physics_settle_bodies(&g->ph, NULL);
    world_save_all(&g->w);
    if (!g->hl.dead) save_player(&g->sc);
    jobs_wait_idle(g->js);
    audio_device_close(g->audio); /* stops the callback before the mixer goes */
    sound_destroy(g->snd);
    mem_free(g->ents);
    mem_free(g->fx);
    world_destroy(&g->w);
    physics_destroy(&g->ph);
    thermo_destroy(g->th);
    jobs_destroy(g->js);
    renderer_destroy(g->rd);
    glfwDestroyWindow(g->win);
    glfwTerminate();
}

int main(int argc, char **argv)
{
    game *g = &g_game;
    if (!parse_args(argc, argv, &g->o)) return 2;
    mem_init();
    mesher_init();

    settings_default(&g->st);
    if (settings_load(&g->st, g->o.config) == 0) log_info("settings from %s", g->o.config);
    if (!g->o.have_radius) g->o.radius = g->st.render_distance;
    g->cfg_vsync = g->st.vsync; /* the file's value, kept while --no-vsync overrides it */
    if (!g->o.no_vsync) g->o.vsync = g->st.vsync;
    else g->st.vsync = 0;

    if (g->o.bench) {
        int r = bench_run(g->o.have_seed ? g->o.seed : 1337u);
        if (g->o.mem_stats) mem_print_stats();
        return r;
    }

    game_init(g);
    double prev = glfwGetTime();
    g->title_t = g->save_t = prev;
    while (!glfwWindowShouldClose(g->win)) {
        glfwPollEvents();
        double now = glfwGetTime();
        double dt = now - prev;
        prev = now;
        debug_frames_push(&g->dframes, (float)(dt * 1000.0));
        if (dt > 0.25) dt = 0.25;
        if (g_in.resized) {
            renderer_on_resize(g->rd);
            g_in.resized = 0;
        }
        frame_menus(g, dt);
        player_input in = frame_input(g);
        frame_simulate(g, &in, dt);
        frame_interact(g, dt);
        if (g->o.demo) demo_step(&g->w, &g->ph, g->fx, &g->pl, &g->demo, g->frame);
        world_update(&g->w, g->pl.pos.x, g->pl.pos.y + PLAYER_EYE, g->pl.pos.z);
        frame_render(g, now, dt);
        g_in.ui_click = g_in.ui_rclick = g_in.ui_up = g_in.ui_dn = g_in.ui_left = g_in.ui_right = g_in.ui_enter =
            g_in.ui_back = 0;
        frame_bookkeeping(g, now);
        if (g->o.frames && g->frame >= g->o.frames) break;
    }
    game_shutdown(g);
    if (g->o.mem_stats) mem_print_stats();
    return 0;
}
