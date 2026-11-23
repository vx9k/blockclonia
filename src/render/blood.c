#include "blood.h"
#include "health.h"
#include "light.h"
#include "mem.h"

#include <math.h>
#include <string.h>

#define RHO 1060.0f    /* kg/m3, whole blood */
#define MMHG 133.322f  /* Pa */
#define G 9.81f
/* A drop of a few millimetres falls at about 9 m/s at most (Gunn and
 * Kinzer's measured terminal speeds); quadratic drag reaches it there. */
#define V_TERM 9.0f
#define STEP (1.0f / 240.0f) /* s; under 4 cm a step at the fastest, so a drop crosses at most one cell face per axis */
#define FILM 0.001f /* m, how thick blood spreads on a face */
#define WASH_S 2.0f /* s for water to wash a stain off */
#define STAIN_THICK 0.0015f /* m, the drawn stain, held just off the face */
#define MIN_STAINS 16

static float frand(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return (float)(*s >> 8) * (1.0f / 16777216.0f);
}

static int finite3(dvec3 v) { return isfinite(v.x) && isfinite(v.y) && isfinite(v.z); }

void blood_init(blood_state *b, int max_stains, uint32_t seed)
{
    memset(b, 0, sizeof *b);
    b->max_stains = max_stains < MIN_STAINS ? MIN_STAINS : (max_stains > 65536 ? 65536 : max_stains);
    b->stain = mem_calloc((size_t)b->max_stains, sizeof *b->stain);
    b->rng = seed ? seed : 1u;
}

void blood_free(blood_state *b)
{
    mem_free(b->stain);
    b->stain = NULL;
    b->max_stains = 0;
    b->drops = 0;
}

/* Oxygenated haemoglobin is scarlet and deoxygenated dark red; shade runs
 * from venous (0) to fully saturated arterial (255). */
static uint8_t shade_of(float sat, int arterial)
{
    float s = clampf((sat - 0.4f) / 0.55f, 0.0f, 1.0f);
    if (!arterial) s *= 0.15f; /* venous blood has given up a third of its oxygen */
    return (uint8_t)lroundf(s * 255.0f);
}

static void emit(blood_state *b, dvec3 at, dvec3 vel, float ml, uint8_t shade)
{
    if (b->drops >= BLOOD_MAX_DROPS) return;
    blood_drop *d = &b->drop[b->drops++];
    d->pos = at;
    d->vel = vel;
    d->ml = ml;
    d->shade = shade;
}

void blood_bleed(blood_state *b, const blood_source *s, int n, int beat, float p_sys, float hr, float sat, dvec3 body,
                 float dt)
{
    if (!b->stain || !isfinite(dt) || dt <= 0.0f) return;
    if (!finite3(body)) body = dv3(0, 0, 0);
    if (!isfinite(p_sys) || p_sys < 0.0f) p_sys = 0.0f;
    if (!isfinite(sat)) sat = 0.0f;
    /* Below about 20 beats a minute the heart throws no jet: the artery
     * only wells up. */
    int pulsing = isfinite(hr) && hr >= 20.0f;
    if (n > BLOOD_SOURCES) n = BLOOD_SOURCES;
    for (int i = 0; i < n; i++) {
        float rate = s[i].ml_min;
        if (!isfinite(rate) || rate < 0.0f || !finite3(s[i].at)) continue;
        dvec3 out = s[i].out;
        double len = isfinite(out.x) && isfinite(out.y) && isfinite(out.z) ? dv3_len(out) : 0.0;
        out = len > 1e-6 ? dv3(out.x / len, out.y / len, out.z / len) : dv3(0, -1, 0);
        int jet = s[i].arterial && pulsing;
        uint8_t shade = shade_of(sat, s[i].arterial);
        /* Whatever beat never came, a minute of bleeding is the most kept;
         * what was already bled stays owed when the bleeding stops. */
        float cap = fmaxf(rate, 1.0f);
        if (b->owed[i] < cap) b->owed[i] = fminf(b->owed[i] + rate / 60.0f * dt, cap);
        if (jet) {
            if (!beat || b->owed[i] < 1e-4f) continue;
            /* Systole drives blood through the hole at Bernoulli's speed,
             * less what the wound's narrow track takes. Its drops leave
             * over the spurt, so they start strung out along the jet. */
            float p = p_sys + (isfinite(s[i].p_column) ? s[i].p_column : 0.0f);
            float v = BLOOD_JET * sqrtf(2.0f * fmaxf(p, 0.0f) * MMHG / RHO);
            int k = (int)(b->owed[i] / BLOOD_DROP_ML);
            k = k < 1 ? 1 : (k > 48 ? 48 : k);
            float ml = b->owed[i] / (float)k;
            for (int j = 0; j < k && b->drops < BLOOD_MAX_DROPS; j++) {
                float sp = v * (0.9f + 0.2f * frand(&b->rng));
                dvec3 d = dv3(out.x + (double)((frand(&b->rng) - 0.5f) * 0.2f),
                              out.y + (double)((frand(&b->rng) - 0.5f) * 0.2f),
                              out.z + (double)((frand(&b->rng) - 0.5f) * 0.2f));
                double dl = dv3_len(d) / (double)sp;
                dvec3 vel = dv3(body.x + d.x / dl, body.y + d.y / dl, body.z + d.z / dl);
                double ahead = (double)(0.08f * frand(&b->rng));
                emit(b,
                     dv3(s[i].at.x + (vel.x - body.x) * ahead, s[i].at.y + (vel.y - body.y) * ahead,
                         s[i].at.z + (vel.z - body.z) * ahead),
                     vel, ml, shade);
                b->owed[i] -= ml;
            }
            if (b->owed[i] < 0.0f) b->owed[i] = 0.0f;
        } else {
            /* A vein, or an artery without a pulse behind it, wells up
             * and drips off the wound a drop at a time. */
            while (b->owed[i] >= BLOOD_DROP_ML && b->drops < BLOOD_MAX_DROPS) {
                dvec3 at = dv3(s[i].at.x + (double)((frand(&b->rng) - 0.5f) * 0.008f), s[i].at.y,
                               s[i].at.z + (double)((frand(&b->rng) - 0.5f) * 0.008f));
                dvec3 vel = dv3(body.x + out.x * 0.05 + (double)((frand(&b->rng) - 0.5f) * 0.04f),
                                body.y + out.y * 0.05,
                                body.z + out.z * 0.05 + (double)((frand(&b->rng) - 0.5f) * 0.04f));
                emit(b, at, vel, BLOOD_DROP_ML, shade);
                b->owed[i] -= BLOOD_DROP_ML;
            }
        }
    }
}

/* The radius blood of ml millilitres covers as a film on a face. */
static float stain_radius(float ml) { return sqrtf(ml * 1e-6f / FILM / 3.14159265f); }

static void land(blood_state *b, dvec3 at, int face, float ml, uint8_t shade)
{
    /* Blood landing on wet blood joins it. */
    for (int i = 0; i < b->max_stains; i++) {
        blood_stain *s = &b->stain[i];
        if (!s->live || s->face != face) continue;
        double r = (double)stain_radius(s->ml) + 0.01;
        double dx = at.x - s->pos.x, dy = at.y - s->pos.y, dz = at.z - s->pos.z;
        if (dx * dx + dy * dy + dz * dz > r * r) continue;
        float sum = s->ml + ml;
        double wt = (double)(ml / sum);
        s->pos = dv3(s->pos.x + dx * wt, s->pos.y + dy * wt, s->pos.z + dz * wt);
        s->shade = (uint8_t)lroundf(((float)s->shade * s->ml + (float)shade * ml) / sum);
        s->age *= s->ml / sum; /* fresh blood keeps it wet */
        s->ml = sum;
        return;
    }
    /* A new stain, in the ring's next slot: past the budget the oldest
     * goes. */
    blood_stain *s = &b->stain[b->next];
    b->next = (b->next + 1) % b->max_stains;
    *s = (blood_stain){at, ml, 0.0f, (uint8_t)face, shade, 1};
}

static int cell(double v) { return (int)floor(v); }

/* Moves drop d one step of h seconds: 1 if it is still flying. */
static int fly(blood_state *b, const world *w, blood_drop *d, float h)
{
    double speed = dv3_len(d->vel);
    double k = (double)(G / (V_TERM * V_TERM)) * speed * (double)h;
    d->vel = dv3(d->vel.x / (1.0 + k), (d->vel.y - (double)(G * h)) / (1.0 + k), d->vel.z / (1.0 + k));
    dvec3 p0 = d->pos, p1 = dv3(p0.x + d->vel.x * (double)h, p0.y + d->vel.y * (double)h, p0.z + d->vel.z * (double)h);
    d->pos = p1;
    if (p1.y < 0.0) return 0;
    const int c1[3] = {cell(p1.x), cell(p1.y), cell(p1.z)};
    uint8_t id = world_get(w, c1[0], c1[1], c1[2]);
    if (block_fluid(id)) return 0; /* gone into water (or lava) */
    if (!block_solid(id)) return 1;
    /* It hit the face it crossed last. */
    const double a0[3] = {p0.x, p0.y, p0.z}, a1[3] = {p1.x, p1.y, p1.z};
    int axis = -1;
    double t_hit = -1.0;
    for (int a = 0; a < 3; a++) {
        int c0 = cell(a0[a]);
        if (c0 == c1[a]) continue;
        double plane = (double)(c1[a] > c0 ? c1[a] : c0);
        double t = (plane - a0[a]) / (a1[a] - a0[a]);
        if (t > t_hit) {
            t_hit = t;
            axis = a;
        }
    }
    if (axis < 0) return 0; /* started inside a block */
    double hit[3] = {0};
    for (int a = 0; a < 3; a++) hit[a] = a0[a] + (a1[a] - a0[a]) * t_hit;
    int up = a1[axis] < a0[axis]; /* moving down the axis: the face's normal points up it */
    hit[axis] = (double)(up ? c1[axis] + 1 : c1[axis]);
    land(b, dv3(hit[0], hit[1], hit[2]), axis * 2 + up, d->ml, d->shade);
    return 0;
}

static const int NORMAL[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};

void blood_step(blood_state *b, const world *w, float dt)
{
    if (!b->stain || !isfinite(dt) || dt <= 0.0f) return;
    if (dt > 0.25f) dt = 0.25f;
    int n = (int)ceilf(dt / STEP);
    float h = dt / (float)n;
    int kept = 0;
    for (int i = 0; i < b->drops; i++) {
        blood_drop d = b->drop[i];
        int alive = finite3(d.pos) && finite3(d.vel);
        for (int s = 0; s < n && alive; s++) alive = fly(b, w, &d, h);
        if (alive) b->drop[kept++] = d;
    }
    b->drops = kept;
    for (int i = 0; i < b->max_stains; i++) {
        blood_stain *s = &b->stain[i];
        if (!s->live) continue;
        const int *nm = NORMAL[s->face];
        /* The block under it gone, the stain goes too; water over it
         * washes it off. */
        uint8_t under = world_get(w, cell(s->pos.x - nm[0] * 0.01), cell(s->pos.y - nm[1] * 0.01),
                                  cell(s->pos.z - nm[2] * 0.01));
        uint8_t over = world_get(w, cell(s->pos.x + nm[0] * 0.01), cell(s->pos.y + nm[1] * 0.01),
                                 cell(s->pos.z + nm[2] * 0.01));
        s->age += block_fluid(over) ? dt * BLOOD_STAIN_S / WASH_S : dt;
        if (!block_solid(under) || s->age >= BLOOD_STAIN_S) s->live = 0;
    }
}

uint32_t blood_colour(int shade, float age)
{
    /* Wet: dark venous red to arterial scarlet. Drying, haemoglobin
     * oxidises to methaemoglobin and the stain browns. */
    float a = clampf((float)shade / 255.0f, 0.0f, 1.0f);
    float d = isfinite(age) ? clampf(age / BLOOD_STAIN_S, 0.0f, 1.0f) : 1.0f;
    const float wet[3] = {95.0f + 95.0f * a, 8.0f + 7.0f * a, 12.0f + 8.0f * a};
    const float dry[3] = {85.0f, 35.0f, 25.0f};
    uint32_t c = 0;
    for (int i = 0; i < 3; i++) c = c << 8 | (uint32_t)lroundf(clampf(wet[i] + (dry[i] - wet[i]) * d, 0.0f, 255.0f));
    return c;
}

static float lit(const world *w, const lightfx_frame *lf, dvec3 eye, dvec3 p, float daylight, float rgb[3])
{
    rgb[0] = rgb[1] = rgb[2] = 1.0f;
    if (!w) return daylight;
    if (!lf) return light_at(w, p.x, p.y, p.z, daylight);
    uint16_t l = world_light(w, cell(p.x), cell(p.y), cell(p.z));
    return lightfx_colour(lf, l, (float)dv3_len(dv3(p.x - eye.x, p.y - eye.y, p.z - eye.z)), rgb);
}

/* The renderer lights and blends in linear light and writes sRGB, so a
 * colour chosen in sRGB goes in as its linear value. */
static uint32_t linear(uint32_t srgb)
{
    uint32_t c = 0;
    for (int i = 2; i >= 0; i--) {
        float v = (float)((srgb >> (8 * i)) & 255u) / 255.0f;
        v = v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
        c = c << 8 | (uint32_t)lroundf(v * 255.0f);
    }
    return c;
}

static void put(entity_instance *e, dvec3 c, dvec3 eye, quat r, float sx, float sy, float sz, uint32_t rgb, float light,
                const float tint[3])
{
    memset(e, 0, sizeof *e);
    e->pos[0] = (float)(c.x - eye.x);
    e->pos[1] = (float)(c.y - eye.y);
    e->pos[2] = (float)(c.z - eye.z);
    e->tex = entity_tex(T_MAT_PLAIN, T_MAT_PLAIN, T_MAT_PLAIN, 0);
    e->rot[0] = r.x;
    e->rot[1] = r.y;
    e->rot[2] = r.z;
    e->rot[3] = r.w;
    e->scale[0] = sx;
    e->scale[1] = sy;
    e->scale[2] = sz;
    e->light = light;
    e->tint = entity_tint(linear(rgb));
    entity_shine(e, 1, tint);
}

int blood_build(const blood_state *b, const world *w, const lightfx_frame *lf, dvec3 eye, float daylight,
                entity_instance *out, int max)
{
    if (!out || max <= 0 || !b->stain) return 0;
    int n = 0;
    float rgb[3] = {0};
    for (int i = 0; i < b->max_stains && n + 1 < max; i++) {
        const blood_stain *s = &b->stain[i];
        if (!s->live) continue;
        const int *nm = NORMAL[s->face];
        int axis = s->face / 2;
        double lift = (double)STAIN_THICK; /* its centre, so it floats half its thickness off the face */
        dvec3 c = dv3(s->pos.x + nm[0] * lift, s->pos.y + nm[1] * lift, s->pos.z + nm[2] * lift);
        float light = lit(w, lf, eye, dv3(s->pos.x + nm[0] * 0.02, s->pos.y + nm[1] * 0.02, s->pos.z + nm[2] * 0.02),
                          daylight, rgb);
        float d = 2.0f * stain_radius(s->ml);
        float sc[3] = {d, d, d};
        sc[axis] = STAIN_THICK;
        /* Two squares a quarter of a right angle apart make an octagon,
         * near enough a round pool, turned by an angle of its own so the
         * stains don't line up with the blocks. */
        float turn = (float)fmod(fabs(s->pos.x * 7.3 + s->pos.y * 3.1 + s->pos.z * 5.7), 1.5707963);
        uint32_t col = blood_colour(s->shade, s->age);
        for (int k = 0; k < 2; k++) {
            quat r = quat_axis((float)nm[0], (float)nm[1], (float)nm[2], turn + 0.7853982f * (float)k);
            put(&out[n++], c, eye, r, sc[0], sc[1], sc[2], col, light, rgb);
        }
    }
    for (int i = 0; i < b->drops && n < max; i++) {
        const blood_drop *d = &b->drop[i];
        float size = cbrtf(6.0f * d->ml * 1e-6f / 3.14159265f);
        float light = lit(w, lf, eye, d->pos, daylight, rgb);
        put(&out[n++], d->pos, eye, quat_identity(), size, size, size, blood_colour(d->shade, 0.0f), light, rgb);
    }
    return n;
}

void blood_spot(int part, int salt, dvec3 feet, float yaw, dvec3 *at, dvec3 *out)
{
    /* Where on a standing 1.8 m figure each part's wound opens, in its
     * own frame: right, up, forward (m), and the way that skin faces. */
    static const float SPOT[7][6] = {
        {0.0f, 1.62f, 0.10f, 0.0f, 0.3f, 1.0f}, /* head: the face and scalp */
        {0.0f, 1.35f, 0.12f, 0.0f, 0.0f, 1.0f}, /* chest */
        {0.0f, 1.05f, 0.11f, 0.0f, 0.0f, 1.0f}, /* abdomen */
        {-0.25f, 1.10f, 0.03f, -1.0f, 0.0f, 0.3f}, /* left arm */
        {0.25f, 1.10f, 0.03f, 1.0f, 0.0f, 0.3f}, /* right arm */
        {-0.10f, 0.45f, 0.07f, -0.3f, 0.0f, 1.0f}, /* left leg */
        {0.10f, 0.45f, 0.07f, 0.3f, 0.0f, 1.0f}, /* right leg */
    };
    const float *sp = SPOT[part >= 0 && part < 7 ? part : 1];
    float sy = sinf(isfinite(yaw) ? yaw : 0.0f), cy = cosf(isfinite(yaw) ? yaw : 0.0f);
    /* yaw 0 faces -z with the right toward +x. */
    const float right[3] = {cy, 0.0f, sy}, fwd[3] = {sy, 0.0f, -cy};
    int k = salt < 0 ? -salt : salt;
    float dr = 0.03f * (float)(k % 3 - 1), du = 0.05f * (float)((k * 3) % 5 - 2);
    const float l[3] = {sp[0] + dr, sp[1] + du, sp[2]};
    *at = dv3(feet.x + (double)(right[0] * l[0] + fwd[0] * l[2]), feet.y + (double)l[1],
              feet.z + (double)(right[2] * l[0] + fwd[2] * l[2]));
    const float o[3] = {right[0] * sp[3] + fwd[0] * sp[5], sp[4], right[2] * sp[3] + fwd[2] * sp[5]};
    float ol = sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
    *out = dv3((double)(o[0] / ol), (double)(o[1] / ol), (double)(o[2] / ol));
}

int blood_from_health(const struct health *h, dvec3 feet, float yaw, blood_source *out)
{
    int n = h->wound_count < BLOOD_SOURCES ? h->wound_count : BLOOD_SOURCES;
    int upright = health_awake(h) && h->down <= 0.0f;
    double heart = feet.y + 1.3; /* m, the heart's level on a 1.8 m figure */
    for (int i = 0; i < n; i++) {
        const wound *wd = &h->wounds[i];
        blood_spot(wd->part, i, feet, yaw, &out[i].at, &out[i].out);
        out[i].ml_min = wd->bandage ? 0.0f : wd->bleed;
        out[i].arterial = wd->arterial;
        /* Standing, blood presses on a wound by the height of the column
         * between it and the heart (rho g h); lying flat, it doesn't. */
        out[i].p_column = upright ? RHO * G * (float)(heart - out[i].at.y) / MMHG : 0.0f;
    }
    return n < 0 ? 0 : n;
}

int blood_beat(double *seen, double last_r)
{
    if (!isfinite(last_r) || last_r == *seen) return 0;
    *seen = last_r;
    return 1;
}
