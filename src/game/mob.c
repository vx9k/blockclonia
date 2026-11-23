#include "mob.h"
#include "health.h"
#include "item.h"
#include "mathlib.h"
#include "thermo.h"

#include <ctype.h>
#include <math.h>
#include <string.h>

typedef struct {
    float hp, height, half_w;
    int cap;             /* most of this kind near the players at once */
} mob_rule;

/* A wild boar stands ~0.9 m at the shoulder and weighs ~80 kg; a grey wolf
 * ~0.8 m and ~40 kg. Hit points are scaled to blows (see mobs_hurt). */
static const mob_rule RULES[MOB_KINDS] = {
    [MOB_BOAR] = {20.0f, 0.9f, 0.4f, 6},
    [MOB_WOLF] = {16.0f, 0.85f, 0.35f, 2},
};

#define SPAWN_MIN 24.0   /* m from a player: out of sight, but not far */
#define SPAWN_MAX 44.0
#define DESPAWN 96.0     /* m from every player */
#define NEAR 64.0        /* the per-kind cap counts mobs this close to a player */
#define HUNT_RANGE 20.0  /* m a wolf notices a player from */
#define BITE_RANGE 1.3   /* m between bodies' centres */
#define NIGHT (THERMO_NIGHT_LIGHT + 0.207f * (1.0f - THERMO_NIGHT_LIGHT)) /* daylight below this is night for wolves */
#define CHARGE 8.0 /* m from its prey a stalking wolf breaks into a run */
#define CALM 45.0f /* s after a world opens before the first wolf */
#define LOOK_RANGE 8.0 /* m a creature glances at a player from */
#define LIT_FULL 0.5f /* light level a body is in plain view at */
#define FIRE_FEAR 0.45f /* firelight (mob_sense.fire) a wolf will not hunt into */
#define FIRE_KEEP 6.0 /* m a wolf keeps from prey standing in firelight */
#define BURN_HP_S 2.5f /* hit points a second burning takes: a boar lasts 8 s */

/* The share of its daylight sight a creature keeps in the dark: wolves
 * hunt by night and have a reflecting layer behind the retina, boars
 * rely on smell and see poorly. */
static const float DARK_EYES[MOB_KINDS] = {[MOB_BOAR] = 0.4f, [MOB_WOLF] = 0.6f};

static dvec3 sub(dvec3 a, dvec3 b) { return dv3(a.x - b.x, a.y - b.y, a.z - b.z); }

static uint32_t next(uint32_t *rng)
{
    *rng = *rng * 1664525u + 1013904223u;
    return *rng >> 8;
}

static float frand(uint32_t *rng) { return (float)next(rng) / 16777216.0f; }

void mobs_init(mobs *s, uint32_t seed)
{
    memset(s, 0, sizeof *s);
    s->rng = seed ? seed : 1u;
    s->spawn_t = 2.0f;
}

int mobs_add(mobs *s, int kind, dvec3 pos)
{
    if (s->count >= MOB_MAX || kind < 0 || kind >= MOB_KINDS) return -1;
    mob *m = &s->m[s->count];
    memset(m, 0, sizeof *m);
    m->kind = (uint8_t)kind;
    m->hp = RULES[kind].hp;
    m->body.pos = m->body.prev_pos = pos;
    m->body.height = RULES[kind].height;
    m->body.half_w = RULES[kind].half_w;
    m->rng = next(&s->rng) | 1u;
    m->body.yaw = frand(&m->rng) * 6.2831853f;
    return s->count++;
}

int mobs_kind_named(const char *name)
{
    static const char *const NAMES[MOB_KINDS] = {[MOB_BOAR] = "boar", [MOB_WOLF] = "wolf"};
    for (int k = 0; k < MOB_KINDS; k++) {
        size_t i = 0;
        while (name[i] && NAMES[k][i] && tolower((unsigned char)name[i]) == NAMES[k][i]) i++;
        if (!name[i] && !NAMES[k][i]) return k;
    }
    return -1;
}

static float yaw_to(dvec3 from, dvec3 to);

int mobs_add_ahead(mobs *s, const world *w, const player *p, int kind, double ahead, double aside)
{
    /* Forward is (sin yaw, -cos yaw), as physics_move_player walks. */
    double sy = sin((double)p->yaw), cy = cos((double)p->yaw);
    double x = p->pos.x + sy * ahead + cy * aside, z = p->pos.z - cy * ahead + sy * aside;
    int y = world_surface_y(w, (int)floor(x), (int)floor(z));
    int i = mobs_add(s, kind, dv3(x, (double)y, z));
    if (i >= 0) s->m[i].body.yaw = yaw_to(s->m[i].body.pos, p->pos);
    return i;
}

static double dist_xz(dvec3 a, dvec3 b) { return sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z)); }

static float yaw_to(dvec3 from, dvec3 to) { return (float)atan2(to.x - from.x, -(to.z - from.z)); }

static void remove_at(mobs *s, int i) { s->m[i] = s->m[--s->count]; }

/* Tries once to put a mob on open ground around a random player: boars on
 * grass by day, wolves on grass, snow or rock at night. */
static void try_spawn(mobs *s, const physics *ph, player *const *players, int n, float daylight)
{
    int live[PHYS_MAX_PLAYERS] = {0}, nl = 0;
    for (int i = 0; i < n && nl < PHYS_MAX_PLAYERS; i++)
        if (players[i]) live[nl++] = i;
    if (!nl) return;
    int kind = daylight < NIGHT ? MOB_WOLF : MOB_BOAR;
    if (kind == MOB_WOLF && s->age < CALM) return;
    int near = 0;
    for (int i = 0; i < s->count; i++) {
        if (s->m[i].kind != kind) continue;
        for (int k = 0; k < nl; k++)
            if (dist_xz(s->m[i].body.pos, players[live[k]]->pos) < NEAR) {
                near++;
                break;
            }
    }
    if (near >= RULES[kind].cap) return;
    const player *p = players[live[next(&s->rng) % (uint32_t)nl]];
    double a = (double)frand(&s->rng) * 6.2831853, r = SPAWN_MIN + (double)frand(&s->rng) * (SPAWN_MAX - SPAWN_MIN);
    int x = (int)floor(p->pos.x + cos(a) * r), z = (int)floor(p->pos.z + sin(a) * r);
    int y = world_surface_y(ph->w, x, z);
    if (y <= 1 || y >= WORLD_H - 2) return;
    uint8_t ground = world_get(ph->w, x, y - 1, z);
    int ok = ground == B_GRASS || (kind == MOB_WOLF && (ground == B_SNOW || ground == B_ROCK || ground == B_STONE));
    if (!ok || world_get(ph->w, x, y, z) != B_AIR || world_get(ph->w, x, y + 1, z) != B_AIR) return;
    /* Never in sight range of anyone (a mob popping into view breaks the
     * illusion). */
    for (int k = 0; k < nl; k++)
        if (dist_xz(dv3(x + 0.5, y, z + 0.5), players[live[k]]->pos) < SPAWN_MIN) return;
    mobs_add(s, kind, dv3(x + 0.5, y, z + 0.5));
}

/* The nearest present player within range of p, -1 for none. */
double mobs_sight(int kind, double range, float level)
{
    float eyes = kind >= 0 && kind < MOB_KINDS ? DARK_EYES[kind] : 1.0f;
    float lit = level < LIT_FULL ? (level > 0.0f ? level / LIT_FULL : 0.0f) : 1.0f;
    return range * (double)(eyes + (1.0f - eyes) * lit);
}

/* The nearest player a creature of kind notices within range, each seen
 * from as far as the light around them allows. */
static int nearest(player *const *players, const mob_sense *sense, int n, int kind, dvec3 p, double range, double *d)
{
    int best = -1;
    *d = range;
    for (int i = 0; i < n; i++) {
        if (!players[i]) continue;
        double di = dv3_len(sub(players[i]->pos, p));
        double ri = sense ? mobs_sight(kind, range, sense[i].level) : range;
        if (di < ri && di < *d) {
            *d = di;
            best = i;
        }
    }
    return best;
}

/* Decides this step's movement for one mob, and reports a bite. */
static player_input think(mob *m, player *const *players, const mob_sense *sense, int n, float daylight, mob_events *ev)
{
    player_input in = {0};
    player *b = &m->body;
    double d = 0.0;
    int target = -1;
    if (m->kind == MOB_WOLF && (daylight < NIGHT || m->angry_t > 0.0f))
        target = nearest(players, sense, n, m->kind, b->pos, HUNT_RANGE, &d);
    /* Fire keeps a hunting wolf off, unless it has been hurt. */
    int wary = target >= 0 && sense && sense[target].fire >= FIRE_FEAR && m->angry_t <= 0.0f;

    if (wary) {
        /* It circles out of the light: backs off when close, watches from
         * the edge otherwise. */
        b->yaw = yaw_to(b->pos, players[target]->pos);
        in.forward = d < FIRE_KEEP ? -0.6f : 0.0f;
    } else if (target >= 0) {
        /* Hunt: stalk at a walk, charge the last few metres, and bite when
         * close; the player gets a moment to see it coming. */
        b->yaw = yaw_to(b->pos, players[target]->pos);
        in.forward = d > BITE_RANGE * 0.8 ? (d < CHARGE ? 1.0f : 0.7f) : 0.0f;
        in.sprint = d < CHARGE;
        if (d < BITE_RANGE && m->attack_t <= 0.0f && ev->bite_count < MOB_MAX_BITES) {
            /* Wolves go for the legs, and the arms raised against them. */
            static const int PARTS[] = {BP_LLEG, BP_RLEG, BP_LLEG, BP_RLEG, BP_LARM, BP_RARM};
            ev->bites[ev->bite_count++] = (mob_bite){target, PARTS[next(&m->rng) % 6u], b->pos};
            m->attack_t = 1.8f;
        }
    } else if (m->flee_t > 0.0f) {
        b->yaw = yaw_to(m->threat, b->pos);
        in.forward = 1.0f;
        in.sprint = 1;
    } else {
        /* Wander: a heading and a pace for a few seconds at a time, often
         * standing still to graze. */
        if (m->think_t <= 0.0f) {
            m->think_t = 2.0f + frand(&m->rng) * 4.0f;
            m->walk = frand(&m->rng) < 0.55f ? 1.0f : 0.0f;
            b->yaw += (frand(&m->rng) - 0.5f) * 2.5f;
        }
        in.forward = m->walk * 0.6f;
    }
    /* Up a step or out of water: a jump, as a player would. */
    in.jump = (b->hit_wall && b->on_ground && in.forward > 0.0f) || b->submerged > 0.4;
    const float dt = (float)PHYS_DT;
    /* Where the head points, for drawing: at a player close by, down to the
     * grass while a boar stands grazing, low while a wolf stalks. */
    double near_d = 0.0;
    int seen = nearest(players, sense, n, m->kind, b->pos, LOOK_RANGE, &near_d);
    float look = 0.0f, nod = 0.0f;
    if (target >= 0) {
        nod = d >= CHARGE ? 0.3f : 0.0f;
    } else if (seen >= 0 && m->flee_t <= 0.0f) {
        float a = yaw_to(b->pos, players[seen]->pos) - b->yaw;
        a = atan2f(sinf(a), cosf(a));
        look = a < -0.9f ? -0.9f : (a > 0.9f ? 0.9f : a);
    } else if (m->kind == MOB_BOAR && m->flee_t <= 0.0f && m->walk == 0.0f) {
        nod = 0.9f;
    }
    m->look += (look - m->look) * 0.08f;
    m->nod += (nod - m->nod) * 0.05f;
    m->think_t -= dt;
    m->attack_t -= dt;
    m->flee_t -= dt;
    m->angry_t -= dt;
    m->hurt_t -= dt;
    return in;
}

static int burn(mobs *s, physics *ph, int i);

void mobs_step(mobs *s, physics *ph, player *const *players, const mob_sense *sense, int n, float daylight,
               mob_events *ev)
{
    memset(ev, 0, sizeof *ev);
    s->age += (float)PHYS_DT;
    s->spawn_t -= (float)PHYS_DT;
    if (s->spawn_t <= 0.0f) {
        s->spawn_t = 1.5f;
        try_spawn(s, ph, players, n, daylight);
    }
    for (int i = 0; i < s->count; i++) {
        mob *m = &s->m[i];
        double d = 0.0;
        if (nearest(players, NULL, n, m->kind, m->body.pos, DESPAWN, &d) < 0 || m->body.pos.y < -32.0) {
            remove_at(s, i--);
            continue;
        }
        player_input in = think(m, players, sense, n, daylight, ev);
        physics_move_player(ph, &m->body, &in);
        if (burn(s, ph, i)) {
            i--;
            continue;
        }
        m->stride += (float)(PHYS_DT * sqrt(m->body.vel.x * m->body.vel.x + m->body.vel.z * m->body.vel.z)) * 4.0f;
    }
}

float mobs_fear(const mobs *s, const player *p, float daylight)
{
    /* Wolves combine like independent odds of staying calm, so a pack is
     * worse than its nearest member. Only a hunting wolf counts: one
     * wandering by day is no threat until it is hurt. */
    float calm = 1.0f;
    for (int i = 0; i < s->count; i++) {
        const mob *m = &s->m[i];
        if (m->kind != MOB_WOLF || !(daylight < NIGHT || m->angry_t > 0.0f)) continue;
        double d = dv3_len(sub(m->body.pos, p->pos));
        if (d >= HUNT_RANGE) continue;
        double f = (HUNT_RANGE - d) / (HUNT_RANGE - BITE_RANGE);
        calm *= 1.0f - (float)(f < 1.0 ? f : 1.0);
    }
    return 1.0f - calm;
}

int mobs_raycast(const mobs *s, dvec3 o, vec3 dir, double max_dist, double *dist)
{
    int best = -1;
    double bt = max_dist;
    for (int i = 0; i < s->count; i++) {
        aabb b = player_box(&s->m[i].body);
        /* Slab test against the box. */
        double t0 = 0.0, t1 = bt;
        const double o3[3] = {o.x, o.y, o.z}, d3[3] = {(double)dir.x, (double)dir.y, (double)dir.z};
        const double lo[3] = {b.min.x, b.min.y, b.min.z}, hi[3] = {b.max.x, b.max.y, b.max.z};
        int miss = 0;
        for (int k = 0; k < 3 && !miss; k++) {
            if (fabs(d3[k]) < 1e-12) {
                miss = o3[k] < lo[k] || o3[k] > hi[k];
                continue;
            }
            double a = (lo[k] - o3[k]) / d3[k], c = (hi[k] - o3[k]) / d3[k];
            if (a > c) {
                double t = a;
                a = c;
                c = t;
            }
            if (a > t0) t0 = a;
            if (c < t1) t1 = c;
            miss = t0 > t1;
        }
        if (!miss && t0 < bt) {
            bt = t0;
            best = i;
        }
    }
    if (dist) *dist = bt;
    return best;
}

/* Removes creature i, leaving what it leaves: a boar its meat, cooked
 * if it burned to death. */
static void die(mobs *s, physics *ph, int i, int burnt)
{
    mob *m = &s->m[i];
    dvec3 at = dv3(m->body.pos.x, m->body.pos.y + 0.5, m->body.pos.z);
    if (m->kind == MOB_BOAR)
        physics_drop_item(ph, at, dv3(0.0, 2.0, 0.0), burnt ? I_COOKED_MEAT : I_RAW_MEAT, 2 + (int)(next(&m->rng) % 2u),
                          0.4f);
    remove_at(s, i);
}

/* Fire and lava at its feet or body set a creature alight, water puts it
 * out, and burning wears it down. Returns 1 if it burned to death (it is
 * removed). */
static int burn(mobs *s, physics *ph, int i)
{
    mob *m = &s->m[i];
    const player *b = &m->body;
    for (int k = 0; k < 2 && ph->w; k++) {
        uint8_t id = world_get(ph->w, (int)floor(b->pos.x), (int)floor(b->pos.y + 0.05 + 0.5 * k),
                               (int)floor(b->pos.z));
        if (id == B_CAMPFIRE || id == B_LAVA) m->burn_t = MOB_BURN_S;
        else if (id == B_WATER || id == B_BRINE) m->burn_t = 0.0f;
    }
    if (m->burn_t <= 0.0f) return 0;
    m->burn_t -= (float)PHYS_DT;
    m->hp -= BURN_HP_S * (float)PHYS_DT;
    if (m->hp > 0.0f) return 0;
    die(s, ph, i, 1);
    return 1;
}

int mobs_hurt(mobs *s, physics *ph, int i, float damage, dvec3 from)
{
    if (i < 0 || i >= s->count || !(damage > 0.0f)) return 0;
    mob *m = &s->m[i];
    m->hp -= damage;
    m->hurt_t = 0.35f;
    /* Knocked back from the blow, a little up. */
    dvec3 away = sub(m->body.pos, from);
    away.y = 0.0;
    double l = dv3_len(away);
    if (l > 1e-6) {
        m->body.vel.x += away.x / l * 4.0;
        m->body.vel.z += away.z / l * 4.0;
    }
    m->body.vel.y += 3.0;
    if (m->kind == MOB_BOAR) {
        m->flee_t = 6.0f;
        m->threat = from;
    } else {
        m->angry_t = 30.0f;
    }
    if (m->hp > 0.0f) return 0;
    die(s, ph, i, 0);
    return 1;
}

/* ------------------------------------------------------------- drawing */

/* A creature's shape in texels (1/16 m, the pixel art's scale) from its
 * feet, front towards -z: every box's size matches its picture in
 * mobskin.c. The head, snout and ears turn together about the neck, the
 * tail about the rump, each leg at the hip and again at the knee. */
typedef struct {
    float size[3], at[3];
    int side, top, bottom, front;
} shape_box;

typedef struct {
    shape_box body, shoulders;
    float neck[3]; /* the head's pivot */
    shape_box head, snout, ear; /* at: from the neck; the ear is mirrored to the other side */
    float rump[3], droop; /* the tail's pivot, and how far it hangs (rad) */
    shape_box tail; /* at: from the rump */
    float hip[3]; /* front left hip; the others mirror it */
    shape_box upper, lower; /* at: unused, they hang from the hip and the knee */
    float shift; /* texels the figure moves back to centre it on its feet */
} shape;

/* A grey wolf: ~0.75 m at the shoulder, ~1.3 m nose to rump. */
static const shape WOLF_SHAPE = {
    .body = {{6, 6, 13}, {0, 9, 0}, T_WOLF_BODY_SIDE, T_WOLF_BODY_TOP, T_WOLF_BELLY, T_WOLF_FUR},
    .shoulders = {{7, 7, 5}, {0, 9.5f, -4.5f}, T_WOLF_MANE_SIDE, T_WOLF_FUR, T_WOLF_FUR, T_WOLF_MANE_FRONT},
    .neck = {0, 11, -7},
    .head = {{5, 5, 5}, {0, 1.5f, -2.5f}, T_WOLF_HEAD_SIDE, T_WOLF_HEAD_TOP, T_WOLF_FUR, T_WOLF_HEAD_FRONT},
    .snout = {{3, 2, 3}, {0, 0, -6.5f}, T_WOLF_SNOUT_SIDE, T_WOLF_FUR, T_WOLF_FUR, T_WOLF_SNOUT_FRONT},
    .ear = {{2, 2, 1}, {-1.5f, 5, -1.5f}, T_WOLF_EAR, T_WOLF_FUR, T_WOLF_FUR, T_WOLF_EAR},
    .rump = {0, 11.5f, 6.5f},
    .droop = 0.7f,
    .tail = {{2, 2, 7}, {0, 0, 3.5f}, T_WOLF_TAIL_SIDE, T_WOLF_TAIL_TOP, T_WOLF_TAIL_TOP, T_WOLF_FUR},
    .hip = {-2, 6, -4.5f},
    .upper = {{2, 3, 2}, {0}, T_WOLF_LEG_UPPER, T_WOLF_FUR, T_WOLF_FUR, T_WOLF_LEG_UPPER},
    .lower = {{2, 3, 2}, {0}, T_WOLF_LEG_LOWER, T_WOLF_FUR, T_MOB_SOLE, T_WOLF_LEG_LOWER},
    .shift = 3,
};

/* A wild boar: low-legged and deep-bodied, ~0.85 m at the bristle crest. */
static const shape BOAR_SHAPE = {
    .body = {{8, 8, 15}, {0, 9, 0}, T_BOAR_BODY_SIDE, T_BOAR_BODY_TOP, T_BOAR_BELLY, T_BOAR_FUR},
    .shoulders = {{4, 2, 8}, {0, 14, -3.5f}, T_BOAR_RIDGE, T_BOAR_RIDGE, T_BOAR_RIDGE, T_BOAR_RIDGE},
    .neck = {0, 10, -7.5f},
    .head = {{6, 6, 5}, {0, 0, -2.5f}, T_BOAR_HEAD_SIDE, T_BOAR_HEAD_TOP, T_BOAR_FUR, T_BOAR_HEAD_FRONT},
    .snout = {{4, 3, 3}, {0, -1.5f, -6.5f}, T_BOAR_SNOUT_SIDE, T_BOAR_FUR, T_BOAR_FUR, T_BOAR_SNOUT_FRONT},
    .ear = {{2, 2, 1}, {-2, 4, -1}, T_BOAR_EAR, T_BOAR_FUR, T_BOAR_FUR, T_BOAR_EAR},
    .rump = {0, 12, 7.5f},
    .droop = -0.25f, /* a short tail that hangs, swung a little back */
    .tail = {{1, 4, 1}, {0, -2, 0.5f}, T_BOAR_TAIL, T_BOAR_TAIL, T_BOAR_TAIL, T_BOAR_TAIL},
    .hip = {-2.5f, 5, -5.5f},
    .upper = {{2, 3, 2}, {0}, T_BOAR_LEG_UPPER, T_BOAR_FUR, T_BOAR_FUR, T_BOAR_LEG_UPPER},
    .lower = {{2, 2, 2}, {0}, T_BOAR_LEG_LOWER, T_BOAR_FUR, T_MOB_SOLE, T_BOAR_LEG_LOWER},
    .shift = 3,
};

#define TX (1.0f / 16.0f) /* metres per texel */

typedef struct {
    entity_instance *out;
    int n, max;
    const float *feet;
    quat facing;
    float shift, light;
} figure;

/* One box: centre at `at` (texels, body frame), turned by `own` within the
 * body. */
static void put(figure *f, const shape_box *s, const float at[3], quat own)
{
    if (f->n >= f->max) return;
    entity_instance e = {0};
    const float local[3] = {at[0] * TX, at[1] * TX, (at[2] + f->shift) * TX};
    float r[3] = {0};
    quat_apply(f->facing, local, r);
    for (int k = 0; k < 3; k++) {
        e.pos[k] = f->feet[k] + r[k];
        e.scale[k] = s->size[k] * TX;
    }
    e.tex = entity_tex(s->side, s->top, s->bottom, s->front) | ENTITY_TEX_METRIC;
    entity_set_rot(&e, quat_norm(quat_mul(f->facing, own)));
    e.light = f->light;
    f->out[f->n++] = e;
}

/* A child box of a joint: pivot plus the offset turned by q. */
static void put_from(figure *f, const shape_box *s, const float pivot[3], const float off[3], quat q)
{
    float r[3] = {0};
    quat_apply(q, off, r);
    const float at[3] = {pivot[0] + r[0], pivot[1] + r[1], pivot[2] + r[2]};
    put(f, s, at, q);
}

int mob_figure(const mob *m, const float feet[3], float light, float t, entity_instance *out, int max)
{
    const shape *s = m->kind == MOB_BOAR ? &BOAR_SHAPE : &WOLF_SHAPE;
    figure f = {out,
                0,
                max,
                feet,
                quat_axis(0.0f, 1.0f, 0.0f, -m->body.yaw),
                s->shift,
                m->hurt_t > 0.0f ? light * 1.8f : light}; /* the hurt flash */
    put(&f, &s->body, s->body.at, quat_identity());
    put(&f, &s->shoulders, s->shoulders.at, quat_identity());

    /* The head nods a little with each stride. */
    float nod = m->nod + 0.05f * sinf(2.0f * m->stride);
    quat head = quat_mul(quat_axis(0.0f, 1.0f, 0.0f, -m->look), quat_axis(1.0f, 0.0f, 0.0f, -nod));
    put_from(&f, &s->head, s->neck, s->head.at, head);
    put_from(&f, &s->snout, s->neck, s->snout.at, head);
    for (int side = 0; side < 2; side++) {
        const float off[3] = {side ? -s->ear.at[0] : s->ear.at[0], s->ear.at[1], s->ear.at[2]};
        put_from(&f, &s->ear, s->neck, off, head);
    }

    /* A wolf's tail sways slowly; a boar's swishes. */
    float wag = m->kind == MOB_WOLF ? 0.25f * sinf(t * 2.2f) : 0.3f * sinf(t * 3.1f);
    quat tail = quat_mul(quat_axis(0.0f, 1.0f, 0.0f, wag), quat_axis(1.0f, 0.0f, 0.0f, s->droop));
    put_from(&f, &s->tail, s->rump, s->tail.at, tail);

    /* A trot: diagonal legs together. Each leg swings at the hip, and the
     * lower leg folds back while the foot comes forward. */
    for (int k = 0; k < 4; k++) {
        const float hip[3] = {k & 1 ? -s->hip[0] : s->hip[0], s->hip[1], k & 2 ? -s->hip[2] : s->hip[2]};
        float phase = m->stride + ((k == 0 || k == 3) ? 0.0f : 3.14159265f);
        quat upper = quat_axis(1.0f, 0.0f, 0.0f, 0.5f * sinf(phase));
        quat lower = quat_mul(upper, quat_axis(1.0f, 0.0f, 0.0f, -0.7f * fmaxf(0.0f, cosf(phase))));
        const float half_up[3] = {0.0f, -0.5f * s->upper.size[1], 0.0f};
        const float knee_off[3] = {0.0f, -s->upper.size[1], 0.0f};
        const float half_low[3] = {0.0f, -0.5f * s->lower.size[1], 0.0f};
        float knee[3] = {0};
        quat_apply(upper, knee_off, knee);
        for (int i = 0; i < 3; i++) knee[i] += hip[i];
        put_from(&f, &s->upper, hip, half_up, upper);
        put_from(&f, &s->lower, knee, half_low, lower);
    }

    /* Burning: flames over the back, which light themselves. */
    for (int k = 0; k < 2 && m->burn_t > 0.0f && f.n < f.max; k++) {
        entity_instance e = {0};
        float half = 0.25f * s->body.size[2] * (k ? -1.0f : 1.0f);
        const float local[3] = {s->body.at[0] * TX, (s->body.at[1] + 0.5f * s->body.size[1]) * TX + 0.2f,
                                (s->body.at[2] + f.shift + half) * TX};
        float r[3] = {0};
        quat_apply(f.facing, local, r);
        for (int a = 0; a < 3; a++) e.pos[a] = f.feet[a] + r[a];
        e.scale[0] = s->body.size[0] * TX;
        e.scale[1] = 0.5f;
        e.scale[2] = 0.5f * s->body.size[2] * TX;
        e.tex = (uint32_t)(T_FIRE0 + k) * 0x010101u;
        entity_set_rot(&e, f.facing);
        e.light = 1.0f;
        e.tint = ENTITY_GLOWS;
        f.out[f.n++] = e;
    }
    return f.n;
}
