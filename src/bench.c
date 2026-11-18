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
    world w = {0};
    world_init(&w, seed, 8, js, NULL);
    world_load_blocking(&w, 8.0, 8.0, 5);
    mesh_input *in = mem_alloc(sizeof *in);
    uint32_t *out = mem_alloc(sizeof(uint32_t) * MESH_OUT_VERTS);
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
                uint32_t o = 0, t = 0;
                quads += mesh_section(in, out, &o, &t);
                sections++;
            }
        }
    double mesh = os_now() - t0;
    printf("  gather+mesh: %.3f ms/section, %.0f quads/section avg, %u bytes/vertex\n",
           mesh * 1000.0 / sections, (double)quads / sections, (unsigned)sizeof(uint32_t));

    /* Physics: structural check and a collapse. */
    physics ph = {0};
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
    player pl = {0};
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
        if (ph.fl.updates > peak) peak = ph.fl.updates;
        if (!fluid_pending(&ph.fl)) break;
    }
    double ft = os_now() - t0;
    printf("  fluid: %d ticks to settle, peak %d active cells, %.3f ms/tick\n", ticks, peak,
           ft * 1000.0 / (ticks ? ticks : 1));

    /* A dam break: a 24 x 24 m reservoir 6 m deep, on a stone floor above
     * the highest ground under it, loses its east wall and floods down the
     * terrain. */
    const int r0 = -40, rn = 24, depth = 6;
    int top = 0;
    for (int z = r0 - 1; z <= r0 + rn; z++)
        for (int x = r0 - 1; x <= r0 + rn; x++) {
            int s = world_surface_y(&w, x, z);
            top = s > top ? s : top;
        }
    for (int z = r0 - 1; z <= r0 + rn; z++)
        for (int x = r0 - 1; x <= r0 + rn; x++) {
            int edge = x < r0 || x >= r0 + rn || z < r0 || z >= r0 + rn;
            for (int y = world_surface_y(&w, x, z); y <= top; y++) world_set(&w, x, y, z, B_STONE, 0);
            for (int y = top + 1; y <= top + depth; y++) world_set(&w, x, y, z, edge ? B_STONE : B_WATER, 0);
        }
    fluid_forget(&ph.fl);
    for (int z = r0; z < r0 + rn; z++)
        for (int y = top + 1; y <= top + depth; y++) world_set(&w, r0 + rn, y, z, B_AIR, 0);
    long updates = 0;
    peak = 0;
    t0 = os_now();
    for (ticks = 0; ticks < 1500 && fluid_pending(&ph.fl); ticks++) {
        physics_fluid_tick(&ph);
        updates += ph.fl.updates;
        if (ph.fl.updates > peak) peak = ph.fl.updates;
    }
    ft = os_now() - t0;
    printf("  flood: %d ticks to settle, %ld cell updates (peak %d/tick), %.3f ms/tick, %.3f ms per 1000 cells\n",
           ticks, updates, peak, ft * 1000.0 / (ticks ? ticks : 1), ft * 1e6 / (double)(updates ? updates : 1));

    physics_destroy(&ph);
    mem_free(in);
    mem_free(out);
    world_destroy(&w);
    jobs_destroy(js);
    for (int i = 0; i < side * side; i++) column_free(cols[i]);
    mem_free(cols);
    return 0;
}
