#include "fx.h"
#include "item.h"
#include "itemmodel.h"
#include "light.h"

#include <math.h>
#include <string.h>

void fx_init(fx_state *f, uint32_t seed)
{
    f->count = 0;
    f->rng = seed ? seed : 1u;
    f->wind = dv3(0.0, 0.0, 0.0);
    f->flash = 0.0f;
}

static float frand(fx_state *f)
{
    f->rng = f->rng * 1664525u + 1013904223u;
    return (float)(f->rng >> 8) / 16777216.0f;
}

static float srand1(fx_state *f) { return frand(f) * 2.0f - 1.0f; }

static particle *spawn(fx_state *f)
{
    int best = f->count;
    if (f->count < FX_MAX_PARTICLES) {
        f->count++;
    } else {
        /* Full: recycle the oldest-looking one (least life left). */
        best = 0;
        for (int i = 1; i < f->count; i++)
            if (f->p[i].life < f->p[best].life) best = i;
    }
    /* Cleared: a free slot still holds the last particle moved out of it,
     * and a puff of smoke must not come back as debris's kind or spin. */
    f->p[best] = (particle){0};
    return &f->p[best];
}

void fx_break(fx_state *f, uint8_t block, int x, int y, int z, int n)
{
    const block_def *d = block_get(block);
    int brittle = (d->flags & BF_BRITTLE) != 0;
    for (int i = 0; i < n; i++) {
        particle *p = spawn(f);
        p->pos = p->prev_pos = dv3(x + 0.15 + 0.7 * (double)frand(f), y + 0.15 + 0.7 * (double)frand(f),
                                   z + 0.15 + 0.7 * (double)frand(f));
        dvec3 out = dv3(p->pos.x - (x + 0.5), p->pos.y - (y + 0.5), p->pos.z - (z + 0.5));
        double speed = brittle ? 3.5 : 2.0;
        p->vel = dv3(out.x * speed * 2.0 + (double)srand1(f) * 0.6, 1.5 + (double)frand(f) * 2.0 + out.y * speed,
                     out.z * speed * 2.0 + (double)srand1(f) * 0.6);
        p->max_life = p->life = 0.7f + frand(f) * 0.8f;
        p->size = brittle ? 0.04f + 0.05f * frand(f) : 0.07f + 0.08f * frand(f);
        p->spin = frand(f) * 6.28f;
        p->spin_rate = srand1(f) * 12.0f;
        /* Mostly side texture; some from the top so grass shows green bits. */
        p->tex = frand(f) < 0.3f ? d->tex[2] : d->tex[0];
        p->bounce = (uint8_t)(d->restitution * 255.0f);
    }
}

void fx_chip(fx_state *f, uint8_t block, dvec3 at)
{
    const block_def *d = block_get(block);
    for (int i = 0; i < 2; i++) {
        particle *p = spawn(f);
        p->pos = p->prev_pos = at;
        p->vel = dv3((double)srand1(f) * 1.5, 1.0 + (double)frand(f) * 1.5, (double)srand1(f) * 1.5);
        p->max_life = p->life = 0.4f + frand(f) * 0.3f;
        p->size = 0.05f + 0.04f * frand(f);
        p->spin = frand(f) * 6.28f;
        p->spin_rate = srand1(f) * 10.0f;
        p->tex = d->tex[0];
        p->bounce = 40;
    }
}

void fx_splash(fx_state *f, dvec3 at, double speed)
{
    int n = (int)fmin(24.0, 4.0 + speed * 1.5);
    for (int i = 0; i < n; i++) {
        particle *p = spawn(f);
        p->pos = p->prev_pos = dv3(at.x + (double)srand1(f) * 0.3, at.y, at.z + (double)srand1(f) * 0.3);
        double up = fmin(speed * 0.35, 5.0);
        p->vel = dv3((double)srand1(f) * 1.2, up * (0.5 + (double)frand(f) * 0.6), (double)srand1(f) * 1.2);
        p->max_life = p->life = 0.5f + frand(f) * 0.4f;
        p->size = 0.05f + 0.05f * frand(f);
        p->spin = 0.0f;
        p->spin_rate = 0.0f;
        p->tex = T_WATER;
        p->bounce = 0;
    }
}

/* Light given to a flash's sparks: past full, so the tone curve whitens
 * them whatever the light around. */
#define FX_GLOW_LIGHT 3.0f

void fx_muzzle(fx_state *f, dvec3 at, dvec3 dir, int black)
{
    /* The flash: hot gas a hand's length past the muzzle, flung forward and
     * stopped by the air within a few centimetres. */
    for (int i = 0; i < 6; i++) {
        particle *p = spawn(f);
        double ahead = 0.05 + 0.25 * (double)frand(f);
        p->pos = p->prev_pos = dv3(at.x + dir.x * ahead, at.y + dir.y * ahead, at.z + dir.z * ahead);
        p->vel = dv3(dir.x * 20.0 + (double)srand1(f) * 4.0, dir.y * 20.0 + (double)srand1(f) * 4.0,
                     dir.z * 20.0 + (double)srand1(f) * 4.0);
        p->max_life = p->life = 0.04f + 0.03f * frand(f);
        p->size = 0.05f + 0.07f * frand(f);
        p->spin = frand(f) * 6.28f;
        p->tex = T_FLASH;
        p->kind = FXP_GLOW;
    }
    /* The smoke leaves fast and is stopped by the air within a metre or
     * two; black powder's hangs in a cloud for several seconds. */
    int n = black ? 14 : 3;
    for (int i = 0; i < n; i++) {
        particle *p = spawn(f);
        double ahead = 0.1 + 0.4 * (double)frand(f), v = (black ? 6.0 : 3.0) * (0.4 + (double)frand(f));
        p->pos = p->prev_pos = dv3(at.x + dir.x * ahead + (double)srand1(f) * 0.05,
                                   at.y + dir.y * ahead + (double)srand1(f) * 0.05,
                                   at.z + dir.z * ahead + (double)srand1(f) * 0.05);
        p->vel = dv3(dir.x * v + (double)srand1(f) * 0.8, dir.y * v + (double)srand1(f) * 0.8,
                     dir.z * v + (double)srand1(f) * 0.8);
        p->max_life = p->life = black ? 5.0f + 4.0f * frand(f) : 1.2f + frand(f);
        p->size = black ? 0.18f + 0.14f * frand(f) : 0.08f + 0.05f * frand(f);
        p->spin = frand(f) * 6.28f;
        p->spin_rate = srand1(f) * 0.6f;
        p->tex = T_SMOKE0;
        p->kind = FXP_SMOKE;
    }
}

void fx_step(fx_state *f, const world *w, double dt)
{
    for (int i = 0; i < f->count; i++) {
        particle *p = &f->p[i];
        p->life -= (float)dt;
        if (p->life <= 0.0f) {
            f->p[i--] = f->p[--f->count];
            continue;
        }
        p->prev_pos = p->pos;
        if (p->kind == FXP_SMOKE) {
            /* The air carries smoke: its velocity settles to the wind's in
             * about half a second, and the warm cloud rises a little. */
            double k = 1.0 - exp(-2.0 * dt);
            p->vel = dv3(p->vel.x + (f->wind.x - p->vel.x) * k, p->vel.y + (f->wind.y + 0.25 - p->vel.y) * k,
                         p->vel.z + (f->wind.z - p->vel.z) * k);
        } else if (p->kind == FXP_GLOW) {
            p->vel = dv3_scale(p->vel, exp(-12.0 * dt));
        } else {
            p->vel.y -= 9.81 * dt;
            double drag = 1.0 / (1.0 + 1.5 * dt);
            p->vel = dv3_scale(p->vel, drag);
        }
        dvec3 next = dv3_add(p->pos, dv3_scale(p->vel, dt));
        /* Axis by axis against solid cells: a cheap bounce. */
        double e = (double)p->bounce / 255.0;
        if (block_solid(world_get(w, (int)floor(next.x), (int)floor(p->pos.y), (int)floor(p->pos.z)))) {
            p->vel.x = -p->vel.x * e;
            next.x = p->pos.x;
        }
        if (block_solid(world_get(w, (int)floor(next.x), (int)floor(next.y), (int)floor(p->pos.z)))) {
            if (p->vel.y < 0) {
                p->vel.x *= 0.6;
                p->vel.z *= 0.6;
                p->spin_rate *= 0.5f;
            }
            p->vel.y = -p->vel.y * e;
            next.y = p->pos.y;
        }
        if (block_solid(world_get(w, (int)floor(next.x), (int)floor(next.y), (int)floor(next.z)))) {
            p->vel.z = -p->vel.z * e;
            next.z = p->pos.z;
        }
        p->pos = next;
        p->spin += p->spin_rate * (float)dt;
    }
}

static entity_instance cube_at(dvec3 p, dvec3 eye, uint32_t tex, quat q, float sx, float sy, float sz, float light)
{
    entity_instance e = {0};
    e.pos[0] = (float)(p.x - eye.x);
    e.pos[1] = (float)(p.y - eye.y);
    e.pos[2] = (float)(p.z - eye.z);
    e.tex = tex;
    entity_set_rot(&e, q);
    e.scale[0] = sx;
    e.scale[1] = sy;
    e.scale[2] = sz;
    e.light = light;
    return e;
}

static uint32_t block_tex(uint8_t b)
{
    const block_def *d = block_get(b);
    return (uint32_t)d->tex[0] | (uint32_t)d->tex[2] << 8 | (uint32_t)d->tex[3] << 16;
}

/* m: the longest side a dropped item is shown at, at least (a cartridge is 7 cm). */
#define DROP_SIZE 0.25f

static uint32_t same_tex(uint8_t layer) { return (uint32_t)layer * 0x010101u; }

/* The output array is shared: opaque instances fill it from the front,
 * translucent ones from the back, and the translucent block is moved down
 * behind the opaque one at the end. Neither kind has a cap of its own. */
typedef struct {
    entity_instance *out;
    int no, tb, max; /* opaque in [0, no), translucent in [tb, max) */
} ent_list;

static void put_opaque(ent_list *l, entity_instance e)
{
    if (l->no < l->tb) l->out[l->no++] = e;
}

static void put_trans(ent_list *l, entity_instance e)
{
    if (l->tb > l->no) l->out[--l->tb] = e;
}

/* The brightness at p: its cell's light, or daylight without a world. */
static float lit(const physics *ph, dvec3 p, float daylight)
{
    if (!ph || !ph->w) return daylight;
    return light_at(ph->w, p.x, p.y, p.z, daylight);
}

void fx_build_entities(const fx_state *f, const physics *ph, const fx_crack *crack, dvec3 eye, double alpha, float time,
                       float daylight, entity_instance *out, int max, int *n_opaque, int *n_trans)
{
    ent_list l = {out, 0, max > 0 ? max : 0, max > 0 ? max : 0};
    /* The crack goes first: the block being broken always shows it. */
    if (crack && crack->active && crack->progress > 0.0f) {
        int stage = (int)(crack->progress * 4.0f);
        stage = stage < 0 ? 0 : (stage > 3 ? 3 : stage);
        dvec3 c = dv3(crack->x + 0.5, crack->y + 0.5, crack->z + 0.5);
        /* Lit by the cell in front of the face looked at, not the solid one. */
        float light = 0.0f;
        for (int d = 0; d < 6; d++) {
            dvec3 n = dv3(c.x + (d == 0) - (d == 1), c.y + (d == 2) - (d == 3), c.z + (d == 4) - (d == 5));
            light = fmaxf(light, lit(ph, n, daylight));
        }
        put_trans(&l, cube_at(c, eye, same_tex((uint8_t)(T_CRACK0 + stage)), quat_identity(), 1.004f, 1.004f, 1.004f,
                              light));
    }
    for (int i = 0; ph && i < ph->item_count; i++) {
        const item_ent *it = &ph->items[i];
        dvec3 p = dv3_lerp(it->prev_pos, it->pos, alpha);
        /* Resting items hover and bob a little; everything spins. */
        float bob = it->on_ground ? 0.05f + 0.04f * sinf(time * 2.2f + (float)i) : 0.0f;
        p.y += (double)bob;
        quat q = quat_axis(0, 1, 0, it->spin);
        float light = lit(ph, p, daylight);
        int stacks = it->count > 32 ? 3 : (it->count > 1 ? 2 : 1); /* a pile looks like a pile */
        item_model mdl = {0};
        int has_model = itemmodel_build(it->id, &mdl);
        for (int k = 0; k < stacks; k++) {
            dvec3 pk = dv3(p.x + 0.05 * k, p.y + 0.04 * k, p.z - 0.04 * k);
            if (has_model) {
                /* Its model, centred where the item is, small things shown
                 * larger so they can be found; tools and guns lie flat. */
                const float at[3] = {(float)(pk.x - eye.x), (float)(pk.y - eye.y), (float)(pk.z - eye.z)};
                float c[3] = {0.0f, 0.0f, 0.0f};
                itemmodel_centre(&mdl, c);
                float size = fminf(fmaxf(DROP_SIZE / itemmodel_extent(&mdl), 1.0f), 3.0f);
                quat lie = mdl.hold == IM_HOLD_PALM ? q : quat_mul(q, quat_axis(1.0f, 0.0f, 0.0f, 1.5708f));
                entity_instance im[IM_MAX_BOXES] = {0};
                int nb = itemmodel_place(&mdl, c, at, lie, size, light, im, IM_MAX_BOXES);
                for (int b = 0; b < nb; b++) {
                    if (itemmodel_layer_clear((uint8_t)(im[b].tex & 255u))) put_trans(&l, im[b]);
                    else put_opaque(&l, im[b]);
                }
                continue;
            }
            uint8_t b = (uint8_t)it->id; /* a block item's id is its block */
            entity_instance e = cube_at(pk, eye, block_tex(b), q, 0.25f, 0.25f, 0.25f, light);
            if (block_get(b)->flags & BF_TRANSLUCENT) put_trans(&l, e);
            else put_opaque(&l, e);
        }
    }
    for (int i = 0; f && i < f->count; i++) {
        const particle *pt = &f->p[i];
        dvec3 p = dv3_lerp(pt->prev_pos, pt->pos, alpha);
        /* Shrink over the last third of the particle's life. */
        float life = pt->life / pt->max_life;
        float s = pt->size * (life < 0.33f ? life * 3.0f : 1.0f);
        quat q = quat_axis(0.3f, 1.0f, 0.2f, pt->spin);
        if (pt->kind == FXP_SMOKE) {
            /* A puff spreads as it thins: three times its size and two
             * steps fainter by the end. */
            float age = 1.0f - life, ss = pt->size * (1.0f + 2.0f * age);
            uint8_t tex = (uint8_t)(T_SMOKE0 + (age < 0.3f ? 0 : (age < 0.65f ? 1 : 2)));
            float light = fmaxf(0.9f * lit(ph, p, daylight), f->flash);
            put_trans(&l, cube_at(p, eye, same_tex(tex), q, ss, ss, ss, light));
            continue;
        }
        if (pt->kind == FXP_GLOW) {
            put_opaque(&l, cube_at(p, eye, same_tex(pt->tex), q, pt->size, pt->size, pt->size, FX_GLOW_LIGHT));
            continue;
        }
        entity_instance e = cube_at(p, eye, same_tex(pt->tex), q, s, s, s, 0.9f * lit(ph, p, daylight));
        if (pt->tex == T_WATER || pt->tex == T_GLASS || pt->tex == T_ICE) put_trans(&l, e);
        else put_opaque(&l, e);
    }
    int nt = l.max - l.tb;
    memmove(out + l.no, out + l.tb, (size_t)nt * sizeof(entity_instance));
    *n_opaque = l.no;
    *n_trans = nt;
}
