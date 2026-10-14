#include "combat.h"
#include "mathlib.h"

#include <math.h>
#include <string.h>

#define BULLET_DAMAGE 14.0f
#define BULLET_LIFE 2.0f

void combat_init(combat *c, uint32_t seed)
{
    memset(c, 0, sizeof *c);
    c->rng = seed ? seed : 1u;
}

static float frand(combat *c)
{
    c->rng = c->rng * 1664525u + 1013904223u;
    return (float)(c->rng >> 8) / 16777216.0f;
}

float combat_blow(int held)
{
    int kind = 0, tier = 0;
    if (!item_tool(held, &kind, &tier)) return held == I_RIFLE ? 2.0f : 1.0f; /* a rifle butt, a fist */
    float t = (float)tier;
    switch (kind) {
    case TOOL_SWORD: return 5.0f + 1.3f * t;
    case TOOL_KNIFE: return 3.0f + 0.7f * t;
    case TOOL_AXE: return 3.0f + 1.0f * t;
    default: return 2.0f + 0.5f * t; /* pickaxe, shovel */
    }
}

/* s between blows: a knife is quick, a sword or an axe has to be swung. */
static float blow_time(int held)
{
    int kind = 0, tier = 0;
    if (!item_tool(held, &kind, &tier)) return 0.45f;
    return kind == TOOL_SWORD || kind == TOOL_AXE ? 0.6f : 0.45f;
}

/* Ray against player i's box; distance or a negative number for a miss. */
static double hit_player(const player *p, dvec3 o, vec3 dir, double max_dist)
{
    aabb b = player_box(p);
    double t0 = 0.0, t1 = max_dist;
    const double o3[3] = {o.x, o.y, o.z}, d3[3] = {(double)dir.x, (double)dir.y, (double)dir.z};
    const double lo[3] = {b.min.x, b.min.y, b.min.z}, hi[3] = {b.max.x, b.max.y, b.max.z};
    for (int k = 0; k < 3; k++) {
        if (fabs(d3[k]) < 1e-12) {
            if (o3[k] < lo[k] || o3[k] > hi[k]) return -1.0;
            continue;
        }
        double a = (lo[k] - o3[k]) / d3[k], e = (hi[k] - o3[k]) / d3[k];
        if (a > e) {
            double s = a;
            a = e;
            e = s;
        }
        if (a > t0) t0 = a;
        if (e < t1) t1 = e;
        if (t0 > t1) return -1.0;
    }
    return t0;
}

/* The nearest target along a ray: a mob index in *mi or a player index in
 * *pl (the other -1). Returns the distance, or max_dist with neither. */
static double nearest_target(const mobs *ms, const combat_targets *t, int who, dvec3 o, vec3 dir, double max_dist,
                             int *mi, int *pl)
{
    double best = max_dist, d = 0.0;
    *mi = mobs_raycast(ms, o, dir, max_dist, &d);
    *pl = -1;
    if (*mi >= 0) best = d;
    for (int i = 0; i < t->n; i++) {
        if (i == who || !t->pl[i] || !t->hl[i]) continue;
        double di = hit_player(t->pl[i], o, dir, best);
        if (di >= 0.0 && di < best) {
            best = di;
            *pl = i;
            *mi = -1;
        }
    }
    return best;
}

int combat_aim(const mobs *ms, const combat_targets *t, int who, dvec3 eye, vec3 dir, double block_dist)
{
    int mi = -1, pl = -1;
    double reach = block_dist < COMBAT_REACH ? block_dist : COMBAT_REACH;
    nearest_target(ms, t, who, eye, dir, reach, &mi, &pl);
    return mi >= 0 || pl >= 0;
}

/* Which part a hit at height y lands on, for a player standing at feet. */
static int part_at(const player *p, double y, combat *c)
{
    double f = (y - p->pos.y) / PLAYER_HEIGHT;
    int side = frand(c) < 0.5f;
    if (f > 0.85) return BP_HEAD;
    if (f > 0.62) return frand(c) < 0.6f ? BP_CHEST : (side ? BP_LARM : BP_RARM);
    if (f > 0.48) return frand(c) < 0.6f ? BP_ABDOMEN : (side ? BP_LARM : BP_RARM);
    return side ? BP_LLEG : BP_RLEG;
}

/* A wound on a player from a blow or a bullet worth `damage` hit points:
 * blades and bullets cut (deeper the harder), fists bruise. */
static void injure(combat *c, health *h, int part, float damage, int edged)
{
    if (!edged) {
        health_blunt(h, part, 1.5 + 0.8 * (double)damage);
        return;
    }
    float severity = fminf(0.95f, damage / 20.0f);
    int arterial = severity > 0.35f && frand(c) < severity * 0.4f;
    health_cut(h, part, severity, arterial, 0.3f);
    if (part == BP_HEAD && damage >= BULLET_DAMAGE) health_blunt(h, part, 9.0);
}

combat_out combat_frame(combat *c, mobs *ms, physics *ph, const combat_targets *t, int who, inventory *inv, dvec3 eye,
                        vec3 dir, double block_dist, int attack_click, int use_click, float dt)
{
    combat_out o = {0};
    if (who < 0 || who >= PHYS_MAX_PLAYERS) return o;
    c->cooldown[who] -= dt;
    if (c->cooldown[who] > 0.0f) return o;
    const item_stack *held = inv_held(inv);
    int id = held->count ? held->id : 0;

    if (use_click && id == I_RIFLE) {
        if (!inv_take(inv, I_CARTRIDGE, 1)) {
            o.msg = "No cartridges";
            return o;
        }
        if (c->count < COMBAT_SHOTS) {
            /* A little spread: a rifle held standing, not clamped in a vice. */
            vec3 d = v3(dir.x + (frand(c) - 0.5f) * 0.004f, dir.y + (frand(c) - 0.5f) * 0.004f,
                        dir.z + (frand(c) - 0.5f) * 0.004f);
            c->b[c->count++] = (bullet){eye,
                                        dv3((double)d.x * BULLET_SPEED, (double)d.y * BULLET_SPEED,
                                            (double)d.z * BULLET_SPEED),
                                        BULLET_LIFE, who};
        }
        c->cooldown[who] = 1.2f; /* working the action */
        o.fired = 1;
        o.at = eye;
        int was = held->id;
        if (inv_wear_held(inv, 1)) o.tool_broke = was;
        return o;
    }

    if (!attack_click) return o;
    int mi = -1, pl = -1;
    double reach = block_dist < COMBAT_REACH ? block_dist : COMBAT_REACH;
    double d = nearest_target(ms, t, who, eye, dir, reach, &mi, &pl);
    if (mi < 0 && pl < 0) return o;
    float dmg = combat_blow(id);
    int kind = 0, tier = 0, edged = item_tool(id, &kind, &tier) && kind != TOOL_SHOVEL && kind != TOOL_PICKAXE;
    o.at = dv3(eye.x + (double)dir.x * d, eye.y + (double)dir.y * d, eye.z + (double)dir.z * d);
    if (mi >= 0) {
        o.killed = mobs_hurt(ms, ph, mi, dmg, eye);
    } else {
        injure(c, t->hl[pl], part_at(t->pl[pl], o.at.y, c), dmg, edged);
        t->pl[pl]->vel.x += (double)dir.x * 3.0;
        t->pl[pl]->vel.z += (double)dir.z * 3.0;
    }
    o.struck = 1;
    c->cooldown[who] = blow_time(id);
    if (item_durability(id)) {
        int was = held->id;
        if (inv_wear_held(inv, 1)) o.tool_broke = was;
    }
    return o;
}

int combat_creatures(combat *c, mobs *ms, physics *ph, const combat_targets *t, float daylight)
{
    mob_events ev = {0};
    mobs_step(ms, ph, t->pl, t->n, daylight, &ev);
    for (int k = 0; k < ev.bite_count; k++) {
        const mob_bite *b = &ev.bites[k];
        if (b->who < 0 || b->who >= t->n || !t->hl[b->who]) continue;
        /* A wolf's bite: punctures and tearing, dirty with what is in its
         * mouth, now and then deep enough to open a vessel. */
        health_cut(t->hl[b->who], b->part, 0.1f + 0.12f * frand(c), frand(c) < 0.03f, 0.7f);
    }
    return ev.bite_count + combat_step(c, ms, ph, t);
}

int combat_step(combat *c, mobs *ms, physics *ph, const combat_targets *t)
{
    int hits = 0;
    for (int i = 0; i < c->count; i++) {
        bullet *b = &c->b[i];
        b->vel.y -= GRAVITY * PHYS_DT;
        double speed = dv3_len(b->vel);
        b->life -= (float)PHYS_DT;
        int done = b->life <= 0.0f || speed < 1.0;
        if (!done) {
            double step = speed * PHYS_DT;
            vec3 dir = v3((float)(b->vel.x / speed), (float)(b->vel.y / speed), (float)(b->vel.z / speed));
            ray_hit wall = physics_raycast(ph->w, b->pos, dir, step);
            double wall_d = step;
            if (wall.hit) {
                dvec3 c0 = dv3(wall.block.x + 0.5, wall.block.y + 0.5, wall.block.z + 0.5);
                wall_d = fmin(step, dv3_len(dv3(c0.x - b->pos.x, c0.y - b->pos.y, c0.z - b->pos.z)));
            }
            int mi = -1, pl = -1;
            double d = nearest_target(ms, t, b->owner, b->pos, dir, wall_d, &mi, &pl);
            /* Damage falls with the square of the speed left: energy. */
            float frac = (float)(speed * speed / (BULLET_SPEED * BULLET_SPEED));
            if (mi >= 0) {
                mobs_hurt(ms, ph, mi, BULLET_DAMAGE * frac, b->pos);
                hits++;
                done = 1;
            } else if (pl >= 0) {
                double y = b->pos.y + (double)dir.y * d;
                injure(c, t->hl[pl], part_at(t->pl[pl], y, c), BULLET_DAMAGE * frac, 1);
                hits++;
                done = 1;
            } else if (wall.hit) {
                /* Glass and ice shatter; everything else stops it. */
                if (block_get(wall.id)->flags & BF_BRITTLE)
                    world_set(ph->w, wall.block.x, wall.block.y, wall.block.z, B_AIR, 0);
                done = 1;
            } else {
                b->pos = dv3(b->pos.x + b->vel.x * PHYS_DT, b->pos.y + b->vel.y * PHYS_DT,
                             b->pos.z + b->vel.z * PHYS_DT);
            }
        }
        if (done) c->b[i--] = c->b[--c->count];
    }
    return hits;
}
