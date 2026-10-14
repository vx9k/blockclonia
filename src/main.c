/* blockclonia: entry point, input, and the fixed-timestep game loop. */
#include "anim.h"
#include "audio_device.h"
#include "bench.h"
#include "camera.h"
#include "debug.h"
#include "discord.h"
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
#include "mp.h"
#include "net.h"
#include "netkeys.h"
#include "os.h"
#include "paths.h"
#include "physics.h"
#include "renderer.h"
#include "save.h"
#include "save_db.h"
#include "settings.h"
#include "sound.h"
#include "survival.h"
#include "sysinfo.h"
#include "thermo.h"
#include "ui.h"
#include "viewmodel.h"
#include "world.h"
#include "worlds.h"

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
    int world_given;     /* --world or --no-save: play that, never the backdrop */
    const char *screen;  /* open this screen at start (screenshots) */
    int debug;           /* start with F3 on */
    const char *give;    /* items to start with: "stone:32,apple:3" */
    int no_sound;
    int have_time;       /* --time */
    double time_of_day;  /* 0..1 */
    int host;            /* --host: open the world to other players */
    const char *join;    /* --join ADDR[:PORT]: play in someone else's world */
    int port;            /* --port: where to host (0: NET_PORT) */
    const char *name;    /* --name: shown to other players */
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
    int focused;        /* the window has the keyboard (for mute in background) */
    int focus_changed;
    int toggle_fullscreen; /* F11 */
    int sprint_latch;   /* toggle-sprint mode: Ctrl flips this */
    int sprint_tap;     /* W double-tapped: sprint until W is released */
    double last_w;      /* time of the last W press */
    int sprint_toggle;  /* copy of the setting, for the key callback */
    /* Menu input, gathered by the callbacks and consumed once per frame. */
    int ui_click, ui_down, ui_up, ui_dn, ui_left, ui_right, ui_enter, ui_back;
    int ui_letter;      /* the navigation key this frame was W/A/S/D/Space */
    int ui_backspace;
    char text[16];      /* printable ASCII typed this frame, for text fields */
    int text_len;
    /* Multiplayer chat line being typed (T). */
    int mp_active;      /* in a multiplayer session: T opens the chat */
    int chat_open, chat_send;
    char chat[NET_CHAT_MAX + 1];
    int chat_len;
} input_state;

static input_state g_in;
static paths g_paths; /* where files go unless --config or --world say otherwise */

/* ---------------------------------------------------------- arguments */

/* long long: `long` is 32-bit on 32-bit ARM, where (long)UINT32_MAX is -1. */
static int parse_int(const char *s, long long lo, long long hi, long long *out)
{
    char *end = NULL;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (errno || end == s || *end || v < lo || v > hi) return 0;
    *out = v;
    return 1;
}

/* "<a><sep><b>" with both integers in [lo, hi], e.g. 1280x720 or -40,12. */
static int parse_int_pair(const char *s, char sep, long long lo, long long hi, long long *a, long long *b)
{
    char *end = NULL;
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
    char *end = NULL;
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

/* Default locations, as paths.c resolves them. */
#define USAGE_WORLD                                                                          \
    "  --world DIR       save directory (default $XDG_DATA_HOME/blockclonia/worlds/world,\n" \
    "                    or ~/.local/share/blockclonia/worlds/world)\n"
#define USAGE_CONFIG                                                                             \
    "  --config FILE     settings file (default $XDG_CONFIG_HOME/blockclonia/blockclonia.cfg,\n" \
    "                    or ~/.config/blockclonia/blockclonia.cfg)\n"

static void usage(void)
{
    /* clang-format off */
    printf("usage: blockclonia [options]\n"
           "  --seed N          world seed (default: random, or the saved world's)\n"
           "  --radius N        render distance in chunks, 2-32 (default 8)\n"
           "  --size WxH        window size (default 1280x720)\n"
           "  --no-vsync        uncapped frame rate\n"
           "  --threads N       worker threads (default: cores - 1, fewer on low memory)\n"
           "  --pool-mb N       GPU vertex pool size (default: from radius)\n"
           "  --gpu N           Vulkan device index\n"
           USAGE_WORLD
           "  --no-save         do not load or save the world\n"
           "  --validate        enable Vulkan validation layers\n"
           "  --frames N        quit after N frames\n"
           "  --screenshot F    write a PPM of the last frame to F\n"
           "  --look YAW,PITCH  initial view angles in degrees\n"
           "  --spawn X,Z       spawn position (default: nearest dry land)\n"
           USAGE_CONFIG
           "  --play            skip the menus and play the default world (or --world)\n"
           "  --screen NAME     open title, worlds, create, pause, settings, controls or\n"
           "                    debug-health at start\n"
           "  --debug           start with the F3 overlay on\n"
           "  --give LIST       start with items, e.g. log:8,sand:4 (names as shown in game)\n"
           "  --time HH         start at this hour of the day (0-23)\n"
           "  --demo            scripted structural-collapse demo\n"
           "  --health-panel    start with the health panel (H) open\n"
           "  --hurt LIST       start injured: comma list of %s,\n"
           "                    pads (defibrillator pads on the chest)\n"
           "  --bench           run CPU benchmarks and exit (no window)\n"
           "  --mem-stats       print mimalloc statistics at exit\n"
           "  --no-sound        do not open an audio device\n"
           "  --host            let others join this world (UDP port 27450, or --port)\n"
           "  --join ADDR[:P]   play in the world another player hosts\n"
           "  --port N          UDP port to host on\n"
           "  --name NAME       name other players see (1-16 letters, digits, ' ', -, _, .)\n",
           hurt_names());
    /* clang-format on */
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
        {"--host", &o->host},
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
        o->world_given = 1;
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
        {"--port", &o->port, 1, 65535, NULL},
    };
    for (size_t i = 0; i < sizeof ints / sizeof ints[0]; i++) {
        if (strcmp(a, ints[i].name) != 0) continue;
        long long n = 0;
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
    static const char *const NAMES[] = {"title", "pause",     "settings", "controls", "debug-health",
                                        "game",  "inventory", "worlds",   "create"};
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
        o->world_given = 1;
    } else if (strcmp(a, "--screen") == 0) {
        *ok = valid_screen(v);
        o->screen = v;
    } else if (strcmp(a, "--screenshot") == 0) {
        o->screenshot = v;
    } else if (strcmp(a, "--give") == 0) {
        o->give = v;
    } else if (strcmp(a, "--hurt") == 0) {
        o->hurt = v;
    } else if (strcmp(a, "--join") == 0) {
        *ok = strlen(v) > 0 && strlen(v) < 200;
        o->join = v;
    } else if (strcmp(a, "--name") == 0) {
        *ok = net_name_valid(v);
        o->name = v;
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
    paths_resolve(&g_paths);
    o->world_dir = g_paths.world;
    o->config = g_paths.config;
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
    case GLFW_KEY_BACKSPACE: g_in.ui_backspace = 1; break;
    default: break;
    }
    /* A text field types these letters rather than moving with them. */
    switch (key) {
    case GLFW_KEY_W:
    case GLFW_KEY_A:
    case GLFW_KEY_S:
    case GLFW_KEY_D:
    case GLFW_KEY_SPACE: g_in.ui_letter = 1; break;
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
    if (action == GLFW_PRESS && key == GLFW_KEY_F11) g_in.toggle_fullscreen = 1;
    if (action == GLFW_PRESS && key == GLFW_KEY_F3 && g_in.screen != SCREEN_TITLE) g_in.debug = !g_in.debug;
    if (g_in.screen != SCREEN_NONE) {
        menu_key(key, action);
        return;
    }
    if (g_in.chat_open) {
        /* Typing: the game sees none of these keys. */
        g_in.keys[key] = 0;
        if (action == GLFW_RELEASE) return;
        if (key == GLFW_KEY_ESCAPE) g_in.chat_open = 0;
        else if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) g_in.chat_send = 1;
        else if (key == GLFW_KEY_BACKSPACE && g_in.chat_len > 0) {
            /* A whole UTF-8 character: back over its continuation bytes. */
            g_in.chat_len--;
            while (g_in.chat_len > 0 && ((unsigned char)g_in.chat[g_in.chat_len] & 0xC0) == 0x80) g_in.chat_len--;
        }
        g_in.chat[g_in.chat_len] = '\0';
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
    } else if (key == GLFW_KEY_T && g_in.mp_active) {
        g_in.chat_open = 2; /* 2: the T itself must not be typed */
        g_in.chat_len = 0;
        g_in.chat[0] = '\0';
        memset(g_in.keys, 0, sizeof g_in.keys);
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

/* Typed text for the menus' text fields: printable ASCII only, which is
 * what the font draws and what world names allow. */
static void char_cb(GLFWwindow *win, unsigned int cp)
{
    (void)win;
    if (g_in.chat_open && g_in.screen == SCREEN_NONE) {
        if (g_in.chat_open == 2) { /* the T that opened it */
            g_in.chat_open = 1;
            return;
        }
        /* UTF-8, shortest form; the font draws ASCII and shows the rest
         * as it can. Surrogates and controls are not text. */
        char u8[4] = {0};
        int n = 0;
        if (cp < 0x20 || cp == 0x7f || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) return;
        if (cp < 0x80) u8[n++] = (char)cp;
        else if (cp < 0x800) {
            u8[n++] = (char)(0xC0 | cp >> 6);
            u8[n++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            u8[n++] = (char)(0xE0 | cp >> 12);
            u8[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            u8[n++] = (char)(0x80 | (cp & 0x3F));
        } else {
            u8[n++] = (char)(0xF0 | cp >> 18);
            u8[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            u8[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            u8[n++] = (char)(0x80 | (cp & 0x3F));
        }
        if (g_in.chat_len + n > NET_CHAT_MAX) return;
        memcpy(g_in.chat + g_in.chat_len, u8, (size_t)n);
        g_in.chat_len += n;
        g_in.chat[g_in.chat_len] = '\0';
        return;
    }
    if (g_in.screen == SCREEN_NONE || cp < 0x20 || cp > 0x7e) return;
    if (g_in.text_len < (int)sizeof g_in.text) g_in.text[g_in.text_len++] = (char)cp;
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
    g_in.focused = focused != 0;
    g_in.focus_changed = 1;
}

/* --------------------------------------------------------------- game */

typedef struct {
    world *w;
    physics *ph;
    const player *pl;
    const inventory *inv;
    const thermo *th;
    save_db *db;
} save_ctx;

/* The world's on_save hook: the player goes into the transaction that
 * saves the columns, so the two on disk always match. The hook's type
 * fixes the non-const parameter. */
/* cppcheck-suppress constParameterCallback */
static int save_player(void *user)
{
    const save_ctx *sc = user;
    uint8_t buf[PLAYER_FILE_SIZE] = {0};
    size_t n = survival_encode_player(sc->pl, sc->inv, thermo_day_time(sc->th), buf, sizeof buf);
    if (n && save_db_put_blob(sc->db, "player", buf, n) == 0) return 0;
    log_warn("could not save the player");
    return -1;
}

/* 1 if a saved player was loaded into pl and inv, -1 if it could not be
 * read (a database error). */
static int load_player(save_db *db, player *pl, inventory *inv, double *day)
{
    if (!db) return 0;
    uint8_t buf[PLAYER_FILE_SIZE + 1] = {0};
    long n = save_db_get_blob(db, "player", buf, sizeof buf);
    if (n < 0) return n == -2 ? -1 : 0;
    if (survival_decode_player(pl, inv, day, buf, (size_t)n) != 0) {
        log_warn("the saved player is corrupt; starting fresh");
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
    world_save_all(sc->w); /* and the player, through on_save */
}

/* --give: "name:count" pairs; names match case-insensitively, spaces as
 * underscores ("plant_fibre:4"). */
static void give_items(inventory *inv, const char *list)
{
    char buf[256] = {0};
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
    mp *mp;         /* the session, when there is one */
    int client;     /* the world is someone else's: edits arrive from the host */
    sound *snd;
    fx_state *fx;
} world_hooks;

static void on_block_changed(void *user, int x, int y, int z, uint8_t old_id, uint8_t new_id)
{
    world_hooks *h = user;
    if (h->client) {
        /* The host simulates; physics here only predicts the player, so
         * nothing may wake its structure or water queues. What changed is
         * heard and seen as if done here. */
        dvec3 at = dv3(x + 0.5, y + 0.5, z + 0.5);
        if (new_id == B_AIR && old_id != B_AIR && old_id != B_WATER) {
            sound_play(h->snd, SND_BREAK, &at, old_id, 0.8f);
            fx_break(h->fx, old_id, x, y, z, 12);
        } else if (old_id == B_AIR && block_solid(new_id)) {
            sound_play(h->snd, SND_PLACE, &at, new_id, 0.8f);
        }
    } else {
        physics_on_block_changed(h->ph, x, y, z, old_id, new_id);
    }
    thermo_on_block_changed(h->th, h->w, x, y, z, old_id, new_id);
    if (h->mp) mp_on_block_changed(h->mp, x, y, z);
}

static void on_column_unload(void *user, const column *c)
{
    world_hooks *h = user;
    physics_on_column_unload(h->ph, c);
}

/* What the skin is exposed to: air, fires, the ground under the feet. */

/* --hurt: start with a chosen set of injuries, for screenshots and for
 * trying treatments without having to get hurt first. The names and what
 * they do come from health.h's HEALTH_DEBUG_KINDS, the same table the F3
 * debug menu (K) uses, so the two can't drift apart. */
static void apply_hurt(health *h, const char *list)
{
    char buf[256] = {0};
    snprintf(buf, sizeof buf, "%s", list);
    char *save = NULL;
    for (const char *t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
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
    int cfg_keep;             /* the settings file could not be used: never written over */
    uint32_t seed;
    sys_info sys;
    sys_budget plan;          /* budgets for this machine (sysinfo.h) */
    GLFWwindow *win;
    world_entry *world_list;  /* WORLDS_MAX, refreshed when the list opens */
    menu_world *world_rows;
    jobs *js;
    renderer *rd;
    int backdrop;             /* the menus' fixed, unsaved world is loaded, not a player's */
    int world_count;
    int fullscreen;           /* what the window is now */
    int win_x, win_y, win_w, win_h; /* the windowed frame, restored on leaving fullscreen */
    int snd_inv_open;
    char world_path[WORLDS_PATH_MAX]; /* the world being played (o.world_dir points here) */
    char worlds_root[WORLDS_PATH_MAX];
    world w;
    physics ph;
    thermo *th;
    world_hooks hooks;
    save_db *db;              /* NULL: not saving */
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
    discord *dc;              /* NULL: the Discord status is off or this build has no application */
    char world_name[SAVE_NAME_MAX + 1]; /* the name the world was given, "" for none */
    int64_t session_start, play_start; /* Unix seconds: the game started, the world was opened */
    mp *mp;                   /* multiplayer session, NULL in single player */
    int client;               /* playing in someone else's world */
    char note[512];           /* why the last session ended, for the title */

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
    if (g->cfg_keep) return 0; /* warned at start */
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
    /* Muted while the window is in the background, if the player asked. */
    float master = g->st.mute_unfocused && !g_in.focused ? 0.0f : (float)g->st.volume_master / 100.0f;
    sound_set_volumes(g->snd, master, (float)g->st.volume_effects / 100.0f, (float)g->st.volume_ambient / 100.0f,
                      (float)g->st.volume_ui / 100.0f, (float)g->st.volume_body / 100.0f);
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
    int on_title = g->backdrop;
    float wind = wind_level(&g->w, &g->pl, v->eye), water = water_level(&g->w, &g->pl, v->eye);
    sound_ambience(g->snd, wind * (on_title ? 0.6f : 1.0f), water);
    dvec3 fires[SOUND_MAX_FIRES] = {0};
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
        else if (!strcmp(o->screen, "worlds")) g_in.screen = SCREEN_WORLDS;
        else if (!strcmp(o->screen, "create")) g_in.screen = SCREEN_CREATE_WORLD;
        else g_in.screen = SCREEN_NONE;
        g_in.inv_open = !strcmp(o->screen, "inventory");
    }
    if (g_in.screen != SCREEN_NONE) {
        /* Settings and controls return to the pause menu in a world, to
         * the title over the backdrop. */
        if (g_in.screen != SCREEN_TITLE) g->mn.screen = g->backdrop ? SCREEN_TITLE : SCREEN_PAUSE;
        menu_open(&g->mn, g_in.screen);
    }
}

/* Creates the default directories and copies files older builds kept in
 * the working directory there, once. The copies leave the originals alone
 * and never overwrite, so running an old build again still finds its own. */
static void use_xdg_paths(const options *o)
{
    char dir[PATHS_MAX] = {0};
    if (o->config == g_paths.config && g_paths.xdg_config && paths_parent(o->config, dir, sizeof dir) == 0) {
        if (paths_make_dirs(dir) != 0) log_warn("cannot create %s", dir);
        int r = save_copy_file("blockclonia.cfg", o->config, SETTINGS_MAX_FILE);
        if (r > 0) log_info("copied ./blockclonia.cfg to %s; settings are kept there now", o->config);
        if (r < 0) log_warn("could not copy ./blockclonia.cfg to %s", o->config);
    }
    if (o->world_dir == g_paths.world && g_paths.xdg_world && !o->bench) {
        if (paths_make_dirs(o->world_dir) != 0) log_warn("cannot create %s", o->world_dir);
        int n = save_copy_world("world", o->world_dir);
        if (n > 0) log_info("copied %d files from ./world to %s; the world is kept there now", n, o->world_dir);
    }
}

/* Opens the world's database into *db, first bringing in a world older
 * builds kept as files in the same folder, and returns its seed (a new
 * world takes --seed or a random one). Saving is off if that fails. */
static uint32_t choose_seed(options *o, save_db **db)
{
    uint32_t seed = o->have_seed ? o->seed : (uint32_t)time(NULL) * 2654435761u;
    *db = NULL;
    if (!o->world_dir) return seed;
    if (save_ensure_dir(o->world_dir) == 0) *db = save_db_open(o->world_dir);
    if (!*db) {
        log_error("cannot open world '%s'; saving disabled", o->world_dir);
        o->world_dir = NULL;
        return seed;
    }
    /* Starting a new world over an old one that failed to import would
     * hide it for good, so a failed import turns saving off instead. */
    int n = save_db_import(*db, o->world_dir);
    if (n < 0) {
        log_error("could not import the world files in '%s'; saving disabled", o->world_dir);
        save_db_close(*db);
        *db = NULL;
        o->world_dir = NULL;
        return seed;
    }
    if (n > 0) log_info("imported %d world files into %s/world.db; they stay but are no longer read", n, o->world_dir);
    uint32_t saved = 0;
    int got = save_db_get_seed(*db, &saved);
    if (got == 0) {
        if (o->have_seed && saved != o->seed) log_warn("--seed ignored: world '%s' uses seed %u", o->world_dir, saved);
        /* A world saved before generators were recorded was made by the first. */
        int64_t gen = 0;
        if (save_db_get_meta(*db, "generator", &gen) == -1) save_db_set_meta(*db, "generator", 1);
        return saved;
    }
    if (got != -1) {
        /* Storing a new seed would change every column not saved yet. */
        log_error("cannot read world '%s'; saving disabled", o->world_dir);
        save_db_close(*db);
        *db = NULL;
        o->world_dir = NULL;
        return seed;
    }
    if (save_db_set_seed(*db, seed) != 0) log_warn("could not store the world seed");
    save_db_set_meta(*db, "generator", WORLDGEN_VERSION);
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
    glfwSetCharCallback(win, char_cb);
    glfwSetMouseButtonCallback(win, mouse_button_cb);
    glfwSetCursorPosCallback(win, cursor_cb);
    glfwSetScrollCallback(win, scroll_cb);
    glfwSetFramebufferSizeCallback(win, resize_cb);
    glfwSetWindowFocusCallback(win, focus_cb);
    if (glfwRawMouseMotionSupported()) glfwSetInputMode(win, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    return win;
}

/* Borderless fullscreen on the monitor the window is on (the primary one
 * if GLFW cannot tell), at that monitor's current mode, so no mode switch
 * flickers the screen; leaving restores the windowed frame. */
static void set_fullscreen(game *g, int on)
{
    if (on == g->fullscreen) return;
    if (on) {
        glfwGetWindowPos(g->win, &g->win_x, &g->win_y);
        glfwGetWindowSize(g->win, &g->win_w, &g->win_h);
        GLFWmonitor *mon = glfwGetPrimaryMonitor();
        int count = 0;
        GLFWmonitor **mons = glfwGetMonitors(&count);
        for (int i = 0; i < count; i++) {
            int mx = 0, my = 0;
            const GLFWvidmode *vm = glfwGetVideoMode(mons[i]);
            glfwGetMonitorPos(mons[i], &mx, &my);
            int cx = g->win_x + g->win_w / 2, cy = g->win_y + g->win_h / 2;
            if (vm && cx >= mx && cx < mx + vm->width && cy >= my && cy < my + vm->height) mon = mons[i];
        }
        const GLFWvidmode *mode = mon ? glfwGetVideoMode(mon) : NULL;
        if (!mode) return;
        glfwSetWindowMonitor(g->win, mon, 0, 0, mode->width, mode->height, mode->refreshRate);
    } else {
        glfwSetWindowMonitor(g->win, NULL, g->win_x, g->win_y, g->win_w > 0 ? g->win_w : g->o.width,
                             g->win_h > 0 ? g->win_h : g->o.height, 0);
    }
    g->fullscreen = on;
    g_in.resized = 1;
}

/* The world, physics, heat, the player and their things. */
/* The menus' world: a fixed seed, place and hour, never saved, so the
 * title looks the same every time and no world of the player's is open
 * (or locked) while they choose, rename or delete one. */
#define BACKDROP_SEED 20231012u

/* Loads a world: the player's in dir (NULL: --no-save, nothing kept), or
 * the menus' backdrop. The renderer and job threads stay up across
 * worlds; everything that belongs to one world is made here and undone by
 * close_world. */
static void open_world(game *g, const char *dir, int backdrop)
{
    options *o = &g->o;
    g->backdrop = backdrop;
    g->db = NULL;
    if (backdrop) {
        o->world_dir = NULL;
        g->seed = BACKDROP_SEED;
    } else {
        if (dir && dir != g->world_path) snprintf(g->world_path, sizeof g->world_path, "%s", dir);
        o->world_dir = dir ? g->world_path : NULL;
        g->seed = choose_seed(o, &g->db);
    }
    log_info("%s, seed %u", backdrop ? "menu backdrop" : (o->world_dir ? o->world_dir : "unsaved world"), g->seed);

    world_init(&g->w, g->seed, o->radius, g->js, g->db);
    int64_t gen = WORLDGEN_VERSION;
    if (g->db) save_db_get_meta(g->db, "generator", &gen);
    if (gen == 1) g->w.generator = worldgen_column_v1; /* no seams against what is saved */
    physics_init(&g->ph, &g->w);
    g->th = thermo_create(g->seed);
    g->hooks = (world_hooks){&g->ph, g->th, &g->w, NULL, 0, g->snd, g->fx};
    g->w.edit_user = &g->hooks;
    g->w.on_block_changed = on_block_changed;
    g->w.on_column_unload = on_column_unload;
    g->sc = (save_ctx){&g->w, &g->ph, &g->pl, &g->inv, g->th, g->db};
    log_set_fatal_hook(save_on_fatal, &g->sc);
    renderer_bind_world(g->rd, &g->w);

    if (o->have_spawn && !backdrop) {
        g->sx = o->spawn_x + 0.5;
        g->sz = o->spawn_z + 0.5;
    } else {
        find_spawn(g->seed, &g->sx, &g->sz);
    }
    world_load_blocking(&g->w, g->sx, g->sz, 1);
    player_spawn(&g->pl, &g->w, g->sx, g->sz);
    if (o->have_look && !backdrop) {
        g->pl.yaw = o->look_yaw * (float)(MC_PI / 180.0);
        g->pl.pitch = clampf(o->look_pitch, -89.0f, 89.0f) * (float)(MC_PI / 180.0);
    }
    inv_starting_kit(&g->inv);
    double day = THERMO_DAY_START;
    int loaded = !o->have_spawn && !o->demo ? load_player(g->db, &g->pl, &g->inv, &day) : 0;
    if (backdrop) thermo_set_day_time(g->th, THERMO_DAY_START);
    if (loaded < 0) {
        /* Playing on is fine; saving would write a fresh player over it. */
        log_error("cannot read the saved player in '%s'; saving disabled", o->world_dir);
        g->w.db = NULL;
        g->sc.db = NULL;
    }
    if (loaded > 0) {
        thermo_set_day_time(g->th, day);
        world_load_blocking(&g->w, g->pl.pos.x, g->pl.pos.z, 1);
        log_info("player restored from %s/world.db", o->world_dir);
    }
    /* Only now: a save before this point would write the fresh player
     * over the saved one. */
    g->w.save_user = &g->sc;
    g->w.on_save = save_player;
    if (o->give && !backdrop) give_items(&g->inv, o->give);
    if (o->have_time && !backdrop) thermo_set_day_time(g->th, o->time_of_day);
    log_info("spawn at %.1f %.1f %.1f with %d worker threads", g->pl.pos.x, g->pl.pos.y, g->pl.pos.z,
             jobs_worker_count(g->js));

    health_init_varied(&g->hl, g->seed ^ 0x9E3779B9u);
    if (o->hurt && !backdrop) apply_hurt(&g->hl, o->hurt);

    fx_init(g->fx, g->seed);
    interact_init(&g->ia, g->seed ^ 0xA5A5u);
    viewmodel_init(&g->vm);
    camera_init(&g->cam);
    g->death_rng = g->seed;
    g->hs.alert_count = 0;
    if (g->db) save_db_set_meta(g->db, "last_played", (int64_t)time(NULL));
    /* The name goes out to Discord, and world files are shared: only a
     * valid one is used. */
    g->world_name[0] = '\0';
    char name[SAVE_NAME_MAX + 1] = {0};
    long len = g->db ? save_db_get_blob(g->db, "name", name, SAVE_NAME_MAX) : -1;
    if (len > 0 && save_name_valid(name, (size_t)len)) memcpy(g->world_name, name, (size_t)len + 1);
    g->play_start = (int64_t)time(NULL);
}

/* Saves the world being played (the backdrop has nothing to save) and
 * frees everything open_world made. */
static void close_world(game *g)
{
    /* Leaving ends the session: a host's players are told, a client's
     * world was never saved here. */
    mp_destroy(g->mp);
    g->mp = NULL;
    g->hooks.mp = NULL;
    g_in.mp_active = g_in.chat_open = 0;
    if (g->client) g->w.on_save = NULL;
    g->client = 0;
    log_set_fatal_hook(NULL, NULL);
    physics_settle_bodies(&g->ph, NULL);
    if (g->hl.dead) g->w.on_save = NULL; /* quitting dead keeps the last saved player */
    world_save_all(&g->w);
    world_destroy(&g->w); /* waits for the jobs and hands the meshes back to the renderer */
    save_db_close(g->db);
    g->db = NULL;
    physics_destroy(&g->ph);
    thermo_destroy(g->th);
    g->th = NULL;
}

/* ---------------------------------------------------------- multiplayer */

/* Next to the settings file: this player's identity key and the keys of
 * hosts joined before. */
static void key_path(const game *g, const char *file, char *out, size_t cap)
{
    char dir[PATHS_MAX] = {0};
    if (paths_parent(g->o.config, dir, sizeof dir) != 0) snprintf(dir, sizeof dir, ".");
    snprintf(out, cap, "%s/%s", dir, file);
}

static void my_identity(const game *g, nc_keypair *id, char *name, size_t cap)
{
    char path[PATHS_MAX + 32] = {0};
    key_path(g, "identity.key", path, sizeof path);
    nc_init();
    netkeys_identity(path, id);
    if (g->o.name) {
        snprintf(name, cap, "%s", g->o.name);
    } else {
        char hex[2 * NC_KEY_BYTES + 1] = {0};
        nc_key_hex(id->pk, hex);
        snprintf(name, cap, "Player-%.4s", hex);
    }
}

static mp_world mp_ctx(game *g)
{
    static char known[PATHS_MAX + 32];
    key_path(g, "known_players", known, sizeof known);
    return (mp_world){&g->w, &g->ph, g->th, g->fx, &g->pl, &g->hl, &g->inv, g->seed, known};
}

/* Opens the world being played to other players. */
static void start_host(game *g)
{
    nc_keypair id = {0};
    char name[NET_NAME_MAX + 1] = {0};
    my_identity(g, &id, name, sizeof name);
    net *n = net_host(&id, name, (uint16_t)g->o.port);
    nc_wipe(&id, sizeof id);
    if (!n) {
        snprintf(g->hs.msg, sizeof g->hs.msg, "Could not host (port in use?)");
        g->hs.msg_age = 0.0f;
        return;
    }
    mp_world ctx = mp_ctx(g);
    g->mp = mp_create(n, &ctx);
    g->hooks.mp = g->mp;
    g_in.mp_active = 1;
}

/* Someone else's world: empty until the host sends it, never saved. */
static void open_remote_world(game *g, uint32_t seed)
{
    options *o = &g->o;
    g->backdrop = 0;
    g->db = NULL;
    o->world_dir = NULL;
    g->seed = seed;
    world_init(&g->w, seed, o->radius, g->js, NULL);
    physics_init(&g->ph, &g->w);
    g->th = thermo_create(seed);
    g->hooks = (world_hooks){&g->ph, g->th, &g->w, g->mp, 1, g->snd, g->fx};
    g->w.edit_user = &g->hooks;
    g->w.on_block_changed = on_block_changed;
    g->sc = (save_ctx){&g->w, &g->ph, &g->pl, &g->inv, g->th, NULL};
    renderer_bind_world(g->rd, &g->w);
    memset(&g->pl, 0, sizeof g->pl);
    inv_init(&g->inv);
    health_init(&g->hl, seed);
    fx_init(g->fx, seed);
    interact_init(&g->ia, seed);
    viewmodel_init(&g->vm);
    camera_init(&g->cam);
    g->hs.alert_count = 0;
    g->world_name[0] = '\0';
    g->play_start = (int64_t)time(NULL);
    g->client = 1;
}

/* Polls the session until done() holds, it ends, the time runs out or the
 * window closes; the window stays responsive meanwhile. */
static int wait_session(game *g, int (*done)(const game *g), double seconds)
{
    double end = glfwGetTime() + seconds;
    while (!mp_ended(g->mp, NULL) && !glfwWindowShouldClose(g->win) && glfwGetTime() < end) {
        glfwPollEvents();
        double now = glfwGetTime();
        mp_poll(g->mp, now);
        if (done(g)) return 1;
        if (g->w.grid) {
            world_update(&g->w, g->pl.pos.x, g->pl.pos.y + PLAYER_EYE, g->pl.pos.z);
            jobs_poll(g->js, MESH_UPLOADS_PER_FRAME);
        }
        mp_flush(g->mp, now);
        os_sleep(0.005);
    }
    return 0;
}

static int welcomed(const game *g)
{
    uint32_t seed = 0;
    return mp_welcome(g->mp, &seed);
}

static int ready(const game *g) { return mp_ready(g->mp); }

/* --join: the host's world replaces the one loaded at start. On failure
 * the menus come back, saying why. */
static void start_join(game *g, const char *where)
{
    char addr[200] = {0}, host_id[NETKEYS_HOST_MAX + 1] = {0};
    snprintf(addr, sizeof addr, "%s", where);
    long long port = NET_PORT;
    char *colon = strrchr(addr, ':');
    if (colon && parse_int(colon + 1, 1, 65535, &port)) *colon = '\0';
    snprintf(host_id, sizeof host_id, "%s:%lld", addr, port);
    char known[PATHS_MAX + 32] = {0};
    key_path(g, "known_hosts", known, sizeof known);
    uint8_t pinned[NC_KEY_BYTES] = {0};
    int have_pin = netkeys_known(known, host_id, pinned);
    if (have_pin < 0) {
        /* Joining without checking would quietly drop the protection. */
        snprintf(g->note, sizeof g->note, "Cannot read %s; not joining without checking the host's key", known);
        log_error("%s", g->note);
        return;
    }

    nc_keypair id = {0};
    char name[NET_NAME_MAX + 1] = {0};
    my_identity(g, &id, name, sizeof name);
    net *n = net_join(&id, name, addr, (uint16_t)port, 0, have_pin ? pinned : NULL);
    nc_wipe(&id, sizeof id);
    if (!n) {
        snprintf(g->note, sizeof g->note, "Cannot reach %s", host_id);
        return;
    }
    close_world(g);
    mp_world ctx = mp_ctx(g);
    g->mp = mp_create(n, &ctx);
    log_info("joining %s as %s", host_id, name);
    uint32_t seed = 0;
    if (wait_session(g, welcomed, 15.0) && mp_welcome(g->mp, &seed)) {
        open_remote_world(g, seed);
        ctx = mp_ctx(g);
        mp_attach(g->mp, &ctx);
        if (wait_session(g, ready, 15.0)) {
            /* First visit: remember the host's key, so an impostor at
             * this address is caught next time. */
            const uint8_t *key = net_host_key(n);
            if (!have_pin && key && netkeys_pin(known, host_id, key) == 0)
                log_info("pinned the identity key of %s", host_id);
            g_in.mp_active = 1;
            g_in.screen = SCREEN_NONE;
            return;
        }
    }
    const char *why = "the host did not answer";
    mp_ended(g->mp, &why);
    snprintf(g->note, sizeof g->note, "Could not join %s: %s", host_id, why);
    log_error("%s", g->note);
    if (g->w.grid) {
        close_world(g);
    } else {
        mp_destroy(g->mp);
        g->mp = NULL;
        g->client = 0;
    }
    open_world(g, NULL, 1);
    g_in.screen = SCREEN_TITLE;
}

/* What lives across worlds: the job threads and the renderer. */
static void game_init_world(game *g)
{
    const options *o = &g->o;
    int workers = o->threads >= 0 ? o->threads : g->plan.workers;
    g->js = jobs_create(workers);

    render_opts ro = {.vsync = o->vsync,
                      .validate = o->validate,
                      .render_radius = o->radius,
                      .gpu_index = o->gpu,
                      .pool_mb = o->pool_mb,
                      .screenshots = o->screenshot != NULL,
                      .filtering = g->st.filtering,
                      .pipeline_cache = g_paths.pipelines};
    g->rd = renderer_create(g->win, &ro);
}

/* Whether the game starts in a world rather than over the backdrop: asked
 * to play, to show something only a world has, or given one by name. */
static int starts_in_world(const options *o)
{
    if (o->play || o->demo || o->open_panel || o->hurt || o->world_given) return 1;
    return o->screen && (!strcmp(o->screen, "game") || !strcmp(o->screen, "pause") || !strcmp(o->screen, "inventory") ||
                         !strcmp(o->screen, "debug-health"));
}

static void refresh_worlds(game *g);

static void game_init(game *g)
{
    g->win = open_window(&g->o);
    g_in.focused = glfwGetWindowAttrib(g->win, GLFW_FOCUSED) != 0;
    game_init_world(g);

    char mi[48] = {0};
    mem_version_string(mi, sizeof mi);
    int gmaj = 0, gmin = 0, grev = 0;
    glfwGetVersion(&gmaj, &gmin, &grev);
    /* The Vulkan version and features get their own lines in F3. */
    snprintf(g->versions, sizeof g->versions, "%s  GLFW %d.%d.%d", mi, gmaj, gmin, grev);

    g->fx = mem_alloc(sizeof *g->fx);
    invui_init(&g->iu);
    g->ents = mem_alloc(sizeof(entity_instance) * RENDER_MAX_ENTS);
    g->world_list = mem_alloc(sizeof(world_entry) * WORLDS_MAX);
    g->world_rows = mem_alloc(sizeof(menu_world) * WORLDS_MAX);
    if (paths_parent(g_paths.world, g->worlds_root, sizeof g->worlds_root) != 0)
        snprintf(g->worlds_root, sizeof g->worlds_root, "worlds");
    if (starts_in_world(&g->o)) open_world(g, g->o.world_dir, 0);
    else open_world(g, NULL, 1);
    g->snd = sound_create(g->seed, SOUND_RATE, SOUND_CHANNELS);
    apply_volumes(g);
    g->audio = g->o.no_sound ? NULL : audio_device_open(sound_render, g->snd, SOUND_RATE, SOUND_CHANNELS);
    log_info("sound: %s", g->audio ? audio_device_name(g->audio) : "off");

    g_in.treat = -1;
    g_in.slot = -1;
    g_in.panel = g->o.open_panel;
    g_in.debug = g->o.debug;
    open_start_screen(g);
    refresh_worlds(g); /* --screen worlds opens the list without asking for it */
    if (g->o.join) start_join(g, g->o.join);
    else if (g->o.host && !g->backdrop) start_host(g);
    snprintf(g->mn.footer, sizeof g->mn.footer, "%s", g->versions);
}

#ifndef MC_DISCORD_APP_ID
#define MC_DISCORD_APP_ID "" /* CMake sets it from -DMC_DISCORD_APP_ID */
#endif

/* Starts or stops the Discord status to match the setting. Builds without
 * an application ID never connect. */
static void apply_discord(game *g)
{
    if (g->st.discord && !g->dc) g->dc = discord_create(MC_DISCORD_APP_ID);
    else if (!g->st.discord && g->dc) {
        discord_destroy(g->dc);
        g->dc = NULL;
    }
}

/* What the status shows: the menus, or how the body is doing in which
 * world, with the time since it was opened. Sent only when it changes,
 * and discord.c limits how often. */
static void update_discord(game *g)
{
    if (!g->dc) return;
    if (g->backdrop) {
        discord_set(g->dc, "In the menus", NULL, g->session_start);
        return;
    }
    char cond[128] = {0}, where[64] = {0};
    hud_condition(&g->hl, cond, sizeof cond);
    if (g->world_name[0]) snprintf(where, sizeof where, "In %s", g->world_name);
    discord_set(g->dc, cond, where[0] ? where : NULL, g->play_start);
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
    if (g->in_menu && !g->mp) {
        g->acc = 0.0; /* the simulation stands still */
        memset(g_in.keys, 0, sizeof g_in.keys); /* no keys stuck down on return */
        g_in.click_break = g_in.click_place = g_in.click_pick = 0;
        g_in.attack = 0;
    } else {
        /* Other players keep playing behind the menus. */
        if (g->in_menu) {
            memset(g_in.keys, 0, sizeof g_in.keys);
            g_in.click_break = g_in.click_place = g_in.click_pick = 0;
            g_in.attack = 0;
        }
        g->acc += dt;
    }
    /* The inventory screen frees the cursor; closing it takes it back. */
    if (g_in.inv_open && !g->inv_was_open) {
        set_captured(g->win, 0);
        g->iu.open_t = 0.0f;
    } else if (!g_in.inv_open && g->inv_was_open) {
        if (g->client) {
            inv_return_cursor(&g->inv);
            mp_client_op(g->mp, (mp_op){MP_OP_RETURN_CURSOR, 0, 0, 0});
        } else {
            survival_toss(&g->ph, &g->inv, &g->pl, inv_return_cursor(&g->inv));
        }
        if (!g->in_menu) set_captured(g->win, 1);
    }
    g->inv_was_open = g_in.inv_open;
    if (g->hl.dead) g_in.inv_open = 0;
    /* The panel, death and unconsciousness freeze the player's input. */
    g->frozen = g->in_menu || g_in.panel || g_in.inv_open || g_in.chat_open || g->hl.dead ||
                g->hl.conscious == CONS_UNCONSCIOUS;
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
    return in;
}

/* Fixed 60 Hz steps: physics, what it did to the body, and the body. */
static void frame_simulate(game *g, const player_input *raw, double dt)
{
    g->ph.contact[0].magnet = !g->hl.dead && !g->in_menu;
    int steps = 0;
    float landing = 0.0f; /* hardest landing this frame, for the camera */
    double t0 = glfwGetTime();
    if (g->client) {
        /* The host simulates; this side predicts its own movement. */
        while (g->acc >= PHYS_DT && steps < MAX_STEPS_PER_FRAME) {
            mp_client_step(g->mp, raw, g_in.attack && !g->frozen);
            if ((float)g->pl.fall_speed > landing) landing = (float)g->pl.fall_speed;
            g->acc -= PHYS_DT;
            steps++;
        }
        if (steps == MAX_STEPS_PER_FRAME) g->acc = 0.0;
        g->landing = landing;
        fx_step(g->fx, &g->w, dt);
        g->phys_ms = anim_approach(g->phys_ms, (float)((glfwGetTime() - t0) * 1000.0), 5.0f, (float)dt);
        g->pose = camera_update(&g->cam, &g->pl, landing, g->hl.hurt_flash, g->st.view_bob, g->st.fov_effects,
                                g->in_menu ? 0.0f : (float)dt);
        return;
    }
    player_input lim = *raw;
    survival_limit_input(&g->hl, &g->pl, &lim);
    const player_input *in = &lim;
    while (g->acc >= PHYS_DT && steps < MAX_STEPS_PER_FRAME) {
        survival_before sb = survival_capture(&g->pl);
        if (g->mp) mp_host_step(g->mp, in);
        else physics_step(&g->ph, &g->pl, in);
        if ((float)g->pl.fall_speed > landing) landing = (float)g->pl.fall_speed;
        health_env env = {0};
        survival_env(&g->w, &g->pl, &sb, g->actions, &env);
        survival_thermal(g->th, &g->w, &g->pl, &env);
        g->actions = 0;
        survival_impacts(&g->hl, &g->w, &g->ph.contact[0], &g->pl, &sb);
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
    if (steps && physics_pickup(&g->ph, 0, inv_accept, &g->inv)) sound_play(g->snd, SND_PICKUP, NULL, 0, 0.7f);
    if (!g->in_menu || g->mp) {
        thermo_step(g->th, &g->w, dt, g->pl.pos);
        fx_step(g->fx, &g->w, dt);
    }
    g->phys_ms = anim_approach(g->phys_ms, (float)((glfwGetTime() - t0) * 1000.0), 5.0f, (float)dt);
    g->pose = camera_update(&g->cam, &g->pl, landing, g->hl.hurt_flash, g->st.view_bob, g->st.fov_effects,
                            g->in_menu ? 0.0f : (float)dt);
}

/* A client's hands: clicks, treatments and respawning go to the host,
 * which sends back what happened. The arm swings here at once. */
static void client_hands(game *g, double dt)
{
    if (!g->frozen) {
        if (g_in.click_break) mp_client_click(g->mp, MP_CLICK_BREAK);
        if (g_in.click_place) mp_client_click(g->mp, MP_CLICK_PLACE);
        if (g_in.click_pick) mp_client_click(g->mp, MP_CLICK_PICK);
        if (g_in.drop) mp_client_click(g->mp, g_in.drop == 2 ? MP_CLICK_DROP_ALL : MP_CLICK_DROP);
        if (g_in.click_break || g_in.click_place) g->ia.swing = 1.0f;
    }
    g->ia.swing = fmaxf(0.0f, g->ia.swing - (float)dt * 3.0f);
    g->ia.swinging = g_in.attack && g->hit.hit && !g->frozen;
    ipos at = {0, 0, 0};
    float progress = 0.0f;
    int cracking = mp_client_crack(g->mp, &at, &progress);
    g->crack = (fx_crack){cracking && !g->pl.flying, at.x, at.y, at.z, progress};
    g_in.click_break = g_in.click_place = g_in.click_pick = g_in.drop = 0;
    if (g_in.treat >= 0) {
        if (!g->hl.dead) {
            mp_client_op(g->mp, (mp_op){MP_OP_TREAT, (uint8_t)g_in.sel_part, (uint8_t)g_in.treat, 0});
            if (g_in.treat == TREAT_EAT || g_in.treat == TREAT_DRINK) g->eat_anim = 1.0f;
        }
        g_in.treat = -1;
    }
    if (g_in.respawn) {
        if (g->hl.dead) mp_client_op(g->mp, (mp_op){MP_OP_RESPAWN, 0, 0, 0});
        g_in.respawn = 0;
    }
    const char *msg = mp_take_message(g->mp);
    if (msg) {
        snprintf(g->hs.msg, sizeof g->hs.msg, "%s", msg);
        g->hs.msg_age = 0.0f;
    }
    g->hs.msg_age += (float)dt;
}

/* The hands: breaking, placing, using; then treatments and respawning. */
static void frame_interact(game *g, double dt)
{
    player *pl = &g->pl;
    g->eye = dv3(pl->pos.x, pl->pos.y + PLAYER_EYE - (double)g->cam.crouch, pl->pos.z);
    g->look = look_dir(pl->yaw, pl->pitch);
    g->hit = physics_raycast(&g->w, g->eye, g->look, REACH);
    if (g->client) {
        client_hands(g, dt);
        return;
    }
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
            survival_drop_all(&g->ph, &g->inv, pl, &g->death_rng);
            world_load_blocking(&g->w, g->sx, g->sz, 1);
            player_spawn(pl, &g->w, g->sx, g->sz);
            health_init_varied(&g->hl, g->seed ^ (uint32_t)g->frame * 2654435761u);
            inv_starting_kit(&g->inv);
            g->hs.alert_count = 0;
            g_in.panel = 0;
            log_info("respawned");
            /* Loading the spawn may have unloaded the death site, saving the
             * player there with an emptied inventory; a crash before the next
             * autosave would bring them back alive at that spot. */
            world_save_all(&g->w);
        }
        g_in.respawn = 0;
    }
    g->hs.msg_age += (float)dt;
}

/* The other players as figures, among the opaque entities (which come
 * before the translucent ones). */
static void add_players(game *g, dvec3 eye, double alpha, double now, float light, int *n_op, int n_tr)
{
    mp_avatar av[NET_MAX_PLAYERS];
    int count = mp_avatars(g->mp, now, alpha, av, NET_MAX_PLAYERS);
    for (int i = 0; i < count; i++) {
        int room = RENDER_MAX_ENTS - *n_op - n_tr;
        if (room < VIEWMODEL_FIGURE) return;
        memmove(g->ents + *n_op + VIEWMODEL_FIGURE, g->ents + *n_op, sizeof g->ents[0] * (size_t)n_tr);
        const float feet[3] = {(float)(av[i].pos.x - eye.x), (float)(av[i].pos.y - eye.y),
                               (float)(av[i].pos.z - eye.z)};
        *n_op += viewmodel_figure(feet, av[i].yaw, av[i].pitch, av[i].sneaking, av[i].dead, light, g->ents + *n_op,
                                  VIEWMODEL_FIGURE);
    }
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
    int on_title = g->backdrop;
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
    /* Brightness lifts the darkest light toward dusk; days are unchanged. */
    v.daylight = fmaxf(thermo_daylight(g->th), 0.006f * (float)g->st.brightness);
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
    if (g->mp) add_players(g, v.eye, alpha, now, v.daylight, &n_op, n_tr);
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
    debug_info di = {0};
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
    di.indirect_draws = rs.indirect_draws;
    di.sections = rs.sections_drawn;
    di.quads = rs.quads;
    di.render_cpu_ms = rs.cpu_ms;
    di.gpu_ms = rs.gpu_ms;
    di.vram_used_mb = rs.vram_used_mb;
    di.vram_budget_mb = rs.vram_budget_mb;
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

/* ---------------------------------------------------------- worlds menu */

/* Fills the world screen's rows from the worlds folder. */
static void refresh_worlds(game *g)
{
    g->world_count = worlds_list(g->worlds_root, g->world_list, WORLDS_MAX);
    for (int i = 0; i < g->world_count; i++) {
        const world_entry *e = &g->world_list[i];
        menu_world *r = &g->world_rows[i];
        snprintf(r->name, sizeof r->name, "%.32s", e->info.name[0] ? e->info.name : e->folder);
        r->playable = 1;
        char when[32] = "never played";
        if (e->info.last_played > 0) {
            time_t t = (time_t)e->info.last_played;
            struct tm tmv = {0};
            if (localtime_r(&t, &tmv)) strftime(when, sizeof when, "played %Y-%m-%d %H:%M", &tmv);
        }
        switch (e->info.status) {
        case SAVE_PEEK_OK:
            if (e->info.generator > WORLDGEN_VERSION) {
                snprintf(r->detail, sizeof r->detail, "made by a newer version of the game");
                r->playable = 0;
            } else {
                snprintf(r->detail, sizeof r->detail, "seed %u   %s", e->info.seed, when);
            }
            break;
        case SAVE_PEEK_LEGACY: snprintf(r->detail, sizeof r->detail, "older format: converted when played"); break;
        case SAVE_PEEK_IN_USE:
            snprintf(r->detail, sizeof r->detail, "open in another game");
            r->playable = 0;
            break;
        default:
            snprintf(r->detail, sizeof r->detail, "cannot be read (damaged, or not a world)");
            r->playable = 0;
            break;
        }
    }
    g->mn.worlds = g->world_rows;
    g->mn.world_count = g->world_count;
}

/* Leaves whatever world is loaded and plays the one in dir. */
static void play_world(game *g, const char *dir)
{
    char path[WORLDS_PATH_MAX] = {0};
    snprintf(path, sizeof path, "%s", dir); /* dir may point into the list close_world outlives */
    close_world(g);
    open_world(g, path, 0);
    g->note[0] = '\0';
    g_in.inv_open = 0;
    g_in.panel = 0;
    g_in.screen = SCREEN_NONE;
    set_captured(g->win, 1);
}

/* The world screens' requests. Problems go to the menu's note line. */
static void world_action(game *g, menu_action a)
{
    menu *m = &g->mn;
    const world_entry *sel = m->world_sel >= 0 && m->world_sel < g->world_count ? &g->world_list[m->world_sel] : NULL;
    switch (a) {
    case MENU_WORLDS: refresh_worlds(g); break;
    case MENU_WORLD_PLAY:
        if (sel) play_world(g, sel->dir);
        break;
    case MENU_WORLD_CREATE: {
        const char *name = m->text_name[0] ? m->text_name : "New world";
        if (!save_name_valid(name, strlen(name))) {
            snprintf(m->note, sizeof m->note, "Names are 1-32 letters, digits or signs, no edge spaces.");
            break;
        }
        uint32_t seed = worlds_seed_from_text(m->text_seed, (uint32_t)time(NULL) * 2654435761u);
        char dir[WORLDS_PATH_MAX] = {0};
        if (worlds_create(g->worlds_root, name, seed, dir, sizeof dir) != 0) {
            snprintf(m->note, sizeof m->note, "Could not create the world (see the log).");
            break;
        }
        play_world(g, dir);
        break;
    }
    case MENU_WORLD_RENAME:
        if (!sel) break;
        if (!save_name_valid(m->text_name, strlen(m->text_name))) {
            snprintf(m->note, sizeof m->note, "Names are 1-32 letters, digits or signs, no edge spaces.");
            break;
        }
        if (worlds_rename(sel->dir, m->text_name) != 0) {
            snprintf(m->note, sizeof m->note, "Could not rename it (open in another game?).");
            break;
        }
        menu_open(m, SCREEN_WORLDS);
        refresh_worlds(g);
        break;
    case MENU_WORLD_DELETE:
        if (!sel) break;
        if (worlds_delete(sel->dir) < 0) {
            snprintf(m->note, sizeof m->note, "Could not delete it (open in another game?).");
            break;
        }
        menu_open(m, SCREEN_WORLDS);
        refresh_worlds(g);
        break;
    default: break;
    }
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
                     .letter_nav = g_in.ui_letter,
                     .text_len = g_in.text_len,
                     .backspace = g_in.ui_backspace,
                     .dt = (float)dt};
    memcpy(mi.text, g_in.text, sizeof mi.text);
    mi.mx = mx;
    mi.my = my;
    int mins = (int)(g->hl.t / 60.0);
    snprintf(g->mn.status, sizeof g->mn.status, "Alive %d:%02d   core %.1f" UI_CH_DEGREE "C   %s", mins / 60, mins % 60,
             (double)g->hl.temp, g->hl.dead ? "dead" : "");
    menu_action act = menu_frame(&g->mn, u, &mi, &g->st);
    switch (act) {
    case MENU_RESUME:
        g_in.screen = SCREEN_NONE;
        set_captured(g->win, 1);
        break;
    case MENU_TO_TITLE:
        /* Saves and closes the world; the menus get their backdrop back. */
        close_world(g);
        open_world(g, NULL, 1);
        g_in.inv_open = 0;
        g_in.panel = 0;
        menu_open(&g->mn, SCREEN_TITLE); /* the menu's own screen too, or the copy below undoes it */
        g_in.screen = SCREEN_TITLE;
        break;
    case MENU_WORLDS:
    case MENU_WORLD_PLAY:
    case MENU_WORLD_CREATE:
    case MENU_WORLD_RENAME:
    case MENU_WORLD_DELETE: world_action(g, act); break;
    case MENU_QUIT: glfwSetWindowShouldClose(g->win, GLFW_TRUE); break;
    case MENU_SETTINGS:
        if (g->o.no_vsync && g->st.vsync) g->o.no_vsync = 0; /* turned back on in the menu: a real choice */
        renderer_set_vsync(g->rd, g->st.vsync);
        renderer_set_filtering(g->rd, g->st.filtering);
        set_fullscreen(g, g->st.fullscreen);
        apply_volumes(g);
        apply_discord(g);
        update_discord(g);
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
    double wx = 0.0, wy = 0.0;
    int ww = 0, wh = 0;
    glfwGetCursorPos(g->win, &wx, &wy);
    glfwGetWindowSize(g->win, &ww, &wh);
    *mx = *my = -1.0f;
    if (ww > 0 && wh > 0) {
        *mx = (float)(wx * fb_w / ww) / u->scale;
        *my = (float)(wy * fb_h / wh) / u->scale;
    }
}

/* Chat: the last lines above the hotbar, fading after ten seconds unless
 * the chat is open, and the line being typed. */
static void draw_chat(const game *g, ui *u, int fb_h)
{
    const mp_chat_line *lines = NULL;
    int n = mp_chat(g->mp, &lines);
    double now = glfwGetTime();
    float y = (float)fb_h / u->scale - 64.0f;
    if (g_in.chat_open) {
        char line[NET_CHAT_MAX + 8] = {0};
        snprintf(line, sizeof line, "> %s_", g_in.chat);
        ui_rect(u, 4, y - 2, 330, 12, ui_rgba(0, 0, 0, 160));
        ui_text(u, 8, y, 1, ui_rgba(255, 255, 255, 255), line);
    }
    for (int i = n - 1; i >= 0; i--) {
        y -= 11.0f;
        double age = now - lines[i].at;
        float a = g_in.chat_open ? 1.0f : (float)fmin(1.0, fmax(0.0, (12.0 - age) / 2.0));
        if (a <= 0.0f) continue;
        char line[NET_NAME_MAX + NET_CHAT_MAX + 8] = {0};
        if (lines[i].who[0]) snprintf(line, sizeof line, "<%s> %s", lines[i].who, lines[i].text);
        else snprintf(line, sizeof line, "* %s", lines[i].text);
        ui_rect(u, 4, y - 2, 330, 11, ui_rgba(0, 0, 0, (int)(110.0f * a)));
        ui_text(u, 8, y, 1, ui_rgba(255, 255, lines[i].who[0] ? 255 : 160, (int)(255.0f * a)), line);
    }
}

/* The 2D overlay: HUD, hotbar, inventory, F3 and menus. Returns the quads. */
static int draw_overlay(game *g, double dt, double alpha)
{
    int max_quads = 0, fb_w = 0, fb_h = 0;
    ui_vertex *uv = renderer_ui_buffer(g->rd, &max_quads, &fb_w, &fb_h);
    ui u = {0};
    ui_begin(&u, uv, max_quads, fb_w, fb_h);
    ui_set_scale(&u, g->st.gui_scale, fb_w, fb_h);
    int on_title = g->backdrop;
    hud_state *hs = &g->hs;
    hs->panel = g_in.panel;
    hs->sel = g_in.sel_part;
    hs->held = NULL;
    hs->flying = g->pl.flying;
    hs->debug = g_in.debug;
    hs->inv = &g->inv;
    hs->hide_hints = !g->st.show_hints;
    hs->chat = g->mp != NULL;
    hud_update(hs, &g->hl, g->in_menu ? 0.0f : (float)dt);
    if (!on_title) hud_draw(&u, &g->hl, hs);
    if (!on_title && !g->hl.dead && !g_in.panel && !g_in.inv_open) invui_hotbar(&u, &g->iu, &g->inv, (float)dt);
    float mx = 0.0f, my = 0.0f;
    cursor_ui(g, &u, fb_w, fb_h, &mx, &my);
    if (g_in.inv_open && !g->in_menu && !g->hl.dead) {
        invui_input in = {mx,
                          my,
                          g_in.ui_click,
                          g_in.ui_rclick,
                          g_in.keys[GLFW_KEY_LEFT_SHIFT] || g_in.keys[GLFW_KEY_RIGHT_SHIFT],
                          thermo_stations(g->th, &g->w, g->pl.pos),
                          (float)dt};
        invui_result ir = invui_screen(&u, &g->iu, &g->inv, &in);
        if (g->client) {
            /* Shown at once; the host repeats each and has the last word. */
            if (ir.click_slot >= 0)
                mp_client_op(g->mp, (mp_op){MP_OP_CLICK, (uint8_t)ir.click_slot, (uint8_t)ir.click_button,
                                            (uint8_t)ir.click_shift});
            if (ir.crafted >= 0)
                mp_client_op(g->mp, (mp_op){MP_OP_CRAFT, (uint8_t)ir.crafted, (uint8_t)ir.craft_all, 0});
            if (ir.drop.count) mp_client_op(g->mp, (mp_op){MP_OP_DROP_CURSOR, 0, 0, 0});
        } else {
            survival_toss(&g->ph, &g->inv, &g->pl, ir.drop);
        }
        if (ir.crafted >= 0) sound_play(g->snd, SND_CRAFT, NULL, 0, 0.8f);
        else if (ir.changed) sound_play(g->snd, SND_UI_CLICK, NULL, 0, 0.5f);
    }
    if (g->mp && !on_title) draw_chat(g, &u, fb_h);
    if (on_title && g->note[0] && g->in_menu)
        ui_text_shadow(&u, 32, (float)fb_h / u.scale - 24.0f, 1, ui_rgba(255, 140, 120, 255), g->note);
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
        char title[256] = {0};
        snprintf(title, sizeof title,
                 "blockclonia | %.0f fps | %.1f %.1f %.1f | %s%s | draws %d | bodies %d | mesh %u/%u MB", fps,
                 g->pl.pos.x, g->pl.pos.y, g->pl.pos.z, held->count ? item_get(held->id)->name : "empty hand",
                 g->pl.flying ? " | fly" : "", rs.draw_calls, g->ph.body_count, rs.pool_used_kb / 1024,
                 rs.pool_total_kb / 1024);
        glfwSetWindowTitle(g->win, title);
        update_discord(g);
    }
    if (now - g->save_t >= AUTOSAVE_SECONDS) {
        g->save_t = now;
        world_save_all(&g->w); /* the player and the columns edited since the last save */
    }
}

static void game_shutdown(game *g)
{
    if (g->settings_dirty && save_settings(g) != 0)
        log_warn("could not save settings to %s", g->o.config);
    log_info("saving and shutting down");
    discord_destroy(g->dc); /* closing the connection clears the status */
    g->dc = NULL;
    close_world(g);
    audio_device_close(g->audio); /* stops the callback before the mixer goes */
    sound_destroy(g->snd);
    mem_free(g->ents);
    mem_free(g->fx);
    mem_free(g->world_list);
    mem_free(g->world_rows);
    jobs_destroy(g->js);
    renderer_destroy(g->rd);
    glfwDestroyWindow(g->win);
    glfwTerminate();
}

int main(int argc, char **argv)
{
    game *g = &g_game;
    if (!parse_args(argc, argv, &g->o)) return 2;
    sys_detect(&g->sys); /* before the allocator, which it sizes */
    sys_plan(&g->sys, &g->plan);
    mem_init(g->plan.arena_mb);
    sys_log(&g->sys, &g->plan);
    save_db_set_cache_kb(g->plan.sqlite_cache_kb);
    mesher_init();
    use_xdg_paths(&g->o);

    settings_default(&g->st);
    int got = settings_load(&g->st, g->o.config);
    if (got >= 0) log_info("settings from %s", g->o.config);
    /* A first run: pick the view distance for this machine. */
    if (got == SETTINGS_MISSING) g->st.render_distance = g->plan.render_distance;
    g->cfg_keep = got == SETTINGS_UNUSABLE;
    if (g->cfg_keep) log_warn("cannot use %s; playing with default settings and not saving over it", g->o.config);
    if (!g->o.have_radius) g->o.radius = g->st.render_distance;
    g->cfg_vsync = g->st.vsync; /* the file's value, kept while --no-vsync overrides it */
    if (!g->o.no_vsync) g->o.vsync = g->st.vsync;
    else g->st.vsync = 0;

    if (g->o.bench) {
        int r = bench_run(g->o.have_seed ? g->o.seed : 1337u);
        if (g->o.mem_stats) mem_print_stats();
        return r;
    }

    g->session_start = (int64_t)time(NULL);
    game_init(g);
    set_fullscreen(g, g->st.fullscreen);
    g->mn.discord_ok = MC_DISCORD_APP_ID[0] != '\0';
    apply_discord(g);
    update_discord(g);
    double prev = glfwGetTime();
    g->title_t = g->save_t = prev;
    while (!glfwWindowShouldClose(g->win)) {
        glfwPollEvents();
        double now = glfwGetTime();
        double dt = now - prev;
        prev = now;
        debug_frames_push(&g->dframes, (float)(dt * 1000.0));
        if (dt > 0.25) dt = 0.25;
        if (g_in.toggle_fullscreen) {
            g->st.fullscreen = !g->fullscreen;
            set_fullscreen(g, g->st.fullscreen);
            g->settings_dirty = 1;
            g_in.toggle_fullscreen = 0;
        }
        if (g_in.focus_changed) {
            apply_volumes(g);
            g_in.focus_changed = 0;
        }
        if (g_in.resized) {
            renderer_on_resize(g->rd);
            g_in.resized = 0;
        }
        if (g->mp) {
            mp_poll(g->mp, now);
            const char *why = NULL;
            if (mp_ended(g->mp, &why)) {
                /* The host left or dropped us: back to the menus, saying why. */
                snprintf(g->note, sizeof g->note, "Session ended: %s", why);
                close_world(g);
                open_world(g, NULL, 1);
                g_in.inv_open = g_in.panel = 0;
                menu_open(&g->mn, SCREEN_TITLE);
                g_in.screen = SCREEN_TITLE;
            }
        }
        if (g_in.chat_send) {
            if (g->mp && g_in.chat_len) mp_say(g->mp, g_in.chat);
            g_in.chat_open = g_in.chat_send = g_in.chat_len = 0;
            g_in.chat[0] = '\0';
        }
        frame_menus(g, dt);
        player_input in = frame_input(g);
        frame_simulate(g, &in, dt);
        frame_interact(g, dt);
        if (g->o.demo) demo_step(&g->w, &g->ph, g->fx, &g->pl, &g->demo, g->frame);
        world_update(&g->w, g->pl.pos.x, g->pl.pos.y + PLAYER_EYE, g->pl.pos.z);
        frame_render(g, now, dt);
        if (g->mp) mp_flush(g->mp, now);
        g_in.ui_click = g_in.ui_rclick = g_in.ui_up = g_in.ui_dn = g_in.ui_left = g_in.ui_right = g_in.ui_enter =
            g_in.ui_back = g_in.ui_letter = g_in.ui_backspace = g_in.text_len = 0;
        frame_bookkeeping(g, now);
        discord_poll(g->dc, now);
        if (g->o.frames && g->frame >= g->o.frames) break;
        /* Frame limit with VSync off: sleep out the rest of the frame. The
         * physics accumulator reads the clock, so it is unaffected. */
        if (!g->st.vsync && g->st.max_fps > 0) {
            int fps = g->st.max_fps < SETTINGS_FPS_MIN ? SETTINGS_FPS_MIN : g->st.max_fps;
            os_sleep(now + 1.0 / fps - glfwGetTime());
        }
    }
    game_shutdown(g);
    if (g->o.mem_stats) mem_print_stats();
    return 0;
}
