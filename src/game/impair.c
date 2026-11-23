#include "impair.h"

#include <math.h>
#include <string.h>

#define STUMBLE_S 0.5f /* s a catch of the foot breaks the stride */

static float rnd(impair *m)
{
    m->rng ^= m->rng << 13;
    m->rng ^= m->rng >> 17;
    m->rng ^= m->rng << 5;
    return (float)(m->rng >> 8) / 16777216.0f;
}

void impair_init(impair *m, uint32_t seed)
{
    memset(m, 0, sizeof *m);
    m->rng = seed * 2654435761u | 1u;
}

/* A press the hand may miss. */
static int press(impair *m, int n, float drop)
{
    int kept = 0;
    for (int i = 0; i < n; i++) kept += drop <= 0.0f || rnd(m) >= drop;
    return kept;
}

impair_frame impair_step(impair *m, const impair_frame *now, float lag, float drop, float stumble, double t, float dt)
{
    /* Hold this frame back; a full ring lets its oldest go early rather
     * than lose it. */
    if (m->count == IMPAIR_RING) m->count--;
    const int slot = (m->head + m->count) % IMPAIR_RING;
    m->ring[slot] = *now;
    m->at[slot] = t;
    m->count++;

    /* Everything taken at least lag ago comes through now: the latest
     * movement wins, while turns and presses add up so none is lost. */
    impair_frame out = {0};
    out.move = m->held;
    while (m->count > 0 && m->at[m->head] <= t - (double)lag + 1e-9) {
        const impair_frame *f = &m->ring[m->head];
        out.move = f->move;
        out.look_dx += f->look_dx;
        out.look_dy += f->look_dy;
        out.click_break += press(m, f->click_break, drop);
        out.click_place += press(m, f->click_place, drop);
        m->head = (m->head + 1) % IMPAIR_RING;
        m->count--;
    }
    m->held = out.move;

    /* A foot catches now and then while walking: the stride breaks and
     * the body stops short for a moment. The chance is per second, so a
     * fast frame rate trips no more often than a slow one. */
    const int walking = fabsf(out.move.forward) + fabsf(out.move.right) > 0.0f;
    if (m->stumble_t > 0.0f) m->stumble_t = fmaxf(0.0f, m->stumble_t - dt);
    else if (walking && stumble > 0.0f && rnd(m) < 1.0f - expf(-stumble * dt)) m->stumble_t = STUMBLE_S;
    if (m->stumble_t > 0.0f) {
        out.move.forward *= 0.2f;
        out.move.right *= 0.2f;
        out.move.sprint = out.move.jump = 0;
    }
    return out;
}
