/* blockclonia: entry point, input, and the fixed-timestep game loop. */
#include "bench.h"
#include "health.h"
#include "hud.h"
#include "jobs.h"
#include "log.h"
#include "mem.h"
#include "mesher.h"
#include "physics.h"
#include "renderer.h"
#include "save.h"
#include "survival.h"
#include "ui.h"
#include "world.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

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
} options;

typedef struct {
    int keys[GLFW_KEY_LAST + 1];
    double mouse_dx, mouse_dy, last_x, last_y;
    int have_mouse, captured;
    int click_break, click_place, click_pick;
    int toggle_fly, want_shot, resized;
    int slot;
    double scroll;
    int panel;          /* health panel open */
    int sel_part;       /* body part selected in the panel */
    int treat;          /* treatment requested, -1 none */
    int respawn;
} input_state;

static input_state g_in;

static const uint8_t HOTBAR[] = {B_STONE, B_DIRT, B_GRASS, B_PLANKS, B_LOG, B_GLASS, B_SAND, B_BRICK, B_WATER};
#define HOTBAR_N ((int)(sizeof HOTBAR / sizeof HOTBAR[0]))

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
           "  --demo            scripted structural-collapse demo\n"
           "  --health-panel    start with the health panel (H) open\n"
           "  --hurt LIST       start injured: comma list of bleed, artery, fracture,\n"
           "                    open-fracture, infection, concussion\n"
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
        } else if (!strcmp(a, "--size")) {
            NEED_VALUE();
            long long w, h;
            if (!parse_int_pair(v, 'x', 64, 16384, &w, &h)) goto bad;
            o->width = (int)w;
            o->height = (int)h;
        } else if (!strcmp(a, "--no-vsync")) {
            o->vsync = 0;
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

static void key_cb(GLFWwindow *win, int key, int sc, int action, int mods)
{
    (void)sc; (void)mods;
    if (key < 0 || key > GLFW_KEY_LAST) return;
    if (action == GLFW_PRESS) g_in.keys[key] = 1;
    else if (action == GLFW_RELEASE) g_in.keys[key] = 0;
    if (action != GLFW_PRESS) return;
    if (key == GLFW_KEY_ESCAPE) {
        if (g_in.captured) {
            glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            g_in.captured = 0;
        } else {
            glfwSetWindowShouldClose(win, GLFW_TRUE);
        }
    } else if (key == GLFW_KEY_H) {
        g_in.panel = !g_in.panel;
    } else if (key == GLFW_KEY_F2) {
        g_in.want_shot = 1;
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
    } else if (key == GLFW_KEY_F) {
        g_in.toggle_fly = 1;
    } else if (key >= GLFW_KEY_1 && key < GLFW_KEY_1 + HOTBAR_N) {
        g_in.slot = key - GLFW_KEY_1;
    }
}

static void mouse_button_cb(GLFWwindow *win, int button, int action, int mods)
{
    (void)mods;
    if (action != GLFW_PRESS) return;
    if (!g_in.captured) {
        glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        g_in.captured = 1;
        g_in.have_mouse = 0;
        return;
    }
    if (button == GLFW_MOUSE_BUTTON_LEFT) g_in.click_break = 1;
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

/* --------------------------------------------------------------- game */

static int box_hits_cell(aabb b, int x, int y, int z)
{
    return b.min.x < x + 1 && b.max.x > x && b.min.y < y + 1 && b.max.y > y && b.min.z < z + 1 && b.max.z > z;
}

typedef struct { world *w; physics *ph; } save_ctx;

/* A fatal error (device lost, surface lost) must not take the player's
 * edits with it: the world lives in CPU memory and is still intact. */
static void save_on_fatal(void *user)
{
    save_ctx *sc = user;
    physics_settle_bodies(sc->ph, NULL);
    world_save_all(sc->w);
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

/* --hurt: start with a chosen set of injuries, for screenshots and for
 * trying treatments without having to get hurt first. */
static void apply_hurt(health *h, const char *list)
{
    char buf[256];
    snprintf(buf, sizeof buf, "%s", list);
    for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
        if (!strcmp(t, "bleed")) health_cut(h, BP_LLEG, 0.6f, 0, 0.3f);
        else if (!strcmp(t, "artery")) health_cut(h, BP_LLEG, 0.7f, 1, 0.2f);
        else if (!strcmp(t, "fracture")) health_break_bone(h, BP_RARM, 0);
        else if (!strcmp(t, "open-fracture")) health_break_bone(h, BP_RLEG, 1);
        else if (!strcmp(t, "concussion")) health_blunt(h, BP_HEAD, 12.0);
        else if (!strcmp(t, "infection")) {
            health_cut(h, BP_LARM, 0.5f, 0, 0.9f);
            h->wounds[h->wound_count - 1].infection = 0.45f;
        } else log_warn("--hurt: unknown injury '%s'", t);
    }
}

static void demo_step(world *w, const player *p, demo_state *d, int frame)
{
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
    w.edit_user = &ph;
    w.on_block_changed = physics_on_block_changed;
    w.on_column_unload = physics_on_column_unload;
    save_ctx sc = {&w, &ph};
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
    log_info("spawn at %.1f %.1f %.1f with %d worker threads", pl.pos.x, pl.pos.y, pl.pos.z,
             jobs_worker_count(js));

    health hl;
    health_init(&hl, seed ^ 0x9E3779B9u);
    if (o.hurt) apply_hurt(&hl, o.hurt);
    hud_state hs;
    memset(&hs, 0, sizeof hs);
    g_in.treat = -1;
    g_in.panel = o.open_panel;
    int actions = 0; /* blocks broken or placed since the last physics step */

    demo_state demo = {0};
    double prev = glfwGetTime(), acc = 0.0, title_t = prev, save_t = prev;
    int frame = 0, fps_frames = 0;
    char title[256];

    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();
        double now = glfwGetTime();
        double dt = now - prev;
        prev = now;
        if (dt > 0.25) dt = 0.25;
        acc += dt;

        if (g_in.resized) {
            renderer_on_resize(rd);
            g_in.resized = 0;
        }

        /* The panel, death and unconsciousness freeze the player's input. */
        int frozen = g_in.panel || hl.dead || hl.conscious == CONS_UNCONSCIOUS;
        if (frozen) g_in.mouse_dx = g_in.mouse_dy = 0;

        /* Look. */
        const float sens = 0.0022f;
        pl.yaw += (float)g_in.mouse_dx * sens;
        pl.pitch -= (float)g_in.mouse_dy * sens;
        pl.pitch = clampf(pl.pitch, -1.55f, 1.55f);
        pl.yaw = fmodf(pl.yaw, (float)(2 * MC_PI));
        g_in.mouse_dx = g_in.mouse_dy = 0;
        if (g_in.scroll != 0) {
            int s = g_in.slot - (g_in.scroll > 0 ? 1 : -1);
            g_in.slot = (s % HOTBAR_N + HOTBAR_N) % HOTBAR_N;
            g_in.scroll = 0;
        }
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
        in.sprint = g_in.keys[GLFW_KEY_LEFT_CONTROL];
        if (frozen) memset(&in, 0, sizeof in);
        survival_limit_input(&hl, &pl, &in);

        int steps = 0;
        while (acc >= PHYS_DT && steps < MAX_STEPS_PER_FRAME) {
            survival_before sb = survival_capture(&pl);
            physics_step(&ph, &pl, &in);
            health_env env;
            survival_env(&w, &pl, &sb, actions, &env);
            actions = 0;
            survival_impacts(&hl, &w, &ph, &pl, &sb);
            health_step(&hl, &env, PHYS_DT);
            acc -= PHYS_DT;
            steps++;
        }
        if (steps == MAX_STEPS_PER_FRAME) acc = 0.0; /* too slow to keep up: slow down time instead */

        /* Block interaction. */
        dvec3 eye = dv3(pl.pos.x, pl.pos.y + PLAYER_EYE, pl.pos.z);
        ray_hit hit = physics_raycast(&w, eye, look_dir(pl.yaw, pl.pitch), REACH);
        health_limits lim = health_get_limits(&hl);
        int hands = !frozen && (pl.flying || (lim.has_control && lim.can_act));
        if (g_in.click_break && hands && hit.hit && hit.id != B_BEDROCK) {
            world_set_player(&w, hit.block.x, hit.block.y, hit.block.z, B_AIR, 0);
            if (pl.flying) health_scavenge(&hl, hit.id);
            else survival_on_break(&hl, hit.id);
            actions++;
        }
        if (g_in.click_place && hands && hit.hit) {
            ipos t = hit.before;
            int blocked = box_hits_cell(player_box(&pl), t.x, t.y, t.z);
            for (int i = 0; i < ph.body_count && !blocked; i++) {
                aabb bb = {ph.bodies[i].pos, dv3_add(ph.bodies[i].pos, dv3(1, 1, 1))};
                blocked = box_hits_cell(bb, t.x, t.y, t.z);
            }
            uint8_t cur = world_get(&w, t.x, t.y, t.z);
            if (!blocked && (cur == B_AIR || cur == B_WATER)) {
                world_set_player(&w, t.x, t.y, t.z, HOTBAR[g_in.slot], 0);
                actions++;
            }
        }
        if (g_in.click_pick && hit.hit) {
            for (int i = 0; i < HOTBAR_N; i++)
                if (HOTBAR[i] == hit.id) g_in.slot = i;
        }
        g_in.click_break = g_in.click_place = g_in.click_pick = 0;

        /* Treatments, eating and drinking. */
        if (g_in.treat >= 0) {
            if (!hl.dead) {
                int water = survival_water_nearby(&w, &pl, look_dir(pl.yaw, pl.pitch));
                health_treat(&hl, g_in.sel_part, g_in.treat, water, hs.msg, sizeof hs.msg);
                hs.msg_age = 0.0f;
            }
            g_in.treat = -1;
        }
        if (g_in.respawn) {
            if (hl.dead) {
                world_load_blocking(&w, sx, sz, 1);
                player_spawn(&pl, &w, sx, sz);
                health_init(&hl, seed ^ (uint32_t)frame * 2654435761u);
                g_in.panel = 0;
                log_info("respawned");
            }
            g_in.respawn = 0;
        }
        hs.msg_age += (float)dt;

        if (o.demo) demo_step(&w, &pl, &demo, frame);

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
                             .fov = 70.0f * (float)(MC_PI / 180.0), .has_selection = hit.hit,
                             .selection = hit.block};
            v.underwater = world_get(&w, (int)floor(v.eye.x), (int)floor(v.eye.y), (int)floor(v.eye.z)) == B_WATER;

            /* Overlay: the game writes its quads straight into the frame's
             * mapped buffer. */
            int max_quads, fb_w, fb_h;
            ui_vertex *uv = renderer_ui_buffer(rd, &max_quads, &fb_w, &fb_h);
            ui u;
            ui_begin(&u, uv, max_quads, fb_w, fb_h);
            hs.panel = g_in.panel;
            hs.sel = g_in.sel_part;
            hs.held = block_get(HOTBAR[g_in.slot])->name;
            hs.flying = pl.flying;
            hud_draw(&u, &hl, &hs);
            v.ui_quads = u.quads;
            v.hide_crosshair = frozen;
            renderer_end_frame(rd, &w, &ph, &v, alpha);
        } else {
            jobs_poll(js, MESH_UPLOADS_PER_FRAME);
            world_schedule(&w);
            glfwWaitEventsTimeout(0.05);
        }

        if (frame < INT_MAX) frame++; /* int overflow is UB; only --frames and the demo read it */
        fps_frames++;
        if (now - title_t >= 0.5) {
            double fps = fps_frames / (now - title_t);
            fps_frames = 0;
            title_t = now;
            render_stats rs = renderer_stats(rd);
            snprintf(title, sizeof title,
                     "blockclonia | %.0f fps | %.1f %.1f %.1f | %s%s | draws %d | bodies %d | mesh %u/%u MB",
                     fps, pl.pos.x, pl.pos.y, pl.pos.z, block_get(HOTBAR[g_in.slot])->name,
                     pl.flying ? " | fly" : "", rs.draw_calls, ph.body_count, rs.pool_used_kb / 1024,
                     rs.pool_total_kb / 1024);
            glfwSetWindowTitle(win, title);
        }
        if (now - save_t >= AUTOSAVE_SECONDS) {
            save_t = now;
            world_save_all(&w); /* only columns edited since the last save */
        }
        if (o.frames && frame >= o.frames) break;
    }

    log_info("saving and shutting down");
    log_set_fatal_hook(NULL, NULL);
    physics_settle_bodies(&ph, NULL);
    world_save_all(&w);
    jobs_wait_idle(js);
    world_destroy(&w);
    physics_destroy(&ph);
    jobs_destroy(js);
    renderer_destroy(rd);
    glfwDestroyWindow(win);
    glfwTerminate();
    if (o.mem_stats) mem_print_stats();
    return 0;
}
