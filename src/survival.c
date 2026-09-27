#include "survival.h"

#include <math.h>
#include <string.h>

survival_before survival_capture(const player *p)
{
    survival_before b = {p->vel, p->on_ground, p->submerged};
    return b;
}

static uint8_t block_at(const world *w, dvec3 p)
{
    return world_get(w, (int)floor(p.x), (int)floor(p.y), (int)floor(p.z));
}

void survival_env(const world *w, const player *p, const survival_before *b, int actions, health_env *e)
{
    memset(e, 0, sizeof *e);
    e->speed = hypot(p->vel.x, p->vel.z);
    e->submerged = p->flying ? 0.0 : p->submerged;
    e->on_ground = p->on_ground;
    e->jumped = b->on_ground && !p->on_ground && p->vel.y > 3.0;
    e->actions = actions;
    e->invulnerable = p->flying;
    e->water_temp = SURVIVAL_WATER_TEMP;
    e->air_temp = e->contact_temp = SURVIVAL_AIR_TEMP;
    /* Standing on a burning campfire (or in one) puts the legs in flames. */
    if (!p->flying)
        for (int k = 0; k < 3 && !e->in_fire; k++)
            e->in_fire = block_at(w, dv3(p->pos.x, p->pos.y - 0.05 + 0.5 * k, p->pos.z)) == B_CAMPFIRE;
    uint8_t eye = block_at(w, dv3(p->pos.x, p->pos.y + PLAYER_EYE, p->pos.z));
    if (eye == B_WATER) e->airway = AIRWAY_WATER;
    else if (block_solid(eye) && block_opaque(eye)) e->airway = AIRWAY_BLOCKED; /* buried */
    else e->airway = AIRWAY_AIR;
}

void survival_env_thermal(health_env *e, double air_temp, double radiant, double contact_temp, int in_fire)
{
    e->air_temp = air_temp;
    e->radiant = radiant > 0.0 ? radiant : 0.0;
    e->contact_temp = contact_temp;
    e->in_fire |= in_fire != 0;
}

/* How much of a landing's speed the ground takes out gently. */
static double cushion_under(const world *w, const player *p)
{
    uint8_t id = block_at(w, dv3(p->pos.x, p->pos.y - 0.05, p->pos.z));
    switch (id) {
    case B_LEAVES: return 0.35; /* mostly air and springy twigs */
    case B_SNOW: return 0.6;
    case B_SAND:
    case B_GRAVEL: return 0.85;
    case B_DIRT:
    case B_GRASS: return 0.92;
    default: return 1.0;
    }
}

/* How much grit a surface grinds into scraped skin: 0 for surfaces that
 * do not take skin off (snow, ice, leaves, water). */
static float grit_under(const world *w, const player *p)
{
    uint8_t id = block_at(w, dv3(p->pos.x, p->pos.y - 0.05, p->pos.z));
    switch (id) {
    case B_GRAVEL: return 0.85f;
    case B_SAND: return 0.6f;
    case B_DIRT:
    case B_GRASS:
    case B_ASH: return 0.55f;
    case B_STONE:
    case B_BEDROCK:
    case B_BRICK: return 0.4f;
    case B_PLANKS:
    case B_LOG: return 0.3f;
    default: return 0.0f;
    }
}

void survival_impacts(health *h, const world *w, const physics *ph, const player *p, const survival_before *b)
{
    if (p->flying || h->dead) return;
    /* Landing on ground: the whole downward speed goes in one stop. */
    int landed = !b->on_ground && p->on_ground;
    if (landed && b->vel.y < -5.0) health_fall(h, -b->vel.y, cushion_under(w, p));
    /* Hitting water from height: water is soft only when entered slowly. */
    if (b->submerged < 0.25 && p->submerged >= 0.25 && b->vel.y < -12.0) health_fall(h, -b->vel.y, 0.55);
    /* Touching down with more sideways speed than legs can run out (a
     * sprinter lands jumps at ~6.5 m/s and keeps going), or knocked off the
     * feet by a hard landing: the body slides and rough ground takes the
     * skin off the legs, a hand and the hip (road rash). */
    double vh = hypot(b->vel.x, b->vel.z);
    float grit = p->submerged < 0.3 ? grit_under(w, p) : 0.0f;
    if (landed && vh > 6.0 && grit > 0.0f && (vh > 9.0 || -b->vel.y * cushion_under(w, p) > 7.0)) {
        float area = 0.1f + 0.3f * (float)fmin((vh - 6.0) / 10.0, 1.0);
        health_abrasion(h, BP_LLEG, area, grit);
        health_abrasion(h, BP_RLEG, area, grit);
        health_abrasion(h, BP_RARM, 0.7f * area, grit);
        if (vh > 10.0) health_abrasion(h, BP_ABDOMEN, 0.5f * area, grit);
    }
    /* Running or falling sideways into a wall. */
    double dvh = hypot(b->vel.x - p->vel.x, b->vel.z - p->vel.z);
    if (dvh > 6.0 && p->submerged < 0.5) {
        health_blunt(h, BP_CHEST, dvh);
        /* Stopped dead on the ground: the hands go out and scrape. */
        if (b->on_ground && p->on_ground) health_abrasion(h, BP_RARM, 0.08f, 0.3f);
    }
    /* Blocks resting on the player: the weight bears where they lie. */
    if (ph->pinned_mass > 0.0f) {
        double m = (double)ph->pinned_mass, y = (double)ph->pinned_height;
        if (y >= 0.87) health_crush(h, BP_HEAD, m, PHYS_DT);
        else if (y >= 0.6) health_crush(h, BP_CHEST, m, PHYS_DT);
        else if (y >= 0.45) health_crush(h, BP_ABDOMEN, m, PHYS_DT);
        else {
            health_crush(h, BP_LLEG, 0.5 * m, PHYS_DT);
            health_crush(h, BP_RLEG, 0.5 * m, PHYS_DT);
        }
    }
    /* Falling blocks that struck the player. */
    for (int i = 0; i < ph->hit_count; i++) {
        const player_hit *k = &ph->hits[i];
        double soft = k->block == B_LEAVES
                          ? 0.95
                          : (k->block == B_SNOW ? 0.7 : (k->block == B_SAND || k->block == B_GRAVEL ? 0.3 : 0.0));
        health_struck(h, (double)k->mass, (double)k->speed, (double)k->height, soft);
    }
}

void survival_limit_input(const health *h, const player *p, player_input *in)
{
    if (p->flying) return; /* creative flight ignores the body */
    health_limits l = health_get_limits(h);
    if (!l.has_control) {
        memset(in, 0, sizeof *in);
        return;
    }
    in->forward *= l.move_scale;
    in->right *= l.move_scale;
    if (!l.can_sprint) in->sprint = 0;
    /* In water, "jump" is a swim stroke: it needs arms, not legs. */
    if (p->submerged > 0.3) {
        if (!l.can_act) in->jump = 0;
    } else if (!l.can_jump) {
        in->jump = 0;
    }
}

void survival_on_break(health *h, uint8_t id)
{
    if (h->dead) return;
    if (id == B_GLASS) {
        /* Smashing glass bare-handed: usually a gash, sometimes worse. */
        uint32_t r = h->rng = h->rng * 1664525u + 1013904223u;
        float sev = 0.25f + 0.45f * (float)(r >> 8) / 16777216.0f;
        int arterial = (r & 0xff) < 20; /* about 8% */
        health_cut(h, BP_RARM, sev, arterial, 0.15f);
    }
}

int survival_find_water(const world *w, dvec3 eye, vec3 dir, double reach, ipos *out)
{
    int steps = (int)(reach / 0.1);
    for (int k = 0; k <= steps; k++) {
        double t = 0.1 * k;
        int x = (int)floor(eye.x + (double)dir.x * t), y = (int)floor(eye.y + (double)dir.y * t),
            z = (int)floor(eye.z + (double)dir.z * t);
        uint8_t id = world_get(w, x, y, z);
        if (id == B_WATER) {
            *out = (ipos){x, y, z};
            return 1;
        }
        if (block_solid(id)) return 0;
    }
    return 0;
}

/* Little-endian doubles and floats by their bit patterns. */
static void put64(uint8_t *p, double v)
{
    uint64_t u;
    memcpy(&u, &v, 8);
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(u >> (8 * i));
}

static double get64(const uint8_t *p)
{
    uint64_t u = 0;
    for (int i = 0; i < 8; i++) u |= (uint64_t)p[i] << (8 * i);
    double v;
    memcpy(&v, &u, 8);
    return v;
}

static void put32f(uint8_t *p, float v)
{
    uint32_t u;
    memcpy(&u, &v, 4);
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(u >> (8 * i));
}

static float get32f(const uint8_t *p)
{
    uint32_t u = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    float v;
    memcpy(&v, &u, 4);
    return v;
}

/* "MCPL", u8 version (1), 3 reserved, x y z (f64), yaw pitch (f32), u8
 * flying, 1 reserved, u16 time of day (1/65536 day), then the inventory
 * block. */
size_t survival_encode_player(const player *p, const inventory *inv, double day, uint8_t *out, size_t cap)
{
    if (cap < PLAYER_FILE_SIZE) return 0;
    static const uint8_t MAGIC[4] = {'M', 'C', 'P', 'L'};
    memset(out, 0, PLAYER_FILE_SIZE);
    memcpy(out, MAGIC, sizeof MAGIC);
    out[4] = 1;
    put64(out + 8, p->pos.x);
    put64(out + 16, p->pos.y);
    put64(out + 24, p->pos.z);
    put32f(out + 32, p->yaw);
    put32f(out + 36, p->pitch);
    out[40] = (uint8_t)(p->flying != 0);
    double frac = day - floor(day);
    uint32_t d16 = (uint32_t)(frac >= 0.0 && frac < 1.0 ? frac * 65536.0 : 0.0) & 0xffffu;
    out[42] = (uint8_t)d16;
    out[43] = (uint8_t)(d16 >> 8);
    if (inv_encode(inv, out + 44, cap - 44) != INV_ENCODED_SIZE) return 0;
    return PLAYER_FILE_SIZE;
}

int survival_decode_player(player *p, inventory *inv, double *day, const uint8_t *in, size_t len)
{
    if (len != PLAYER_FILE_SIZE || memcmp(in, "MCPL", 4) != 0 || in[4] != 1 || in[40] > 1) return -1;
    double x = get64(in + 8), y = get64(in + 16), z = get64(in + 24);
    float yaw = get32f(in + 32), pitch = get32f(in + 36);
    /* NaN fails every comparison, so these also reject it. */
    if (!(fabs(x) <= WORLD_LIMIT - 64) || !(fabs(z) <= WORLD_LIMIT - 64) || !(y >= -64.0 && y <= WORLD_H + 64.0) ||
        !(fabsf(yaw) <= 100.0f) || !(fabsf(pitch) <= 1.6f))
        return -1;
    inventory t;
    if (inv_decode(&t, in + 44, INV_ENCODED_SIZE) != 0) return -1;
    *inv = t;
    memset(p, 0, sizeof *p);
    p->pos = p->prev_pos = dv3(x, y, z);
    p->yaw = yaw;
    p->pitch = pitch;
    p->flying = in[40];
    if (day) *day = (double)((uint32_t)in[42] | (uint32_t)in[43] << 8) / 65536.0;
    return 0;
}

int survival_water_nearby(const world *w, const player *p, vec3 dir)
{
    if (p->submerged > 0.1) return 1;
    dvec3 eye = dv3(p->pos.x, p->pos.y + PLAYER_EYE, p->pos.z);
    for (int k = 0; k <= 15; k++) { /* 20 cm steps out to 3 m */
        double t = 0.2 * k;
        uint8_t id = block_at(w, dv3(eye.x + (double)dir.x * t, eye.y + (double)dir.y * t, eye.z + (double)dir.z * t));
        if (id == B_WATER) return 1;
        if (block_solid(id)) break;
    }
    return 0;
}
