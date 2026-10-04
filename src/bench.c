#include "bench.h"
#include "jobs.h"
#include "mem.h"
#include "mesher.h"
#include "os.h"
#include "physics.h"
#include "world.h"

#include <stdio.h>
#include <string.h>

int bench_run(uint32_t seed)
{
    const int side = 16; /* 16 x 16 columns */
    printf("blockclonia benchmark (seed %u, single thread)\n", seed);

    /* Terrain generation. */
    column **cols = mem_calloc((size_t)side * (size_t)side, sizeof *cols);
    double t0 = os_now();
    for (int z = 0; z < side; z++)
        for (int x = 0; x < side; x++) {
            column *c = column_alloc(x, z);
            worldgen_column(seed, c);
            column_recount(c);
            cols[z * side + x] = c;
        }
    double gen = os_now() - t0;
    printf("  worldgen: %.3f ms/column\n", gen * 1000.0 / (side * side));

    /* Meshing, via a real world so gather/mesh paths are exercised. */
    jobs *js = jobs_create(0);
    world w;
    world_init(&w, seed, 8, js, NULL);
    world_load_blocking(&w, 8.0, 8.0, 5);
    mesh_input *in = mem_alloc(sizeof *in);
    uint32_t *out = mem_alloc(sizeof(uint32_t) * 4 * MESH_MAX_QUADS);
    uint64_t quads = 0;
    int sections = 0;
    t0 = os_now();
    for (int cz = -4; cz <= 4; cz++)
        for (int cx = -4; cx <= 4; cx++) {
            const column *c = world_column(&w, cx, cz);
            if (!c) continue;
            for (int sy = 0; sy < SECTIONS; sy++) {
                /* Build the padded input the same way the mesher job does. */
                for (int y = -1; y <= SECTION_H; y++)
                    for (int z = -1; z <= CHUNK_W; z++)
                        for (int x = -1; x <= CHUNK_W; x++) {
                            int wx = cx * CHUNK_W + x, wy = sy * SECTION_H + y, wz = cz * CHUNK_W + z;
                            in->blocks[mesh_pidx(x, y, z)] = world_get(&w, wx, wy, wz);
                            in->meta[mesh_pidx(x, y, z)] = world_get_meta(&w, wx, wy, wz);
                        }
                uint32_t o, t;
                quads += mesh_section(in, out, &o, &t);
                sections++;
            }
        }
    double mesh = os_now() - t0;
    printf("  gather+mesh: %.3f ms/section, %.0f quads/section avg, %u bytes/vertex\n",
           mesh * 1000.0 / sections, (double)quads / sections, (unsigned)sizeof(uint32_t));

    /* Physics: structural check and a collapse. */
    physics ph;
    physics_init(&ph, &w);
    w.edit_user = &ph;
    w.on_block_changed = physics_on_block_changed;
    int gy = world_surface_y(&w, 8, 8);
    for (int y = gy; y < gy + 10 && y < WORLD_H; y++) world_set(&w, 8, y, 8, B_STONE, 0);
    for (int i = 1; i <= 6; i++) world_set(&w, 8 + i, gy + 9, 8, B_PLANKS, 0);
    t0 = os_now();
    int iters = 50;
    for (int i = 0; i < iters; i++) physics_check_structure(&ph, 8, gy + 5, 8);
    double sc = os_now() - t0;
    printf("  structural check (25^3 region): %.3f ms\n", sc * 1000.0 / iters);
    world_set(&w, 8, gy + 2, 8, B_AIR, 0);
    int fell = physics_check_structure(&ph, 8, gy + 2, 8);
    player pl;
    player_spawn(&pl, &w, 0.5, 0.5);
    player_input pin = {0};
    t0 = os_now();
    int steps = 0;
    while (ph.body_count && steps < 600) {
        physics_step(&ph, &pl, &pin);
        steps++;
    }
    double ps = os_now() - t0;
    printf("  collapse: %d blocks fell, settled in %d steps (%.2f s sim), %.3f ms/step\n", fell, steps,
           steps * PHYS_DT, ps * 1000.0 / (steps ? steps : 1));

    /* Fluids: pour a column of water on a slope. */
    int wy = world_surface_y(&w, 20, 20);
    for (int y = wy; y < wy + 4 && y < WORLD_H; y++) world_set(&w, 20, y, 20, B_WATER, 0);
    t0 = os_now();
    int ticks = 0, peak = 0;
    for (; ticks < 200; ticks++) {
        physics_fluid_tick(&ph);
        if (ph.fluid_updates > peak) peak = ph.fluid_updates;
        if (!ph.fluid_next.count) break;
    }
    double ft = os_now() - t0;
    printf("  fluid: %d ticks to settle, peak %d active cells, %.3f ms/tick\n", ticks, peak,
           ft * 1000.0 / (ticks ? ticks : 1));

    physics_destroy(&ph);
    mem_free(in);
    mem_free(out);
    world_destroy(&w);
    jobs_destroy(js);
    for (int i = 0; i < side * side; i++) column_free(cols[i]);
    mem_free(cols);
    return 0;
}
