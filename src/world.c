#include "world.h"
#include "jobs.h"
#include "log.h"
#include "mathlib.h"
#include "mem.h"
#include "mesher.h"
#include "os.h"
#include "save.h"
#include "save_db.h"

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
        int n = 0, o = 0;
        for (int i = 0; i < SECTION_H * COL_AREA; i++) {
            n += b[i] != B_AIR;
            o += block_opaque(b[i]);
        }
        c->solid_count[s] = (uint16_t)n;
        c->opaque_count[s] = (uint16_t)o;
    }
}

static int slot_of(const world *w, int cx, int cz)
{
    return (cz & w->grid_mask) * w->grid_w + (cx & w->grid_mask);
}

static int in_limits(int cx, int cz)
{
    const int lim = WORLD_LIMIT / CHUNK_W;
    return cx > -lim && cx < lim && cz > -lim && cz < lim;
}

/* Columns are loaded one ring beyond the mesh radius so every meshable
 * column has all eight neighbours; `slack` adds unload hysteresis. */
static int in_ring(int dx, int dz, int radius, int slack)
{
    int ax = abs(dx) - 1 - slack, az = abs(dz) - 1 - slack;
    if (ax < 0) ax = 0;
    if (az < 0) az = 0;
    return ax * ax + az * az <= radius * radius;
}

/* ------------------------------------------------------------ block access */

uint8_t world_get_meta(const world *w, int x, int y, int z)
{
    if (y < 0 || y >= WORLD_H) return 0;
    const column *c = world_column(w, chunk_of(x), chunk_of(z));
    if (!c || !c->meta) return 0;
    return c->meta[col_index(x & (CHUNK_W - 1), y, z & (CHUNK_W - 1))];
}

int world_water_level(const world *w, int x, int y, int z)
{
    if (y < 0 || y >= WORLD_H) return 0;
    const column *c = world_column(w, chunk_of(x), chunk_of(z));
    if (!c) return 0;
    int i = col_index(x & (CHUNK_W - 1), y, z & (CHUNK_W - 1));
    if (c->blocks[i] != B_WATER) return 0;
    uint8_t m = c->meta ? c->meta[i] : 0;
    return m ? m : WATER_FULL;
}

static void mark_section(world *w, int cx, int cz, int sy, int urgent)
{
    if (sy < 0 || sy >= SECTIONS) return;
    column *c = world_column(w, cx, cz);
    if (!c) return;
    c->dirty[sy] = 1;
    c->version[sy] = ++w->version_counter;
    w->sched_pending = 1;
    if (urgent && w->urgent_count < WORLD_URGENT) {
        int *u = w->urgent[w->urgent_count++];
        u[0] = cx;
        u[1] = cz;
        u[2] = sy;
    }
}

static int set_block(world *w, int x, int y, int z, uint8_t id, uint8_t meta, int urgent)
{
    if (y < 0 || y >= WORLD_H || id >= B_COUNT) return 0;
    column *c = world_column(w, chunk_of(x), chunk_of(z));
    if (!c) return 0;
    int lx = x & (CHUNK_W - 1), lz = z & (CHUNK_W - 1);
    int i = col_index(lx, y, lz);
    uint8_t old = c->blocks[i];
    uint8_t old_meta = c->meta ? c->meta[i] : 0;
    if (!block_has_meta(id) || meta >= WATER_FULL) meta = 0;
    if (old == id && old_meta == meta) return 1;

    c->blocks[i] = id;
    if (meta && !c->meta) c->meta = mem_calloc(COL_VOL, 1);
    if (c->meta) c->meta[i] = meta;
    int sy = y / SECTION_H;
    c->solid_count[sy] = (uint16_t)(c->solid_count[sy] + (id != B_AIR) - (old != B_AIR));
    c->opaque_count[sy] = (uint16_t)(c->opaque_count[sy] + block_opaque(id) - block_opaque(old));
    c->modified = 1;
    c->unsaved = 1;

    /* Sections the block's neighbourhood reaches: at most two per axis. */
    int cx = chunk_of(x), cz = chunk_of(z);
    int xs[2] = {cx, cx}, zs[2] = {cz, cz}, ys[2] = {sy, sy};
    int nx = 1, nz = 1, ny = 1;
    if (lx == 0) xs[nx++] = cx - 1;
    else if (lx == CHUNK_W - 1) xs[nx++] = cx + 1;
    if (lz == 0) zs[nz++] = cz - 1;
    else if (lz == CHUNK_W - 1) zs[nz++] = cz + 1;
    if (y % SECTION_H == 0 && sy > 0) ys[ny++] = sy - 1;
    else if (y % SECTION_H == SECTION_H - 1 && sy + 1 < SECTIONS) ys[ny++] = sy + 1;
    if (block_opaque(id) != block_opaque(old)) {
        /* Opacity changes neighbours' ambient occlusion, diagonals included. */
        for (int a = 0; a < nx; a++)
            for (int b = 0; b < nz; b++)
                for (int k = 0; k < ny; k++) mark_section(w, xs[a], zs[b], ys[k], urgent);
    } else {
        /* Otherwise only faces shared with the six direct neighbours change. */
        mark_section(w, cx, cz, sy, urgent);
        if (nx > 1) mark_section(w, xs[1], cz, sy, urgent);
        if (nz > 1) mark_section(w, cx, zs[1], sy, urgent);
        if (ny > 1) mark_section(w, cx, cz, ys[1], urgent);
    }

    if (w->on_block_changed) w->on_block_changed(w->edit_user, x, y, z, old, id);
    return 1;
}

int world_set(world *w, int x, int y, int z, uint8_t id, uint8_t meta)
{
    return set_block(w, x, y, z, id, meta, 0);
}

int world_set_player(world *w, int x, int y, int z, uint8_t id, uint8_t meta)
{
    return set_block(w, x, y, z, id, meta, 1);
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
    uint8_t *saved; /* the column's saved bytes, fetched on the main thread */
    long saved_len; /* -1: never saved, -2: unusable row */
} gen_job;

static void gen_run(job *j)
{
    gen_job *g = (gen_job *)j;
    int loaded = 0;
    if (g->saved_len != -1) {
        loaded = g->saved_len >= 0 && save_decode_column(g->c, g->saved, (size_t)g->saved_len) == 0;
        if (!loaded) log_warn("column %d,%d: saved data corrupt, regenerating", g->c->cx, g->c->cz);
        mem_free(g->saved);
        g->saved = NULL;
    }
    if (!loaded) {
        if (g->w->generator) g->w->generator(g->w->seed, g->c);
        else worldgen_column(g->w->seed, g->c);
        mem_free(g->c->meta);
        g->c->meta = NULL;
    }
    g->c->modified = (uint8_t)loaded; /* keep saved columns saved */
    g->c->unsaved = 0;
    column_recount(g->c);
}

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
        w->gen_cursor = 0;
    } else {
        c->state = COL_READY;
        for (int s = 0; s < SECTIONS; s++) {
            c->dirty[s] = 1;
            c->version[s] = ++w->version_counter;
        }
        w->sched_pending = 1;
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
    g->saved_len = w->db ? save_db_get_column(w->db, cx, cz, &g->saved) : -1;
    w->gen_in_flight++;
    jobs_submit(w->jobs, &g->base);
}

static void store_column(const world *w, column *c)
{
    if (!c->unsaved || !w->db) return;
    if (save_db_put_column(w->db, c) != 0) log_error("failed to save column %d,%d", c->cx, c->cz);
    else
        c->unsaved = 0;
}

static void unload_column(world *w, column *c)
{
    if (c->state == COL_LOADING) {
        c->want_unload = 1;
        return;
    }
    if (w->on_column_unload) w->on_column_unload(w->edit_user, c);
    store_column(w, c);
    if (w->on_mesh_free)
        for (int s = 0; s < SECTIONS; s++)
            if (c->mesh[s].vtx_capacity) w->on_mesh_free(w->render_user, &c->mesh[s]);
    w->grid[slot_of(w, c->cx, c->cz)] = NULL;
    column_free(c);
}

/* -------------------------------------------------------------- mesh jobs */

typedef struct mesh_job {
    job base;
    world *w;
    int cx, cz, sy;
    uint32_t version;
    uint32_t *verts;
    mesh_counts mc;
    struct mesh_job *retry_next;
    mesh_input in;
} mesh_job;

static void mesh_run(job *j)
{
    mesh_job *m = (mesh_job *)j;
    uint32_t *scratch = jobs_scratch(sizeof(uint32_t) * 4 * MESH_MAX_QUADS);
    uint32_t quads = mesh_section_counts(&m->in, scratch, &m->mc);
    if (quads) {
        m->verts = mem_alloc(sizeof(uint32_t) * 4 * quads);
        memcpy(m->verts, scratch, sizeof(uint32_t) * 4 * quads);
    }
}

static void mesh_free(mesh_job *m)
{
    mem_free(m->verts);
    mem_free(m);
}

/* Hands a finished mesh to the renderer. Returns 0 if the renderer had no
 * room this frame; the job then waits on the retry list. */
static int deliver(world *w, mesh_job *m)
{
    column *c = world_column(w, m->cx, m->cz);
    if (!c || !c->meshing[m->sy] || c->pending_version[m->sy] != m->version) {
        /* Stale: the column went away or the section was meshed again. */
        if (c && c->pending_version[m->sy] == m->version) c->meshing[m->sy] = 0;
        return 1;
    }
    if (w->on_mesh_ready && !w->on_mesh_ready(w->render_user, c, m->sy, m->verts, &m->mc)) return 0;
    c->meshing[m->sy] = 0;
    return 1;
}

static void mesh_done(job *j)
{
    mesh_job *m = (mesh_job *)j;
    world *w = m->w;
    w->mesh_in_flight--;
    w->sched_pending = 1;
    if (deliver(w, m)) {
        mesh_free(m);
    } else {
        m->retry_next = w->retry;
        w->retry = m;
    }
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
            const column *cw = cols[zi][0], *cc = cols[zi][1], *ce = cols[zi][2];
            if (cw && cc && ce) {
                /* The usual case: one fixed 16-byte row plus two border
                 * bytes, which the compiler turns into plain moves. */
                size_t row = (size_t)col_index(0, y, lz);
                dst[0] = cw->blocks[row + CHUNK_W - 1];
                memcpy(dst + 1, &cc->blocks[row], CHUNK_W);
                dst[CHUNK_W + 1] = ce->blocks[row];
                if (cw->meta || cc->meta || ce->meta) {
                    dstm[0] = cw->meta ? cw->meta[row + CHUNK_W - 1] : 0;
                    if (cc->meta) memcpy(dstm + 1, &cc->meta[row], CHUNK_W);
                    else memset(dstm + 1, 0, CHUNK_W);
                    dstm[CHUNK_W + 1] = ce->meta ? ce->meta[row] : 0;
                } else {
                    memset(dstm, 0, MESH_PAD);
                }
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

/* A face is only emitted where a block meets a non-opaque neighbour, so a
 * section that is opaque throughout and whose six bordering planes are
 * opaque too produces nothing. Deep underground that is most sections. */
static int plane_y_opaque(const column *c, int y)
{
    if (y < 0) return 1; /* gather pads below the world with bedrock */
    if (y >= WORLD_H) return 0;
    const uint8_t *b = &c->blocks[y * COL_AREA];
    for (int i = 0; i < COL_AREA; i++)
        if (!block_opaque(b[i])) return 0;
    return 1;
}

static int plane_x_opaque(const column *c, int lx, int y0)
{
    if (!c) return 1; /* gather pads missing columns with stone */
    for (int y = y0; y < y0 + SECTION_H; y++)
        for (int z = 0; z < CHUNK_W; z++)
            if (!block_opaque(c->blocks[col_index(lx, y, z)])) return 0;
    return 1;
}

static int plane_z_opaque(const column *c, int lz, int y0)
{
    if (!c) return 1;
    for (int y = y0; y < y0 + SECTION_H; y++) {
        const uint8_t *row = &c->blocks[col_index(0, y, lz)];
        for (int x = 0; x < CHUNK_W; x++)
            if (!block_opaque(row[x])) return 0;
    }
    return 1;
}

static int section_enclosed(const world *w, const column *c, int sy)
{
    int y0 = sy * SECTION_H;
    return c->opaque_count[sy] == SECTION_H * COL_AREA && plane_y_opaque(c, y0 - 1) &&
           plane_y_opaque(c, y0 + SECTION_H) && plane_x_opaque(world_column(w, c->cx - 1, c->cz), CHUNK_W - 1, y0) &&
           plane_x_opaque(world_column(w, c->cx + 1, c->cz), 0, y0) &&
           plane_z_opaque(world_column(w, c->cx, c->cz - 1), CHUNK_W - 1, y0) &&
           plane_z_opaque(world_column(w, c->cx, c->cz + 1), 0, y0);
}

/* Starts a mesh job for one section if it needs one. Returns 1 if a job
 * (and so a gather on this thread) was started. */
static int schedule_section(world *w, column *c, int sy, int urgent)
{
    if (!c->dirty[sy] || c->meshing[sy]) return 0;
    c->dirty[sy] = 0;
    if (c->solid_count[sy] == 0 || section_enclosed(w, c, sy)) {
        if (c->mesh[sy].vtx_capacity && w->on_mesh_free) w->on_mesh_free(w->render_user, &c->mesh[sy]);
        memset(&c->mesh[sy], 0, sizeof c->mesh[sy]);
        return 0;
    }
    mesh_job *m = mem_alloc(sizeof *m);
    memset(m, 0, offsetof(mesh_job, in));
    m->base.run = mesh_run;
    m->base.done = mesh_done;
    m->w = w;
    m->cx = c->cx;
    m->cz = c->cz;
    m->sy = sy;
    m->version = c->version[sy];
    gather(w, c->cx, c->cz, sy, &m->in);
    c->meshing[sy] = 1;
    c->pending_version[sy] = m->version;
    w->mesh_in_flight++;
    if (urgent) jobs_submit_front(w->jobs, &m->base);
    else jobs_submit(w->jobs, &m->base);
    return 1;
}

#define GATHER_BUDGET_S 0.0015 /* main-thread time per frame spent copying sections */

/* backlog: the renderer is still refusing finished meshes, so only the
 * player's own edits are started until it catches up. */
static void schedule_meshes(world *w, int backlog)
{
    /* The player's own edits jump the queue. */
    for (int i = 0; i < w->urgent_count; i++) {
        column *c = world_column(w, w->urgent[i][0], w->urgent[i][1]);
        if (c && neighbours_ready(w, c->cx, c->cz)) schedule_section(w, c, w->urgent[i][2], 1);
    }
    w->urgent_count = 0;
    if (!w->sched_pending || backlog) return;

    double t0 = os_now();
    int started = 0, capped = 0;
    for (int i = 0; i < w->spiral_count && !capped; i++) {
        int dx = w->spiral[i][0], dz = w->spiral[i][1];
        if (dx * dx + dz * dz > w->radius * w->radius) break; /* spiral is sorted by distance */
        column *c = world_column(w, w->center_cx + dx, w->center_cz + dz);
        if (!c || !neighbours_ready(w, c->cx, c->cz)) continue;
        /* Nearest the camera's height first: ey, ey+1, ey-1, ey+2, ... */
        for (int k = 0; k < 2 * SECTIONS; k++) {
            int sy = w->eye_sy + ((k & 1) ? (k + 1) / 2 : -(k / 2));
            if (sy < 0 || sy >= SECTIONS) continue;
            if (w->mesh_in_flight >= w->max_mesh_in_flight) { capped = 1; break; }
            started += schedule_section(w, c, sy, 0);
            if ((started & 7) == 7 && os_now() - t0 > GATHER_BUDGET_S) {
                capped = 1;
                break;
            }
        }
    }
    if (!capped) w->sched_pending = 0; /* full pass found nothing left to start */
}

void world_schedule(world *w)
{
    mesh_job **pp = &w->retry;
    while (*pp) {
        mesh_job *m = *pp;
        if (deliver(w, m)) {
            *pp = m->retry_next;
            mesh_free(m);
        } else {
            pp = &m->retry_next;
        }
    }
    schedule_meshes(w, w->retry != NULL);
}

/* -------------------------------------------------------------- streaming */

static int cmp_dist(const void *a, const void *b)
{
    const int *p = a, *q = b;
    int da = p[0] * p[0] + p[1] * p[1], db = q[0] * q[0] + q[1] * q[1];
    return (da > db) - (da < db);
}

void world_init(world *w, uint32_t seed, int radius, jobs *js, struct save_db *db)
{
    memset(w, 0, sizeof *w);
    w->seed = seed;
    w->radius = clampi(radius, 2, 32);
    /* Kept columns reach radius + 2 each way; a power-of-two ring makes the
     * slot lookup a mask. */
    w->grid_w = 1;
    while (w->grid_w < 2 * (w->radius + 2) + 1) w->grid_w <<= 1;
    w->grid_mask = w->grid_w - 1;
    w->grid = mem_calloc((size_t)w->grid_w * (size_t)w->grid_w, sizeof(column *));
    w->jobs = js;
    int workers = jobs_worker_count(js);
    w->max_gen_in_flight = workers ? 8 * workers : 1;
    w->max_mesh_in_flight = workers ? 16 * workers : 1;
    w->eye_sy = SEA_LEVEL / SECTION_H;
    w->db = db;

    int r = w->radius + 1;
    w->spiral = mem_alloc(mem_array_size((size_t)(2 * r + 1) * (size_t)(2 * r + 1), sizeof *w->spiral));
    w->spiral_count = 0;
    for (int dz = -r; dz <= r; dz++)
        for (int dx = -r; dx <= r; dx++)
            if (in_ring(dx, dz, w->radius, 0)) {
                w->spiral[w->spiral_count][0] = dx;
                w->spiral[w->spiral_count][1] = dz;
                w->spiral_count++;
            }
    qsort(w->spiral, (size_t)w->spiral_count, sizeof *w->spiral, cmp_dist);
}

/* Moves the streaming centre and unloads columns that fell out of range. */
static void set_center(world *w, int pcx, int pcz)
{
    if (w->have_center && pcx == w->center_cx && pcz == w->center_cz) return;
    w->center_cx = pcx;
    w->center_cz = pcz;
    w->have_center = 1;
    w->gen_cursor = 0;
    w->sched_pending = 1;
    for (int i = 0; i < w->grid_w * w->grid_w; i++) {
        column *c = w->grid[i];
        if (c && !in_ring(c->cx - pcx, c->cz - pcz, w->radius, 1)) unload_column(w, c);
    }
}

void world_update(world *w, double px, double py, double pz)
{
    int pcx = chunk_of((int)floor(px)), pcz = chunk_of((int)floor(pz));
    w->eye_sy = clampi((int)floor(py) / SECTION_H, 0, SECTIONS - 1);

    set_center(w, pcx, pcz);

    for (int i = w->gen_cursor; i < w->spiral_count && w->gen_in_flight < w->max_gen_in_flight; i++) {
        int cx = pcx + w->spiral[i][0], cz = pcz + w->spiral[i][1];
        if (i == w->gen_cursor) w->gen_cursor++; /* everything before i is loaded or requested */
        if (!in_limits(cx, cz)) continue;
        column *c = w->grid[slot_of(w, cx, cz)];
        if (c) {
            /* Came back into range before its load finished: keep it. */
            if (c->cx == cx && c->cz == cz) c->want_unload = 0;
            else w->gen_cursor = i; /* slot still held by an old column: revisit */
            continue;
        }
        request_column(w, cx, cz);
    }

    schedule_meshes(w, w->retry != NULL);
}

void world_load_blocking(world *w, double px, double pz, int radius)
{
    int pcx = chunk_of((int)floor(px)), pcz = chunk_of((int)floor(pz));
    radius = clampi(radius, 0, w->radius + 1);
    set_center(w, pcx, pcz);
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
            if (c) c->want_unload = 0; /* a pending unload would drop it */
            else request_column(w, cx, cz);
        }
    jobs_wait_idle(w->jobs);
    jobs_poll(w->jobs, -1);
    w->gen_cursor = 0;
}

void world_save_all(world *w)
{
    if (!w->db) return;
    /* One transaction for the batch. A column counts as saved only once
     * the commit went through; otherwise the next save tries it again. */
    int batch = save_db_begin(w->db) == 0;
    for (int i = 0; i < w->grid_w * w->grid_w; i++) {
        column *c = w->grid[i];
        if (!c || c->state != COL_READY || !c->unsaved) continue;
        if (save_db_put_column(w->db, c) == 0) c->unsaved = 2;
        else log_error("failed to save column %d,%d", c->cx, c->cz);
    }
    int ok = !batch || save_db_commit(w->db) == 0;
    for (int i = 0; i < w->grid_w * w->grid_w; i++) {
        column *c = w->grid[i];
        if (c && c->unsaved == 2) c->unsaved = ok ? 0 : 1;
    }
}

void world_destroy(world *w)
{
    jobs_wait_idle(w->jobs);
    jobs_poll(w->jobs, -1);
    while (w->retry) {
        mesh_job *m = w->retry;
        w->retry = m->retry_next;
        mesh_free(m);
    }
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
