#include "fx.h"
#include "item.h"

#include <math.h>
#include <string.h>

void fx_init(fx_state *f, uint32_t seed)
{
    f->count = 0;
    f->rng = seed ? seed : 1u;
}

static float frand(fx_state *f)
{
    f->rng = f->rng * 1664525u + 1013904223u;
    return (float)(f->rng >> 8) / 16777216.0f;
}

static float srand1(fx_state *f) { return frand(f) * 2.0f - 1.0f; }

static particle *spawn(fx_state *f)
{
    if (f->count < FX_MAX_PARTICLES) return &f->p[f->count++];
    /* Full: recycle the oldest-looking one (least life left). */
    int best = 0;
    for (int i = 1; i < f->count; i++)
        if (f->p[i].life < f->p[best].life) best = i;
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
        p->vel.y -= 9.81 * dt;
        double drag = 1.0 / (1.0 + 1.5 * dt);
        p->vel = dv3_scale(p->vel, drag);
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
    entity_instance e;
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

static uint32_t same_tex(uint8_t layer) { return (uint32_t)layer * 0x010101u; }

void fx_build_entities(const fx_state *f, const physics *ph, const fx_crack *crack, dvec3 eye, double alpha,
                       float time, entity_instance *out, int max, int *n_opaque, int *n_trans)
{
    /* Opaque from the front of out[], translucent collected in a second
     * pass so the caller gets [opaque..., translucent...]. */
    int no = 0, nt = 0;
    entity_instance trans[256];
    const int max_trans = (int)(sizeof trans / sizeof trans[0]);

    for (int i = 0; ph && i < ph->item_count; i++) {
        const item_ent *it = &ph->items[i];
        dvec3 p = dv3_lerp(it->prev_pos, it->pos, alpha);
        /* Resting items hover and bob a little; everything spins. */
        float bob = it->on_ground ? 0.05f + 0.04f * sinf(time * 2.2f + (float)i) : 0.0f;
        p.y += (double)bob;
        quat q = quat_axis(0, 1, 0, it->spin);
        int stacks = it->count > 32 ? 3 : (it->count > 1 ? 2 : 1); /* a pile looks like a pile */
        for (int k = 0; k < stacks; k++) {
            dvec3 pk = dv3(p.x + 0.05 * k, p.y + 0.04 * k, p.z - 0.04 * k);
            if (item_is_block(it->id)) {
                const block_def *d = block_get(it->id);
                entity_instance e = cube_at(pk, eye, block_tex(it->id), q, 0.25f, 0.25f, 0.25f, 1.0f);
                if ((d->flags & BF_TRANSLUCENT) && nt < max_trans) trans[nt++] = e;
                else if (!(d->flags & BF_TRANSLUCENT) && no < max) out[no++] = e;
            } else if (nt < max_trans) {
                /* Items are cards: their sprite on a thin slab, standing up. */
                trans[nt++] = cube_at(pk, eye, same_tex(item_get(it->id)->tex), q, 0.32f, 0.32f, 0.02f, 1.1f);
            }
        }
    }
    for (int i = 0; f && i < f->count; i++) {
        const particle *pt = &f->p[i];
        dvec3 p = dv3_lerp(pt->prev_pos, pt->pos, alpha);
        /* Shrink over the last third of the particle's life. */
        float life = pt->life / pt->max_life;
        float s = pt->size * (life < 0.33f ? life * 3.0f : 1.0f);
        quat q = quat_axis(0.3f, 1.0f, 0.2f, pt->spin);
        entity_instance e = cube_at(p, eye, same_tex(pt->tex), q, s, s, s, 0.9f);
        const int translucent = pt->tex == T_WATER || pt->tex == T_GLASS || pt->tex == T_ICE;
        if (translucent && nt < max_trans) trans[nt++] = e;
        else if (!translucent && no < max) out[no++] = e;
    }
    if (crack && crack->active && crack->progress > 0.0f && nt < max_trans) {
        int stage = (int)(crack->progress * 4.0f);
        stage = stage < 0 ? 0 : (stage > 3 ? 3 : stage);
        dvec3 c = dv3(crack->x + 0.5, crack->y + 0.5, crack->z + 0.5);
        trans[nt++] = cube_at(c, eye, same_tex((uint8_t)(T_CRACK0 + stage)), quat_identity(), 1.004f, 1.004f, 1.004f,
                              1.0f);
    }
    if (no + nt > max) nt = max - no;
    memcpy(out + no, trans, (size_t)(nt > 0 ? nt : 0) * sizeof(entity_instance));
    *n_opaque = no;
    *n_trans = nt > 0 ? nt : 0;
}
