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
/* 1/s blood leaves a cut artery at mean pressure, scaled by the cube of
 * the pressure over its mean, so most of a beat's blood goes in systole
 * and the jet visibly surges. */
#define RELEASE 12.0f
#define JET_MIN 10.0f /* mmHg below which an artery only wells up */
#define FADE_S 30.0f /* s over which an old stain shrinks away */
#define STREAK_S 0.02f /* s of travel a falling drop is drawn stretched over */
/* A regular octagon's side over the span between opposite sides
 * (tan 22.5 degrees), and that span for a circle's radius at equal area:
 * 2 sqrt(pi / (8 tan 22.5 degrees)). */
#define OCT_SIDE 0.41421356f
#define SPILL_MAX 8 /* lobes blood may run over before it stops */
#define SPLASH_V 3.5f /* m/s into a face above which a drop splashes */
#define SPLASH_SHARE 0.3f /* of a splashing drop thrown off as spots */
#define OCT_SPAN 1.94740f

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

/* A drop leaving at vel, spread over the last dt seconds: one that left
 * earlier in the frame is further along, so the stream has no gaps at any
 * frame rate. */
static void leave(blood_state *b, dvec3 at, dvec3 vel, dvec3 body, float dt, float ml, uint8_t shade)
{
    double u = (double)(frand(&b->rng) * dt);
    emit(b, dv3(at.x + (vel.x - body.x) * u, at.y + (vel.y - body.y) * u, at.z + (vel.z - body.z) * u), vel, ml, shade);
}

void blood_bleed(blood_state *b, const blood_source *s, int n, float p_now, float p_mean, float sat, dvec3 body,
                 float dt)
{
    if (!b->stain || !isfinite(dt) || dt <= 0.0f) return;
    if (!finite3(body)) body = dv3(0, 0, 0);
    if (!isfinite(p_mean) || p_mean <= 0.0f) p_mean = isfinite(p_now) && p_now > 0.0f ? p_now : 0.0f;
    if (!isfinite(p_now) || p_now < 0.0f) p_now = p_mean;
    if (!isfinite(sat)) sat = 0.0f;
    /* How much faster than on average blood leaves at this moment: the
     * systolic surge pushes most of each beat's blood out. */
    float surge = p_mean > 0.0f ? clampf(p_now / p_mean, 0.0f, 3.0f) : 1.0f;
    float release = 1.0f - expf(-RELEASE * surge * surge * surge * dt);
    if (n > BLOOD_SOURCES) n = BLOOD_SOURCES;
    for (int i = 0; i < n; i++) {
        float rate = s[i].ml_min;
        if (!isfinite(rate) || rate < 0.0f || !finite3(s[i].at)) continue;
        dvec3 out = s[i].out;
        double len = isfinite(out.x) && isfinite(out.y) && isfinite(out.z) ? dv3_len(out) : 0.0;
        out = len > 1e-6 ? dv3(out.x / len, out.y / len, out.z / len) : dv3(0, -1, 0);
        uint8_t shade = shade_of(sat, s[i].arterial);
        /* A minute of bleeding is the most a wound holds back; what was
         * already bled stays owed when the bleeding stops. */
        float cap = fmaxf(rate, 1.0f);
        if (b->owed[i] < cap) b->owed[i] = fminf(b->owed[i] + rate / 60.0f * dt, cap);
        float p = p_now + (isfinite(s[i].p_column) ? s[i].p_column : 0.0f);
        if (s[i].arterial && p >= JET_MIN) {
            /* Blood leaves the artery through the hole at Bernoulli's
             * speed, less what the wound's narrow track takes, in a tight
             * jet. */
            float a = b->owed[i] * release;
            b->owed[i] -= a;
            b->ready[i] += a;
            float v = BLOOD_JET * sqrtf(2.0f * p * MMHG / RHO);
            while (b->ready[i] >= BLOOD_DROP_ML && b->drops < BLOOD_MAX_DROPS) {
                float sp = v * (0.97f + 0.06f * frand(&b->rng));
                dvec3 d = dv3(out.x + (double)((frand(&b->rng) - 0.5f) * 0.06f),
                              out.y + (double)((frand(&b->rng) - 0.5f) * 0.06f),
                              out.z + (double)((frand(&b->rng) - 0.5f) * 0.06f));
                double dl = dv3_len(d) / (double)sp;
                leave(b, s[i].at, dv3(body.x + d.x / dl, body.y + d.y / dl, body.z + d.z / dl), body, dt, BLOOD_DROP_ML,
                      shade);
                b->ready[i] -= BLOOD_DROP_ML;
            }
        } else {
            /* A vein, or an artery without the pressure to jet, wells up
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

static int cell(double v) { return (int)floor(v); }

/* Blood of ml millilitres landing at `at` on a face. */
static void land(blood_state *b, dvec3 at, int face, float ml, uint8_t shade)
{
    /* Blood landing on wet blood joins it, until the stain is a lobe's
     * width: then it runs off over the stain's edge, a random way, as a
     * pool on real ground spreads along whatever unevenness it finds.
     * The pool grows lopsided lobe by lobe instead of as one disc. */
    for (int spill = 0;; spill++) {
        const blood_stain *full = NULL;
        for (int i = 0; i < b->max_stains; i++) {
            blood_stain *s = &b->stain[i];
            if (!s->live || s->face != face) continue;
            double r = (double)stain_radius(s->ml) + 0.005;
            double dx = at.x - s->pos.x, dy = at.y - s->pos.y, dz = at.z - s->pos.z;
            if (dx * dx + dy * dy + dz * dz > r * r) continue;
            if (stain_radius(s->ml + ml) > BLOOD_LOBE_R) {
                if (!full) full = s;
                continue;
            }
            float sum = s->ml + ml;
            double wt = (double)(ml / sum);
            s->pos = dv3(s->pos.x + dx * wt, s->pos.y + dy * wt, s->pos.z + dz * wt);
            s->shade = (uint8_t)lroundf(((float)s->shade * s->ml + (float)shade * ml) / sum);
            s->age *= s->ml / sum; /* fresh blood keeps it wet */
            s->ml = sum;
            return;
        }
        if (!full || spill >= SPILL_MAX) break;
        /* No face is quite level: each block's tilts its own fixed way,
         * and most spills run down it, so pools stretch out lopsided. */
        int axis = face / 2;
        uint32_t h = (uint32_t)cell(full->pos.x) * 73856093u ^ (uint32_t)cell(full->pos.y) * 19349663u ^
                     (uint32_t)cell(full->pos.z) * 83492791u ^ (uint32_t)face * 2654435761u;
        float down = (float)(h >> 8) * (6.2831853f / 16777216.0f);
        float ang = frand(&b->rng) < 0.7f ? down + (frand(&b->rng) - 0.5f) * 2.0f : frand(&b->rng) * 6.2831853f;
        double r = 1.3 * (double)stain_radius(full->ml), p[3] = {full->pos.x, full->pos.y, full->pos.z};
        p[(axis + 1) % 3] += r * (double)cosf(ang);
        p[(axis + 2) % 3] += r * (double)sinf(ang);
        at = dv3(p[0], p[1], p[2]);
    }
    /* A new stain, in the ring's next slot: past the budget the oldest
     * goes. */
    blood_stain *s = &b->stain[b->next];
    b->next = (b->next + 1) % b->max_stains;
    *s = (blood_stain){at, ml, 0.0f, (uint8_t)face, shade, 1};
}

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
    int face = axis * 2 + up;
    /* A drop hitting hard splashes: part of it is thrown off as small
     * spots around the impact, mostly the way it was going. */
    const double vel[3] = {d->vel.x, d->vel.y, d->vel.z};
    double vn = fabs(vel[axis]);
    float ml = d->ml;
    if (vn > (double)SPLASH_V) {
        int t1 = (axis + 1) % 3, t2 = (axis + 2) % 3, spots = vn > 7.5 ? 5 : (int)(vn / 1.5);
        double along = atan2(vel[t2], vel[t1]), side = hypot(vel[t1], vel[t2]);
        float each = ml * SPLASH_SHARE / (float)spots;
        for (int i = 0; i < spots; i++) {
            double ang = side > 0.2 * vn ? along + (double)((frand(&b->rng) - 0.5f) * 1.6f)
                                         : (double)(frand(&b->rng) * 6.2831853f);
            double reach = (0.02 + (double)frand(&b->rng) * 0.08) * vn / 4.0, p[3] = {hit[0], hit[1], hit[2]};
            p[t1] += reach * cos(ang);
            p[t2] += reach * sin(ang);
            land(b, dv3(p[0], p[1], p[2]), face, each, d->shade);
        }
        ml -= each * (float)spots;
    }
    land(b, dv3(hit[0], hit[1], hit[2]), face, ml, d->shade);
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
    float d = isfinite(age) ? clampf(age / BLOOD_DRY_S, 0.0f, 1.0f) : 1.0f;
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
    /* Stains first, but never into the room the drops in the air need. */
    int stain_max = max - (b->drops < max ? b->drops : max);
    for (int i = 0; i < b->max_stains && n + 4 <= stain_max; i++) {
        const blood_stain *s = &b->stain[i];
        if (!s->live) continue;
        const int *nm = NORMAL[s->face];
        int axis = s->face / 2;
        double lift = (double)STAIN_THICK; /* its centre, so it floats half its thickness off the face */
        dvec3 c = dv3(s->pos.x + nm[0] * lift, s->pos.y + nm[1] * lift, s->pos.z + nm[2] * lift);
        float light = lit(w, lf, eye, dv3(s->pos.x + nm[0] * 0.02, s->pos.y + nm[1] * 0.02, s->pos.z + nm[2] * 0.02),
                          daylight, rgb);
        /* A regular octagon of the pool's area, drawn as four boxes 45
         * degrees apart, each spanning it across a pair of opposite sides
         * and as wide as one side: together they are the octagon exactly.
         * It is turned by an angle of its own so the stains don't line up
         * with the blocks. */
        float span = OCT_SPAN * stain_radius(s->ml) * clampf((BLOOD_STAIN_S - s->age) / FADE_S, 0.0f, 1.0f);
        float sc[3] = {span, span, span};
        sc[axis] = STAIN_THICK;
        sc[axis == 2 ? 1 : 2] = span * OCT_SIDE;
        float turn = (float)fmod(fabs(s->pos.x * 7.3 + s->pos.y * 3.1 + s->pos.z * 5.7), 0.7853982);
        uint32_t col = blood_colour(s->shade, s->age);
        for (int k = 0; k < 4; k++) {
            quat r = quat_axis((float)nm[0], (float)nm[1], (float)nm[2], turn + 0.7853982f * (float)k);
            put(&out[n++], c, eye, r, sc[0], sc[1], sc[2], col, light, rgb);
        }
    }
    for (int i = 0; i < b->drops && n < max; i++) {
        const blood_drop *d = &b->drop[i];
        float size = cbrtf(6.0f * d->ml * 1e-6f / 3.14159265f);
        float light = lit(w, lf, eye, d->pos, daylight, rgb);
        /* Drawn along its path, stretched over a moment of its travel, as
         * the eye sees a falling drop. */
        double sp = dv3_len(d->vel);
        quat r = quat_identity();
        if (sp > 1e-3)
            r = quat_axis((float)(-d->vel.y / sp), (float)(d->vel.x / sp), 0.0f,
                          acosf(clampf((float)(d->vel.z / sp), -1.0f, 1.0f)));
        put(&out[n++], d->pos, eye, r, size, size, size + (float)sp * STREAK_S, blood_colour(d->shade, 0.0f), light,
            rgb);
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
    /* Each part's thickness, as the radius of an upright cylinder. */
    static const float RADIUS[7] = {0.10f, 0.14f, 0.13f, 0.05f, 0.05f, 0.07f, 0.07f};
    /* Where the wounds after the first open around the part: turned this
     * far (rad) from the first, so several wounds in one part bleed as
     * separate jets, the first ones on the side the eye sees. */
    static const float TURN[8] = {0.0f, 0.9f, -0.9f, 1.75f, -1.75f, 0.45f, -0.45f, 1.3f};
    int p = part >= 0 && part < 7 ? part : 1;
    const float *sp = SPOT[p];
    float sy = sinf(isfinite(yaw) ? yaw : 0.0f), cy = cosf(isfinite(yaw) ? yaw : 0.0f);
    /* yaw 0 faces -z with the right toward +x. */
    const float right[3] = {cy, 0.0f, sy}, fwd[3] = {sy, 0.0f, -cy};
    int k = salt < 0 ? -salt : salt;
    /* Below the head, skin on a standing body mostly faces a little down
     * and blood runs that way out of the wound, so jets leave below level
     * rather than rising like a fountain. */
    float du = 0.05f * (float)((k * 3) % 5 - 2), tilt = 0.25f * (float)((k * 7) % 3 - 1) - (p > 0 ? 0.4f : 0.0f);
    /* The first wound's skin faces (sp[3], sp[5]) across the ground; the
     * others face that turned about the part's upright axis. */
    float hl = sqrtf(sp[3] * sp[3] + sp[5] * sp[5]), a = TURN[k % 8];
    float nr = sp[3] / hl, nf = sp[5] / hl;
    float tr = nr * cosf(a) - nf * sinf(a), tf = nr * sinf(a) + nf * cosf(a);
    float rad = RADIUS[p];
    const float l[3] = {sp[0] + (tr - nr) * rad, sp[1] + du, sp[2] + (tf - nf) * rad};
    *at = dv3(feet.x + (double)(right[0] * l[0] + fwd[0] * l[2]), feet.y + (double)l[1],
              feet.z + (double)(right[2] * l[0] + fwd[2] * l[2]));
    const float o[3] = {(right[0] * tr + fwd[0] * tf) * hl, sp[4] + tilt, (right[2] * tr + fwd[2] * tf) * hl};
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
