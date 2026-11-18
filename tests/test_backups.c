/* World backups (backups.c): copy names and ages, which copies the
 * rolling policy keeps, taking copies of an open world, pruning, restoring
 * one (and the copy of the world as it was), and refusing symlinks, worlds
 * open in a game and files that are not worlds. */
#include "backups.h"
#include "mem.h"
#include "os.h"
#include "save.h"
#include "save_db.h"
#include "test_util.h"
#include "worlds.h"

#include <sqlite3.h>

#include <stdio.h>
#include <string.h>

#define T0 1800000000LL /* 2027-01-15, a fixed "now" */

static char g_root[256];

static void path_in(char *out, size_t cap, const char *dir, const char *name)
{ snprintf(out, cap, "%s/%s", dir, name); }

/* A new world folder holding a database with this seed. */
static void make_world(char *dir, size_t cap, const char *name, uint32_t seed)
{
    path_in(dir, cap, g_root, name);
    (void)os_mkdir(dir, 0700);
    save_db *db = save_db_open(dir);
    CHECK(db && save_db_set_seed(db, seed) == 0);
    save_db_close(db);
}

static uint32_t seed_of(const char *dir)
{
    uint32_t seed = 0;
    save_db *db = save_db_open(dir);
    if (db) save_db_get_seed(db, &seed);
    save_db_close(db);
    return seed;
}

/* One copy taken at time t; the result of backups_take. */
static int take_at(const char *dir, int64_t t)
{
    save_db *db = save_db_open(dir);
    int r = db ? backups_take(db, dir, t) : -2;
    save_db_close(db);
    return r;
}

static void test_names_and_ages(void)
{
    int64_t t = 0;
    CHECK(backups_name_time("backup-1800000000.db", &t) == 1 && t == T0);
    CHECK(backups_name_time("backup-0000000001.db", &t) == 1 && t == 1);
    CHECK(!backups_name_time("backup-180000000.db", &t));   /* nine digits */
    CHECK(!backups_name_time("backup-18000000000.db", &t)); /* eleven */
    CHECK(!backups_name_time("backup-18000a0000.db", &t));
    CHECK(!backups_name_time("backup-1800000000.dbx", &t));
    CHECK(!backups_name_time("Backup-1800000000.db", &t));
    CHECK(!backups_name_time("world.db", &t));
    char s[32] = {0};
    backups_age_text(0, s, sizeof s);
    CHECK(strcmp(s, "just now") == 0);
    backups_age_text(59, s, sizeof s);
    CHECK(strcmp(s, "just now") == 0);
    backups_age_text(60, s, sizeof s);
    CHECK(strcmp(s, "1 minute ago") == 0);
    backups_age_text(3 * 3600 + 5, s, sizeof s);
    CHECK(strcmp(s, "3 hours ago") == 0);
    backups_age_text(2 * 86400, s, sizeof s);
    CHECK(strcmp(s, "2 days ago") == 0);
    backups_age_text(-5, s, sizeof s);
    CHECK(strcmp(s, "in the future") == 0);
}

/* The newest three, then the newest of each of the last seven days. */
static void test_keep_policy(void)
{
    const int64_t day = 86400, today = T0 - T0 % day;
    const int64_t times[] = {
        T0 + 3600,           /* in the future: dropped */
        T0,                  /* 0: newest three */
        T0 - 60,             /* 1 */
        T0 - 120,            /* 2 */
        T0 - 180,            /* 3: today, but not the newest of today */
        today - 10,          /* yesterday's newest */
        today - 20,          /* yesterday's second: dropped */
        today - 6 * day + 5, /* six days ago: the last day kept */
        today - 7 * day + 5, /* seven days ago: dropped */
    };
    const int want[] = {0, 1, 1, 1, 0, 1, 0, 1, 0};
    int n = (int)(sizeof times / sizeof times[0]), keep[9] = {0};
    backups_keep(times, n, T0, keep);
    for (int i = 0; i < n; i++) CHECK(keep[i] == want[i]);
    backups_keep(times, 0, T0, keep); /* nothing to do */
}

static void test_take_and_prune(void)
{
    char dir[400] = {0}, p[600] = {0};
    make_world(dir, sizeof dir, "take", 11);
    backup_entry list[BACKUPS_LIST_MAX] = {0};
    CHECK(backups_list(dir, list, BACKUPS_LIST_MAX) == 0);
    CHECK(take_at(dir, T0) == 1);
    CHECK(take_at(dir, T0 + BACKUPS_GAP - 1) == 0); /* too soon */
    CHECK(take_at(dir, T0 + BACKUPS_GAP) == 1);
    int n = backups_list(dir, list, BACKUPS_LIST_MAX);
    CHECK(n == 2 && list[0].time == T0 + BACKUPS_GAP && list[1].time == T0);
    CHECK(backups_list(dir, list, 1) == 1 && list[0].time == T0 + BACKUPS_GAP); /* the newest, when cut short */

    /* The copy is a world in its own right, with no -wal or -shm. */
    path_in(p, sizeof p, dir, "backups/backup-1800000000.db");
    sqlite3 *h = NULL;
    sqlite3_stmt *s = NULL;
    CHECK(sqlite3_open_v2(p, &h, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK &&
          sqlite3_prepare_v2(h, "PRAGMA journal_mode", -1, &s, NULL) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW &&
          strcmp((const char *)sqlite3_column_text(s, 0), "delete") == 0);
    sqlite3_finalize(s);
    sqlite3_close(h);

    /* Fifteen days of play, four sessions a day: what is left is the
     * newest three and one for each of the last seven days. */
    for (int d = 0; d < 15; d++)
        for (int k = 0; k < 4; k++) CHECK(take_at(dir, T0 + 86400LL * (d + 1) + 3600LL * k) == 1);
    int64_t now = T0 + 86400LL * 15 + 3600LL * 3;
    n = backups_list(dir, list, BACKUPS_LIST_MAX);
    CHECK(n == 9); /* the three newest are all from the last day, which also counts as one of the seven */
    CHECK(list[0].time == now && list[n - 1].time / 86400 == now / 86400 - 6);

    /* Other files in the folder stay. */
    path_in(p, sizeof p, dir, "backups/notes.txt");
    CHECK(save_write_file(p, "mine", 4) == 0);
    CHECK(take_at(dir, now + 86400) == 1);
    CHECK(os_path_kind(p) == OS_FILE);
    tmp_remove(p);
}

static void test_restore(void)
{
    char dir[400] = {0}, p[600] = {0};
    make_world(dir, sizeof dir, "restore", 21);
    CHECK(take_at(dir, T0) == 1);
    save_db *db = save_db_open(dir);
    CHECK(db && save_db_set_seed(db, 22) == 0);
    /* Open in a game: no restore, nothing changes. */
    CHECK(backups_restore(dir, "backup-1800000000.db", T0 + 10) == -1);
    save_db_close(db);
    CHECK(seed_of(dir) == 22);

    CHECK(backups_restore(dir, "backup-1800000000.db", T0 + 10) == 0);
    CHECK(seed_of(dir) == 21);
    /* The world as it was before the restore is a copy too. */
    backup_entry list[BACKUPS_LIST_MAX] = {0};
    CHECK(backups_list(dir, list, BACKUPS_LIST_MAX) == 2 && list[0].time == T0 + 10);
    CHECK(backups_restore(dir, list[0].name, T0 + 20) == 0);
    CHECK(seed_of(dir) == 22); /* undone */

    /* Not a world, a bad name, or missing: refused, the world kept. */
    path_in(p, sizeof p, dir, "backups/backup-1700000000.db");
    CHECK(save_write_file(p, "not a database at all, just text", 32) == 0);
    CHECK(backups_restore(dir, "backup-1700000000.db", T0 + 30) == -1);
    CHECK(backups_restore(dir, "../world.db", T0 + 30) == -1);
    CHECK(backups_restore(dir, "backup-1600000000.db", T0 + 30) == -1);
    CHECK(seed_of(dir) == 22);
    tmp_remove(p);
}

/* A shared world whose backups folder or copy is a symlink: nothing is
 * written, listed or restored through it. */
static void test_symlinks(void)
{
    char dir[400] = {0}, p[600] = {0}, elsewhere[400] = {0}, victim[600] = {0};
    make_world(dir, sizeof dir, "linked", 31);
    path_in(elsewhere, sizeof elsewhere, g_root, "elsewhere");
    (void)os_mkdir(elsewhere, 0700);
    path_in(victim, sizeof victim, elsewhere, "backup-1700000000.db");
    CHECK(save_write_file(victim, "keep", 4) == 0);
    path_in(p, sizeof p, dir, "backups");
    if (tmp_symlink(elsewhere, p) == 1) {
        backup_entry list[4] = {0};
        CHECK(backups_list(dir, list, 4) == 0);
        CHECK(take_at(dir, T0) == -1);
        CHECK(os_path_kind(victim) == OS_FILE); /* not pruned through the link */
        tmp_remove(p);
    }
    /* A copy that is a symlink to another world. */
    char other[400] = {0}, target[600] = {0};
    make_world(other, sizeof other, "other", 99);
    path_in(target, sizeof target, other, "world.db");
    (void)os_mkdir(p, 0700);
    path_in(p, sizeof p, dir, "backups/backup-1700000000.db");
    if (tmp_symlink(target, p) == 1) {
        CHECK(backups_restore(dir, "backup-1700000000.db", T0) == -1);
        CHECK(seed_of(dir) == 31);
        tmp_remove(p);
    }
    tmp_remove(victim);
    tmp_remove(elsewhere);
}

/* Deleting a world takes its backups with it. */
static void test_delete(void)
{
    char dir[400] = {0}, p[600] = {0};
    make_world(dir, sizeof dir, "gone", 41);
    CHECK(take_at(dir, T0) == 1);
    CHECK(worlds_delete(dir) == 0);
    path_in(p, sizeof p, dir, "backups");
    CHECK(os_path_kind(p) == OS_MISSING && os_path_kind(dir) == OS_MISSING);
}

/* Removes a world folder the tests made: its database, side files and
 * backups. */
static void remove_world(const char *name)
{
    char dir[400] = {0};
    path_in(dir, sizeof dir, g_root, name);
    if (os_path_kind(dir) == OS_DIR) worlds_delete(dir);
}

void test_backups_all(void)
{
    int made = tmp_dir_make(g_root, sizeof g_root, "mc_backups_");
    CHECK(made);
    if (!made) return;
    test_names_and_ages();
    test_keep_policy();
    test_take_and_prune();
    test_restore();
    test_symlinks();
    test_delete();
    const char *worlds[] = {"take", "restore", "linked", "other"};
    for (size_t i = 0; i < sizeof worlds / sizeof worlds[0]; i++) remove_world(worlds[i]);
    tmp_remove(g_root);
}
