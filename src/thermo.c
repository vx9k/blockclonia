#include "thermo.h"
#include "health.h"
#include "mem.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- the sun and the air
 *
 * An equinox sun at latitude 40 degrees: sin(elevation) = cos(lat) cos(hour
 * angle), so it rises at 06:00, peaks at 50 degrees at noon and sets at
 * 18:00. Light fades in from civil twilight (sun 6 degrees below the
 * horizon) and is full once the sun is 20 degrees up.
 *
 * Air temperature: 14 C at sea level (the ISA's 15 C, a little cooler for a
 * temperate climate). The real lapse rate is 6.5 K/km, but the terrain is
 * squeezed into 128 m of height, so it is scaled to put 0 C at the snow line
 * (y = 92, 44 m above the sea): 14/44 = 0.32 K/m, about 49 times the real
 * rate. Below sea level only pits and caves have air, so the lapse is not
 * extrapolated down. The day swings +/-5 C around the mean, peaking at 15:00:
 * the ground stores the morning sun and the air lags it by about 3 hours.
 * Under a roof or in a cave (an opaque block within 8 m above) the air
 * follows the ground instead, whose mean is ~10 C in a temperate climate,
 * and the diurnal swing mostly disappears. */
#define SUN_LAT_COS 0.766           /* cos(40 degrees) */
#define TWILIGHT_SIN (-0.105)       /* sin(-6 degrees): civil twilight */
#define FULL_DAY_SIN 0.35           /* sin(20 degrees) */
#define NIGHT_LIGHT 0.18
#define T_SEA 14.0                  /* C */
#define SNOW_LINE 92
#define LAPSE (T_SEA / (double)(SNOW_LINE - SEA_LEVEL)) /* K/m, scaled */
#define DIURNAL_K 5.0               /* K amplitude */
#define PEAK_DAY (15.0 / 24.0)      /* warmest at 15:00 */
#define GROUND_MEAN 10.0            /* C */
#define SHELTER_REACH 8             /* m of roof search */
#define SHELTER_PULL 0.6            /* fraction pulled toward the ground mean */
#define SHELTER_SWING 0.2           /* fraction of the diurnal swing left inside */

/* ---- fire
 *
 * A campfire releases ~12 kW (about 3 kg of wood an hour at 15 MJ/kg). Its
 * flames (~1100 K, 827 C) are optically thin: a sooty flame half a metre
 * thick has an emissivity of ~0.1 (absorption coefficient ~0.2 /m). With
 * an effective radiating area of 0.5 m^2 it radiates
 *   P = eps sigma (T_f^4 - T_s^4) A_f = 0.09 * 5.67e-8 * (1100^4 - 306^4) * 0.5
 *     = 3.7 kW,
 * 31% of the heat release, the usual radiant fraction of wood fires. The
 * rest (8.3 kW) is convected into the air of its cell. Skin 1 m from the
 * flame centre receives P / (4 pi d^2) = 296 W/m^2 (a warm fireside), at 2 m
 * 74 W/m^2; closer than 0.3 m the distance is clamped (3.3 kW/m^2, burns in
 * seconds). A block touching the fire's cell sees one face of the cube
 * around the flame, a sixth of its radiation, and absorbs 90% of it (most
 * surfaces are near black in the infrared). */
#define FIRE_W 12000.0              /* W heat release */
#define FLAME_K 1100.0              /* K */
#define FLAME_EMISSIVITY 0.09
#define FLAME_AREA 0.5              /* m^2 */
#define SKIN_K 306.0                /* K, skin surface */
#define SIGMA 5.670374e-8           /* W/(m^2 K^4), Stefan-Boltzmann */
#define FIRE_REACH 12.0             /* m */
#define FIRE_MIN_D 0.3              /* m */
#define ABSORPTIVITY 0.9
#define FUEL_UNIT_S 3600.0          /* survival-clock s per fuel unit */
#define FIRE_PIN_R 3                /* field cells kept around a burning fire */
#define MAX_FIRES 256

/* ---- the heat field
 *
 * Each cell is a 1 m cube with one temperature, stored as its deviation
 * from the ambient air at that point: the background (lapse, day, shelter)
 * is taken as already in equilibrium with its own forcing, so only
 * disturbances (fires, melting, freezing) are simulated and a cell that
 * returns to ambient can be forgotten.
 *
 * Conductance across a shared 1 m^2 face, W/K:
 * - solid to solid: two half-cells of conductivity k in series,
 *   1 / (0.5/k1 + 0.5/k2) = 2 k1 k2 / (k1 + k2);
 * - solid to air: the solid's half cell (2k) in series with a convective
 *   film, h = 10 W/(m^2 K) for still air;
 * - liquid water mixes by natural convection, so its own side adds no
 *   resistance: h to air, 2k to a solid, and a mixing term to more water;
 * - air to air sideways: eddy mixing with an exchange velocity of ~2.5 mm/s,
 *   rho cp u A = 1231 * 0.0025 = 3 W/K. Upward from a warmer cell below,
 *   the buoyant updraught carries far more: ~8 cm/s averaged over the face,
 *   100 W/K (Heskestad's plume moves 0.1-0.2 kg/s 1 m above a 10 kW fire).
 *   The plume this makes, as cell averages: ~120 K above ambient in the
 *   fire's cell, ~75 K 1 m above and ~45 K 2 m above, ~1 K 2 m to the side
 *   (Heskestad's centreline, hotter than a cell average, is ~100 and ~30 K
 *   1 and 2 m above a 12 kW fire);
 * - air cells also lose heat to the open atmosphere (wind and the plume
 *   dispersing): 15 W/K in the open, 4 W/K under a roof (a shelter with a
 *   door and a smoke hole changes its air ~12 times an hour).
 * Heat capacities are density * specific heat (air 1.225 * 1005 = 1231 J/K,
 * stone 2 MJ/K). A burning campfire's cell is treated as air: its flames
 * and hot gas, not the wood being consumed.
 *
 * Integration is explicit with sub-steps below half the smallest C/sum(G)
 * (~2.7 s for air in a plume), on the survival clock: a 60 Hz frame is 1.2 s
 * of thermal time, one sub-step. Work per call is capped at 64 sub-steps
 * and 2^18 cell updates (16 sub-steps of a full field); beyond that the field
 * falls behind the clock rather than going unstable. Housekeeping runs every
 * 10 s of thermal time (8 frames) or when the field or its blocks change:
 * cells deviating more than 1 K bring their six neighbours into the field,
 * cells within 0.2 K of ambient with no phase progress and no such
 * neighbour are dropped, and the links (neighbour indices, conductances,
 * shelter) are rebuilt. Between housekeepings a call costs one pass over
 * the cells per sub-step. */
#define RHO_CP_AIR (1.225 * 1005.0) /* J/(m^3 K) */
#define RHO_CP_WATER (1000.0 * 4186.0)
#define H_CONV 10.0                 /* W/(m^2 K) */
#define G_AIR 3.0                   /* W/K */
#define G_UP 100.0                  /* W/K */
#define G_WATER 20.0                /* W/K */
#define VENT_OPEN 15.0              /* W/K */
#define VENT_SHELTER 4.0            /* W/K */
#define SPREAD_K 1.0                /* K */
#define DROP_K 0.2                  /* K */
#define DEV_MAX 1500.0              /* K, a safety clamp; never reached */
#define FIELD_RANGE 64              /* m from the player; farther cells are forgotten */
#define MAX_SUBSTEPS 64
#define WORK_CAP (1 << 18)
#define HASH_SIZE (2 * THERMO_MAX_CELLS)
#define MAX_CONVERT 64              /* phase changes applied per call */
#define HOUSEKEEP_S 10.0            /* survival s between spreading, dropping and relinking */

/* ---- phase change
 *
 * Latent heat of fusion of water 334 kJ/kg. Ice and snow are held at 0 C
 * while they absorb it (enthalpy method): a 1 m^3 ice block (917 kg) needs
 * 306 MJ, snow (400 kg) 134 MJ. Melting conserves mass: 917 kg of water is
 * 0.917 m^3, level round(8 * 0.917) = 7; snow gives level 3.
 *
 * Freezing: the ice skin on still water thickens as sqrt(t) (Stefan's
 * problem), so a full metre takes a whole winter. A water cell becomes an
 * ice block once a 10 cm skin has formed (the usual thickness that holds a
 * person): 0.1 m * 1000 kg/m^3 * 334 kJ/kg = 33 MJ. The new ice cell keeps
 * the rest of the latent heat as phase progress (the water under the skin),
 * so it keeps thickening in the cold and melts back quickly if warmed. */
#define L_FUSION 334000.0           /* J/kg */
#define FREEZE_SKIN 0.1             /* m */

/* ---- scanning near the player */
#define SCAN_R 3                    /* columns searched for campfires from saves */
#define SCAN_COLS 2                 /* columns per call, round robin */
#define POND_R 4                    /* m around the player where still water may freeze */
#define POND_COLS 9                 /* (x, z) columns per call */

enum { K_GAS, K_SOLID, K_MELT, K_WATER };

typedef struct {
    int x, y, z;
    double dev;        /* K above the ambient air at the cell centre */
    double lat;        /* J of phase progress: melted (ice, snow) or frozen (water) */
    uint32_t keep;     /* stamp of the last housekeeping that needed it */
    /* Rebuilt at housekeeping (amb every call). */
    double amb, cap, src, vent;
    double g[6];
    int32_t nb[6];     /* neighbour cell, -1 for the ambient */
    uint8_t mat, kind, freezable, shelter;
    uint8_t dyn;       /* bit f set for an air-air vertical face */
} cell;

typedef struct {
    int x, y, z;
    double burn;       /* survival-clock s of the current fuel unit burnt */
} fire;

typedef struct {
    int x, y, z;
    uint8_t id, meta;
} block_edit;

struct thermo {
    uint32_t seed;     /* nothing is random yet; kept for weather */
    double day;
    uint32_t stamp;
    cell *cells;
    int count;
    int linked;        /* cells [0, linked) have current links */
    int dirty;         /* the field or the blocks in it changed: relink */
    double since_house;/* survival s since the last housekeeping */
    double hmax;       /* stable sub-step of the current links */
    int32_t *table;
    double *q;
    fire fires[MAX_FIRES];
    int fire_count;
    block_edit edits[MAX_FIRES];
    int scan_cursor, pond_cursor;
};

static const int FACE[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};

/* ---- helpers */

static double smooth(double v, double a, double b)
{
    double t = clampd((v - a) / (b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

/* Block coordinate of a position, safe for any double. */
static int coord(double v, int lo, int hi)
{
    if (!(v == v)) return 0;
    return (int)floor(clampd(v, (double)lo, (double)hi));
}

static int kind_of(uint8_t id)
{
    if (id == B_AIR || id == B_CAMPFIRE) return K_GAS;
    if (id == B_WATER) return K_WATER;
    if (id == B_ICE || id == B_SNOW) return K_MELT;
    return K_SOLID;
}

static double conductivity(uint8_t id) { return (double)block_get(id)->conductivity; }

/* Latent heat to melt a whole block. */
static double melt_energy(uint8_t id) { return (double)block_get(id)->density * L_FUSION; }

static double radiated_w(void)
{
    double tf2 = FLAME_K * FLAME_K, ts2 = SKIN_K * SKIN_K;
    return FLAME_EMISSIVITY * SIGMA * (tf2 * tf2 - ts2 * ts2) * FLAME_AREA;
}

/* ---- the sun */

double thermo_day_time(const thermo *t) { return t->day; }

void thermo_set_day_time(thermo *t, double day)
{
    if (!isfinite(day)) day = THERMO_DAY_START;
    t->day = day - floor(day);
}

static double sun_sin(const thermo *t) { return SUN_LAT_COS * cos(2.0 * MC_PI * (t->day - 0.5)); }

static double daylight_frac(const thermo *t) { return smooth(sun_sin(t), TWILIGHT_SIN, FULL_DAY_SIN); }

float thermo_daylight(const thermo *t)
{
    return (float)(NIGHT_LIGHT + (1.0 - NIGHT_LIGHT) * daylight_frac(t));
}

/* Night blue to the renderer's day sky, with the orange of a low sun (long
 * paths through the air scatter the blue away) around sunrise and sunset. */
void thermo_sky(const thermo *t, float rgb[3])
{
    static const double night[3] = {0.02, 0.03, 0.08}, day[3] = {0.53, 0.74, 1.0}, dusk[3] = {1.0, 0.50, 0.22};
    double f = daylight_frac(t), s = sun_sin(t);
    double tint = 0.6 * exp(-((s - 0.03) / 0.09) * ((s - 0.03) / 0.09));
    for (int i = 0; i < 3; i++) {
        double base = night[i] + (day[i] - night[i]) * f;
        rgb[i] = (float)(base * (1.0 - tint) + dusk[i] * tint);
    }
}

/* ---- ambient air */

static double diurnal(const thermo *t) { return DIURNAL_K * cos(2.0 * MC_PI * (t->day - PEAK_DAY)); }

static int sheltered(const world *w, int x, int y, int z)
{
    for (int k = 1; k <= SHELTER_REACH; k++)
        if (block_opaque(world_get(w, x, y + k, z))) return 1;
    return 0;
}

/* swing: the diurnal term, computed once per call. */
static double ambient(double y, int shelter, double swing)
{
    double open = T_SEA - LAPSE * fmax(y - (double)SEA_LEVEL, 0.0);
    if (!shelter) return open + swing;
    return GROUND_MEAN + (1.0 - SHELTER_PULL) * (open - GROUND_MEAN) + SHELTER_SWING * swing;
}

static double cell_ambient(const thermo *t, const world *w, int x, int y, int z, int *shelter)
{
    int s = sheltered(w, x, y, z);
    if (shelter) *shelter = s;
    return ambient((double)y + 0.5, s, diurnal(t));
}

float thermo_ambient(const thermo *t, const world *w, double x, double y, double z)
{
    double yy = (y == y) ? clampd(y, -1.0, (double)WORLD_H) : (double)SEA_LEVEL;
    int s = sheltered(w, coord(x, -WORLD_LIMIT, WORLD_LIMIT), coord(yy, -1, WORLD_H), coord(z, -WORLD_LIMIT, WORLD_LIMIT));
    return (float)ambient(yy, s, diurnal(t));
}

/* ---- the cell table: open addressing, linear probing, backward-shift
 * deletion; cells live in a dense array so iteration is O(count). */

static uint32_t hash3(int x, int y, int z)
{
    uint32_t h = (uint32_t)x * 0x9E3779B1u ^ (uint32_t)y * 0x85EBCA77u ^ (uint32_t)z * 0xC2B2AE3Du;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    return h & (HASH_SIZE - 1);
}

static int find(const thermo *t, int x, int y, int z)
{
    for (uint32_t s = hash3(x, y, z);; s = (s + 1) & (HASH_SIZE - 1)) {
        int32_t i = t->table[s];
        if (i < 0) return -1;
        const cell *c = &t->cells[i];
        if (c->x == x && c->y == y && c->z == z) return i;
    }
}

static uint32_t slot_of(const thermo *t, int i)
{
    const cell *c = &t->cells[i];
    uint32_t s = hash3(c->x, c->y, c->z);
    while (t->table[s] != i) s = (s + 1) & (HASH_SIZE - 1);
    return s;
}

/* Starting temperature of a block joining the field: at ambient, except
 * that ice and snow cannot be above 0 C nor liquid water below it. */
static double initial_dev(uint8_t id, double amb)
{
    int k = kind_of(id);
    if (k == K_MELT) return fmin(0.0, -amb);
    if (k == K_WATER) return fmax(0.0, -amb);
    return 0.0;
}

/* Index of the cell at a block, adding it if needed; -1 if it cannot be
 * in the field (outside the world, unloaded, or the table is full). */
static int ensure(thermo *t, const world *w, int x, int y, int z)
{
    if (y < 0 || y >= WORLD_H) return -1;
    int i = find(t, x, y, z);
    if (i >= 0) return i;
    if (t->count >= THERMO_MAX_CELLS) return -1;
    uint8_t id = world_get(w, x, y, z);
    if (id == B_UNLOADED) return -1;
    uint32_t s = hash3(x, y, z);
    while (t->table[s] >= 0) s = (s + 1) & (HASH_SIZE - 1);
    i = t->count++;
    t->table[s] = i;
    cell *c = &t->cells[i];
    memset(c, 0, sizeof *c);
    c->x = x;
    c->y = y;
    c->z = z;
    c->mat = id;
    c->kind = (uint8_t)kind_of(id);
    int shelter;
    c->amb = cell_ambient(t, w, x, y, z, &shelter);
    c->shelter = (uint8_t)shelter;
    c->dev = initial_dev(id, c->amb);
    c->cap = RHO_CP_AIR;
    for (int f = 0; f < 6; f++) c->nb[f] = -1;
    t->dirty = 1;
    return i;
}

static void remove_cell(thermo *t, int i)
{
    uint32_t s = slot_of(t, i), j = s;
    t->table[s] = -1;
    for (;;) {
        j = (j + 1) & (HASH_SIZE - 1);
        int32_t k = t->table[j];
        if (k < 0) break;
        uint32_t home = hash3(t->cells[k].x, t->cells[k].y, t->cells[k].z);
        /* Move it back unless its home lies cyclically in (s, j]. */
        int stays = s <= j ? (home > s && home <= j) : (home > s || home <= j);
        if (!stays) {
            t->table[s] = k;
            t->table[j] = -1;
            s = j;
        }
    }
    int last = t->count - 1;
    if (i != last) {
        t->table[slot_of(t, last)] = i;
        t->cells[i] = t->cells[last];
    }
    t->count--;
    t->dirty = 1;
}

/* ---- fires */

static int fire_find(const thermo *t, int x, int y, int z)
{
    for (int i = 0; i < t->fire_count; i++)
        if (t->fires[i].x == x && t->fires[i].y == y && t->fires[i].z == z) return i;
    return -1;
}

static void fire_register(thermo *t, int x, int y, int z)
{
    if (fire_find(t, x, y, z) >= 0 || t->fire_count >= MAX_FIRES) return;
    fire *f = &t->fires[t->fire_count++];
    f->x = x;
    f->y = y;
    f->z = z;
    f->burn = 0.0;
    t->dirty = 1;
}

static void fire_forget(thermo *t, int x, int y, int z)
{
    int i = fire_find(t, x, y, z);
    if (i >= 0) {
        t->fires[i] = t->fires[--t->fire_count];
        t->dirty = 1;
    }
}

static int fuel_of(const world *w, int x, int y, int z)
{
    uint8_t m = world_get_meta(w, x, y, z);
    return m ? m : CAMPFIRE_FUEL_MAX;
}

static int doused(const world *w, int x, int y, int z)
{
    return world_get(w, x + 1, y, z) == B_WATER || world_get(w, x - 1, y, z) == B_WATER ||
           world_get(w, x, y, z + 1) == B_WATER || world_get(w, x, y, z - 1) == B_WATER ||
           world_get(w, x, y + 1, z) == B_WATER;
}

static double flame_dist(const fire *f, dvec3 p)
{
    return dv3_len(dv3(p.x - ((double)f->x + 0.5), p.y - ((double)f->y + 0.5), p.z - ((double)f->z + 0.5)));
}

/* Burns fuel, puts out doused fires and forgets ones that are gone. Block
 * edits are collected and applied after the loop, since they call back into
 * thermo_on_block_changed, which edits the registry. */
static void burn_fires(thermo *t, world *w, double dts)
{
    int edits = 0;
    for (int i = 0; i < t->fire_count;) {
        fire *f = &t->fires[i];
        if (world_get(w, f->x, f->y, f->z) != B_CAMPFIRE) {
            t->fires[i] = t->fires[--t->fire_count];
            t->dirty = 1;
            continue;
        }
        int fuel = fuel_of(w, f->x, f->y, f->z), was = fuel;
        if (doused(w, f->x, f->y, f->z)) {
            fuel = 0;
        } else {
            f->burn += dts;
            while (f->burn >= FUEL_UNIT_S && fuel > 0) {
                f->burn -= FUEL_UNIT_S;
                fuel--;
            }
        }
        if (fuel != was) {
            block_edit *e = &t->edits[edits++];
            e->x = f->x;
            e->y = f->y;
            e->z = f->z;
            e->id = fuel > 0 ? B_CAMPFIRE : B_ASH;
            e->meta = (uint8_t)(fuel >= CAMPFIRE_FUEL_MAX ? 0 : fuel);
        }
        i++;
    }
    for (int i = 0; i < edits; i++) {
        const block_edit *e = &t->edits[i];
        world_set(w, e->x, e->y, e->z, e->id, e->meta);
        if (e->id == B_ASH) fire_forget(t, e->x, e->y, e->z);
    }
}

/* Registers campfires in loaded columns near the player, a few columns per
 * call, so fires from saves are found without a full scan. */
static void scan_fires(thermo *t, const world *w, dvec3 player)
{
    int side = 2 * SCAN_R + 1;
    int pcx = chunk_of(coord(player.x, -WORLD_LIMIT, WORLD_LIMIT));
    int pcz = chunk_of(coord(player.z, -WORLD_LIMIT, WORLD_LIMIT));
    for (int n = 0; n < SCAN_COLS; n++) {
        int k = t->scan_cursor;
        t->scan_cursor = (k + 1) % (side * side);
        int cx = pcx + k % side - SCAN_R, cz = pcz + k / side - SCAN_R;
        const column *c = world_column(w, cx, cz);
        if (!c) continue;
        const uint8_t *b = c->blocks, *end = c->blocks + COL_VOL;
        while (b < end) {
            const uint8_t *hit = memchr(b, B_CAMPFIRE, (size_t)(end - b));
            if (!hit) break;
            int i = (int)(hit - c->blocks);
            fire_register(t, cx * CHUNK_W + (i & (CHUNK_W - 1)), i / COL_AREA, cz * CHUNK_W + ((i / CHUNK_W) & (CHUNK_W - 1)));
            b = hit + 1;
        }
    }
}

/* Still, full water under open air near the player joins the field when
 * the air is freezing, so ponds can ice over (the ocean never freezes: it
 * lies where the air is above 0 C). */
static void scan_ponds(thermo *t, const world *w, dvec3 player)
{
    int side = 2 * POND_R + 1;
    int px = coord(player.x, -WORLD_LIMIT, WORLD_LIMIT), pz = coord(player.z, -WORLD_LIMIT, WORLD_LIMIT);
    int py = coord(player.y, 0, WORLD_H - 1);
    /* Nothing can freeze if the open air at the top of the range, the
     * coldest there, is thawing: sheltered air is then above 3 C. */
    if (ambient((double)(py + POND_R) + 0.5, 0, diurnal(t)) >= 0.0) return;
    for (int n = 0; n < POND_COLS; n++) {
        int k = t->pond_cursor;
        t->pond_cursor = (k + 1) % (side * side);
        int x = px + k % side - POND_R, z = pz + k / side - POND_R;
        for (int y = py - POND_R; y <= py + POND_R; y++) {
            if (world_get(w, x, y, z) != B_WATER || world_get(w, x, y + 1, z) != B_AIR) continue;
            if (world_water_level(w, x, y, z) != WATER_FULL) continue;
            if (cell_ambient(t, w, x, y, z, NULL) < 0.0) ensure(t, w, x, y, z);
        }
    }
}

/* ---- field update */

static double face_g(uint8_t a, uint8_t b)
{
    int ka = kind_of(a), kb = kind_of(b);
    if (ka == K_GAS && kb == K_GAS) return G_AIR;
    if (ka == K_GAS || kb == K_GAS) {
        uint8_t s = ka == K_GAS ? b : a;
        if (kind_of(s) == K_WATER) return H_CONV;
        return 1.0 / (1.0 / H_CONV + 0.5 / conductivity(s));
    }
    if (ka == K_WATER && kb == K_WATER) return G_WATER;
    if (ka == K_WATER) return 2.0 * conductivity(b);
    if (kb == K_WATER) return 2.0 * conductivity(a);
    double k1 = conductivity(a), k2 = conductivity(b);
    return 2.0 * k1 * k2 / (k1 + k2);
}

/* Material, shelter and links of every cell, and the largest stable
 * sub-step. Run at housekeeping or after anything in the field changed. */
static void build_links(thermo *t, const world *w)
{
    double rad = radiated_w(), min_tau = 1e30;
    for (int i = 0; i < t->count; i++) {
        cell *c = &t->cells[i];
        uint8_t id = world_get(w, c->x, c->y, c->z);
        if (id != c->mat) {
            c->mat = id;
            c->kind = (uint8_t)kind_of(id);
            c->lat = 0.0;
        }
        c->shelter = (uint8_t)sheltered(w, c->x, c->y, c->z);
        c->vent = 0.0;
        c->freezable = 0;
        if (c->kind == K_GAS) {
            c->cap = RHO_CP_AIR;
            c->vent = c->shelter ? VENT_SHELTER : VENT_OPEN;
        } else if (c->kind == K_WATER) {
            double frac = (double)world_water_level(w, c->x, c->y, c->z) / WATER_FULL;
            c->cap = frac * RHO_CP_WATER + (1.0 - frac) * RHO_CP_AIR;
            c->freezable = frac >= 1.0 && world_get(w, c->x, c->y + 1, c->z) == B_AIR;
            if (!c->freezable) c->lat = 0.0;
        } else {
            c->cap = (double)block_get(id)->density * (double)block_get(id)->heat_capacity;
            if (c->kind == K_SOLID) c->lat = 0.0;
        }
        c->src = 0.0;
        c->dyn = 0;
        double sum = c->vent;
        for (int f = 0; f < 6; f++) {
            int nx = c->x + FACE[f][0], ny = c->y + FACE[f][1], nz = c->z + FACE[f][2];
            c->nb[f] = -1;
            c->g[f] = 0.0;
            if (ny < 0) continue; /* bedrock and below: insulated */
            uint8_t n = ny >= WORLD_H ? B_AIR : world_get(w, nx, ny, nz);
            if (n == B_UNLOADED) continue;
            c->nb[f] = ny >= WORLD_H ? -1 : find(t, nx, ny, nz);
            c->g[f] = face_g(id, n);
            if (FACE[f][1] != 0 && c->kind == K_GAS && kind_of(n) == K_GAS) {
                c->dyn = (uint8_t)(c->dyn | (1u << f));
                sum += G_UP;
            } else {
                sum += c->g[f];
            }
        }
        min_tau = fmin(min_tau, c->cap / sum);
    }
    /* Fires: convection into their own cell, radiation onto the blocks
     * around it. */
    for (int i = 0; i < t->fire_count; i++) {
        const fire *f = &t->fires[i];
        int ci = find(t, f->x, f->y, f->z);
        if (ci < 0) continue;
        t->cells[ci].src += FIRE_W - rad;
        for (int k = 0; k < 6; k++) {
            int ni = t->cells[ci].nb[k];
            if (ni >= 0 && t->cells[ni].kind != K_GAS) t->cells[ni].src += ABSORPTIVITY * rad / 6.0;
        }
    }
    t->linked = t->count;
    t->hmax = 0.5 * min_tau;
    t->dirty = 0;
}

/* Ambient at every linked cell for the current time of day. Phase cells
 * stay on their side of 0 C when the ambient moves under them. */
static void refresh_ambient(thermo *t)
{
    double swing = diurnal(t);
    for (int i = 0; i < t->linked; i++) {
        cell *c = &t->cells[i];
        c->amb = ambient((double)c->y + 0.5, c->shelter, swing);
        double temp = c->amb + c->dev;
        if (c->kind == K_WATER && (c->lat > 0.0 || temp < 0.0)) c->dev = -c->amb;
        if (c->kind == K_MELT && (c->lat > 0.0 || temp > 0.0)) c->dev = -c->amb;
    }
}

/* Conductance of face f of cell c towards a neighbour at deviation nd: an
 * air-air vertical face carries the updraught when the lower cell is warmer. */
static double link_g(const cell *c, int f, double nd)
{
    if (!(c->dyn & (1u << f))) return c->g[f];
    double lower = f == 2 ? c->dev : nd, upper = f == 2 ? nd : c->dev;
    return lower > upper ? G_UP : G_AIR;
}

static void substep(thermo *t, double h)
{
    cell *cs = t->cells;
    double *q = t->q;
    for (int i = 0; i < t->linked; i++) q[i] = cs[i].src - cs[i].vent * cs[i].dev;
    for (int i = 0; i < t->linked; i++) {
        const cell *c = &cs[i];
        for (int f = 0; f < 6; f++) {
            int j = c->nb[f];
            if (j >= 0) {
                if (f & 1) continue; /* the neighbour's positive face owns the pair */
                double flow = link_g(c, f, cs[j].dev) * (c->dev - cs[j].dev);
                q[i] -= flow;
                q[j] += flow;
            } else {
                q[i] -= link_g(c, f, 0.0) * c->dev;
            }
        }
    }
    /* Enthalpy update: relative to 0 C, phase cells hold at the melting
     * point while their latent heat is paid or released. */
    for (int i = 0; i < t->linked; i++) {
        cell *c = &cs[i];
        double e = q[i] * h;
        if (c->kind == K_MELT) {
            double hh = c->cap * (c->amb + c->dev) + c->lat + e;
            c->lat = fmax(hh, 0.0);
            c->dev = fmin(hh, 0.0) / c->cap - c->amb;
        } else if (c->kind == K_WATER) {
            double hh = c->cap * (c->amb + c->dev) - c->lat + e;
            c->lat = c->freezable ? fmax(-hh, 0.0) : 0.0;
            c->dev = fmax(hh, 0.0) / c->cap - c->amb;
        } else {
            c->dev += e / c->cap;
        }
        c->dev = clampd(c->dev, -DEV_MAX, DEV_MAX);
    }
}

/* Keeps the field where it matters: around fires and next to deviant cells;
 * forgets the rest. */
static void maintain(thermo *t, const world *w, dvec3 player, int have_player)
{
    for (int i = 0; i < t->fire_count; i++) {
        const fire *f = &t->fires[i];
        for (int dy = -FIRE_PIN_R; dy <= FIRE_PIN_R; dy++)
            for (int dz = -FIRE_PIN_R; dz <= FIRE_PIN_R; dz++)
                for (int dx = -FIRE_PIN_R; dx <= FIRE_PIN_R; dx++) {
                    if (dx * dx + dy * dy + dz * dz > FIRE_PIN_R * FIRE_PIN_R) continue;
                    int ci = ensure(t, w, f->x + dx, f->y + dy, f->z + dz);
                    if (ci >= 0) t->cells[ci].keep = t->stamp;
                }
    }
    int n0 = t->count;
    for (int i = 0; i < n0; i++) {
        if (fabs(t->cells[i].dev) <= SPREAD_K) continue;
        for (int f = 0; f < 6; f++) {
            const cell *c = &t->cells[i];
            int ci = ensure(t, w, c->x + FACE[f][0], c->y + FACE[f][1], c->z + FACE[f][2]);
            if (ci >= 0) t->cells[ci].keep = t->stamp;
        }
    }
    int px = coord(player.x, -WORLD_LIMIT, WORLD_LIMIT), pz = coord(player.z, -WORLD_LIMIT, WORLD_LIMIT);
    for (int i = t->count - 1; i >= 0; i--) {
        const cell *c = &t->cells[i];
        int gone = world_get(w, c->x, c->y, c->z) == B_UNLOADED;
        int kept = c->keep == t->stamp;
        int quiet = fabs(c->dev) < DROP_K && c->lat == 0.0;
        int far = have_player && (abs(c->x - px) > FIELD_RANGE || abs(c->z - pz) > FIELD_RANGE);
        if (gone || (!kept && (quiet || far))) remove_cell(t, i);
    }
}

/* Melting and freezing that completed this call. */
static void convert(thermo *t, world *w)
{
    int n = 0;
    for (int i = 0; i < t->linked && n < MAX_CONVERT; i++) {
        const cell *c = &t->cells[i];
        block_edit *e = &t->edits[n];
        if (c->kind == K_MELT && c->lat >= melt_energy(c->mat)) {
            double rho = (double)block_get(c->mat)->density;
            int level = (int)clampd(floor(WATER_FULL * rho / 1000.0 + 0.5), 1.0, WATER_FULL);
            e->id = B_WATER;
            e->meta = (uint8_t)(level >= WATER_FULL ? 0 : level);
        } else if (c->kind == K_WATER && c->freezable && c->lat >= FREEZE_SKIN * 1000.0 * L_FUSION) {
            e->id = B_ICE;
            e->meta = 0;
        } else {
            continue;
        }
        e->x = c->x;
        e->y = c->y;
        e->z = c->z;
        n++;
    }
    for (int k = 0; k < n; k++) {
        const block_edit *e = &t->edits[k];
        int i = find(t, e->x, e->y, e->z);
        double lat = i >= 0 ? t->cells[i].lat : 0.0;
        uint8_t old = world_get(w, e->x, e->y, e->z);
        if (!world_set(w, e->x, e->y, e->z, e->id, e->meta)) continue;
        i = find(t, e->x, e->y, e->z);
        if (i < 0) continue;
        cell *c = &t->cells[i];
        c->mat = e->id;
        c->kind = (uint8_t)kind_of(e->id);
        c->freezable = 0;
        t->dirty = 1;
        if (e->id == B_WATER) {
            /* Heat beyond the latent warms the meltwater. */
            double extra = lat - melt_energy(old);
            c->lat = 0.0;
            c->dev = -c->amb + fmax(extra, 0.0) / (RHO_CP_WATER * (double)(e->meta ? e->meta : WATER_FULL) / WATER_FULL);
        } else {
            /* Skin over water: the rest of the latent heat is still to go. */
            c->lat = fmax(melt_energy(B_ICE) - lat, 0.0);
            c->dev = -c->amb;
        }
    }
}

/* ---- API */

thermo *thermo_create(uint32_t seed)
{
    thermo *t = mem_calloc(1, sizeof *t);
    t->seed = seed;
    t->day = THERMO_DAY_START;
    t->dirty = 1;
    t->cells = mem_calloc(THERMO_MAX_CELLS, sizeof *t->cells);
    t->q = mem_calloc(THERMO_MAX_CELLS, sizeof *t->q);
    t->table = mem_alloc(mem_array_size(HASH_SIZE, sizeof *t->table));
    for (int i = 0; i < HASH_SIZE; i++) t->table[i] = -1;
    return t;
}

void thermo_destroy(thermo *t)
{
    if (!t) return;
    mem_free(t->cells);
    mem_free(t->q);
    mem_free(t->table);
    mem_free(t);
}

float thermo_air(const thermo *t, const world *w, double x, double y, double z)
{
    float amb = thermo_ambient(t, w, x, y, z);
    int i = find(t, coord(x, -WORLD_LIMIT, WORLD_LIMIT), coord(y, -1, WORLD_H), coord(z, -WORLD_LIMIT, WORLD_LIMIT));
    return i >= 0 ? (float)((double)amb + t->cells[i].dev) : amb;
}

float thermo_block_temp(const thermo *t, const world *w, int x, int y, int z)
{
    uint8_t id = world_get(w, x, y, z);
    if (id == B_CAMPFIRE) return (float)(FLAME_K - 273.15);
    int i = find(t, x, y, z);
    double amb = cell_ambient(t, w, x, y, z, NULL);
    if (i >= 0) return (float)(amb + t->cells[i].dev);
    return (float)(amb + initial_dev(id, amb));
}

/* Line of sight from p to the flame centre through non-opaque blocks,
 * marched in 10 cm steps. */
static int fire_visible(const world *w, const fire *f, dvec3 p, double d)
{
    dvec3 c = dv3((double)f->x + 0.5, (double)f->y + 0.5, (double)f->z + 0.5);
    int sx = (int)floor(p.x), sy = (int)floor(p.y), sz = (int)floor(p.z);
    int n = (int)ceil(d / 0.1);
    for (int k = 1; k < n; k++) {
        dvec3 s = dv3_lerp(p, c, (double)k / n);
        int x = (int)floor(s.x), y = (int)floor(s.y), z = (int)floor(s.z);
        if ((x == f->x && y == f->y && z == f->z) || (x == sx && y == sy && z == sz)) continue;
        if (block_opaque(world_get(w, x, y, z))) return 0;
    }
    return 1;
}

double thermo_radiant(const thermo *t, const world *w, dvec3 p)
{
    if (!isfinite(p.x) || !isfinite(p.y) || !isfinite(p.z)) return 0.0;
    double emit = radiated_w() / (4.0 * MC_PI), q = 0.0;
    for (int i = 0; i < t->fire_count; i++) {
        const fire *f = &t->fires[i];
        double d = flame_dist(f, p);
        if (d > FIRE_REACH || world_get(w, f->x, f->y, f->z) != B_CAMPFIRE) continue;
        if (!fire_visible(w, f, p, d)) continue;
        d = fmax(d, FIRE_MIN_D);
        q += emit / (d * d);
    }
    return q;
}

int thermo_near_fire(const thermo *t, const world *w, dvec3 p, double r)
{
    for (int i = 0; i < t->fire_count; i++) {
        const fire *f = &t->fires[i];
        if (flame_dist(f, p) <= r && world_get(w, f->x, f->y, f->z) == B_CAMPFIRE) return 1;
    }
    return 0;
}

void thermo_on_block_changed(thermo *t, const world *w, int x, int y, int z, uint8_t old_id, uint8_t new_id)
{
    if (new_id == B_CAMPFIRE) fire_register(t, x, y, z);
    else if (old_id == B_CAMPFIRE) fire_forget(t, x, y, z);
    if (old_id == new_id || y < 0 || y >= WORLD_H) return;
    int i = find(t, x, y, z);
    if (i >= 0) {
        cell *c = &t->cells[i];
        c->mat = new_id;
        c->kind = (uint8_t)kind_of(new_id);
        c->lat = 0.0;
        c->freezable = 0;
        t->dirty = 1;
        /* A doused or burnt-out fire leaves hot embers; anything else
         * starts at the temperature of a fresh block. */
        if (!(old_id == B_CAMPFIRE && new_id == B_ASH)) c->dev = initial_dev(new_id, c->amb);
        return;
    }
    int wake = new_id == B_CAMPFIRE;
    int k = kind_of(new_id);
    if (k == K_MELT || k == K_WATER) {
        double amb = cell_ambient(t, w, x, y, z, NULL);
        wake |= (k == K_MELT && amb > 0.0) || (k == K_WATER && amb < 0.0);
    }
    for (int f = 0; f < 6 && !wake; f++) wake = find(t, x + FACE[f][0], y + FACE[f][1], z + FACE[f][2]) >= 0;
    if (wake) ensure(t, w, x, y, z);
}

void thermo_step(thermo *t, world *w, double dt, dvec3 player)
{
    if (!(dt > 0.0)) return;
    dt = fmin(dt, 60.0);
    double dts = dt * HEALTH_CLOCK;
    t->day += dts / 86400.0;
    t->day -= floor(t->day);

    int have_player = isfinite(player.x) && isfinite(player.y) && isfinite(player.z);
    if (have_player) {
        scan_fires(t, w, player);
        scan_ponds(t, w, player);
    }
    burn_fires(t, w, dts);
    t->since_house += dts;
    if (t->dirty || t->since_house >= HOUSEKEEP_S) {
        t->stamp++;
        maintain(t, w, player, have_player);
        build_links(t, w);
        t->since_house = 0.0;
    }
    if (t->linked == 0) return;

    refresh_ambient(t);
    int cap = WORK_CAP / t->linked;
    if (cap > MAX_SUBSTEPS) cap = MAX_SUBSTEPS;
    if (cap < 1) cap = 1;
    double want = ceil(dts / t->hmax);
    int n = want <= 1.0 ? 1 : (want >= (double)cap ? cap : (int)want);
    double h = fmin(dts / n, t->hmax);
    for (int s = 0; s < n; s++) substep(t, h);
    convert(t, w);
}

int thermo_add_fuel(thermo *t, world *w, int x, int y, int z, int units)
{
    if (units <= 0 || world_get(w, x, y, z) != B_CAMPFIRE) return 0;
    int fuel = fuel_of(w, x, y, z);
    if (fuel >= CAMPFIRE_FUEL_MAX) return 0;
    fuel = units >= CAMPFIRE_FUEL_MAX - fuel ? CAMPFIRE_FUEL_MAX : fuel + units;
    if (!world_set(w, x, y, z, B_CAMPFIRE, (uint8_t)(fuel >= CAMPFIRE_FUEL_MAX ? 0 : fuel))) return 0;
    fire_register(t, x, y, z);
    return 1;
}

int thermo_cell_count(const thermo *t) { return t->count; }
int thermo_fire_count(const thermo *t) { return t->fire_count; }
