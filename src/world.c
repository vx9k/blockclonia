#include "world.h"
#include "jobs.h"
#include "mem.h"
#include "mesher.h"
#include "save.h"
#include "log.h"
#include "mathlib.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- columns */

column *column_alloc(int cx, int cz)
{
    column *c = mem_calloc(1, sizeof *c);
    c->cx = cx;
    c->cz = cz;
    c->state = COL_LOADING;
    c->blocks = mem_alloc(COL_VOL);
    return c;
}

void column_free(column *c)
{
    if (!c) return;
    mem_free(c->blocks);
    mem_free(c->meta);
    mem_free(c);
}

void column_recount(column *c)
{
    for (int s = 0; s < SECTIONS; s++) {
        const uint8_t *b = &c->blocks[s * SECTION_H * COL_AREA];
        int n = 0;
        for (int i = 0; i < SECTION_H * COL_AREA; i++) n += b[i] != B_AIR;
        c->solid_count[s] = (uint16_t)n;
    }
}

static int slot_of(const world *w, int cx, int cz)
{
    return floormod(cz, w->grid_w) * w->grid_w + floormod(cx, w->grid_w);
}

static int in_limits(int cx, int cz)
{
    const int lim = WORLD_LIMIT / CHUNK_W;
    return cx > -lim && cx < lim && cz > -lim && cz < lim;
}

column *world_column(const world *w, int cx, int cz)
{
    column *c = w->grid[slot_of(w, cx, cz)];
    if (c && c->cx == cx && c->cz == cz && c->state == COL_READY) return c;
    return NULL;
}

/* ------------------------------------------------------------ block access */

uint8_t world_get(const world *w, int x, int y, int z)
{
    if (y < 0) return B_BEDROCK;
    if (y >= WORLD_H) return B_AIR;
    const column *c = world_column(w, chunk_of(x), chunk_of(z));
    if (!c) return B_UNLOADED;
    return c->blocks[col_index(x & (CHUNK_W - 1), y, z & (CHUNK_W - 1))];
}

uint8_t world_get_meta(const world *w, int x, int y, int z)
{
    if (y < 0 || y >= WORLD_H) return 0;
    const column *c = world_column(w, chunk_of(x), chunk_of(z));
    if (!c || !c->meta) return 0;
    return c->meta[col_index(x & (CHUNK_W - 1), y, z & (CHUNK_W - 1))];
}

int world_water_level(const world *w, int x, int y, int z)
{
    if (world_get(w, x, y, z) != B_WATER) return 0;
    uint8_t m = world_get_meta(w, x, y, z);
    return m ? m : WATER_FULL;
}

static void mark_section(world *w, int cx, int cz, int sy)
{
    if (sy < 0 || sy >= SECTIONS) return;
    column *c = world_column(w, cx, cz);
    if (!c) return;
    c->dirty[sy] = 1;
    c->version[sy] = ++w->version_counter;
}

int world_set(world *w, int x, int y, int z, uint8_t id, uint8_t meta)
{
    if (y < 0 || y >= WORLD_H || id >= B_COUNT) return 0;
    column *c = world_column(w, chunk_of(x), chunk_of(z));
    if (!c) return 0;
    int lx = x & (CHUNK_W - 1), lz = z & (CHUNK_W - 1);
    int i = col_index(lx, y, lz);
    uint8_t old = c->blocks[i];
    uint8_t old_meta = c->meta ? c->meta[i] : 0;
    if (id != B_WATER || meta >= WATER_FULL) meta = 0;
    if (old == id && old_meta == meta) return 1;

    c->blocks[i] = id;
    if (meta && !c->meta) c->meta = mem_calloc(COL_VOL, 1);
    if (c->meta) c->meta[i] = meta;
    int sy = y / SECTION_H;
    c->solid_count[sy] = (uint16_t)(c->solid_count[sy] + (id != B_AIR) - (old != B_AIR));
    c->modified = 1;

    /* Any block within one step (diagonals included, for AO) may change
     * its faces, so dirty every section the 3x3x3 neighbourhood touches. */
    int keys[27][3], nk = 0;
    for (int dy = -1; dy <= 1; dy++)
        for (int dz = -1; dz <= 1; dz++)
            for (int dx = -1; dx <= 1; dx++) {
                int k0 = chunk_of(x + dx), k1 = chunk_of(z + dz);
                int k2 = (y + dy) < 0 ? -1 : (y + dy) / SECTION_H;
                int seen = 0;
                for (int k = 0; k < nk && !seen; k++)
                    seen = keys[k][0] == k0 && keys[k][1] == k1 && keys[k][2] == k2;
                if (seen) continue;
                keys[nk][0] = k0; keys[nk][1] = k1; keys[nk][2] = k2;
                nk++;
            }
    for (int k = 0; k < nk; k++) mark_section(w, keys[k][0], keys[k][1], keys[k][2]);

    if (w->on_block_changed) w->on_block_changed(w->edit_user, x, y, z, old, id);
    return 1;
}

int world_surface_y(const world *w, int x, int z)
{
    const column *c = world_column(w, chunk_of(x), chunk_of(z));
    if (!c) return -1;
    int lx = x & (CHUNK_W - 1), lz = z & (CHUNK_W - 1);
    for (int y = WORLD_H - 1; y >= 0; y--) {
        uint8_t b = c->blocks[col_index(lx, y, lz)];
        if (block_solid(b) || b == B_WATER) return y + 1;
    }
    return 0;
}

/* -------------------------------------------------------------- load jobs */

typedef struct {
    job base;
    world *w;
    column *c;
} gen_job;

static void gen_run(job *j)
{
    gen_job *g = (gen_job *)j;
    int loaded = 0;
    if (g->w->save_dir[0]) {
        int r = save_load_column(g->w->save_dir, g->c);
        if (r < 0)
            log_warn("column %d,%d: save file corrupt, regenerating", g->c->cx, g->c->cz);
        loaded = r > 0;
    }
    if (!loaded) {
        if (g->w->generator) g->w->generator(g->w->seed, g->c);
        else worldgen_column(g->w->seed, g->c);
        mem_free(g->c->meta);
        g->c->meta = NULL;
    }
    g->c->modified = (uint8_t)loaded; /* keep saved columns saved */
    column_recount(g->c);
}

static void unload_column(world *w, column *c);

static void gen_done(job *j)
{
    gen_job *g = (gen_job *)j;
    world *w = g->w;
    column *c = g->c;
    w->gen_in_flight--;
    if (c->want_unload) {
        int slot = slot_of(w, c->cx, c->cz);
        if (w->grid[slot] == c) w->grid[slot] = NULL;
        column_free(c);
    } else {
        c->state = COL_READY;
        for (int s = 0; s < SECTIONS; s++) {
            c->dirty[s] = 1;
            c->version[s] = ++w->version_counter;
        }
    }
    mem_free(g);
}

static void request_column(world *w, int cx, int cz)
{
    column *c = column_alloc(cx, cz);
    w->grid[slot_of(w, cx, cz)] = c;
    gen_job *g = mem_calloc(1, sizeof *g);
    g->base.run = gen_run;
    g->base.done = gen_done;
    g->w = w;
    g->c = c;
    w->gen_in_flight++;
    jobs_submit(w->jobs, &g->base);
}

static void unload_column(world *w, column *c)
{
    if (c->state == COL_LOADING) {
        c->want_unload = 1;
        return;
    }
    if (c->modified && w->save_dir[0]) {
        if (save_store_column(w->save_dir, c) != 0)
            log_error("failed to save column %d,%d", c->cx, c->cz);
    }
    if (w->on_mesh_free)
        for (int s = 0; s < SECTIONS; s++)
            if (c->mesh[s].vtx_capacity) w->on_mesh_free(w->render_user, &c->mesh[s]);
    w->grid[slot_of(w, c->cx, c->cz)] = NULL;
    column_free(c);
}

/* -------------------------------------------------------------- mesh jobs */

typedef struct {
    job base;
    world *w;
    int cx, cz, sy;
    uint32_t version;
    uint32_t *verts;
    uint32_t opaque, trans;
    mesh_input in;
} mesh_job;

static void mesh_run(job *j)
{
    mesh_job *m = (mesh_job *)j;
    uint32_t *scratch = jobs_scratch(sizeof(uint32_t) * 4 * MESH_MAX_QUADS);
    uint32_t quads = mesh_section(&m->in, scratch, &m->opaque, &m->trans);
    if (quads) {
        m->verts = mem_alloc(sizeof(uint32_t) * 4 * quads);
        memcpy(m->verts, scratch, sizeof(uint32_t) * 4 * quads);
    }
}

static void mesh_done(job *j)
{
    mesh_job *m = (mesh_job *)j;
    world *w = m->w;
    w->mesh_in_flight--;
    column *c = world_column(w, m->cx, m->cz);
    if (c && c->meshing[m->sy] && c->pending_version[m->sy] == m->version) {
        c->meshing[m->sy] = 0;
        if (w->on_mesh_ready &&
            !w->on_mesh_ready(w->render_user, c, m->sy, m->verts, m->opaque, m->trans))
            c->dirty[m->sy] = 1;
    }
    mem_free(m->verts);
    mem_free(m);
}

/* Copies the section plus a 1-block border out of up to 9 columns. The
 * worker then meshes a private snapshot, so edits never race with it. */
static void gather(const world *w, int cx, int cz, int sy, mesh_input *in)
{
    const column *cols[3][3];
    for (int dz = -1; dz <= 1; dz++)
        for (int dx = -1; dx <= 1; dx++)
            cols[dz + 1][dx + 1] = world_column(w, cx + dx, cz + dz);

    for (int py = -1; py <= SECTION_H; py++) {
        int y = sy * SECTION_H + py;
        for (int pz = -1; pz <= CHUNK_W; pz++) {
            int zi = pz < 0 ? 0 : (pz >= CHUNK_W ? 2 : 1);
            int lz = pz & (CHUNK_W - 1);
            uint8_t *dst = &in->blocks[mesh_pidx(-1, py, pz)];
            uint8_t *dstm = &in->meta[mesh_pidx(-1, py, pz)];
            if (y < 0 || y >= WORLD_H) {
                memset(dst, y < 0 ? B_BEDROCK : B_AIR, MESH_PAD);
                memset(dstm, 0, MESH_PAD);
                continue;
            }
            /* x = -1, 0..15, 16 */
            for (int xi = 0; xi < 3; xi++) {
                const column *c = cols[zi][xi];
                int x0 = xi == 0 ? CHUNK_W - 1 : 0;
                int n = xi == 1 ? CHUNK_W : 1;
                int off = xi == 0 ? 0 : (xi == 1 ? 1 : CHUNK_W + 1);
                if (!c) {
                    memset(dst + off, B_STONE, (size_t)n);
                    memset(dstm + off, 0, (size_t)n);
                    continue;
                }
                size_t src = (size_t)col_index(x0, y, lz);
                memcpy(dst + off, &c->blocks[src], (size_t)n);
                if (c->meta) memcpy(dstm + off, &c->meta[src], (size_t)n);
                else memset(dstm + off, 0, (size_t)n);
            }
        }
    }
}

static int neighbours_ready(const world *w, int cx, int cz)
{
    for (int dz = -1; dz <= 1; dz++)
        for (int dx = -1; dx <= 1; dx++)
            if (!world_column(w, cx + dx, cz + dz)) return 0;
    return 1;
}

static void schedule_meshes(world *w)
{
    for (int i = 0; i < w->spiral_count && w->mesh_in_flight < w->max_mesh_in_flight; i++) {
        int cx = w->center_cx + w->spiral[i][0], cz = w->center_cz + w->spiral[i][1];
        int dx = cx - w->center_cx, dz = cz - w->center_cz;
        if (dx * dx + dz * dz > w->radius * w->radius) continue;
        column *c = world_column(w, cx, cz);
        if (!c || !neighbours_ready(w, cx, cz)) continue;
        for (int sy = 0; sy < SECTIONS && w->mesh_in_flight < w->max_mesh_in_flight; sy++) {
            if (!c->dirty[sy] || c->meshing[sy]) continue;
            c->dirty[sy] = 0;
            if (c->solid_count[sy] == 0) {
                if (c->mesh[sy].vtx_capacity && w->on_mesh_free)
                    w->on_mesh_free(w->render_user, &c->mesh[sy]);
                c->mesh[sy].opaque_quads = c->mesh[sy].trans_quads = 0;
                continue;
            }
            mesh_job *m = mem_alloc(sizeof *m);
            memset(m, 0, offsetof(mesh_job, in));
            m->base.run = mesh_run;
            m->base.done = mesh_done;
            m->w = w;
            m->cx = cx;
            m->cz = cz;
            m->sy = sy;
            m->version = c->version[sy];
            gather(w, cx, cz, sy, &m->in);
            c->meshing[sy] = 1;
            c->pending_version[sy] = m->version;
            w->mesh_in_flight++;
            jobs_submit(w->jobs, &m->base);
        }
    }
}

/* -------------------------------------------------------------- streaming */

static int cmp_dist(const void *a, const void *b)
{
    const int *p = a, *q = b;
    int da = p[0] * p[0] + p[1] * p[1], db = q[0] * q[0] + q[1] * q[1];
    return (da > db) - (da < db);
}

void world_init(world *w, uint32_t seed, int radius, jobs *js, const char *save_dir)
{
    memset(w, 0, sizeof *w);
    w->seed = seed;
    w->radius = clampi(radius, 2, 32);
    w->grid_w = 2 * (w->radius + 1) + 1;
    w->grid = mem_calloc((size_t)w->grid_w * (size_t)w->grid_w, sizeof(column *));
    w->jobs = js;
    int workers = jobs_worker_count(js);
    w->max_gen_in_flight = workers ? 2 * workers : 1;
    w->max_mesh_in_flight = workers ? 4 * workers : 1;
    if (save_dir && save_dir[0]) {
        size_t n = strlen(save_dir);
        if (n >= sizeof w->save_dir) log_fatal("save directory path too long");
        memcpy(w->save_dir, save_dir, n + 1);
    }

    int r = w->radius + 1;
    w->spiral = mem_alloc(mem_array_size((size_t)(2 * r + 1) * (size_t)(2 * r + 1), sizeof *w->spiral));
    w->spiral_count = 0;
    for (int dz = -r; dz <= r; dz++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dz * dz <= r * r) {
                w->spiral[w->spiral_count][0] = dx;
                w->spiral[w->spiral_count][1] = dz;
                w->spiral_count++;
            }
    qsort(w->spiral, (size_t)w->spiral_count, sizeof *w->spiral, cmp_dist);
}

void world_update(world *w, double px, double pz)
{
    int pcx = chunk_of((int)floor(px)), pcz = chunk_of((int)floor(pz));
    int r = w->radius + 1;

    if (!w->have_center || pcx != w->center_cx || pcz != w->center_cz) {
        w->center_cx = pcx;
        w->center_cz = pcz;
        w->have_center = 1;
        for (int i = 0; i < w->grid_w * w->grid_w; i++) {
            column *c = w->grid[i];
            if (!c) continue;
            int dx = c->cx - pcx, dz = c->cz - pcz;
            if (abs(dx) > r || abs(dz) > r || dx * dx + dz * dz > (r + 1) * (r + 1))
                unload_column(w, c);
        }
    }

    for (int i = 0; i < w->spiral_count && w->gen_in_flight < w->max_gen_in_flight; i++) {
        int cx = pcx + w->spiral[i][0], cz = pcz + w->spiral[i][1];
        if (!in_limits(cx, cz)) continue;
        column *c = w->grid[slot_of(w, cx, cz)];
        if (c) {
            /* Came back into range before its load finished: keep it. */
            if (c->cx == cx && c->cz == cz) c->want_unload = 0;
            continue;
        }
        request_column(w, cx, cz);
    }

    schedule_meshes(w);
}

void world_load_blocking(world *w, double px, double pz, int radius)
{
    int pcx = chunk_of((int)floor(px)), pcz = chunk_of((int)floor(pz));
    radius = clampi(radius, 0, w->radius + 1);
    for (int dz = -radius; dz <= radius; dz++)
        for (int dx = -radius; dx <= radius; dx++) {
            int cx = pcx + dx, cz = pcz + dz;
            if (!in_limits(cx, cz)) continue;
            column *c = w->grid[slot_of(w, cx, cz)];
            while (c && (c->cx != cx || c->cz != cz)) {
                unload_column(w, c); /* frees now, or flags a loading column */
                if (w->grid[slot_of(w, cx, cz)] == c) {
                    jobs_wait_idle(w->jobs);
                    jobs_poll(w->jobs, -1);
                }
                c = w->grid[slot_of(w, cx, cz)];
            }
            if (!c) request_column(w, cx, cz);
        }
    jobs_wait_idle(w->jobs);
    jobs_poll(w->jobs, -1);
}

void world_save_all(world *w)
{
    if (!w->save_dir[0]) return;
    for (int i = 0; i < w->grid_w * w->grid_w; i++) {
        column *c = w->grid[i];
        if (c && c->state == COL_READY && c->modified) {
            if (save_store_column(w->save_dir, c) != 0)
                log_error("failed to save column %d,%d", c->cx, c->cz);
        }
    }
}

void world_destroy(world *w)
{
    jobs_wait_idle(w->jobs);
    jobs_poll(w->jobs, -1);
    for (int i = 0; i < w->grid_w * w->grid_w; i++) {
        column *c = w->grid[i];
        if (!c) continue;
        if (w->on_mesh_free)
            for (int s = 0; s < SECTIONS; s++)
                if (c->mesh[s].vtx_capacity) w->on_mesh_free(w->render_user, &c->mesh[s]);
        column_free(c);
    }
    mem_free(w->grid);
    mem_free(w->spiral);
    memset(w, 0, sizeof *w);
}
