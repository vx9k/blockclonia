#include "worlds.h"
#include "log.h"
#include "os.h"
#include "paths.h"
#include "save.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int entry_order(const void *a, const void *b)
{
    const world_entry *x = a, *y = b;
    if (x->info.last_played != y->info.last_played) return x->info.last_played > y->info.last_played ? -1 : 1;
    return strcmp(x->folder, y->folder);
}

int worlds_list(const char *root, world_entry *out, int max)
{
    os_dir *d = os_dir_open(root);
    if (!d) return 0;
    int n = 0;
    const char *name;
    while (n < max && (name = os_dir_next(d)) != NULL) {
        if (name[0] == '.' || strlen(name) >= sizeof out[n].folder) continue;
        world_entry *e = &out[n];
        int len = snprintf(e->dir, sizeof e->dir, "%s/%s", root, name);
        /* Folders only: a symlink in the worlds folder is not followed. */
        if (len < 0 || (size_t)len >= sizeof e->dir || os_path_kind(e->dir) != OS_DIR) continue;
        snprintf(e->folder, sizeof e->folder, "%s", name);
        save_db_peek(e->dir, &e->info);
        if (e->info.status != SAVE_PEEK_NONE) n++;
    }
    os_dir_close(d);
    qsort(out, (size_t)n, sizeof *out, entry_order);
    return n;
}

uint32_t worlds_seed_from_text(const char *text, uint32_t fallback)
{
    size_t len = strlen(text);
    if (len == 0) return fallback;
    int digits = len <= 10;
    for (size_t i = 0; i < len && digits; i++) digits = text[i] >= '0' && text[i] <= '9';
    if (digits) {
        unsigned long long v = strtoull(text, NULL, 10);
        if (v <= UINT32_MAX) return (uint32_t)v;
    }
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) h = (h ^ (uint8_t)text[i]) * 16777619u;
    return h;
}

void worlds_folder_name(const char *name, char *out, size_t cap)
{
    size_t n = 0, limit = cap - 1 < 32 ? cap - 1 : 32;
    for (const char *p = name; *p && n < limit; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out[n++] = c;
        else if (n > 0 && out[n - 1] != '-') out[n++] = '-';
    }
    while (n > 0 && out[n - 1] == '-') n--;
    if (n == 0) n = (size_t)snprintf(out, cap, "world");
    out[n] = '\0';
}

int worlds_create(const char *root, const char *name, uint32_t seed, char *dir_out, size_t cap)
{
    if (!save_name_valid(name, strlen(name))) {
        log_error("world name '%s' is not allowed", name);
        return -1;
    }
    if (paths_make_dirs(root) != 0) {
        log_error("cannot create %s", root);
        return -1;
    }
    char base[40], dir[WORLDS_PATH_MAX];
    worlds_folder_name(name, base, sizeof base);
    int made = 0;
    for (int k = 1; k < 1000 && !made; k++) {
        int len = k == 1 ? snprintf(dir, sizeof dir, "%s/%s", root, base)
                         : snprintf(dir, sizeof dir, "%s/%s-%d", root, base, k);
        if (len < 0 || (size_t)len >= sizeof dir) return -1;
        /* mkdir fails on anything already there, a symlink included, so a
         * taken name moves on instead of reusing someone's folder. */
        if (os_mkdir(dir, 0700) == 0) made = 1;
        else if (errno != EEXIST) {
            log_error("cannot create %s: %s", dir, strerror(errno));
            return -1;
        }
    }
    if (!made) return -1;
    save_db *db = save_db_open(dir);
    if (!db) return -1;
    int ok = save_db_begin(db) == 0;
    ok = ok && save_db_set_seed(db, seed) == 0;
    ok = ok && save_db_set_meta(db, "generator", WORLDGEN_VERSION) == 0;
    ok = ok && save_db_set_meta(db, "created", (int64_t)time(NULL)) == 0;
    ok = ok && save_db_put_blob(db, "name", name, strlen(name)) == 0;
    ok = ok && save_db_commit(db) == 0;
    save_db_close(db);
    if (!ok) {
        log_error("could not set up the world in %s", dir);
        worlds_delete(dir);
        return -1;
    }
    snprintf(dir_out, cap, "%s", dir);
    return 0;
}

int worlds_rename(const char *dir, const char *name)
{
    if (!save_name_valid(name, strlen(name))) return -1;
    save_db *db = save_db_open(dir); /* refused if a game has the world open */
    if (!db) return -1;
    int r = save_db_put_blob(db, "name", name, strlen(name));
    save_db_close(db);
    return r;
}

/* What a world folder may hold that deleting the world removes. */
static int world_file(const char *name)
{
    static const char *const FILES[] = {"world.db",         "world.db-wal", "world.db-shm",
                                        "world.db-journal", "level.dat",    "player.dat"};
    for (size_t i = 0; i < sizeof FILES / sizeof FILES[0]; i++)
        if (strcmp(name, FILES[i]) == 0) return 1;
    int cx, cz;
    return save_column_name(name, &cx, &cz);
}

int worlds_delete(const char *dir)
{
    save_peek p;
    save_db_peek(dir, &p);
    if (p.status == SAVE_PEEK_IN_USE) {
        log_error("%s is open in a game; not deleting it", dir);
        return -1;
    }
    /* The folder itself must not be a symlink, and every name below is
     * removed relative to it with unlinkat, which removes a symlink
     * rather than what it points at. */
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        log_error("cannot open %s: %s", dir, strerror(errno));
        return -1;
    }
    int lister = dup(fd);
    DIR *d = lister >= 0 ? fdopendir(lister) : NULL;
    if (!d) {
        if (lister >= 0) close(lister);
        close(fd);
        return -1;
    }
    int removed = 0, kept = 0;
    const struct dirent *e;
    /* cppcheck-suppress readdirCalled */
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        struct stat st;
        int ours = world_file(e->d_name) && fstatat(fd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
                   !S_ISDIR(st.st_mode);
        if (ours && unlinkat(fd, e->d_name, 0) == 0) removed++;
        else kept++;
    }
    closedir(d);
    close(fd);
    if (kept) {
        log_warn("deleted the world in %s but kept %d other files there", dir, kept);
        return removed ? 1 : -1;
    }
    if (rmdir(dir) != 0) {
        log_error("cannot remove %s: %s", dir, strerror(errno));
        return 1;
    }
    log_info("deleted world %s", dir);
    return 0;
}
