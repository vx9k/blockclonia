#include "backups.h"
#include "log.h"
#include "os.h"

#include <stdio.h>
#include <string.h>

#define PREFIX "backup-"
#define SUFFIX ".db"
#define DIGITS 10 /* Unix seconds until the year 2286 */
#define DAY 86400

void backups_age_text(int64_t age, char *out, size_t cap)
{
    static const struct {
        int64_t secs;
        const char *unit;
    } UNITS[] = {{DAY, "day"}, {3600, "hour"}, {60, "minute"}};
    if (age < 0) {
        snprintf(out, cap, "in the future");
        return;
    }
    for (size_t i = 0; i < sizeof UNITS / sizeof UNITS[0]; i++) {
        long long n = (long long)(age / UNITS[i].secs);
        if (n >= 1) {
            snprintf(out, cap, "%lld %s%s ago", n, UNITS[i].unit, n == 1 ? "" : "s");
            return;
        }
    }
    snprintf(out, cap, "just now");
}

int backups_name_time(const char *name, int64_t *t)
{
    size_t lp = strlen(PREFIX), ls = strlen(SUFFIX);
    if (strlen(name) != lp + DIGITS + ls || strncmp(name, PREFIX, lp) != 0 || strcmp(name + lp + DIGITS, SUFFIX) != 0)
        return 0;
    int64_t v = 0;
    for (size_t i = 0; i < DIGITS; i++) {
        char ch = name[lp + i];
        if (ch < '0' || ch > '9') return 0;
        v = v * 10 + (ch - '0');
    }
    *t = v;
    return 1;
}

/* dir/backups, or "" if it does not fit. */
static void folder_of(const char *world_dir, char *out, size_t cap)
{
    int n = snprintf(out, cap, "%s/backups", world_dir);
    if (n < 0 || (size_t)n >= cap) out[0] = '\0';
}

int backups_list(const char *world_dir, backup_entry *out, int max)
{
    char folder[600] = {0};
    folder_of(world_dir, folder, sizeof folder);
    /* lstat: a symlink planted in a shared world never counts. */
    if (!folder[0] || os_path_kind(folder) != OS_DIR) return 0;
    os_dir *d = os_dir_open(folder);
    if (!d) return 0;
    int n = 0;
    const char *name = NULL;
    while ((name = os_dir_next(d)) != NULL) {
        int64_t t = 0;
        char path[700] = {0};
        if (!backups_name_time(name, &t)) continue;
        snprintf(path, sizeof path, "%s/%s", folder, name);
        if (os_path_kind(path) != OS_FILE) continue;
        /* Insertion into the newest-first list, dropping the oldest past max. */
        int at = n;
        while (at > 0 && out[at - 1].time < t) at--;
        if (at >= max) continue;
        int last = n < max ? n : max - 1;
        for (int i = last; i > at; i--) out[i] = out[i - 1];
        out[at].time = t;
        snprintf(out[at].name, sizeof out[at].name, "%s", name);
        if (n < max) n++;
    }
    os_dir_close(d);
    return n;
}

void backups_keep(const int64_t *times, int n, int64_t now, int *keep)
{
    int64_t today = now / DAY, last_day = -1;
    int recent = 0;
    for (int i = 0; i < n; i++) {
        keep[i] = 0;
        if (times[i] > now) continue; /* a clock that ran ahead, or a planted name */
        if (recent < BACKUPS_RECENT) {
            keep[i] = 1;
            recent++;
        }
        /* Newest first, so the first copy seen from a day is its newest. */
        int64_t day = times[i] / DAY;
        if (today - day < BACKUPS_DAYS && day != last_day) keep[i] = 1;
        last_day = day;
    }
}

/* Deletes the copies backups_keep does not keep. */
static void prune(const char *world_dir, int64_t now)
{
    backup_entry list[BACKUPS_LIST_MAX] = {0};
    int64_t times[BACKUPS_LIST_MAX] = {0};
    int keep[BACKUPS_LIST_MAX] = {0};
    int n = backups_list(world_dir, list, BACKUPS_LIST_MAX);
    for (int i = 0; i < n; i++) times[i] = list[i].time;
    backups_keep(times, n, now, keep);
    char path[700] = {0};
    for (int i = 0; i < n; i++) {
        if (keep[i]) continue;
        snprintf(path, sizeof path, "%s/backups/%s", world_dir, list[i].name);
        if (remove(path) != 0) log_warn("could not delete the old backup %s", path);
    }
}

/* Copies the world to backups/backup-<now>.db; 0 or -1. */
static int copy_now(save_db *db, const char *world_dir, int64_t now)
{
    char folder[600] = {0}, path[700] = {0};
    folder_of(world_dir, folder, sizeof folder);
    if (!folder[0] || now < 0 || now > 9999999999LL) return -1;
    if (os_path_kind(folder) == OS_MISSING) (void)os_mkdir(folder, 0700);
    if (os_path_kind(folder) != OS_DIR) {
        log_error("%s is not a folder; no backup taken", folder);
        return -1;
    }
    snprintf(path, sizeof path, "%s/" PREFIX "%010lld" SUFFIX, folder, (long long)now);
    if (save_db_copy(db, path) != 0) return -1; /* logged */
    log_info("backed up the world to %s", path);
    return 0;
}

int backups_take(save_db *db, const char *world_dir, int64_t now)
{
    backup_entry newest = {0};
    if (backups_list(world_dir, &newest, 1) == 1 && newest.time <= now && now - newest.time < BACKUPS_GAP) return 0;
    if (copy_now(db, world_dir, now) != 0) return -1;
    prune(world_dir, now);
    return 1;
}

int backups_restore(const char *world_dir, const char *name, int64_t now)
{
    int64_t t = 0;
    char path[700] = {0};
    if (!backups_name_time(name, &t)) return -1;
    snprintf(path, sizeof path, "%s/backups/%s", world_dir, name);
    save_db *db = save_db_open(world_dir);
    if (!db) return -1;
    /* The world as it is goes into a copy first; pruning waits until the
     * restore is done, so it cannot take the copy being restored. */
    int r = copy_now(db, world_dir, now) == 0 && save_db_restore(db, path) == 0 ? 0 : -1;
    save_db_close(db);
    if (r == 0) log_info("restored %s from %s", world_dir, name);
    prune(world_dir, now);
    return r;
}
