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

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failed++;                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                        \
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

/* Entry points of the other test files. */
void test_thermo_all(void);
void test_health2_all(void);
void test_bodies_all(void);
void test_visual_all(void);

#endif
