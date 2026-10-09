#include "test_util.h"
#include "mem.h"
#include "os.h"
#include "survival.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

int g_failed, g_checks;

#ifdef _WIN32
/* Tests are run with ASCII temp paths, so the narrow API is enough here. */
int tmp_dir_make(char *out, size_t cap, const char *prefix)
{
    const char *tmp = getenv("TEMP");
    if (!tmp || !tmp[0] || strlen(tmp) >= 200) tmp = getenv("TMP");
    if (!tmp || !tmp[0] || strlen(tmp) >= 200) return 0;
    /* The process id and tick count make names unique enough; CreateDirectory
     * failing on an existing name makes a clash safe, never shared. */
    for (unsigned i = 0; i < 100; i++) {
        int n = snprintf(out, cap, "%s\\%s%06lu", tmp, prefix,
                         (unsigned long)((GetCurrentProcessId() * 7919u + GetTickCount() + i) % 1000000u));
        if (n > 0 && (size_t)n < cap && CreateDirectoryA(out, NULL)) return 1;
    }
    return 0;
}

int tmp_symlink(const char *target, const char *link)
{
    DWORD flags = 0x2; /* SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE (Developer Mode) */
    if (os_is_dir(target)) flags |= SYMBOLIC_LINK_FLAG_DIRECTORY;
    /* Wine reports success without making a link; checking for one keeps
     * the tests from passing on a link that is not there. */
    if (CreateSymbolicLinkA(link, target, flags)) return os_path_kind(link) == OS_OTHER ? 1 : -1;
    DWORD err = GetLastError();
    return err == ERROR_PRIVILEGE_NOT_HELD || err == ERROR_INVALID_PARAMETER || err == ERROR_NOT_SUPPORTED ? -1 : 0;
}

void tmp_remove(const char *path)
{
    /* A directory symlink is removed as a directory, a file one as a file;
     * neither call touches the target. */
    if (!DeleteFileA(path)) (void)RemoveDirectoryA(path);
}

void tmp_setenv(const char *name, const char *value) { (void)_putenv_s(name, value ? value : ""); }
#else
int tmp_dir_make(char *out, size_t cap, const char *prefix)
{
    const char *tmp = getenv("TMPDIR");
    int n = snprintf(out, cap, "%s/%sXXXXXX", tmp && tmp[0] && strlen(tmp) < 200 ? tmp : "/tmp", prefix);
    return n > 0 && (size_t)n < cap && mkdtemp(out) != NULL;
}

int tmp_symlink(const char *target, const char *link) { return symlink(target, link) == 0; }

void tmp_remove(const char *path)
{
    if (unlink(path) != 0) (void)rmdir(path);
}

void tmp_setenv(const char *name, const char *value)
{
    if (value) setenv(name, value, 1);
    else unsetenv(name);
}
#endif

void test_flat_gen(uint32_t seed, column *c)
{
    (void)seed;
    memset(c->blocks, B_AIR, COL_VOL);
    for (int z = 0; z < CHUNK_W; z++)
        for (int x = 0; x < CHUNK_W; x++)
            for (int y = 0; y < GROUND; y++)
                c->blocks[col_index(x, y, z)] = y == 0 ? B_BEDROCK : (y < 10 ? B_STONE : (y < 12 ? B_DIRT : B_GRASS));
}

void tw_init(test_world *t)
{
    t->js = jobs_create(0);
    world_init(&t->w, 1, 3, t->js, NULL);
    t->w.generator = test_flat_gen;
    world_load_blocking(&t->w, 0.5, 0.5, 3);
    physics_init(&t->ph, &t->w);
    t->w.edit_user = &t->ph;
    t->w.on_block_changed = physics_on_block_changed;
}

void tw_free(test_world *t)
{
    physics_destroy(&t->ph);
    world_destroy(&t->w);
    jobs_destroy(t->js);
}

void run_steps(test_world *t, player *p, const player_input *in, int n)
{
    for (int i = 0; i < n; i++) physics_step(&t->ph, p, in);
}

health *new_body(uint32_t seed)
{
    health *h = mem_alloc(sizeof *h); /* 30 KB: too big for a test's stack */
    health_init(h, seed);
    return h;
}

health_env calm_env(void)
{
    health_env e = {0};
    e.on_ground = 1;
    e.airway = AIRWAY_AIR;
    e.water_temp = SURVIVAL_WATER_TEMP;
    e.air_temp = 20.0;
    e.contact_temp = 20.0;
    return e;
}

/* Steps the body at 60 Hz, like the game. */
void live(health *h, const health_env *e, double seconds)
{
    for (int i = 0, n = (int)(seconds * 60.0); i < n; i++) health_step(h, e, 1.0 / 60.0);
}
