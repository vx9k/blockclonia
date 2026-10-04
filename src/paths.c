#include "paths.h"
#include "os.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define HOME_VAR "USERPROFILE"
static int is_sep(char c) { return c == '/' || c == '\\'; }
#else
#define HOME_VAR "HOME"
static int is_sep(char c) { return c == '/'; }
#endif

/* Length of the part of an absolute path that names no directory to
 * create: "/" on POSIX; "C:\" or "\\server\share\" on Windows. 0 if path is
 * relative. */
static size_t root_len(const char *p)
{
#ifdef _WIN32
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':' && is_sep(p[2])) return 3;
    if (is_sep(p[0]) && is_sep(p[1])) {
        size_t i = 2;
        while (p[i] && !is_sep(p[i])) i++; /* server */
        if (i == 2 || !p[i]) return 0;
        size_t share = ++i;
        while (p[i] && !is_sep(p[i])) i++;
        if (i == share) return 0;
        return p[i] ? i + 1 : i;
    }
    return 0;
#else
    return p[0] == '/';
#endif
}

/* An absolute directory from the environment, with its trailing slashes
 * dropped (so "/" has length 0); NULL when unset, empty or relative. */
static const char *env_dir(const char *name, size_t *len)
{
    const char *v = getenv(name);
    if (!v || !root_len(v)) return NULL;
    size_t n = strlen(v);
    while (n > 0 && is_sep(v[n - 1])) n--;
    *len = n;
    return v;
}

int paths_app_dir(const char *var, const char *home_rel, char *out, size_t cap)
{
    size_t n = 0;
    const char *base = env_dir(var, &n);
    const char *rel = "";
    if (!base) {
        base = env_dir(HOME_VAR, &n);
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
#ifdef _WIN32
    /* Settings and worlds roam with a domain profile; the pipeline cache is
     * specific to this machine's GPU and driver, so it stays local. */
    resolve(p->config, "APPDATA", "AppData/Roaming", "blockclonia.cfg", "blockclonia.cfg", &p->xdg_config);
    resolve(p->world, "APPDATA", "AppData/Roaming", "worlds/world", "world", &p->xdg_world);
    resolve(p->pipelines, "LOCALAPPDATA", "AppData/Local", "blockclonia.pipelines", "blockclonia.pipelines",
            &p->xdg_pipelines);
#else
    resolve(p->config, "XDG_CONFIG_HOME", ".config", "blockclonia.cfg", "blockclonia.cfg", &p->xdg_config);
    resolve(p->world, "XDG_DATA_HOME", ".local/share", "worlds/world", "world", &p->xdg_world);
    resolve(p->pipelines, "XDG_CACHE_HOME", ".cache", "blockclonia.pipelines", "blockclonia.pipelines",
            &p->xdg_pipelines);
#endif
}

int paths_make_dirs(const char *dir)
{
    char buf[PATHS_MAX];
    size_t n = strlen(dir);
    if (n == 0 || n >= sizeof buf) return -1;
    memcpy(buf, dir, n + 1);
    size_t root = root_len(buf);
    for (size_t i = root ? root : 1; i <= n; i++) {
        if (!is_sep(buf[i]) && buf[i] != '\0') continue;
        if (is_sep(buf[i - 1])) continue; /* "a//b" or a trailing separator */
        char c = buf[i];
        buf[i] = '\0';
        /* mkdir can fail on a directory that exists for reasons other than
         * EEXIST (a read-only parent), so what counts is what is there. */
        if (os_mkdir(buf, 0700) != 0 && !os_is_dir(buf)) return -1;
        buf[i] = c;
    }
    return 0;
}

int paths_parent(const char *path, char *out, size_t cap)
{
    const char *slash = NULL;
    for (const char *c = path; *c; c++)
        if (is_sep(*c)) slash = c;
    size_t root = root_len(path);
    if (!slash || (size_t)(slash - path) < (root ? root : 1)) return -1;
    size_t n = (size_t)(slash - path);
    if (n >= cap) return -1;
    memcpy(out, path, n);
    out[n] = '\0';
    return 0;
}
