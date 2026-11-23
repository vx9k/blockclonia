/* The heart's timing (conduction.h). */
#include "conduction.h"

#include "health.h"

#include <math.h>
#include <string.h>

#define AV_REFRACTORY 0.25   /* s, the AV node after passing a sinus impulse */

static double urand(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return (double)(x >> 8) * (1.0 / 16777216.0);
}

void cond_init(conduction *k)
{
    memset(k, 0, sizeof *k);
    k->t_atria = k->t_vent = k->prev_vent = -1.0;
    k->pending_av = k->pending_ect = k->esc_ref = -1.0;
    k->next_af = k->next_own = -1.0;
    k->rhythm = -1;
    k->sa_phase = 0.98f;
}

float cond_atria_on(int rhythm)
{
    return (rhythm == RHYTHM_AF || rhythm == RHYTHM_VF || rhythm == RHYTHM_ASYSTOLE) ? 0.0f : 1.0f;
}

float cond_vent_on(int rhythm)
{
    /* Fibrillating muscle only quivers, out of step, and moves no blood:
     * even a weak squeeze fired hundreds of times a minute would pump once
     * the pressure has fallen to the veins'. */
    return (rhythm == RHYTHM_VF || rhythm == RHYTHM_ASYSTOLE) ? 0.0f : 1.0f;
}

static int sinus_node_runs(int r) { return r != RHYTHM_AF && r != RHYTHM_ASYSTOLE && r != RHYTHM_VF; }
static int own_focus(int r) { return r == RHYTHM_VT || r == RHYTHM_PVT || r == RHYTHM_VF; }

/* s a ventricle can't fire again after a beat. Its action potential
 * shortens with the rate, as the QT interval does (about with the root
 * of the last interval): 0.3 s at rest, 0.19 s (~315 a minute) at the
 * fastest, so a fast AF or VT can run near 300. */
static double vent_refractory(const conduction *k)
{
    if (k->prev_vent < 0.0) return 0.25;
    return fmin(fmax(0.33 * sqrt(k->t_vent - k->prev_vent), 0.19), 0.3);
}

static int fire_vent(conduction *k, double t, int ectopic, int force)
{
    if (!force && k->t_vent >= 0.0 && t < k->t_vent + vent_refractory(k)) return 0;
    k->prev_vent = k->t_vent;
    k->t_vent = t;
    k->esc_ref = t;
    k->last_ectopic = ectopic;
    k->beats++;
    return 1;
}

/* An atrial impulse at t reaches the AV node. */
static void atrial_impulse(conduction *k, const conduction_input *in, double t, double recovery)
{
    k->t_atria = t;
    if (in->rhythm == RHYTHM_AVB3 || own_focus(in->rhythm)) return;
    if (t < k->av_free || k->pending_av >= 0.0) return;
    k->pending_av = t + (double)in->av_delay;
    k->av_free = t + recovery;
}

int cond_step(conduction *k, const conduction_input *in, double t, double dt, uint32_t *rng)
{
    int r = in->rhythm, fired = 0;
    double rate = fmax(1.0, (double)in->sa_rate), sa_rr = 60.0 / rate;
    double end = t + dt;
    if (k->esc_ref < 0.0) k->esc_ref = t;
    if (r != k->rhythm) {
        k->rhythm = r;
        k->pending_av = -1.0;
        k->next_own = own_focus(r) ? t : -1.0;
        k->next_af = r == RHYTHM_AF ? t : -1.0;
    }
    /* An irritable ventricle: ectopic events come as a Poisson stream, and
     * each fires at its focus's coupling interval after a beat, the way
     * re-entrant ectopic beats keep a fixed coupling (0.40-0.55 s at rest,
     * shorter at speed). */
    if (k->pending_ect < 0.0 && in->irritability > 0.0f && !own_focus(r) && r != RHYTHM_ASYSTOLE) {
        double lambda = (double)in->irritability / 60.0;
        if (urand(rng) < 1.0 - exp(-lambda * dt)) {
            k->pending_ect = t + urand(rng) * dt;
            k->ect_coupling = (0.40 + 0.15 * urand(rng)) * fmin(1.0, sa_rr / 0.857);
        }
    }

    double now = t;
    for (int guard = 0; guard < 64; guard++) {
        double t_sa = sinus_node_runs(r) ? now + (1.0 - (double)k->sa_phase) * sa_rr : 1e300;
        double t_af = k->next_af >= 0.0 ? k->next_af : 1e300;
        double t_av = k->pending_av >= 0.0 ? k->pending_av : 1e300;
        double t_own = k->next_own >= 0.0 ? k->next_own : 1e300;
        double t_ect = 1e300;
        if (k->pending_ect >= 0.0) {
            /* Past this beat's coupling point, it waits for the next beat. */
            double at = k->t_vent >= 0.0 ? k->t_vent + k->ect_coupling : k->pending_ect;
            if (at >= k->pending_ect) t_ect = at;
        }
        int escapes = r == RHYTHM_SINUS || r == RHYTHM_AF || r == RHYTHM_AVB3 || r == RHYTHM_PEA;
        double t_esc = escapes ? k->esc_ref + 60.0 / COND_ESCAPE_RATE : 1e300;

        double te = fmin(fmin(fmin(t_sa, t_af), fmin(t_av, t_own)), fmin(t_ect, t_esc));
        if (te >= end) break;
        te = fmax(te, now);
        if (sinus_node_runs(r)) k->sa_phase = (float)fmin(1.0, (double)k->sa_phase + (te - now) / sa_rr);
        now = te;

        if (te == t_sa) {
            k->sa_phase = 0.0f;
            atrial_impulse(k, in, te, AV_REFRACTORY);
        } else if (te == t_af) {
            /* Fibrillating atria hit the AV node 350-600 times a minute; it
             * passes whichever finds it recovered, and sympathetic tone
             * shortens its recovery. */
            double recovery = (0.30 + 0.15 * urand(rng)) * sqrt(70.0 / rate);
            atrial_impulse(k, in, te, recovery);
            k->next_af = te + 60.0 / (350.0 + 250.0 * urand(rng));
        } else if (te == t_av) {
            k->pending_av = -1.0;
            fired += fire_vent(k, te, 0, 0);
        } else if (te == t_own) {
            if (r == RHYTHM_VF) {
                fired += fire_vent(k, te, 0, 1);
                k->next_own = te + 0.15 + 0.10 * urand(rng);
            } else {
                fired += fire_vent(k, te, 0, 1);
                k->next_own = te + 60.0 / (r == RHYTHM_PVT ? COND_PVT_RATE : COND_VT_RATE);
            }
        } else if (te == t_ect) {
            k->pending_ect = -1.0;
            if (fire_vent(k, te, 1, 0)) {
                fired++;
                /* It also runs back up into the AV node and leaves it
                 * refractory, so the next sinus impulse is blocked: the
                 * compensatory pause. */
                k->av_free = te + fmin(0.6, fmax(0.25, 0.75 * sa_rr));
                k->pending_av = -1.0;
            }
        } else {
            fired += fire_vent(k, te, 0, 0);
            k->esc_ref = te;
        }
    }
    if (sinus_node_runs(r)) k->sa_phase = (float)fmin(1.0, (double)k->sa_phase + (end - now) / sa_rr);
    return fired;
}
