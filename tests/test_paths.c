/* Where files go (paths.c) under every mix of XDG variables and $HOME, and
 * the one-off copy of files older builds kept in the working directory
 * (save_copy_file, save_copy_world). */
#include "os.h"
#include "paths.h"
#include "save.h"
#include "test_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif

#ifdef _WIN32
static const char *const VARS[] = {"USERPROFILE", "APPDATA", "LOCALAPPDATA"};
#else
static const char *const VARS[] = {"HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"};
#endif
#define NVARS (sizeof VARS / sizeof VARS[0])

/* The caller's environment, put back when the tests are done. */
static char g_saved[NVARS][1024];
static int g_had[NVARS];

static void env_save(void)
{
    for (size_t i = 0; i < NVARS; i++) {
        const char *v = getenv(VARS[i]);
        g_had[i] = v && strlen(v) < sizeof g_saved[i];
        if (g_had[i]) snprintf(g_saved[i], sizeof g_saved[i], "%s", v);
    }
}

static void env_restore(void)
{
    for (size_t i = 0; i < NVARS; i++) tmp_setenv(VARS[i], g_had[i] ? g_saved[i] : NULL);
}

static int app_dir_is(const char *var, const char *rel, const char *want)
{
    char out[PATHS_MAX];
    return paths_app_dir(var, rel, out, sizeof out) == 0 && strcmp(out, want) == 0;
}

#ifdef _WIN32
/* Sets the three variables; NULL unsets one. */
static void env_set(const char *profile, const char *roaming, const char *local)
{
    const char *v[NVARS] = {profile, roaming, local};
    for (size_t i = 0; i < NVARS; i++) tmp_setenv(VARS[i], v[i]);
}

static void test_app_dir(void)
{
    env_set("C:\\Users\\u", "D:\\roam\\", NULL);
    CHECK(app_dir_is("APPDATA", "AppData/Roaming", "D:\\roam/blockclonia"));
    CHECK(app_dir_is("LOCALAPPDATA", "AppData/Local", "C:\\Users\\u/AppData/Local/blockclonia")); /* unset */
    env_set("C:/Users/u/", "roam", "\\\\server\\share");
    CHECK(app_dir_is("APPDATA", "AppData/Roaming", "C:/Users/u/AppData/Roaming/blockclonia")); /* relative */
    CHECK(app_dir_is("LOCALAPPDATA", "AppData/Local", "\\\\server\\share/blockclonia"));       /* UNC */
    env_set("C:\\", "\\no-drive", "C:relative");
    CHECK(app_dir_is("APPDATA", "AppData/Roaming", "C:/AppData/Roaming/blockclonia"));
    CHECK(app_dir_is("LOCALAPPDATA", "AppData/Local", "C:/AppData/Local/blockclonia"));

    char out[PATHS_MAX];
    env_set(NULL, NULL, NULL);
    CHECK(paths_app_dir("APPDATA", "AppData/Roaming", out, sizeof out) == -1);
    env_set("/home/u", NULL, NULL); /* a POSIX path is not absolute on Windows */
    CHECK(paths_app_dir("APPDATA", "AppData/Roaming", out, sizeof out) == -1);
}

static void test_resolve(void)
{
    paths p;
    env_set("C:\\Users\\u", "C:\\Users\\u\\AppData\\Roaming", NULL);
    paths_resolve(&p);
    CHECK(strcmp(p.config, "C:\\Users\\u\\AppData\\Roaming/blockclonia/blockclonia.cfg") == 0 && p.xdg_config);
    CHECK(strcmp(p.world, "C:\\Users\\u\\AppData\\Roaming/blockclonia/worlds/world") == 0 && p.xdg_world);
    CHECK(strcmp(p.pipelines, "C:\\Users\\u/AppData/Local/blockclonia/blockclonia.pipelines") == 0 && p.xdg_pipelines);

    env_set(NULL, NULL, NULL);
    paths_resolve(&p);
    CHECK(strcmp(p.config, "blockclonia.cfg") == 0 && !p.xdg_config);
    CHECK(strcmp(p.world, "world") == 0 && !p.xdg_world);
}

static void test_parent(void)
{
    char out[64];
    CHECK(paths_parent("C:\\a\\b\\c.cfg", out, sizeof out) == 0 && strcmp(out, "C:\\a\\b") == 0);
    CHECK(paths_parent("C:\\a/b/c.cfg", out, sizeof out) == 0 && strcmp(out, "C:\\a/b") == 0);
    CHECK(paths_parent("b\\c", out, sizeof out) == 0 && strcmp(out, "b") == 0);
    CHECK(paths_parent("C:\\c.cfg", out, sizeof out) == -1);
    CHECK(paths_parent("\\\\server\\share\\c.cfg", out, sizeof out) == -1);
    CHECK(paths_parent("c.cfg", out, sizeof out) == -1);
}
#else
/* Sets the four variables; NULL unsets one. */
static void env_set(const char *home, const char *config, const char *data, const char *cache)
{
    const char *v[NVARS] = {home, config, data, cache};
    for (size_t i = 0; i < NVARS; i++) tmp_setenv(VARS[i], v[i]);
}

static void test_app_dir(void)
{
    env_set("/home/u", "/cfg", NULL, NULL);
    CHECK(app_dir_is("XDG_CONFIG_HOME", ".config", "/cfg/blockclonia"));
    CHECK(app_dir_is("XDG_DATA_HOME", ".local/share", "/home/u/.local/share/blockclonia")); /* unset */
    env_set("/home/u/", "/cfg//", "", "cache");
    CHECK(app_dir_is("XDG_CONFIG_HOME", ".config", "/cfg/blockclonia"));
    CHECK(app_dir_is("XDG_DATA_HOME", ".local/share", "/home/u/.local/share/blockclonia")); /* empty */
    CHECK(app_dir_is("XDG_CACHE_HOME", ".cache", "/home/u/.cache/blockclonia"));           /* relative */
    env_set("/", "/", NULL, NULL);
    CHECK(app_dir_is("XDG_CONFIG_HOME", ".config", "/blockclonia"));
    CHECK(app_dir_is("XDG_DATA_HOME", ".local/share", "/.local/share/blockclonia"));

    /* No absolute XDG value and no usable $HOME: nothing to build on. */
    char out[PATHS_MAX];
    env_set(NULL, NULL, NULL, NULL);
    CHECK(paths_app_dir("XDG_CONFIG_HOME", ".config", out, sizeof out) == -1);
    env_set("", NULL, NULL, NULL);
    CHECK(paths_app_dir("XDG_CONFIG_HOME", ".config", out, sizeof out) == -1);
    env_set("home", "rel", NULL, NULL);
    CHECK(paths_app_dir("XDG_CONFIG_HOME", ".config", out, sizeof out) == -1);

    env_set("/home/u", "/cfg", NULL, NULL);
    CHECK(paths_app_dir("XDG_CONFIG_HOME", ".config", out, 16) == -1); /* "/cfg/blockclonia" + NUL is 17 */
    CHECK(paths_app_dir("XDG_CONFIG_HOME", ".config", out, 17) == 0 && strcmp(out, "/cfg/blockclonia") == 0);
}

static void test_resolve(void)
{
    paths p;
    env_set("/home/u", NULL, "/data", "");
    paths_resolve(&p);
    CHECK(strcmp(p.config, "/home/u/.config/blockclonia/blockclonia.cfg") == 0 && p.xdg_config);
    CHECK(strcmp(p.world, "/data/blockclonia/worlds/world") == 0 && p.xdg_world);
    CHECK(strcmp(p.pipelines, "/home/u/.cache/blockclonia/blockclonia.pipelines") == 0 && p.xdg_pipelines);

    /* Without a usable $HOME the old names in the working directory stay. */
    env_set(NULL, "", "relative", NULL);
    paths_resolve(&p);
    CHECK(strcmp(p.config, "blockclonia.cfg") == 0 && !p.xdg_config);
    CHECK(strcmp(p.world, "world") == 0 && !p.xdg_world);
    CHECK(strcmp(p.pipelines, "blockclonia.pipelines") == 0 && !p.xdg_pipelines);

    /* A value too long for the buffers is not truncated into another path. */
    char big[PATHS_MAX + 16];
    memset(big, 'a', sizeof big - 1);
    big[0] = '/';
    big[sizeof big - 1] = '\0';
    env_set("/home/u", big, NULL, NULL);
    paths_resolve(&p);
    CHECK(strcmp(p.config, "blockclonia.cfg") == 0 && !p.xdg_config);
    CHECK(strcmp(p.world, "/home/u/.local/share/blockclonia/worlds/world") == 0);
}

static void test_parent(void)
{
    char out[64];
    CHECK(paths_parent("/a/b/c.cfg", out, sizeof out) == 0 && strcmp(out, "/a/b") == 0);
    CHECK(paths_parent("b/c", out, sizeof out) == 0 && strcmp(out, "b") == 0);
    CHECK(paths_parent("c.cfg", out, sizeof out) == -1);
    CHECK(paths_parent("/c.cfg", out, sizeof out) == -1);
    CHECK(paths_parent("/abcdef/c", out, 4) == -1);
}
#endif

/* Empties and removes a directory the tests made; its subdirectories go
 * first. Entries are unlinked, never followed. */
static void rm_dir(const char *root, const char *sub)
{
    char dir[400];
    snprintf(dir, sizeof dir, "%s%s", root, sub);
    os_dir *d = os_dir_open(dir);
    if (d) {
        const char *name;
        while ((name = os_dir_next(d)) != NULL) {
            if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
            char p[700];
            snprintf(p, sizeof p, "%s/%s", dir, name);
            tmp_remove(p);
        }
        os_dir_close(d);
    }
    tmp_remove(dir);
}

static int put_file(const char *dir, const char *name, const char *text)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    return save_write_file(p, text, strlen(text));
}

/* 1 if dir/name is a regular file holding text. */
static int has_file(const char *dir, const char *name, const char *text)
{
    char p[512], buf[64];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    long n = save_read_file(p, buf, sizeof buf - 1);
    if (n < 0) return 0;
    buf[n] = '\0';
    return strcmp(buf, text) == 0;
}

static int missing(const char *dir, const char *name)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    return os_path_kind(p) == OS_MISSING;
}

static void test_make_dirs(const char *root)
{
    char d[400], f[400];
    snprintf(d, sizeof d, "%s/a/b/c", root);
    CHECK(paths_make_dirs(d) == 0);
    CHECK(os_is_dir(d));
#ifndef _WIN32
    struct stat st;
    CHECK(stat(d, &st) == 0 && (st.st_mode & 0777) == 0700);
#endif
    CHECK(paths_make_dirs(d) == 0); /* already there */
    snprintf(d, sizeof d, "%s/a/b/c/", root);
    CHECK(paths_make_dirs(d) == 0);

    /* A file where a directory should be is an error, not skipped over. */
    snprintf(f, sizeof f, "%s/a", root);
    CHECK(put_file(f, "file", "x") == 0);
    snprintf(d, sizeof d, "%s/a/file/sub", root);
    CHECK(paths_make_dirs(d) == -1);
    CHECK(paths_make_dirs("") == -1);
}

static void test_copy(const char *root)
{
    char from[400], to[400], victim[400], link[450];
    snprintf(from, sizeof from, "%s/old", root);
    snprintf(to, sizeof to, "%s/new", root);
    CHECK(os_mkdir(from, 0700) == 0 && os_mkdir(to, 0700) == 0);

    /* save_copy_file: copies once, then never overwrites. */
    char a[450], b[450];
    snprintf(a, sizeof a, "%s/blockclonia.cfg", from);
    snprintf(b, sizeof b, "%s/blockclonia.cfg", to);
    CHECK(save_copy_file(a, b, 64) == 0); /* nothing to copy */
    CHECK(put_file(from, "blockclonia.cfg", "fov 90\n") == 0);
    CHECK(save_copy_file(a, b, 4) == 0); /* too big */
    CHECK(missing(to, "blockclonia.cfg"));
    CHECK(save_copy_file(a, b, 64) == 1 && has_file(to, "blockclonia.cfg", "fov 90\n"));
    CHECK(put_file(from, "blockclonia.cfg", "fov 70\n") == 0);
    CHECK(save_copy_file(a, b, 64) == 0 && has_file(to, "blockclonia.cfg", "fov 90\n"));

    snprintf(victim, sizeof victim, "%s/victim.txt", root);
    CHECK(put_file(root, "victim.txt", "precious") == 0);
    snprintf(a, sizeof a, "%s/link.cfg", from);
    snprintf(b, sizeof b, "%s/link.cfg", to);
    int linked = tmp_symlink(victim, a);
    CHECK(linked != 0);
    CHECK(save_copy_file(a, b, 64) == 0 && missing(to, "link.cfg")); /* symlinks are not followed */

    /* save_copy_world: only level.dat, player.dat and canonical column
     * names, never through a symlink. */
    CHECK(save_copy_world(from, to) == 0); /* no level.dat */
    CHECK(put_file(from, "player.dat", "player") == 0);
    CHECK(put_file(from, "c.0.-3.bin", "column") == 0);
    CHECK(put_file(from, "c.01.0.bin", "alias") == 0);
    CHECK(put_file(from, "c.+1.0.bin", "alias") == 0);
    CHECK(put_file(from, "c.0.0.bin.tmp", "temp") == 0);
    CHECK(put_file(from, "notes.txt", "junk") == 0);
    snprintf(link, sizeof link, "%s/c.5.5.bin", from);
    CHECK(tmp_symlink(victim, link) == linked);
    CHECK(put_file(from, "level.dat", "seed 42\n") == 0);
    CHECK(save_copy_world(from, to) == 3);
    uint32_t seed = 0;
    CHECK(save_read_seed(to, &seed) == 0 && seed == 42);
    CHECK(has_file(to, "player.dat", "player") && has_file(to, "c.0.-3.bin", "column"));
    CHECK(missing(to, "c.01.0.bin") && missing(to, "c.+1.0.bin") && missing(to, "c.0.0.bin.tmp"));
    CHECK(missing(to, "notes.txt") && missing(to, "c.5.5.bin"));

    /* A world already there is left alone. */
    CHECK(put_file(from, "player.dat", "newer") == 0);
    CHECK(save_copy_world(from, to) == 0 && has_file(to, "player.dat", "player"));

    /* An old world reached through a symlinked folder is not copied. */
    char via[400], to2[400];
    snprintf(via, sizeof via, "%s/via", root);
    snprintf(to2, sizeof to2, "%s/new2", root);
    CHECK(tmp_symlink(from, via) == linked && os_mkdir(to2, 0700) == 0);
    if (linked == 1) CHECK(save_copy_world(via, to2) == 0 && missing(to2, "level.dat"));
}

void test_paths_all(void)
{
    env_save();
    test_app_dir();
    test_resolve();
    env_restore();
    test_parent();

    char root[256];
    int made = tmp_dir_make(root, sizeof root, "mc_paths_");
    CHECK(made);
    if (!made) return;
    test_make_dirs(root);
    test_copy(root);
    const char *dirs[] = {"/a/b/c", "/a/b", "/a", "/old", "/new", "/new2", "/via", ""};
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) rm_dir(root, dirs[i]);
}
