/* Shared by the unit test files: the CHECK macro, a flat test world and
 * helpers for the body model. */
#ifndef MC_TEST_UTIL_H
#define MC_TEST_UTIL_H

#include "health.h"
#include "jobs.h"
#include "physics.h"
#include "world.h"

#include <stdio.h>

extern int g_failed, g_checks;

#define CHECK(cond)                                                         \
    do {                                                                    \
        g_checks++;                                                         \
        if (!(cond)) {                                                      \
            g_failed++;                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                   \
    } while (0)

#define GROUND 13 /* first air block in the flat test world */

typedef struct {
    jobs *js;
    world w;
    physics ph;
} test_world;

/* A flat world (bedrock, stone, dirt, grass up to GROUND) loaded around
 * the origin with physics attached. */
void test_flat_gen(uint32_t seed, column *c);
void tw_init(test_world *t);
void tw_free(test_world *t);
void run_steps(test_world *t, player *p, const player_input *in, int n);

health *new_body(uint32_t seed);   /* mem_free it */
health_env calm_env(void);         /* standing still in mild air */
void live(health *h, const health_env *e, double seconds); /* 60 Hz steps */

/* File-system helpers that POSIX and Windows spell differently. */
/* Makes a fresh directory "<temp>/<prefix>XXXXXX" in out ($TMPDIR or /tmp;
 * %TEMP% on Windows). Returns 1, or 0 if none could be made. */
int tmp_dir_make(char *out, size_t cap, const char *prefix);
/* Creates a symlink at `link` pointing to `target`: 1 if made, 0 on
 * failure, -1 where this system does not let the tests make one (Windows
 * without Developer Mode or administrator rights); checks that need the
 * link are skipped then. */
int tmp_symlink(const char *target, const char *link);
/* Removes a file, symlink or empty directory; never follows a link. */
void tmp_remove(const char *path);
/* Sets an environment variable; NULL unsets it. */
void tmp_setenv(const char *name, const char *value);

/* Entry points of the other test files. */
void test_thermo_all(void);
void test_health2_all(void);
void test_bodies_all(void);
void test_visual_all(void);
void test_sound_all(void);
void test_gpucaps_all(void);
void test_health_debug_all(void);
void test_defib_all(void);
// void test_cardio_all(void);
void test_paths_all(void);
void test_save_db_all(void);

#endif
