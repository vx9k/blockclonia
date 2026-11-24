#include "survival.h"
#include "gamemode.h"
#include "mem.h"
#include "tagged.h"
#include "thermo.h"

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
    e->invulnerable = p->flying || !mode_body(p->mode);
    e->crouched = p->sneaking && p->on_ground;
    e->water_temp = SURVIVAL_WATER_TEMP;
    e->air_temp = e->contact_temp = SURVIVAL_AIR_TEMP;
    /* Standing on a burning campfire (or in one) puts the legs in flames. */
    if (!p->flying)
        for (int k = 0; k < 3 && !e->in_fire; k++)
            e->in_fire = block_at(w, dv3(p->pos.x, p->pos.y - 0.05 + 0.5 * k, p->pos.z)) == B_CAMPFIRE;
    uint8_t eye = block_at(w, dv3(p->pos.x, p->pos.y + PLAYER_EYE, p->pos.z));
    if (block_fluid(eye)) e->airway = AIRWAY_WATER; /* any liquid drowns */
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

void survival_impacts(health *h, const world *w, const player_contact *c, const player *p, const survival_before *b)
{
    if (p->flying || !mode_body(p->mode) || h->dead) return;
    const block_def *ground = block_get(block_at(w, dv3(p->pos.x, p->pos.y - 0.05, p->pos.z)));
    /* Landing on ground: the whole downward speed goes in one stop. */
    int landed = !b->on_ground && p->on_ground;
    if (landed && b->vel.y < -5.0) health_fall(h, -b->vel.y, (double)ground->cushion);
    /* Hitting water from height: water is soft only when entered slowly. */
    if (b->submerged < 0.25 && p->submerged >= 0.25 && b->vel.y < -12.0) health_fall(h, -b->vel.y, 0.55);
    /* Touching down with more sideways speed than legs can run out (a
     * sprinter lands jumps at ~6.5 m/s and keeps going), or knocked off the
     * feet by a hard landing: the body slides and rough ground takes the
     * skin off the legs, a hand and the hip (road rash). */
    double vh = hypot(b->vel.x, b->vel.z);
    float grit = p->submerged < 0.3 ? ground->grit : 0.0f;
    if (landed && vh > 6.0 && grit > 0.0f && (vh > 9.0 || -b->vel.y * (double)ground->cushion > 7.0)) {
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
    if (c->pinned_mass > 0.0f) {
        double m = (double)c->pinned_mass, y = (double)c->pinned_height;
        if (y >= 0.87) health_crush(h, BP_HEAD, m, PHYS_DT);
        else if (y >= 0.6) health_crush(h, BP_CHEST, m, PHYS_DT);
        else if (y >= 0.45) health_crush(h, BP_ABDOMEN, m, PHYS_DT);
        else {
            health_crush(h, BP_LLEG, 0.5 * m, PHYS_DT);
            health_crush(h, BP_RLEG, 0.5 * m, PHYS_DT);
        }
    }
    /* Falling blocks that struck the player. */
    for (int i = 0; i < c->hit_count; i++) {
        const player_hit *k = &c->hits[i];
        double soft = block_leaves(k->block)
                          ? 0.95
                          : (k->block == B_SNOW ? 0.7 : (k->block == B_SAND || k->block == B_GRAVEL ? 0.3 : 0.0));
        health_struck(h, (double)k->mass, (double)k->speed, (double)k->height, soft);
    }
}

void survival_limit_input(const health *h, const player *p, player_input *in)
{
    if (p->flying || !mode_body(p->mode)) return; /* flight and creative ignore the body */
    health_limits l = health_get_limits(h);
    if (!l.has_control) {
        memset(in, 0, sizeof *in);
        return;
    }
    /* Cap the direction at full speed before the body slows it. Physics
     * caps it too, but after this scaling that would stretch a hurt
     * player's diagonal (0.8, 0.8) back to full speed. */
    float len = sqrtf(in->forward * in->forward + in->right * in->right);
    float scale = len > 1.0f ? l.move_scale / len : l.move_scale;
    in->forward *= scale;
    in->right *= scale;
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

int survival_find_liquid(const world *w, dvec3 eye, vec3 dir, double reach, ipos *out)
{
    int steps = (int)(reach / 0.1);
    for (int k = 0; k <= steps; k++) {
        double t = 0.1 * k;
        int x = (int)floor(eye.x + (double)dir.x * t), y = (int)floor(eye.y + (double)dir.y * t),
            z = (int)floor(eye.z + (double)dir.z * t);
        uint8_t id = world_get(w, x, y, z);
        if (block_fluid(id)) {
            *out = (ipos){x, y, z};
            return 1;
        }
        if (block_solid(id)) return 0;
    }
    return 0;
}

static const tag_field PLAYER_F[] = {
    TAG_F64(player, pos),
    TAG_F64(player, prev_pos),
    TAG_F64(player, vel),
    TAG_F32R(player, yaw, -100.0, 100.0),
    TAG_F32R(player, pitch, -1.6, 1.6),
    TAG_I32R(player, on_ground, 0, 1),
    TAG_I32R(player, flying, 0, 1),
    TAG_I32R(player, mode, 0, MODE_COUNT - 1),
    TAG_F64R(player, submerged, 0.0, 1.0),
    TAG_I32R(player, sprinting, 0, 1),
    TAG_I32R(player, sneaking, 0, 1),
    TAG_I32R(player, hit_wall, 0, 1),
    TAG_F64R(player, fall_speed, -1000.0, 1000.0),
    TAG_F32R(player, height, 0.0, 3.0),
    TAG_F32R(player, half_w, 0.0, 2.0),
};
static const tag_table PLAYER_TAGS = TAG_TABLE(PLAYER_F);
_Static_assert(sizeof(player) == 136, "a field changed in player: update PLAYER_F (survival.c)");

static const uint8_t PLAYER_MAGIC[4] = {'M', 'C', 'P', 'R'};

static void sections(tag_section sec[3], const player *p, const inventory *inv, const health *h, player *pd,
                     inventory *invd, health *hd)
{
    sec[0] = (tag_section){"player", p, pd, &PLAYER_TAGS};
    sec[1] = (tag_section){"inventory", inv, invd, &g_inv_tags};
    sec[2] = (tag_section){"body", h, hd, &g_health_tags};
}

size_t survival_encode_player(const player *p, const inventory *inv, const health *h, uint8_t *out, size_t cap)
{
    tag_section sec[3] = {0};
    sections(sec, p, inv, h, NULL, NULL, NULL);
    wire_w w = wire_writer(out, cap);
    wire_bytes(&w, PLAYER_MAGIC, sizeof PLAYER_MAGIC);
    tag_put_sections(&w, sec, 3);
    return w.bad ? 0 : w.len;
}

/* A position physics can hold: x and z anywhere it clamps to
 * (+-WORLD_LIMIT), so a player saved at the edge is not thrown away. NaN
 * fails every comparison, so this also rejects it. */
static int pos_ok(dvec3 v)
{
    return fabs(v.x) <= WORLD_LIMIT && fabs(v.z) <= WORLD_LIMIT && v.y >= -64.0 && v.y <= WORLD_H + 64.0;
}

int survival_decode_player(player *p, inventory *inv, health *h, const uint8_t *in, size_t len)
{
    if (!in || len < sizeof PLAYER_MAGIC || memcmp(in, PLAYER_MAGIC, sizeof PLAYER_MAGIC) != 0) return -1;
    player tp = *p;
    inventory ti = *inv;
    /* Too big for the stack. */
    health *th = h ? mem_alloc(sizeof *th) : NULL;
    if (th) memcpy(th, h, sizeof *th);
    tag_section sec[3] = {0};
    sections(sec, NULL, NULL, NULL, &tp, &ti, th);
    wire_r r = wire_reader(in + sizeof PLAYER_MAGIC, len - sizeof PLAYER_MAGIC);
    tag_get_sections(&r, sec, 3);
    int ok = !r.bad && r.pos == r.len && pos_ok(tp.pos) && pos_ok(tp.prev_pos) && fabs(tp.vel.x) <= 1000.0 &&
             fabs(tp.vel.y) <= 1000.0 && fabs(tp.vel.z) <= 1000.0 && inv_valid(&ti);
    if (ok) {
        *p = tp;
        *inv = ti;
        if (th) {
            health_loaded(th);
            memcpy(h, th, sizeof *h);
        }
    }
    mem_free(th);
    return ok ? 0 : -1;
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

void survival_thermal(const thermo *th, const world *w, const player *p, health_env *e)
{
    dvec3 mid = dv3(p->pos.x, p->pos.y + 0.9, p->pos.z);
    double air = (double)thermo_air(th, w, mid.x, mid.y, mid.z);
    double radiant = p->flying ? 0.0 : thermo_radiant(th, w, mid);
    int fx = (int)floor(p->pos.x), fz = (int)floor(p->pos.z), fy = (int)floor(p->pos.y - 0.05);
    double contact = block_solid(world_get(w, fx, fy, fz)) ? (double)thermo_block_temp(th, w, fx, fy, fz) : air;
    survival_env_thermal(e, air, radiant, contact, 0);
    /* The liquid the body is in: lava where any of it touches lava. */
    e->water_temp = SURVIVAL_WATER_TEMP;
    for (int k = 0; k < 3; k++) {
        int y = (int)floor(p->pos.y + 0.1 + 0.75 * k);
        if (world_get(w, fx, y, fz) == B_LAVA) e->water_temp = THERMO_LAVA_C;
    }
}

void survival_drop_all(physics *ph, inventory *inv, const player *p, uint32_t *rng)
{
    item_stack left = inv_return_cursor(inv);
    if (left.count) inv_add_stack(inv, left);
    for (int i = 0; i < INV_SLOTS; i++) {
        item_stack *st = &inv->slot[i];
        if (!st->count) continue;
        *rng = *rng * 1664525u + 1013904223u;
        double a = (double)(*rng >> 8) / 16777216.0 * 2.0 * MC_PI;
        if (physics_drop_stack(ph, dv3(p->pos.x, p->pos.y + 1.0, p->pos.z), dv3(cos(a) * 2.0, 3.0, sin(a) * 2.0), *st,
                               1.0f))
            *st = (item_stack){0, 0, 0};
    }
}

void survival_toss(physics *ph, inventory *inv, const player *p, item_stack st)
{
    if (!st.count) return;
    vec3 d = look_dir(p->yaw, p->pitch);
    if (!physics_drop_stack(ph, dv3(p->pos.x, p->pos.y + 1.3, p->pos.z), dv3((double)d.x * 3.0, 1.5, (double)d.z * 3.0),
                            st, 1.2f)) {
        int left = inv_add_stack(inv, st);
        if (left) inv->cursor = (item_stack){st.id, (uint8_t)left, st.wear};
    }
}
