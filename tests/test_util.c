#include "test_util.h"
#include "mem.h"
#include "survival.h"

#include <string.h>

int g_failed, g_checks;

void test_flat_gen(uint32_t seed, column *c)
{
    (void)seed;
    memset(c->blocks, B_AIR, COL_VOL);
    for (int z = 0; z < CHUNK_W; z++)
        for (int x = 0; x < CHUNK_W; x++)
            for (int y = 0; y < GROUND; y++)
                c->blocks[col_index(x, y, z)] = y == 0 ? B_BEDROCK : (y < 10 ? B_STONE : (y < 12 ? B_DIRT : B_GRASS));
}

void tw_init(test_world *t)
{
    t->js = jobs_create(0);
    world_init(&t->w, 1, 3, t->js, NULL);
    t->w.generator = test_flat_gen;
    world_load_blocking(&t->w, 0.5, 0.5, 3);
    physics_init(&t->ph, &t->w);
    t->w.edit_user = &t->ph;
    t->w.on_block_changed = physics_on_block_changed;
}

void tw_free(test_world *t)
{
    physics_destroy(&t->ph);
    world_destroy(&t->w);
    jobs_destroy(t->js);
}

void run_steps(test_world *t, player *p, const player_input *in, int n)
{
    for (int i = 0; i < n; i++) physics_step(&t->ph, p, in);
}

health *new_body(uint32_t seed)
{
    health *h = mem_alloc(sizeof *h); /* 30 KB: too big for a test's stack */
    health_init(h, seed);
    return h;
}

health_env calm_env(void)
{
    health_env e = {0};
    e.on_ground = 1;
    e.airway = AIRWAY_AIR;
    e.water_temp = SURVIVAL_WATER_TEMP;
    e.air_temp = 20.0;
    e.contact_temp = 20.0;
    return e;
}

/* Steps the body at 60 Hz, like the game. */
void live(health *h, const health_env *e, double seconds)
{
    for (int i = 0, n = (int)(seconds * 60.0); i < n; i++) health_step(h, e, 1.0 / 60.0);
}
