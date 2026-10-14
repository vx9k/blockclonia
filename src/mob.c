#include "mob.h"
#include "health.h"
#include "item.h"
#include "mathlib.h"

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
#define NIGHT 0.35f      /* daylight below this is night for wolves */
#define CHARGE 8.0       /* m from its prey a stalking wolf breaks into a run */
#define CALM 45.0f       /* s after a world opens before the first wolf */

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

static double dist_xz(dvec3 a, dvec3 b) { return sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z)); }

static float yaw_to(dvec3 from, dvec3 to) { return (float)atan2(to.x - from.x, -(to.z - from.z)); }

static void remove_at(mobs *s, int i)
{
    s->m[i] = s->m[--s->count];
}

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
static int nearest(player *const *players, int n, dvec3 p, double range, double *d)
{
    int best = -1;
    *d = range;
    for (int i = 0; i < n; i++) {
        if (!players[i]) continue;
        double di = dv3_len(sub(players[i]->pos, p));
        if (di < *d) {
            *d = di;
            best = i;
        }
    }
    return best;
}

/* Decides this step's movement for one mob, and reports a bite. */
static player_input think(mob *m, player *const *players, int n, float daylight, mob_events *ev)
{
    player_input in = {0};
    player *b = &m->body;
    double d = 0.0;
    int target = -1;
    if (m->kind == MOB_WOLF && (daylight < NIGHT || m->angry_t > 0.0f))
        target = nearest(players, n, b->pos, HUNT_RANGE, &d);

    if (target >= 0) {
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
    m->think_t -= dt;
    m->attack_t -= dt;
    m->flee_t -= dt;
    m->angry_t -= dt;
    m->hurt_t -= dt;
    return in;
}

void mobs_step(mobs *s, physics *ph, player *const *players, int n, float daylight, mob_events *ev)
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
        if (nearest(players, n, m->body.pos, DESPAWN, &d) < 0 || m->body.pos.y < -32.0) {
            remove_at(s, i--);
            continue;
        }
        player_input in = think(m, players, n, daylight, ev);
        physics_move_player(ph, &m->body, &in);
        m->stride += (float)(PHYS_DT * sqrt(m->body.vel.x * m->body.vel.x + m->body.vel.z * m->body.vel.z)) * 4.0f;
    }
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
    dvec3 at = dv3(m->body.pos.x, m->body.pos.y + 0.5, m->body.pos.z);
    if (m->kind == MOB_BOAR) physics_drop_item(ph, at, dv3(0.0, 2.0, 0.0), I_RAW_MEAT, 2 + (int)(next(&m->rng) % 2u), 0.4f);
    remove_at(s, i);
    return 1;
}

/* ------------------------------------------------------------- drawing */

static entity_instance box(const float feet[3], quat facing, const float at[3], quat own, const float size[3],
                           int layer, float light)
{
    entity_instance e = {0};
    float r[3] = {0};
    quat_apply(facing, at, r);
    for (int k = 0; k < 3; k++) e.pos[k] = feet[k] + r[k];
    e.tex = (uint32_t)layer * 0x010101u;
    entity_set_rot(&e, quat_norm(quat_mul(facing, own)));
    memcpy(e.scale, size, sizeof e.scale);
    e.light = light;
    return e;
}

int mob_figure(const mob *m, const float feet[3], float light, entity_instance *out, int max)
{
    const int fur = m->kind == MOB_BOAR ? T_FUR_BOAR : T_FUR_WOLF;
    const float h = RULES[m->kind].height;
    quat facing = quat_axis(0.0f, 1.0f, 0.0f, -m->body.yaw);
    float lit = m->hurt_t > 0.0f ? light * 1.8f : light; /* the hurt flash */
    float leg = h * 0.45f, len = m->kind == MOB_BOAR ? 1.1f : 1.0f, wide = RULES[m->kind].half_w * 1.6f;
    float swing = sinf(m->stride) * 0.6f;
    int n = 0;
    if (n < max) {
        float at[3] = {0.0f, leg + (h - leg) * 0.5f, 0.0f}, torso[3] = {wide, h - leg, len};
        out[n++] = box(feet, facing, at, quat_identity(), torso, fur, lit);
    }
    if (n < max) {
        float at[3] = {0.0f, h * 0.8f, -len * 0.5f - 0.15f}, head[3] = {wide * 0.75f, 0.3f, 0.35f};
        out[n++] = box(feet, facing, at, quat_identity(), head, fur, lit);
    }
    for (int k = 0; k < 4 && n < max; k++) {
        float x = (k & 1 ? 1.0f : -1.0f) * wide * 0.32f, z = (k & 2 ? 1.0f : -1.0f) * len * 0.36f;
        float at[3] = {x, leg * 0.5f, z}, size[3] = {0.14f, leg, 0.14f};
        float a = (k == 0 || k == 3) ? swing : -swing;
        out[n++] = box(feet, facing, at, quat_axis(1.0f, 0.0f, 0.0f, a), size, fur, lit);
    }
    /* A wolf's tail, a boar's snout. */
    if (n < max) {
        float at[3] = {0.0f, m->kind == MOB_BOAR ? h * 0.72f : h * 0.75f, m->kind == MOB_BOAR ? -len * 0.5f - 0.38f
                                                                                             : len * 0.5f + 0.2f};
        float size[3] = {m->kind == MOB_BOAR ? 0.2f : 0.1f, m->kind == MOB_BOAR ? 0.14f : 0.1f, 0.3f};
        out[n++] = box(feet, facing, at, quat_axis(1.0f, 0.0f, 0.0f, m->kind == MOB_WOLF ? 0.5f : 0.0f), size,
                       m->kind == MOB_BOAR ? T_SKIN : fur, lit);
    }
    return n;
}
