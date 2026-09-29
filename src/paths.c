#include "paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

/* An absolute directory from the environment, with its trailing slashes
 * dropped (so "/" has length 0); NULL when unset, empty or relative. */
static const char *env_dir(const char *name, size_t *len)
{
    const char *v = getenv(name);
    if (!v || v[0] != '/') return NULL;
    size_t n = strlen(v);
    while (n > 0 && v[n - 1] == '/') n--;
    *len = n;
    return v;
}

int paths_app_dir(const char *var, const char *home_rel, char *out, size_t cap)
{
    size_t n = 0;
    const char *base = env_dir(var, &n);
    const char *rel = "";
    if (!base) {
        base = env_dir("HOME", &n);
        rel = home_rel;
        if (!base) return -1;
    }
    if (n >= cap) return -1; /* also keeps the %.*s precision within int */
    int w = snprintf(out, cap, "%.*s%s%s/blockclonia", (int)n, base, rel[0] ? "/" : "", rel);
    return w > 0 && (size_t)w < cap ? 0 : -1;
}

static void resolve(char *out, const char *var, const char *home_rel, const char *name, const char *old_name, int *xdg)
{
    char dir[PATHS_MAX];
    int w = -1;
    if (paths_app_dir(var, home_rel, dir, sizeof dir) == 0) w = snprintf(out, PATHS_MAX, "%s/%s", dir, name);
    *xdg = w > 0 && w < PATHS_MAX;
    if (!*xdg) snprintf(out, PATHS_MAX, "%s", old_name);
}

void paths_resolve(paths *p)
{
    resolve(p->config, "XDG_CONFIG_HOME", ".config", "blockclonia.cfg", "blockclonia.cfg", &p->xdg_config);
    resolve(p->world, "XDG_DATA_HOME", ".local/share", "worlds/world", "world", &p->xdg_world);
    resolve(p->pipelines, "XDG_CACHE_HOME", ".cache", "blockclonia.pipelines", "blockclonia.pipelines",
            &p->xdg_pipelines);
}

int paths_make_dirs(const char *dir)
{
    char buf[PATHS_MAX];
    size_t n = strlen(dir);
    if (n == 0 || n >= sizeof buf) return -1;
    memcpy(buf, dir, n + 1);
    for (size_t i = 1; i <= n; i++) {
        if (buf[i] != '/' && buf[i] != '\0') continue;
        char c = buf[i];
        buf[i] = '\0';
        /* mkdir can fail on a directory that exists for reasons other than
         * EEXIST (a read-only parent), so what counts is what is there. */
        struct stat st;
        if (mkdir(buf, 0700) != 0 && (stat(buf, &st) != 0 || !S_ISDIR(st.st_mode))) return -1;
        buf[i] = c;
    }
    return 0;
}

int paths_parent(const char *path, char *out, size_t cap)
{
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path) return -1;
    size_t n = (size_t)(slash - path);
    if (n >= cap) return -1;
    memcpy(out, path, n);
    out[n] = '\0';
    return 0;
}
