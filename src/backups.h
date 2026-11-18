/* Rolling copies of a world's database in <world>/backups. One is taken
 * when a world is opened for play, so it holds the world as it was before
 * the session; they are pruned to the newest few plus the newest of each
 * of the last days, and restored from the world list. Restoring first
 * copies the world as it is, so a restore can itself be undone.
 *
 * Copies are named backup-<10-digit Unix seconds>.db. Anything else in the
 * folder is left alone, a folder or copy that is a symlink is refused,
 * and a copy is checked like any world before it is restored. Headless:
 * the caller passes the time. Not here: what the world list shows. */
#ifndef MC_BACKUPS_H
#define MC_BACKUPS_H

#include "save_db.h"

#include <stddef.h>
#include <stdint.h>

#define BACKUPS_RECENT 3  /* the newest copies always kept */
#define BACKUPS_DAYS 7    /* and the newest of each of these last UTC days */
#define BACKUPS_GAP 600   /* s: opening again within this takes no new copy */
#define BACKUPS_LIST_MAX 64

typedef struct {
    int64_t time; /* Unix seconds, from the name */
    char name[32];
} backup_entry;

/* "just now", "5 minutes ago", "3 hours ago", "2 days ago" for a copy
 * taken age seconds ago ("in the future" if negative). */
void backups_age_text(int64_t age, char *out, size_t cap);

/* 1 with *t set if name is a copy's name, 0 otherwise. */
int backups_name_time(const char *name, int64_t *t);

/* The world's copies, newest first, up to max (the newest ones if there
 * are more). 0 if it has none or the folder is not a real directory. */
int backups_list(const char *world_dir, backup_entry *out, int max);

/* Which of n copies (times newest first) to keep at time now: keep[i] is
 * set to 1 or 0. Copies dated in the future are not kept. */
void backups_keep(const int64_t *times, int n, int64_t now, int *keep);

/* Copies the open world into its backups folder and prunes the old ones.
 * 1 if a copy was taken, 0 if the newest is less than BACKUPS_GAP old, -1
 * (logged) if it could not be. */
int backups_take(save_db *db, const char *world_dir, int64_t now);

/* Replaces the world with the copy called name, after copying the world
 * as it is. 0 on success; -1 (logged) if the copy is not a world this
 * build reads, the world is open in a game, or a step failed, and then
 * the world is as it was. */
int backups_restore(const char *world_dir, const char *name, int64_t now);

#endif
