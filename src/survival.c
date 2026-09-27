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
    uint8_t eye = block_at(w, dv3(p->pos.x, p->pos.y + PLAYER_EYE, p->pos.z));
    if (eye == B_WATER) e->airway = AIRWAY_WATER;
    else if (block_solid(eye) && block_opaque(eye)) e->airway = AIRWAY_BLOCKED; /* buried */
    else e->airway = AIRWAY_AIR;
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

void survival_impacts(health *h, const world *w, const physics *ph, const player *p, const survival_before *b)
{
    if (p->flying || h->dead) return;
    /* Landing on ground: the whole downward speed goes in one stop. */
    if (!b->on_ground && p->on_ground && b->vel.y < -5.0) health_fall(h, -b->vel.y, cushion_under(w, p));
    /* Hitting water from height: water is soft only when entered slowly. */
    if (b->submerged < 0.25 && p->submerged >= 0.25 && b->vel.y < -12.0) health_fall(h, -b->vel.y, 0.55);
    /* Running or falling sideways into a wall. */
    double dvh = hypot(b->vel.x - p->vel.x, b->vel.z - p->vel.z);
    if (dvh > 6.0 && p->submerged < 0.5) health_blunt(h, BP_CHEST, dvh);
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
    health_scavenge(h, id);
    if (id == B_GLASS) {
        /* Smashing glass bare-handed: usually a gash, sometimes worse. */
        uint32_t r = h->rng = h->rng * 1664525u + 1013904223u;
        float sev = 0.25f + 0.45f * (float)(r >> 8) / 16777216.0f;
        int arterial = (r & 0xff) < 20; /* about 8% */
        health_cut(h, BP_RARM, sev, arterial, 0.15f);
    }
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
