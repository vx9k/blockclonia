/* The player's worlds: one folder each under a root (by default
 * $XDG_DATA_HOME/blockclonia/worlds), listed, created, renamed and
 * deleted from the world screen. The folder name is a stable id made
 * from the first name; the name shown lives in the world's database, so
 * renaming never moves files.
 *
 * Folder contents and typed names are untrusted: names are checked
 * (save_name_valid), folder names are made from a safe alphabet, listing
 * only peeks read-only (save_db_peek), and deleting removes the files a
 * world has by name, without following links, and keeps anything else.
 * It does not open worlds for play; main.c does, through save_db. */
#ifndef MC_WORLDS_H
#define MC_WORLDS_H

#include "save_db.h"

#include <stdint.h>

#define WORLDS_MAX 64
#define WORLDS_PATH_MAX 512

typedef struct {
    char dir[WORLDS_PATH_MAX];   /* full path of the folder */
    char folder[64];             /* its name, for display when the world has no name */
    save_peek info;
} world_entry;

/* Fills out with up to max worlds under root, most recently played first
 * (never-played and unnamed ones after, by folder name). Returns how
 * many; 0 if root does not exist. */
int worlds_list(const char *root, world_entry *out, int max);

/* The seed a typed string means: a decimal number up to 2^32-1 is used
 * as it is, any other text is hashed (FNV-1a), so "island" always gives
 * the same world; empty text gives `fallback`. */
uint32_t worlds_seed_from_text(const char *text, uint32_t fallback);

/* A folder name for a world called `name`: lower-case letters, digits
 * and single hyphens, at most 32 characters, "world" if nothing is left.
 * Writes it to out (cap >= 33). */
void worlds_folder_name(const char *name, char *out, size_t cap);

/* Creates a world folder under root for `name` (made unique with -2, -3,
 * ... if taken), records the name, seed, generator version and creation
 * time, and writes its path to dir_out. 0 on success, -1 (logged) if the
 * name is invalid or the folder or database cannot be made. */
int worlds_create(const char *root, const char *name, uint32_t seed, char *dir_out, size_t cap);

/* Sets the world's shown name; the folder stays. 0 on success. */
int worlds_rename(const char *dir, const char *name);

/* Deletes a world: the database and its side files, and the files older
 * builds wrote, by name, then the folder if nothing else is in it.
 * Refuses a world open in a game, and never follows links. 0 if the
 * world is gone, 1 if its files are gone but the folder holds other
 * files (kept), -1 if nothing was deleted. */
int worlds_delete(const char *dir);

#endif
