#include "interact.h"
#include "gamemode.h"
#include "survival.h"

#include <math.h>
#include <string.h>

#define REACH 5.0

void interact_init(interact *s, uint32_t seed)
{
    memset(s, 0, sizeof *s);
    s->rng = seed ? seed : 1u;
}

static float frand(interact *s)
{
    s->rng = s->rng * 1664525u + 1013904223u;
    return (float)(s->rng >> 8) / 16777216.0f;
}

int interact_box_hits_cell(aabb b, int x, int y, int z)
{ return b.min.x < x + 1 && b.max.x > x && b.min.y < y + 1 && b.max.y > y && b.min.z < z + 1 && b.max.z > z; }

static void spill_drops(interact *s, physics *ph, uint8_t block, item_id held, ipos at)
{
    item_stack drops[3] = {0};
    int n = item_harvests(held, block) ? item_drops(block, &s->rng, drops) : 0;
    for (int k = 0; k < n; k++) {
        dvec3 p = dv3(at.x + 0.5, at.y + 0.4, at.z + 0.5);
        dvec3 v = dv3((double)(frand(s) - 0.5f) * 2.0, 2.0 + (double)frand(s), (double)(frand(s) - 0.5f) * 2.0);
        physics_drop_item(ph, p, v, drops[k].id, drops[k].count, 0.3f);
    }
}

/* May a block go into cell t? Not inside the player or a falling body, and
 * only into air or liquid. */
static int can_place(const world *w, const physics *ph, const player *pl, ipos t)
{
    if (!pl->flying && interact_box_hits_cell(player_box(pl), t.x, t.y, t.z)) return 0;
    for (int i = 0; i < ph->body_count; i++) {
        aabb bb = {ph->bodies[i].pos, dv3_add(ph->bodies[i].pos, dv3(1, 1, 1))};
        if (interact_box_hits_cell(bb, t.x, t.y, t.z)) return 0;
    }
    uint8_t cur = world_get(w, t.x, t.y, t.z);
    return cur == B_AIR || block_fluid(cur) || block_plant(cur);
}

/* Plants growing on a block that is gone come up with it, roots and all,
 * and vines hanging from it come down. */
static void uproot(interact *s, world *w, physics *ph, const player *pl, ipos b)
{
    for (int dir = 1; dir >= -1; dir -= 2)
        for (ipos p = {b.x, b.y + dir, b.z};; p.y += dir) {
            uint8_t id = world_get(w, p.x, p.y, p.z);
            if (!block_plant(id) || (id == B_VINES) != (dir < 0)) break;
            world_set_player(w, p.x, p.y, p.z, B_AIR, 0);
            if (!mode_creative(pl->mode)) spill_drops(s, ph, id, 0, p);
        }
}

/* Plants take root in soil only. */
static int soil(uint8_t id)
{
    int m = block_get(id)->mat;
    return block_solid(id) && (m == BM_GRASS || m == BM_DIRT || m == BM_SAND);
}

static void emit(interact_out *o, int kind, uint8_t block, dvec3 at)
{
    if (o->ev_count < INTERACT_MAX_EVENTS) o->ev[o->ev_count++] = (interact_event){kind, block, at};
}

static dvec3 centre(ipos b) { return dv3(b.x + 0.5, b.y + 0.5, b.z + 0.5); }

static void do_break(interact *s, world *w, physics *ph, const player *pl, inventory *inv, fx_state *fx, ipos b,
                     uint8_t id, interact_out *o)
{
    emit(o, IE_BREAK, id, centre(b));
    world_set_player(w, b.x, b.y, b.z, B_AIR, 0);
    uproot(s, w, ph, pl, b);
    const block_def *d = block_get(id);
    fx_break(fx, id, b.x, b.y, b.z, (d->flags & BF_BRITTLE) ? 20 : 12);
    o->actions++;
    if (!mode_creative(pl->mode)) {
        const item_stack *held = inv_held(inv);
        spill_drops(s, ph, id, held->count ? held->id : 0, b);
        o->broke = 1;
        o->broken_id = id;
        if (held->count && item_durability(held->id)) {
            item_id was = held->id;
            if (inv_wear_held(inv, 1)) {
                o->tool_broke = was;
                o->msg = "Your tool wore out";
            }
        }
    }
    s->progress = 0.0f;
    s->has_target = 0;
    s->swing = 1.0f;
}

/* Fuel units a campfire gets from an item: a stick 1, planks 2, a log 4. */
static int fuel_units(item_id id)
{
    switch (id) {
    case I_STICK: return 1;
    case B_PLANKS: return 2;
    default: return block_log((uint8_t)id) && id < B_COUNT ? 4 : 0;
    }
}

static void use_item(interact *s, world *w, const physics *ph, thermo *th, const player *pl, inventory *inv, dvec3 eye,
                     vec3 dir, ray_hit hit, interact_out *o)
{
    const item_stack *held = inv_held(inv);
    int creative = mode_creative(pl->mode);
    /* Feeding a fire comes before placing: logs and planks are blocks too. */
    if (th && hit.hit && hit.id == B_CAMPFIRE && held->count && fuel_units(held->id)) {
        if (thermo_add_fuel(th, w, hit.block.x, hit.block.y, hit.block.z, fuel_units(held->id))) {
            if (!creative) inv_use_held(inv, 1);
            s->swing = 1.0f;
            o->msg = "Fed the fire";
            emit(o, IE_FEED, B_CAMPFIRE, centre(hit.block));
        } else {
            o->msg = "The fire has all the fuel it can take";
        }
        return;
    }
    if (held->id == I_BUCKET && held->count) {
        ipos c = {0};
        if (!survival_find_liquid(w, eye, dir, REACH, &c)) {
            o->msg = "No water in reach";
            return;
        }
        uint8_t liquid = world_get(w, c.x, c.y, c.z);
        if (!item_bucket(liquid)) {
            o->msg = "It would burn the bucket";
            return;
        }
        if (world_fluid_level(w, c.x, c.y, c.z) < FLUID_FULL) {
            o->msg = "Too shallow to fill the bucket";
            return;
        }
        world_set_player(w, c.x, c.y, c.z, B_AIR, 0); /* a bucket takes the whole cell: volume is conserved */
        inv_set_held(inv, item_bucket(liquid), 1);
        emit(o, IE_FILL, liquid, centre(c));
        s->swing = 1.0f;
        o->actions++;
        return;
    }
    if (held->id == I_DEFIBRILLATOR && held->count) {
        o->defib = 1;
        return;
    }
    if (!hit.hit || !held->count) {
        if (held->count && item_food(held->id, NULL) > 0.0f) o->eat = 1;
        return;
    }
    if (item_food(held->id, NULL) > 0.0f) {
        o->eat = 1;
        return;
    }
    const item_def *d = item_get(held->id);
    if (d->block == B_AIR) return;
    /* Aimed at a plant, a block takes its place. */
    ipos t = block_plant(hit.id) ? hit.block : hit.before;
    if (!can_place(w, ph, pl, t)) return;
    if (d->block == B_VINES) {
        uint8_t above = world_get(w, t.x, t.y + 1, t.z);
        if (!block_leaves(above) && !block_log(above) && above != B_VINES) {
            o->msg = "Vines hang from a tree";
            return;
        }
    } else if (block_plant(d->block) && !soil(world_get(w, t.x, t.y - 1, t.z))) {
        o->msg = "Plants need soil";
        return;
    }
    /* A bucket holds exactly one cell: pouring onto a liquid would
     * overwrite what is there and lose it. */
    if (block_fluid(d->block) && world_get(w, t.x, t.y, t.z) != B_AIR) {
        o->msg = "Pour onto dry ground";
        return;
    }
    /* A liquid in the cell is pushed up, not deleted (as when a falling
     * block lands in it); what does not fit above is lost to the
     * surroundings. */
    uint8_t was = world_get(w, t.x, t.y, t.z);
    int displaced = world_fluid_level(w, t.x, t.y, t.z);
    /* A log lies along the face it was set against. */
    uint8_t axis = !block_log(d->block) || (t.x == hit.block.x && t.z == hit.block.z) ? LOG_UP
                   : t.x != hit.block.x                                               ? LOG_X
                                                                                      : LOG_Z;
    world_set_player(w, t.x, t.y, t.z, d->block, axis);
    fluid_displace(w, t.x, t.y, t.z, was, displaced, 1);
    o->actions++;
    s->swing = 1.0f;
    emit(o, block_fluid(d->block) ? IE_POUR : IE_PLACE, d->block, centre(t));
    if (block_fluid(d->block) && held->id == item_bucket(d->block)) inv_set_held(inv, I_BUCKET, 1);
    else if (!creative) inv_use_held(inv, 1);
}

static void pick(inventory *inv, const player *pl, uint8_t id)
{
    item_id want = id;
    if (!item_valid(want)) return;
    for (int i = 0; i < INV_HOTBAR; i++)
        if (inv->slot[i].count && inv->slot[i].id == want) {
            inv->selected = i;
            return;
        }
    for (int i = INV_HOTBAR; i < INV_SLOTS; i++)
        if (inv->slot[i].count && inv->slot[i].id == want) {
            item_stack t = inv->slot[inv->selected];
            inv->slot[inv->selected] = inv->slot[i];
            inv->slot[i] = t;
            return;
        }
    if (mode_creative(pl->mode)) inv_set_held(inv, want, item_get(want)->stack);
}

interact_out interact_frame(interact *s, world *w, physics *ph, thermo *th, const player *pl, inventory *inv,
                            fx_state *fx, dvec3 eye, vec3 dir, ray_hit hit, const interact_input *in)
{
    interact_out o = {0};
    memset(&o, 0, sizeof o);
    s->swing = s->swing > 0.0f ? fmaxf(0.0f, s->swing - in->dt * 3.5f) : 0.0f;
    s->swinging = 0;

    /* The rifle's buttons fire and aim (combat.c), so it neither digs nor
     * uses what it points at. */
    const item_stack *carried = inv_held(inv);
    int gun = carried->count && carried->id == I_RIFLE;
    int breakable = hit.hit && block_get(hit.id)->hardness >= 0.0f && hit.id != B_BEDROCK;
    if (in->attack && !gun && in->can_act && breakable) {
        int same = s->has_target && s->target.x == hit.block.x && s->target.y == hit.block.y &&
                   s->target.z == hit.block.z && s->target_id == hit.id;
        if (!same) {
            s->has_target = 1;
            s->target = hit.block;
            s->target_id = hit.id;
            s->progress = 0.0f;
            s->chip_t = 0.0f;
        }
        s->swinging = 1;
        if (mode_creative(pl->mode)) {
            /* Creative: a click breaks at once; holding keeps going. */
            s->repeat -= in->dt;
            if (in->attack_click || s->repeat <= 0.0f) {
                do_break(s, w, ph, pl, inv, fx, hit.block, hit.id, &o);
                s->repeat = 0.22f;
            }
        } else {
            float hard = block_get(hit.id)->hardness;
            const item_stack *held = inv_held(inv);
            float speed = (in->speed > 0.05f ? in->speed : 0.05f) * item_dig_speed(held->count ? held->id : 0, hit.id);
            s->progress += hard > 0.0f ? in->dt * speed / hard : 1.0f;
            s->chip_t -= in->dt;
            if (s->chip_t <= 0.0f) {
                dvec3 at = dv3(hit.block.x + 0.5 + (hit.before.x - hit.block.x) * 0.52,
                               hit.block.y + 0.5 + (hit.before.y - hit.block.y) * 0.52,
                               hit.block.z + 0.5 + (hit.before.z - hit.block.z) * 0.52);
                fx_chip(fx, hit.id, at);
                emit(&o, IE_DIG, hit.id, at);
                s->chip_t = 0.25f;
                s->swing = 1.0f;
            }
            if (s->progress >= 1.0f) do_break(s, w, ph, pl, inv, fx, hit.block, hit.id, &o);
        }
    } else {
        s->has_target = 0;
        s->progress = 0.0f;
        s->repeat = 0.0f;
        if (in->attack_click && !gun) s->swing = 1.0f; /* a swing at nothing */
    }

    if (in->use_click && !gun && in->can_act) use_item(s, w, ph, th, pl, inv, eye, dir, hit, &o);
    if (in->pick_click && hit.hit) pick(inv, pl, hit.id);

    if (in->drop && in->can_act) {
        item_stack *held = inv_held(inv);
        if (held->count) {
            int n = in->drop == 2 ? held->count : 1;
            dvec3 p = dv3(eye.x + (double)dir.x * 0.4, eye.y - 0.3 + (double)dir.y * 0.4, eye.z + (double)dir.z * 0.4);
            dvec3 v = dv3(pl->vel.x + (double)dir.x * 4.5, pl->vel.y + (double)dir.y * 4.5 + 1.0,
                          pl->vel.z + (double)dir.z * 4.5);
            if (physics_drop_stack(ph, p, v, (item_stack){held->id, (uint8_t)n, held->wear}, 1.2f)) {
                emit(&o, IE_DROP, item_is_block(held->id) ? (uint8_t)held->id : 0, p);
                inv_use_held(inv, n);
            }
            s->swing = 1.0f;
        }
    }
    return o;
}
