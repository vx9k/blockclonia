/* The player's worlds (worlds.c, save_db_peek): names and seeds from typed
 * text, creating, listing read-only, renaming, and deleting only a
 * world's own files, never through a symlink and never while in use. */
#include "os.h"
#include "save.h"
#include "save_db.h"
#include "test_util.h"
#include "worlds.h"

#include <stdio.h>
#include <string.h>

static char g_root[256];

static void path_in(char *out, size_t cap, const char *dir, const char *name)
{ snprintf(out, cap, "%s/%s", dir, name); }

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

static int exists(const char *path) { return os_path_kind(path) != OS_MISSING; }

static void test_names_and_seeds(void)
{
    char f[64];
    worlds_folder_name("My World!", f, sizeof f);
    CHECK(strcmp(f, "my-world") == 0);
    worlds_folder_name("  ..//../etc  ", f, sizeof f);
    CHECK(strcmp(f, "etc") == 0); /* no path survives */
    worlds_folder_name("!!!", f, sizeof f);
    CHECK(strcmp(f, "world") == 0);
    worlds_folder_name("abcdefghijklmnopqrstuvwxyz0123456789", f, sizeof f);
    CHECK(strlen(f) == 32);

    CHECK(save_name_valid("A", 1) && save_name_valid("Hello world", 11));
    CHECK(!save_name_valid("", 0) && !save_name_valid(" lead", 5) && !save_name_valid("trail ", 6));
    CHECK(!save_name_valid("tab\there", 8) && !save_name_valid("\xc3\xa9", 2));
    CHECK(!save_name_valid("123456789012345678901234567890123", 33));

    CHECK(worlds_seed_from_text("12345", 7) == 12345u);
    CHECK(worlds_seed_from_text("4294967295", 7) == 4294967295u);
    CHECK(worlds_seed_from_text("", 7) == 7u);
    uint32_t island = worlds_seed_from_text("island", 7);
    CHECK(island == worlds_seed_from_text("island", 9) && island != worlds_seed_from_text("Island", 7));
    CHECK(worlds_seed_from_text("4294967296", 7) != 0u); /* too big for a number: hashed */
}

static void test_create_list_rename(void)
{
    char a[WORLDS_PATH_MAX], b[WORLDS_PATH_MAX], p[WORLDS_PATH_MAX + 32];
    CHECK(worlds_create(g_root, "My World", 42, a, sizeof a) == 0);
    CHECK(worlds_create(g_root, "My World", 43, b, sizeof b) == 0);
    path_in(p, sizeof p, g_root, "my-world-2");
    CHECK(strcmp(b, p) == 0); /* a taken folder gets a number */
    CHECK(worlds_create(g_root, " bad", 1, b, sizeof b) == -1);

    /* An empty folder is not a world, and listing does not make it one;
     * a folder from an older build shows, to be imported when played. */
    char empty[WORLDS_PATH_MAX], old[WORLDS_PATH_MAX];
    path_in(empty, sizeof empty, g_root, "empty");
    os_mkdir(empty, 0700);
    path_in(old, sizeof old, g_root, "old");
    os_mkdir(old, 0700);
    path_in(p, sizeof p, old, "level.dat");
    write_text(p, "not read by the list");

    world_entry list[8];
    int n = worlds_list(g_root, list, 8);
    CHECK(n == 3);
    path_in(p, sizeof p, empty, "world.db");
    CHECK(!exists(p));
    int seen_old = 0, seen_named = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(list[i].folder, "old") == 0) seen_old = list[i].info.status == SAVE_PEEK_LEGACY;
        if (strcmp(list[i].folder, "my-world") == 0)
            seen_named = list[i].info.status == SAVE_PEEK_OK && strcmp(list[i].info.name, "My World") == 0 &&
                         list[i].info.has_seed && list[i].info.seed == 42 && list[i].info.generator == WORLDGEN_VERSION;
    }
    CHECK(seen_old && seen_named);

    /* A world open in a game: listed as in use, not renamed or deleted. */
    save_db *db = save_db_open(a);
    CHECK(db != NULL);
    save_peek pk;
    save_db_peek(a, &pk);
    CHECK(pk.status == SAVE_PEEK_IN_USE);
    CHECK(worlds_rename(a, "Other") == -1 && worlds_delete(a) == -1);
    path_in(p, sizeof p, a, "world.db");
    CHECK(exists(p));
    /* A name a crafted file carries is shown only if valid. */
    CHECK(save_db_put_blob(db, "name", "bad\nname", 8) == 0);
    save_db_close(db);
    save_db_peek(a, &pk);
    CHECK(pk.status == SAVE_PEEK_OK && pk.name[0] == '\0');

    CHECK(worlds_rename(a, "Renamed") == 0 && worlds_rename(a, "") == -1);
    save_db_peek(a, &pk);
    CHECK(strcmp(pk.name, "Renamed") == 0 && pk.seed == 42);
    tmp_remove(empty);
}

static void test_delete(void)
{
    char dir[WORLDS_PATH_MAX], p[WORLDS_PATH_MAX + 32], victim[WORLDS_PATH_MAX + 32];
    CHECK(worlds_create(g_root, "Doomed", 1, dir, sizeof dir) == 0);
    /* A planted symlink named like a world file goes, not its target. */
    path_in(victim, sizeof victim, g_root, "victim.txt");
    write_text(victim, "keep me");
    path_in(p, sizeof p, dir, "level.dat");
    int linked = tmp_symlink(victim, p);
    /* A file the game did not write stays, and so does the folder. */
    char notes[WORLDS_PATH_MAX + 32];
    path_in(notes, sizeof notes, dir, "notes.txt");
    write_text(notes, "mine");
    CHECK(worlds_delete(dir) == 1);
    path_in(p, sizeof p, dir, "world.db");
    CHECK(!exists(p) && exists(notes) && exists(victim));
    if (linked) {
        path_in(p, sizeof p, dir, "level.dat");
        CHECK(!exists(p));
    }
    tmp_remove(notes);
    CHECK(worlds_delete(dir) == 0 && !exists(dir));

    /* A world folder that is itself a symlink is refused. */
    char real[WORLDS_PATH_MAX], link[WORLDS_PATH_MAX];
    CHECK(worlds_create(g_root, "Real", 1, real, sizeof real) == 0);
    path_in(link, sizeof link, g_root, "link");
    if (tmp_symlink(real, link)) {
        CHECK(worlds_delete(link) == -1);
        path_in(p, sizeof p, real, "world.db");
        CHECK(exists(p));
        tmp_remove(link);
    }
    CHECK(worlds_delete(real) == 0);
    tmp_remove(victim);
}

void test_worlds_all(void)
{
    int made = tmp_dir_make(g_root, sizeof g_root, "mc_worlds_");
    CHECK(made);
    if (!made) return;
    test_names_and_seeds();
    test_create_list_rename();
    test_delete();
    world_entry list[WORLDS_MAX];
    int n = worlds_list(g_root, list, WORLDS_MAX);
    for (int i = 0; i < n; i++) worlds_delete(list[i].dir);
    char p[WORLDS_PATH_MAX + 32];
    path_in(p, sizeof p, g_root, "old/level.dat");
    tmp_remove(p);
    path_in(p, sizeof p, g_root, "old");
    tmp_remove(p);
    tmp_remove(g_root);
    CHECK(!exists(g_root)); /* everything the test made is gone */
}
