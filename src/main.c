/* blockclonia: entry point, input, and the fixed-timestep game loop. */
#include "bench.h"
#include "jobs.h"
#include "log.h"
#include "mem.h"
#include "mesher.h"
#include "physics.h"
#include "renderer.h"
#include "save.h"
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

typedef struct {
    uint32_t seed;
    int have_seed;
    int radius, width, height, vsync, validate, gpu, threads;
    uint32_t pool_mb;
    const char *world_dir;
    int frames;
    const char *screenshot;
    int bench, demo, mem_stats;
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
} input_state;

static input_state g_in;

static const uint8_t HOTBAR[] = {B_STONE, B_DIRT, B_GRASS, B_PLANKS, B_LOG, B_GLASS, B_SAND, B_BRICK, B_WATER};
#define HOTBAR_N ((int)(sizeof HOTBAR / sizeof HOTBAR[0]))

/* ---------------------------------------------------------- arguments */

static int parse_int(const char *s, long lo, long hi, long *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || end == s || *end || v < lo || v > hi) return 0;
    *out = v;
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
        long n;
#define NEED_VALUE() do { if (!v) { fprintf(stderr, "%s needs a value\n", a); return 0; } i++; } while (0)
        if (!strcmp(a, "--seed")) {
            NEED_VALUE();
            if (!parse_int(v, 0, (long)UINT32_MAX, &n)) goto bad;
            o->seed = (uint32_t)n;
            o->have_seed = 1;
        } else if (!strcmp(a, "--radius")) {
            NEED_VALUE();
            if (!parse_int(v, 2, 32, &n)) goto bad;
            o->radius = (int)n;
        } else if (!strcmp(a, "--size")) {
            NEED_VALUE();
            int w, h;
            char tail;
            if (sscanf(v, "%dx%d%c", &w, &h, &tail) != 2 || w < 64 || h < 64 || w > 16384 || h > 16384) goto bad;
            o->width = w;
            o->height = h;
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
            char tail;
            if (sscanf(v, "%f,%f%c", &y, &p, &tail) != 2 || !isfinite(y) || !isfinite(p)) goto bad;
            o->look_yaw = y;
            o->look_pitch = p;
            o->have_look = 1;
        } else if (!strcmp(a, "--spawn")) {
            NEED_VALUE();
            int x, z;
            char tail;
            if (sscanf(v, "%d,%d%c", &x, &z, &tail) != 2 || abs(x) > WORLD_LIMIT - 64 || abs(z) > WORLD_LIMIT - 64)
                goto bad;
            o->spawn_x = x;
            o->spawn_z = z;
            o->have_spawn = 1;
        } else if (!strcmp(a, "--demo")) {
            o->demo = 1;
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
    } else if (key == GLFW_KEY_F) {
        g_in.toggle_fly = 1;
    } else if (key == GLFW_KEY_F2) {
        g_in.want_shot = 1;
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

    demo_state demo = {0};
    double prev = glfwGetTime(), acc = 0.0, title_t = prev;
    int frame = 0, fps_frames = 0;
    double fps = 0.0;
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

        int steps = 0;
        while (acc >= PHYS_DT && steps < MAX_STEPS_PER_FRAME) {
            physics_step(&ph, &pl, &in);
            acc -= PHYS_DT;
            steps++;
        }
        if (steps == MAX_STEPS_PER_FRAME) acc = 0.0; /* too slow to keep up: slow down time instead */

        /* Block interaction. */
        dvec3 eye = dv3(pl.pos.x, pl.pos.y + PLAYER_EYE, pl.pos.z);
        ray_hit hit = physics_raycast(&w, eye, look_dir(pl.yaw, pl.pitch), REACH);
        if (g_in.click_break && hit.hit && hit.id != B_BEDROCK)
            world_set_player(&w, hit.block.x, hit.block.y, hit.block.z, B_AIR, 0);
        if (g_in.click_place && hit.hit) {
            ipos t = hit.before;
            int blocked = box_hits_cell(player_box(&pl), t.x, t.y, t.z);
            for (int i = 0; i < ph.body_count && !blocked; i++) {
                aabb bb = {ph.bodies[i].pos, dv3_add(ph.bodies[i].pos, dv3(1, 1, 1))};
                blocked = box_hits_cell(bb, t.x, t.y, t.z);
            }
            uint8_t cur = world_get(&w, t.x, t.y, t.z);
            if (!blocked && (cur == B_AIR || cur == B_WATER))
                world_set_player(&w, t.x, t.y, t.z, HOTBAR[g_in.slot], 0);
        }
        if (g_in.click_pick && hit.hit) {
            for (int i = 0; i < HOTBAR_N; i++)
                if (HOTBAR[i] == hit.id) g_in.slot = i;
        }
        g_in.click_break = g_in.click_place = g_in.click_pick = 0;

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
            renderer_end_frame(rd, &w, &ph, &v, alpha);
        } else {
            jobs_poll(js, MESH_UPLOADS_PER_FRAME);
            world_schedule(&w);
            glfwWaitEventsTimeout(0.05);
        }

        frame++;
        fps_frames++;
        if (now - title_t >= 0.5) {
            fps = fps_frames / (now - title_t);
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
        if (o.frames && frame >= o.frames) break;
    }

    log_info("saving and shutting down");
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
