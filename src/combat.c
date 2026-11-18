#include "combat.h"
#include "mathlib.h"
#include "noise.h"

#include <math.h>
#include <string.h>

#define BULLET_DAMAGE 14.0f
#define BULLET_JOULES 2100.0 /* the energy BULLET_DAMAGE stands for: the black-powder rifle's at the muzzle */
/* Only a backstop: a shot ends where it lands, hits or leaves the loaded
 * world, so its range comes from the flight. The longest any round flies
 * is about 35 s, a 7.62 mm shot at 37 degrees. */
#define BULLET_LIFE 60.0f
/* Steps of flight per physics step: a rifle bullet covers 7 to 14 m in
 * one, too far for one straight segment to follow its drop. */
#define BULLET_SUBSTEPS 4

void combat_init(combat *c, uint32_t seed)
{
    memset(c, 0, sizeof *c);
    c->rng = seed ? seed : 1u;
    c->wind_seed = hash_u32(c->rng ^ 0xA1B5u);
}

static float frand(combat *c)
{
    c->rng = c->rng * 1664525u + 1013904223u;
    return (float)(c->rng >> 8) / 16777216.0f;
}

float combat_blow(item_id held)
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
static float blow_time(item_id held)
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

/* Makes room for a shot when every slot is in flight by dropping the one
 * fired longest ago, so a trigger pull that took a cartridge always fires. */
static void shot_free_oldest(combat *c)
{
    int old = 0;
    for (int i = 1; i < c->count; i++)
        if (c->b[i].life < c->b[old].life) old = i;
    c->b[old] = c->b[--c->count];
}

combat_out combat_frame(combat *c, mobs *ms, physics *ph, const combat_targets *t, int who, inventory *inv, dvec3 eye,
                        vec3 dir, double block_dist, int attack_click, float aim, float dt)
{
    combat_out o = {0};
    if (who < 0 || who >= PHYS_MAX_PLAYERS) return o;
    c->cooldown[who] -= dt;
    if (c->cooldown[who] > 0.0f) return o;
    const item_stack *held = inv_held(inv);
    item_id id = held->count ? held->id : 0;

    if (attack_click && id == I_RIFLE) {
        if (!inv_take(inv, I_CARTRIDGE, 1)) {
            o.msg = "No cartridges";
            return o;
        }
        if (c->count == COMBAT_SHOTS) shot_free_oldest(c);
        if (c->count < COMBAT_SHOTS) {
            /* Spread: aimed, a rifle held standing (4 mrad, not clamped in a
             * vice); from the hip the eye isn't behind the sights and shots
             * wander by a few degrees. */
            float a = isfinite(aim) ? fminf(fmaxf(aim, 0.0f), 1.0f) : 0.0f;
            float sp = 0.004f + HIP_SPREAD * (1.0f - a);
            vec3 d = v3(dir.x + (frand(c) - 0.5f) * sp, dir.y + (frand(c) - 0.5f) * sp, dir.z + (frand(c) - 0.5f) * sp);
            double v = (double)ammo_get(CARTRIDGE_AMMO)->muzzle;
            c->b[c->count++] = (bullet){eye, dv3((double)d.x * v, (double)d.y * v, (double)d.z * v), BULLET_LIFE, who,
                                        CARTRIDGE_AMMO};
        }
        c->cooldown[who] = 1.2f; /* working the action */
        o.fired = 1;
        o.at = eye;
        item_id was = held->id;
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
        item_id was = held->id;
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

/* Is the column holding `p` loaded? Past it there is no terrain to hit, so
 * a shot that flew on would pass through hills that aren't there. */
static int shot_in_world(const world *w, dvec3 p)
{
    int x = (int)floor(p.x), z = (int)floor(p.z);
    return world_column(w, chunk_of(x), chunk_of(z)) != NULL;
}

/* One stretch of a bullet's flight, from its position to `to` at speed
 * `speed`: 1 if it hit something (and *hits counts a body), else 0. */
static int bullet_segment(combat *c, mobs *ms, physics *ph, const combat_targets *t, const bullet *b, dvec3 to,
                          double speed, int *hits)
{
    dvec3 seg = dv3(to.x - b->pos.x, to.y - b->pos.y, to.z - b->pos.z);
    double len = dv3_len(seg);
    if (len < 1e-9) return 0;
    vec3 dir = v3((float)(seg.x / len), (float)(seg.y / len), (float)(seg.z / len));
    ray_hit wall = physics_raycast(ph->w, b->pos, dir, len);
    double wall_d = len;
    if (wall.hit) {
        dvec3 c0 = dv3(wall.block.x + 0.5, wall.block.y + 0.5, wall.block.z + 0.5);
        wall_d = fmin(len, dv3_len(dv3(c0.x - b->pos.x, c0.y - b->pos.y, c0.z - b->pos.z)));
    }
    int mi = -1, pl = -1;
    double d = nearest_target(ms, t, b->owner, b->pos, dir, wall_d, &mi, &pl);
    /* Damage is the energy it still carries. */
    float dmg = BULLET_DAMAGE * (float)(0.5 * (double)ammo_get(b->ammo)->mass * speed * speed / BULLET_JOULES);
    if (mi >= 0) {
        mobs_hurt(ms, ph, mi, dmg, b->pos);
        (*hits)++;
        return 1;
    }
    if (pl >= 0) {
        double y = b->pos.y + (double)dir.y * d;
        injure(c, t->hl[pl], part_at(t->pl[pl], y, c), dmg, 1);
        (*hits)++;
        return 1;
    }
    if (wall.hit) {
        /* Glass and ice shatter; everything else stops it. */
        if (block_get(wall.id)->flags & BF_BRITTLE)
            world_set(ph->w, wall.block.x, wall.block.y, wall.block.z, B_AIR, 0);
        return 1;
    }
    return 0;
}

int combat_step(combat *c, mobs *ms, physics *ph, const combat_targets *t)
{
    int hits = 0;
    c->time += PHYS_DT;
    for (int i = 0; i < c->count; i++) {
        bullet *b = &c->b[i];
        b->life -= (float)PHYS_DT;
        const ammo_def *a = ammo_get(b->ammo);
        int done = b->life <= 0.0f || !a;
        if (!done) {
            /* The air where it is, once a step: its temperature, height
             * and the wind barely change over 14 m. */
            float temp = c->th ? thermo_ambient(c->th, ph->w, b->pos.x, b->pos.y, b->pos.z) : 15.0f;
            atmos_air air = atmos_at(b->pos.y, temp);
            air.wind = atmos_wind_at(ph->w, c->wind_seed, c->time, b->pos);
            for (int s = 0; s < BULLET_SUBSTEPS && !done; s++) {
                dvec3 to = b->pos, vel = b->vel;
                ballistics_step(a, &air, &to, &vel, PHYS_DT / BULLET_SUBSTEPS);
                double speed = dv3_len(b->vel);
                done = speed < 1.0 || !shot_in_world(ph->w, to) || bullet_segment(c, ms, ph, t, b, to, speed, &hits);
                b->pos = to;
                b->vel = vel;
            }
        }
        if (done) c->b[i--] = c->b[--c->count];
    }
    return hits;
}
