#include "lamps.h"

#include <math.h>

void lamps_init(lamps *l, lamps_glow_fn under, const void *under_user)
{
    *l = (lamps){0};
    l->under = under;
    l->under_user = under_user;
}

void lamps_add(lamps *l, dvec3 p, uint16_t light)
{
    light = (uint16_t)(light & 0xFFF0u); /* block light only */
    if (!light || l->n >= LAMPS_MAX || !(fabs(p.x) < 1e9 && fabs(p.z) < 1e9) || p.y < 0.0 || p.y >= WORLD_H) return;
    l->now[l->n++] = (lamp){(int)floor(p.x), (int)floor(p.y), (int)floor(p.z), light};
}

/* 1 if list (n long) holds a lamp equal to k. */
static int holds(const lamp *list, int n, const lamp *k)
{
    for (int i = 0; i < n; i++)
        if (list[i].x == k->x && list[i].y == k->y && list[i].z == k->z && list[i].light == k->light) return 1;
    return 0;
}

int lamps_end(lamps *l, world *w)
{
    int changed = 0;
    for (int i = 0; i < l->n; i++)
        if (!holds(l->was, l->was_n, &l->now[i])) {
            world_light_touch(w, l->now[i].x, l->now[i].z);
            changed++;
        }
    for (int i = 0; i < l->was_n; i++)
        if (!holds(l->now, l->n, &l->was[i])) {
            world_light_touch(w, l->was[i].x, l->was[i].z);
            changed++;
        }
    for (int i = 0; i < l->n; i++) l->was[i] = l->now[i];
    l->was_n = l->n;
    l->n = 0; /* ready for the next frame's list */
    return changed;
}

int lamps_glow(const void *user, int x0, int z0, int top, struct light_emitter *out, int cap)
{
    const lamps *l = user;
    int n = l->under ? l->under(l->under_user, x0, z0, top, out, cap) : 0;
    /* A light job asks after lamps_end, so the last list is in was. */
    for (int i = 0; i < l->was_n && n < cap; i++) {
        const lamp *k = &l->was[i];
        int x = k->x - x0, z = k->z - z0;
        if (x < 0 || x >= LIGHT_REGION || z < 0 || z >= LIGHT_REGION || k->y >= top) continue;
        out[n++] = (light_emitter){(int16_t)x, (int16_t)k->y, (int16_t)z, k->light};
    }
    return n;
}
