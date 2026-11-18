/* Muzzle effects: the flash and its light, powder smoke on the wind, the
 * textures they are drawn with, and tracers. */
#include "test_util.h"

#include "block.h"
#include "combat.h"
#include "fx.h"
#include "lightfx.h"
#include "texgen.h"
#include "viewmodel.h"

#include <math.h>

static int count_kind(const fx_state *f, int kind)
{
    int n = 0;
    for (int i = 0; i < f->count; i++) n += f->p[i].kind == kind;
    return n;
}

/* Black powder smokes; smokeless barely does. Both flash, ahead of the
 * muzzle and briefly. */
static void test_muzzle_spawn(void)
{
    static fx_state black, clean;
    fx_init(&black, 7);
    fx_init(&clean, 7);
    dvec3 at = dv3(0.5, 80.0, 0.5), dir = dv3(1.0, 0.0, 0.0);
    fx_muzzle(&black, at, dir, 1);
    fx_muzzle(&clean, at, dir, 0);
    CHECK(count_kind(&black, FXP_GLOW) == 6 && count_kind(&clean, FXP_GLOW) == 6);
    CHECK(count_kind(&black, FXP_SMOKE) > 3 * count_kind(&clean, FXP_SMOKE) && count_kind(&clean, FXP_SMOKE) > 0);
    int ahead = 1, brief = 1, lasting = 1;
    for (int i = 0; i < black.count; i++) {
        const particle *p = &black.p[i];
        ahead &= p->pos.x > at.x;
        if (p->kind == FXP_GLOW) brief &= p->life < 0.1f && p->tex == T_FLASH;
        if (p->kind == FXP_SMOKE) lasting &= p->life >= 5.0f && p->tex == T_SMOKE0;
    }
    CHECK(ahead && brief && lasting);
}

/* Smoke takes up the wind and rises; the flash is gone at once; debris
 * still falls. */
static void test_smoke_drift(void)
{
    test_world t = {0};
    tw_init(&t);
    static fx_state f;
    fx_init(&f, 9);
    f.wind = dv3(4.0, 0.0, -1.0);
    fx_muzzle(&f, dv3(0.5, GROUND + 20.0, 0.5), dv3(0.0, 0.0, 1.0), 1);
    fx_step(&f, &t.w, 0.1);
    CHECK(count_kind(&f, FXP_GLOW) == 0 && count_kind(&f, FXP_SMOKE) == 14);
    for (int i = 0; i < 180; i++) fx_step(&f, &t.w, 1.0 / 60.0);
    double vx = 0.0, vy = 0.0, vz = 0.0, x = 0.0;
    int n = 0;
    for (int i = 0; i < f.count; i++) {
        if (f.p[i].kind != FXP_SMOKE) continue;
        vx += f.p[i].vel.x;
        vy += f.p[i].vel.y;
        vz += f.p[i].vel.z;
        x += f.p[i].pos.x;
        n++;
    }
    CHECK(n == 14);
    if (n) {
        CHECK(fabs(vx / n - 4.0) < 0.1 && fabs(vz / n + 1.0) < 0.1 && vy / n > 0.15 && vy / n < 0.3);
        CHECK(x / n > 6.0); /* carried several metres downwind */
    }
    /* Debris still falls, wind or none. */
    int first = f.count;
    fx_break(&f, B_STONE, 0, GROUND + 30, 0, 4);
    double up[4] = {0.0, 0.0, 0.0, 0.0};
    for (int i = 0; i < 4; i++) up[i] = f.p[first + i].vel.y;
    for (int i = 0; i < 30; i++) fx_step(&f, &t.w, 1.0 / 60.0);
    int falling = 0;
    for (int i = 0; i < 4; i++) falling += f.p[first + i].kind == FXP_DEBRIS && f.p[first + i].vel.y < up[i] - 3.0;
    CHECK(f.count == first + 4 && falling == 4);
    tw_free(&t);
}

/* A full pool recycles particles cleared, so old smoke doesn't come back
 * as smoke. */
static void test_recycle(void)
{
    static fx_state f;
    fx_init(&f, 3);
    while (f.count < FX_MAX_PARTICLES) fx_muzzle(&f, dv3(0.0, 90.0, 0.0), dv3(1.0, 0.0, 0.0), 1);
    fx_break(&f, B_DIRT, 0, 90, 0, 20);
    CHECK(f.count == FX_MAX_PARTICLES && count_kind(&f, FXP_DEBRIS) == 20);
}

/* Smoke draws see-through and grows thinner and larger as it ages; the
 * flash draws solid and brighter than full light. */
static void test_muzzle_draw(void)
{
    static fx_state f;
    static entity_instance out[64];
    fx_init(&f, 5);
    dvec3 eye = dv3(0.0, 80.0, -3.0);
    fx_muzzle(&f, dv3(0.0, 80.0, 0.0), dv3(0.0, 0.0, 1.0), 1);
    int no = 0, nt = 0;
    fx_build_entities(&f, NULL, NULL, eye, 1.0, 0.0f, 1.0f, out, 64, &no, &nt);
    CHECK(no == 6 && nt == 14);
    int glow = 1, smoke = 1;
    float young = 0.0f;
    for (int i = 0; i < no; i++) glow &= (out[i].tex & 255u) == T_FLASH && out[i].light > 2.0f;
    for (int i = no; i < no + nt; i++) {
        smoke &= (out[i].tex & 255u) == T_SMOKE0 && out[i].light <= 1.0f;
        young = fmaxf(young, out[i].scale[0]);
    }
    CHECK(glow && smoke);
    /* On a dark night the flash lights its own smoke. */
    f.flash = 1.0f;
    fx_build_entities(&f, NULL, NULL, eye, 1.0, 0.0f, 0.0f, out, 64, &no, &nt);
    int lit = nt == 14;
    for (int i = no; i < no + nt; i++) lit &= out[i].light == 1.0f;
    f.flash = 0.0f;
    fx_build_entities(&f, NULL, NULL, eye, 1.0, 0.0f, 0.0f, out, 64, &no, &nt);
    CHECK(lit && out[no].light == 0.0f);
    /* Most of the way through its life: the third, thinnest step, larger. */
    for (int i = 0; i < f.count; i++) f.p[i].life = 0.2f * f.p[i].max_life;
    fx_build_entities(&f, NULL, NULL, eye, 1.0, 0.0f, 1.0f, out, 64, &no, &nt);
    float old = 0.0f;
    int thin = 1;
    for (int i = no; i < no + nt; i++) {
        thin &= (out[i].tex & 255u) == T_SMOKE2;
        old = fmaxf(old, out[i].scale[0]);
    }
    CHECK(nt == 14 && thin && old > 2.0f * young);
}

static float mean_alpha(int layer, int centre)
{
    float sum = 0.0f;
    int n = 0;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            int c = x >= 5 && x <= 10 && y >= 5 && y <= 10;
            if (c != centre) continue;
            uint8_t px[4] = {0};
            texgen_texel(layer, x, y, px);
            sum += (float)px[3];
            n++;
        }
    return sum / (float)n;
}

static void test_muzzle_textures(void)
{
    /* Each step of smoke lets more light through, and every one is
     * densest in the middle and clear at the corners. */
    CHECK(mean_alpha(T_SMOKE0, 1) > mean_alpha(T_SMOKE1, 1) + 30.0f);
    CHECK(mean_alpha(T_SMOKE1, 1) > mean_alpha(T_SMOKE2, 1) + 30.0f);
    for (int l = T_SMOKE0; l <= T_SMOKE2; l++) {
        uint8_t corner[4] = {0};
        texgen_texel(l, 0, 0, corner);
        CHECK(mean_alpha(l, 1) > 2.0f * mean_alpha(l, 0) && corner[3] < 40);
    }
    /* The flash is solid, white in the middle and yellower outside. */
    uint8_t mid[4] = {0}, edge[4] = {0};
    texgen_texel(T_FLASH, 8, 8, mid);
    texgen_texel(T_FLASH, 0, 0, edge);
    CHECK(mid[3] == 255 && edge[3] == 255 && mid[0] > 230 && mid[2] > 190 && edge[2] < mid[2] - 60);
}

static void test_flash_light(void)
{
    const float sun[3] = {0.0f, 1.0f, 0.0f};
    lightfx_frame f = {0};
    lightfx_build(&f, sun, 0.0f, 0, 50, 0.0);
    lightfx_frame g = f;
    lightfx_flash(&g, 0.0f);
    CHECK(g.held[0] == f.held[0] && g.held[1] == f.held[1] && g.held[2] == f.held[2]);
    /* Full: level 15, warm. */
    lightfx_flash(&g, 1.0f);
    CHECK(g.held[0] == 1.0f && g.held[1] > 0.7f && g.held[2] < g.held[1] && g.held[2] > 0.4f);
    /* Half as bright, and nonsense does nothing or clamps. */
    lightfx_frame h = f;
    lightfx_flash(&h, 0.5f);
    CHECK(fabsf(h.held[0] - 0.5f) < 1e-6f);
    lightfx_frame k = f;
    lightfx_flash(&k, NAN);
    lightfx_flash(&k, -3.0f);
    CHECK(k.held[0] == f.held[0]);
    lightfx_flash(&k, 40.0f);
    CHECK(k.held[0] == 1.0f);
    /* A brighter light already held stays as it is. */
    lightfx_frame m = f;
    m.held[2] = 0.9f;
    lightfx_flash(&m, 1.0f);
    CHECK(m.held[2] == 0.9f && m.held[0] == 1.0f);
}

static void test_rounds(void)
{
    /* Black powder for the old rounds, a tracer only on the tracer. */
    CHECK(ammo_get(AMMO_ROUND_BALL)->black && ammo_get(AMMO_BLACK_LEAD)->black);
    CHECK(!ammo_get(AMMO_SMOKELESS)->black && !ammo_get(AMMO_MATCH)->black && !ammo_get(AMMO_TRACER)->black);
    for (int i = 0; i < AMMO_COUNT; i++) CHECK((ammo_get(i)->tracer > 0.0f) == (i == AMMO_TRACER));
    /* It burns out about 900 m away, as a real one does. */
    atmos_air air = atmos_at(SEA_LEVEL, 15.0f);
    dvec3 pos = {0}, vel = dv3((double)ammo_get(AMMO_TRACER)->muzzle, 0.0, 0.0);
    for (int i = 0; i < (int)(ammo_get(AMMO_TRACER)->tracer * 240.0f); i++)
        ballistics_step(ammo_get(AMMO_TRACER), &air, &pos, &vel, 1.0 / 240.0);
    CHECK(pos.x > 800.0 && pos.x < 1100.0);
}

static bullet tracer_at(dvec3 pos, dvec3 vel, float age) { return (bullet){pos, vel, 60.0f - age, 0, AMMO_TRACER}; }

static void test_tracers(void)
{
    static combat c;
    combat_init(&c, 1);
    entity_instance out[8] = {0};
    dvec3 eye = dv3(0.0, 70.0, 0.0);
    /* One in flight, east and a little up: one streak a step long along
     * its path, ending where it is. */
    c.b[0] = tracer_at(dv3(200.0, 72.0, 0.0), dv3(600.0, 80.0, 0.0), 0.5f);
    c.count = 1;
    CHECK(combat_tracers(&c, eye, 1.0, out, 8) == 1);
    double v = sqrt(600.0 * 600.0 + 80.0 * 80.0);
    CHECK(fabsf(out[0].scale[2] - (float)(v * PHYS_DT)) < 1e-3f);
    const float z[3] = {0.0f, 0.0f, 1.0f};
    float along[3] = {0.0f, 0.0f, 0.0f};
    quat q = {out[0].rot[0], out[0].rot[1], out[0].rot[2], out[0].rot[3]};
    quat_apply(q, z, along);
    CHECK(fabsf(along[0] - (float)(600.0 / v)) < 1e-3f && fabsf(along[1] - (float)(80.0 / v)) < 1e-3f);
    float head = out[0].pos[0] + 0.5f * out[0].scale[2] * along[0];
    CHECK(fabsf(head - 200.0f) < 0.01f);
    CHECK((out[0].tex & 255u) == T_FLASH && out[0].light > 2.0f && out[0].tint != 0);
    /* Halfway to the next step it is drawn half a step back. */
    CHECK(combat_tracers(&c, eye, 0.5, out, 8) == 1);
    CHECK(fabsf(out[0].pos[0] + 0.5f * out[0].scale[2] * along[0] - (float)(200.0 - 300.0 * PHYS_DT)) < 0.01f);

    /* Far off it widens to stay visible; close by it stays thin. */
    float near = 0.0f;
    c.b[0].pos = dv3(20.0, 70.0, 0.0);
    combat_tracers(&c, eye, 1.0, out, 8);
    near = out[0].scale[0];
    c.b[0].pos = dv3(800.0, 70.0, 0.0);
    combat_tracers(&c, eye, 1.0, out, 8);
    CHECK(fabsf(near - 0.03f) < 1e-6f && out[0].scale[0] > 1.0f && out[0].scale[0] < 1.3f);

    /* Not lit yet, burnt out, not a tracer, too slow: nothing. */
    c.b[0] = tracer_at(dv3(10.0, 70.0, 0.0), dv3(800.0, 0.0, 0.0), 0.01f);
    c.b[1] = tracer_at(dv3(10.0, 70.0, 0.0), dv3(800.0, 0.0, 0.0), 2.5f);
    c.b[2] = tracer_at(dv3(10.0, 70.0, 0.0), dv3(800.0, 0.0, 0.0), 1.0f);
    c.b[2].ammo = AMMO_SMOKELESS;
    c.b[3] = tracer_at(dv3(10.0, 70.0, 0.0), dv3(0.5, 0.0, 0.0), 1.0f);
    c.b[4] = tracer_at(dv3(10.0, 70.0, 0.0), dv3(800.0, 0.0, 0.0), 1.0f);
    c.b[4].ammo = 99;
    c.count = 5;
    CHECK(combat_tracers(&c, eye, 1.0, out, 8) == 0);

    /* Straight back toward the shooter, and room for only two of three. */
    for (int i = 0; i < 3; i++) c.b[i] = tracer_at(dv3(50.0 + i, 70.0, 0.0), dv3(0.0, 0.0, -800.0), 1.0f);
    c.count = 3;
    CHECK(combat_tracers(&c, eye, 1.0, out, 2) == 2);
    CHECK(fabsf(out[0].scale[2] - (float)(800.0 * PHYS_DT)) < 1e-3f && isfinite(out[0].rot[3]));
}

static int near3(const float a[3], float x, float y, float z, float tol)
{ return fabsf(a[0] - x) < tol && fabsf(a[1] - y) < tol && fabsf(a[2] - z) < tol; }

/* The shot leaves the barrel's end along the bore: down the sights the bore
 * runs 5.8 cm under the line of sight, parallel to it; at the hip it is low,
 * right, and points a little off the crosshair, as the rifle does. */
static void test_muzzle_origin(void)
{
    float p[3] = {0}, d[3] = {0};
    viewmodel_muzzle(1.0f, p, d);
    CHECK(near3(p, 0.0f, -0.058f, -0.88f, 0.002f) && near3(d, 0.0f, 0.0f, -1.0f, 1e-4f));
    viewmodel_muzzle(0.0f, p, d);
    float off = acosf(fminf(1.0f, -d[2]));
    CHECK(p[0] > 0.05f && p[1] < -0.1f && p[2] < -0.5f && off > 0.02f && off < 0.25f);
    CHECK(fabsf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2] - 1.0f) < 1e-4f);
    float q[3] = {0}, e[3] = {0};
    viewmodel_muzzle(NAN, q, e);
    CHECK(near3(q, p[0], p[1], p[2], 1e-6f) && near3(e, d[0], d[1], d[2], 1e-6f));

    /* In the world, looking along +x down the sights. */
    dvec3 at = {0};
    vec3 bore = {0};
    combat_muzzle(NULL, dv3(0.0, 70.0, 0.0), v3(1.0f, 0.0f, 0.0f), 1.0f, &at, &bore);
    CHECK(fabs(at.x - 0.88) < 0.002 && fabs(at.y - 69.942) < 0.002 && fabs(at.z) < 0.002);
    CHECK(fabsf(bore.x - 1.0f) < 1e-4f && fabsf(bore.y) < 1e-4f && fabsf(bore.z) < 1e-4f);
    /* Looking up and to the side, the bore stays under the sight line. */
    vec3 look = v3(0.0f, 0.6f, -0.8f);
    combat_muzzle(NULL, dv3(0.0, 70.0, 0.0), look, 1.0f, &at, &bore);
    CHECK(fabsf(bore.x - look.x) < 1e-3f && fabsf(bore.y - look.y) < 1e-3f && fabsf(bore.z - look.z) < 1e-3f);
    CHECK(fabs(at.y - (70.0 + 0.88 * 0.6 - 0.058 * 0.8)) < 0.003);

    /* A fired bullet starts there; against a wall it starts at the eye. */
    test_world t = {0};
    tw_init(&t);
    mobs ms = {0};
    mobs_init(&ms, 7);
    combat c = {0};
    combat_init(&c, 7);
    combat_targets tg = {NULL, NULL, 0};
    inventory inv = {0};
    inv_init(&inv);
    inv_add(&inv, I_RIFLE, 1);
    inv_add(&inv, I_CARTRIDGE, 2);
    dvec3 eye = dv3(0.5, GROUND + 1.6, 0.5);
    combat_out o = combat_frame(&c, &ms, &t.ph, &tg, 0, &inv, eye, v3(1.0f, 0.0f, 0.0f), 5.0, 1, 1.0f, 1.0f / 60.0f);
    CHECK(o.fired && c.count == 1 && fabs(o.at.x - 1.38) < 0.002 && c.b[0].pos.x == o.at.x && c.b[0].pos.y == o.at.y &&
          c.b[0].vel.x > 0.0);
    world_set(&t.w, 1, GROUND + 1, 0, B_STONE, 0);
    c.cooldown[0] = 0.0f;
    o = combat_frame(&c, &ms, &t.ph, &tg, 0, &inv, eye, v3(1.0f, 0.0f, 0.0f), 5.0, 1, 1.0f, 1.0f / 60.0f);
    CHECK(o.fired && c.count == 2 && c.b[1].pos.x == eye.x && c.b[1].pos.y == eye.y);
    tw_free(&t);
}

void test_muzzle_all(void)
{
    test_muzzle_origin();
    test_muzzle_spawn();
    test_smoke_drift();
    test_recycle();
    test_muzzle_draw();
    test_muzzle_textures();
    test_flash_light();
    test_rounds();
    test_tracers();
}
