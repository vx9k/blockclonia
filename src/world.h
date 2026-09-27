/* World storage: a ring grid of 16 x 128 x 16 block columns streamed around
 * the player. Generation and meshing run on worker threads; everything else
 * (edits, physics, streaming decisions) runs on the main thread. */
#ifndef MC_WORLD_H
#define MC_WORLD_H

#include <stdint.h>
#include "block.h"
#include "mesher.h"

#define CHUNK_W 16
#define CHUNK_SHIFT 4
#define WORLD_H 128
#define SECTION_H 16
#define SECTIONS (WORLD_H / SECTION_H)
#define COL_AREA (CHUNK_W * CHUNK_W)
#define COL_VOL (COL_AREA * WORLD_H)
#define SEA_LEVEL 48

/* Horizontal limit on block coordinates. Keeps int arithmetic far from
 * overflow and physics in double precision well inside sub-mm accuracy. */
#define WORLD_LIMIT 8000000

_Static_assert((-1 >> 1) == -1, "arithmetic right shift required for chunk coordinates");

static inline int chunk_of(int x) { return x >> CHUNK_SHIFT; }
static inline int col_index(int x, int y, int z) { return (y * CHUNK_W + z) * CHUNK_W + x; }

/* Water level 1..8 (8 = full block). Stored in meta, with full water stored
 * as 0 so oceans need no meta array at all. */
#define WATER_FULL 8

typedef struct {
    uint32_t vtx_offset;   /* first vertex in its vertex pool block */
    uint32_t vtx_capacity; /* vertices reserved (0 = no allocation) */
    uint32_t opaque_quads;
    uint32_t trans_quads;
    uint16_t face_end[6];  /* opaque quads grouped by face, see mesh_counts */
    uint8_t block;         /* which vertex pool block holds it */
} section_mesh;

typedef enum { COL_LOADING, COL_READY } col_state;

typedef struct column {
    int cx, cz;
    col_state state;
    uint8_t want_unload;   /* unload once the in-flight load job finishes */
    uint8_t modified;      /* differs from generated terrain: keep a save file */
    uint8_t unsaved;       /* edited since it was last written to disk */
    uint8_t *blocks;       /* COL_VOL block ids */
    uint8_t *meta;         /* COL_VOL fluid levels, NULL when all zero */
    uint8_t dirty[SECTIONS];        /* needs re-meshing */
    uint8_t meshing[SECTIONS];      /* a mesh job is in flight */
    uint32_t version[SECTIONS];     /* bumped on every edit */
    uint32_t pending_version[SECTIONS];
    uint16_t solid_count[SECTIONS]; /* non-air blocks, 0 = skip meshing */
    uint16_t opaque_count[SECTIONS];/* opaque blocks, for the enclosed test */
    section_mesh mesh[SECTIONS];
} column;

struct jobs;

#define WORLD_URGENT 64

struct mesh_job;

typedef struct world {
    uint32_t seed;
    int radius;          /* render radius in columns */
    int grid_w;          /* ring grid side, a power of two covering the load ring */
    int grid_mask;
    column **grid;
    int center_cx, center_cz;
    int have_center;
    uint32_t version_counter;
    char save_dir[256];  /* empty: saving disabled */
    struct jobs *jobs;
    int gen_in_flight, mesh_in_flight;
    int max_gen_in_flight, max_mesh_in_flight;
    int (*spiral)[2];    /* column offsets sorted by distance (load ring) */
    int spiral_count;
    int gen_cursor;      /* spiral entries before this are loaded or loading */
    int sched_pending;   /* something may be ready to mesh */
    int eye_sy;          /* section the camera is in, meshed first */
    int urgent[WORLD_URGENT][3]; /* sections edited by the player: cx, cz, sy */
    int urgent_count;
    struct mesh_job *retry;      /* finished meshes the renderer had no room for */
    /* Renderer hooks, called on the main thread. on_mesh_ready returns 0
     * if it could not take the mesh this frame (the section is re-meshed
     * later). */
    void *render_user;
    int (*on_mesh_ready)(void *user, column *c, int sy, const uint32_t *verts, const mesh_counts *mc);
    void (*on_mesh_free)(void *user, section_mesh *m);
    /* Called whenever a block changes, for physics wake-ups. */
    void *edit_user;
    void (*on_block_changed)(void *user, int x, int y, int z, uint8_t old_id, uint8_t new_id);
    /* Called with edit_user just before a loaded column is saved and freed,
     * so state living outside it (falling bodies) can be written back. */
    void (*on_column_unload)(void *user, const column *c);
    /* Test hook: replaces terrain generation when set. */
    void (*generator)(uint32_t seed, column *c);
} world;

/* An empty world: columns stream in from world_update. radius is clamped
 * to 2..32 columns; js runs generation and meshing; save_dir NULL means
 * nothing is loaded or saved. */
void world_init(world *w, uint32_t seed, int radius, struct jobs *js, const char *save_dir);
/* Waits for outstanding jobs and frees everything. Does not save: call
 * world_save_all first. */
void world_destroy(world *w);

/* Inline: every physics query goes through these. */
static inline column *world_column(const world *w, int cx, int cz)
{
    column *c = w->grid[(cz & w->grid_mask) * w->grid_w + (cx & w->grid_mask)];
    return (c && c->cx == cx && c->cz == cz && c->state == COL_READY) ? c : 0;
}

static inline uint8_t world_get(const world *w, int x, int y, int z)
{
    if (y < 0) return B_BEDROCK;
    if (y >= WORLD_H) return B_AIR;
    const column *c = world_column(w, chunk_of(x), chunk_of(z));
    if (!c) return B_UNLOADED;
    return c->blocks[col_index(x & (CHUNK_W - 1), y, z & (CHUNK_W - 1))];
}

/* The 0..7 meta value of a cell (see block_has_meta); 0 when unloaded. */
uint8_t world_get_meta(const world *w, int x, int y, int z);
/* Water in a cell, 0 (none) .. WATER_FULL. */
int world_water_level(const world *w, int x, int y, int z);

/* Raw edit: marks meshes dirty and the column modified, then fires
 * on_block_changed. Returns 0 if the column is not loaded. */
int world_set(world *w, int x, int y, int z, uint8_t id, uint8_t meta);
/* Same, for the player's own edits: their sections are re-meshed first. */
int world_set_player(world *w, int x, int y, int z, uint8_t id, uint8_t meta);

/* Streams columns around the camera position and schedules jobs. Call
 * once per frame. */
void world_update(world *w, double px, double py, double pz);

/* Schedules meshing again and retries uploads the renderer deferred. Call
 * after jobs_poll() so finished work is refilled in the same frame. */
void world_schedule(world *w);

/* Blocking load of every column within `radius` of (px, pz), used by tests
 * and at spawn so the player never falls through ungenerated terrain. Also
 * moves the streaming centre there, so it works for teleports too. */
void world_load_blocking(world *w, double px, double pz, int radius);

/* Surface height (first air above ground) at x,z, or -1 if not loaded. */
int world_surface_y(const world *w, int x, int z);

/* Writes every column edited since its last save. Cheap when nothing
 * changed, so it doubles as the autosave. */
void world_save_all(world *w);

/* Worker-side entry point for generating one column (also used by bench). */
void worldgen_column(uint32_t seed, column *c);
/* Terrain surface height before trees/water; cheap, needs nothing loaded. */
int worldgen_height(uint32_t seed, int x, int z);

/* Column lifetime. alloc returns a COL_LOADING column whose blocks are
 * not initialised yet; recount refreshes the per-section block counts that
 * let meshing skip empty and fully buried sections. */
column *column_alloc(int cx, int cz);
void column_free(column *c);
void column_recount(column *c);

#endif
