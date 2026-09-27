#include "interact.h"
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

static void spill_drops(interact *s, physics *ph, uint8_t block, ipos at)
{
    item_stack drops[3];
    int n = item_drops(block, &s->rng, drops);
    for (int k = 0; k < n; k++) {
        dvec3 p = dv3(at.x + 0.5, at.y + 0.4, at.z + 0.5);
        dvec3 v = dv3((double)(frand(s) - 0.5f) * 2.0, 2.0 + (double)frand(s), (double)(frand(s) - 0.5f) * 2.0);
        physics_drop_item(ph, p, v, drops[k].id, drops[k].count, 0.3f);
    }
}

/* May a block go into cell t? Not inside the player or a falling body, and
 * only into air or water. */
static int can_place(const world *w, const physics *ph, const player *pl, ipos t)
{
    if (!pl->flying && interact_box_hits_cell(player_box(pl), t.x, t.y, t.z)) return 0;
    for (int i = 0; i < ph->body_count; i++) {
        aabb bb = {ph->bodies[i].pos, dv3_add(ph->bodies[i].pos, dv3(1, 1, 1))};
        if (interact_box_hits_cell(bb, t.x, t.y, t.z)) return 0;
    }
    uint8_t cur = world_get(w, t.x, t.y, t.z);
    return cur == B_AIR || cur == B_WATER;
}

static void emit(interact_out *o, int kind, uint8_t block, dvec3 at)
{
    if (o->ev_count < INTERACT_MAX_EVENTS) o->ev[o->ev_count++] = (interact_event){kind, block, at};
}

static dvec3 centre(ipos b) { return dv3(b.x + 0.5, b.y + 0.5, b.z + 0.5); }

static void do_break(interact *s, world *w, physics *ph, const player *pl, fx_state *fx, ipos b, uint8_t id,
                     interact_out *o)
{
    emit(o, IE_BREAK, id, centre(b));
    world_set_player(w, b.x, b.y, b.z, B_AIR, 0);
    const block_def *d = block_get(id);
    fx_break(fx, id, b.x, b.y, b.z, (d->flags & BF_BRITTLE) ? 20 : 12);
    o->actions++;
    if (!pl->flying) {
        spill_drops(s, ph, id, b);
        o->broke = 1;
        o->broken_id = id;
    }
    s->progress = 0.0f;
    s->has_target = 0;
    s->swing = 1.0f;
}

/* Fuel units a campfire gets from an item: a stick 1, planks 2, a log 4. */
static int fuel_units(int id)
{
    switch (id) {
    case I_STICK: return 1;
    case B_PLANKS: return 2;
    case B_LOG: return 4;
    default: return 0;
    }
}

static void use_item(interact *s, world *w, physics *ph, thermo *th, const player *pl, inventory *inv, dvec3 eye,
                     vec3 dir, ray_hit hit, interact_out *o)
{
    item_stack *held = inv_held(inv);
    int creative = pl->flying;
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
        ipos c;
        if (!survival_find_water(w, eye, dir, REACH, &c)) {
            o->msg = "No water in reach";
            return;
        }
        if (world_water_level(w, c.x, c.y, c.z) < WATER_FULL) {
            o->msg = "Too shallow to fill the bucket";
            return;
        }
        world_set_player(w, c.x, c.y, c.z, B_AIR, 0); /* a bucket takes the whole cell: volume is conserved */
        inv_set_held(inv, I_WATER_BUCKET, 1);
        emit(o, IE_FILL, B_WATER, centre(c));
        s->swing = 1.0f;
        o->actions++;
        return;
    }
    if (!hit.hit || !held->count) {
        if (held->id == I_APPLE && held->count) o->eat = 1;
        return;
    }
    if (held->id == I_APPLE) {
        o->eat = 1;
        return;
    }
    const item_def *d = item_get(held->id);
    if (d->block == B_AIR) return;
    ipos t = hit.before;
    if (!can_place(w, ph, pl, t)) return;
    /* A bucket holds exactly one cell: pouring onto water would overwrite
     * what is there and lose it. */
    if (d->block == B_WATER && world_get(w, t.x, t.y, t.z) != B_AIR) {
        o->msg = "Pour onto dry ground";
        return;
    }
    /* Water in the cell is pushed up, not deleted (as when a falling block
     * lands in water); what does not fit above is lost to the surroundings. */
    int displaced = world_water_level(w, t.x, t.y, t.z);
    world_set_player(w, t.x, t.y, t.z, d->block, 0);
    if (displaced && t.y + 1 < WORLD_H) {
        uint8_t up = world_get(w, t.x, t.y + 1, t.z);
        if (up == B_AIR || up == B_WATER) {
            int total = displaced + world_water_level(w, t.x, t.y + 1, t.z);
            if (total > WATER_FULL) total = WATER_FULL;
            world_set_player(w, t.x, t.y + 1, t.z, B_WATER, (uint8_t)(total >= WATER_FULL ? 0 : total));
        }
    }
    o->actions++;
    s->swing = 1.0f;
    emit(o, d->block == B_WATER ? IE_POUR : IE_PLACE, d->block, centre(t));
    if (held->id == I_WATER_BUCKET) inv_set_held(inv, I_BUCKET, 1);
    else if (!creative) inv_use_held(inv, 1);
}

static void pick(inventory *inv, const player *pl, uint8_t id)
{
    int want = id;
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
    if (pl->flying) inv_set_held(inv, want, item_get(want)->stack);
}

interact_out interact_frame(interact *s, world *w, physics *ph, thermo *th, const player *pl, inventory *inv,
                            fx_state *fx, dvec3 eye, vec3 dir, ray_hit hit, const interact_input *in)
{
    interact_out o;
    memset(&o, 0, sizeof o);
    s->swing = s->swing > 0.0f ? fmaxf(0.0f, s->swing - in->dt * 3.5f) : 0.0f;
    s->swinging = 0;

    int breakable = hit.hit && block_get(hit.id)->hardness >= 0.0f && hit.id != B_BEDROCK;
    if (in->attack && in->can_act && breakable) {
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
        if (pl->flying) {
            /* Creative: a click breaks at once; holding keeps going. */
            s->repeat -= in->dt;
            if (in->attack_click || s->repeat <= 0.0f) {
                do_break(s, w, ph, pl, fx, hit.block, hit.id, &o);
                s->repeat = 0.22f;
            }
        } else {
            float hard = block_get(hit.id)->hardness;
            float speed = in->speed > 0.05f ? in->speed : 0.05f;
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
            if (s->progress >= 1.0f) do_break(s, w, ph, pl, fx, hit.block, hit.id, &o);
        }
    } else {
        s->has_target = 0;
        s->progress = 0.0f;
        s->repeat = 0.0f;
        if (in->attack_click) s->swing = 1.0f; /* a swing at nothing */
    }

    if (in->use_click && in->can_act) use_item(s, w, ph, th, pl, inv, eye, dir, hit, &o);
    if (in->pick_click && hit.hit) pick(inv, pl, hit.id);

    if (in->drop && in->can_act) {
        item_stack *held = inv_held(inv);
        if (held->count) {
            int n = in->drop == 2 ? held->count : 1;
            dvec3 p = dv3(eye.x + (double)dir.x * 0.4, eye.y - 0.3 + (double)dir.y * 0.4, eye.z + (double)dir.z * 0.4);
            dvec3 v = dv3(pl->vel.x + (double)dir.x * 4.5, pl->vel.y + (double)dir.y * 4.5 + 1.0,
                          pl->vel.z + (double)dir.z * 4.5);
            if (physics_drop_item(ph, p, v, held->id, n, 1.2f)) {
                emit(&o, IE_DROP, item_is_block(held->id) ? held->id : 0, p);
                inv_use_held(inv, n);
            }
            s->swing = 1.0f;
        }
    }
    return o;
}
