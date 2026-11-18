/* The frame's lighting (lightfx), the sun's direction (thermo_sun) and the
 * torch model. */
#include "block.h"
#include "item.h"
#include "light.h"
#include "lightfx.h"
#include "mem.h"
#include "sky.h"
#include "test_util.h"
#include "thermo.h"
#include "torch_model.h"

#include <math.h>

static int close_to(float a, float b, float eps) { return fabsf(a - b) <= eps; }

static float len3(const float v[3]) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

static void sun_at(thermo *th, double day, float dir[3])
{
    thermo_set_day_time(th, day);
    thermo_sun(th, dir);
}

static void test_sun(void)
{
    thermo *th = thermo_create(3);
    float d[3] = {0.0f, 0.0f, 0.0f};
    /* Noon: due south of overhead, 50 degrees up at latitude 40. */
    sun_at(th, 0.5, d);
    CHECK(close_to(d[0], 0.0f, 1e-5f));
    CHECK(close_to(d[1], 0.766f, 1e-3f));
    CHECK(d[2] > 0.6f);
    CHECK(close_to(len3(d), 1.0f, 1e-4f));
    /* 06:00 rises in the east (+x) on the horizon; 18:00 sets in the west. */
    sun_at(th, 0.25, d);
    CHECK(close_to(d[0], 1.0f, 1e-4f));
    CHECK(close_to(d[1], 0.0f, 1e-4f));
    sun_at(th, 0.75, d);
    CHECK(close_to(d[0], -1.0f, 1e-4f));
    /* Midnight: below the horizon, every hour a unit vector. */
    sun_at(th, 0.0, d);
    CHECK(d[1] < -0.7f);
    for (int h = 0; h < 24; h++) {
        sun_at(th, h / 24.0, d);
        CHECK(close_to(len3(d), 1.0f, 1e-4f));
        /* block.vert rebuilds z from y with tan(40 deg). */
        CHECK(close_to(d[2], d[1] * 0.8391f, 1e-3f));
    }
    thermo_destroy(th);
}

static void build_at(lightfx_frame *f, double day, uint32_t held, int brightness)
{
    thermo *th = thermo_create(4);
    float d[3] = {0.0f, 0.0f, 0.0f};
    sun_at(th, day, d);
    lightfx_build(f, d, thermo_daylight(th), held, brightness, 12.0, 1);
    thermo_destroy(th);
}

static void test_sky_colour(void)
{
    lightfx_frame f = {0};
    /* Noon: white and full. */
    build_at(&f, 0.5, 0, 0);
    for (int i = 0; i < 3; i++) CHECK(f.sky[i] > 0.97f);
    CHECK(close_to(len3(f.sun), 1.0f, 1e-3f));
    /* Just after sunrise: warm, red over blue, and dimmer. */
    build_at(&f, 0.26, 0, 0);
    CHECK(f.sky[0] > f.sky[1] && f.sky[1] > f.sky[2]);
    CHECK(f.sky[0] < 0.9f);
    CHECK(f.sun[0] > 0.0f); /* lit from the east */
    /* Midnight: moonlight is dim and blue, and comes from the moon,
     * opposite the sun, at half the contrast. */
    build_at(&f, 0.0, 0, 0);
    CHECK(f.sky[2] > f.sky[0]);
    CHECK(f.sky[2] < 0.25f);
    CHECK(f.sun[1] > 0.0f);
    CHECK(len3(f.sun) <= LIGHTFX_MOON_CONTRAST + 1e-4f);
    CHECK(len3(f.sun) > 0.4f);
    /* Twilight: no direction to speak of. */
    thermo *th = thermo_create(5);
    float d[3] = {0.0f, 0.0f, 0.0f};
    sun_at(th, 0.25, d);
    lightfx_build(&f, d, thermo_daylight(th), 0, 0, 0.0, 1);
    CHECK(len3(f.sun) < 0.01f);
    thermo_destroy(th);
}

static void test_held(void)
{
    lightfx_frame f = {0};
    build_at(&f, 0.5, 0, 0);
    for (int i = 0; i < 3; i++) CHECK(f.held[i] == 0.0f);
    /* A torch (14, 10, 5) is a level dimmer in the hand, and flickers. */
    build_at(&f, 0.5, B_TORCH, 0);
    CHECK(f.held[0] > f.held[1] && f.held[1] > f.held[2] && f.held[2] > 0.0f);
    CHECK(f.held[0] <= 13.0f / 15.0f + 1e-5f);
    CHECK(f.held[0] >= 13.0f / 15.0f * 0.9f);
    /* Foxfire glows steadily: exactly a level below its block. */
    build_at(&f, 0.5, B_FOXFIRE, 0);
    CHECK(close_to(f.held[1], 6.0f / 15.0f, 1e-6f));
    CHECK(close_to(f.held[0], 1.0f / 15.0f, 1e-6f));
    /* Things that give no light, blocks or items. */
    build_at(&f, 0.5, B_STONE, 0);
    CHECK(f.held[0] == 0.0f && f.held[1] == 0.0f && f.held[2] == 0.0f);
    build_at(&f, 0.5, I_STICK, 0);
    CHECK(f.held[0] == 0.0f);
}

static void test_floor_and_flicker(void)
{
    lightfx_frame f = {0};
    build_at(&f, 0.5, 0, 0);
    CHECK(f.floor == 0.0f);
    build_at(&f, 0.5, 0, 100);
    CHECK(close_to(f.floor, LIGHTFX_FLOOR_MAX, 1e-6f));
    build_at(&f, 0.5, 0, 50);
    CHECK(close_to(f.floor, LIGHTFX_FLOOR_MAX * 0.5f, 1e-6f));
    build_at(&f, 0.5, 0, 400);
    CHECK(close_to(f.floor, LIGHTFX_FLOOR_MAX, 1e-6f));
    build_at(&f, 0.5, 0, -20);
    CHECK(f.floor == 0.0f);
    float lo = 2.0f, hi = -1.0f;
    for (int i = 0; i < 2000; i++) {
        float v = lightfx_flicker(i * 0.013);
        lo = fminf(lo, v);
        hi = fmaxf(hi, v);
    }
    CHECK(lo >= 0.9f && hi <= 1.0f);
    CHECK(hi - lo > 0.04f); /* it does move */
    CHECK(lightfx_flicker(3.7) == lightfx_flicker(3.7));
    CHECK(fabsf(0.5f * (lo + hi) - LIGHTFX_STEADY) < 0.02f); /* steady sits mid-way */

    /* Flicker off: a held torch is the same at every moment. */
    float d[3] = {0.0f, 1.0f, 0.0f}, first = -1.0f;
    for (int i = 0; i < 50; i++) {
        lightfx_build(&f, d, 1.0f, B_TORCH, 0, i * 0.037, 0);
        if (i == 0) first = f.held[0];
        CHECK(f.held[0] == first);
    }
    CHECK(first > 0.0f);
    lightfx_build(&f, d, 1.0f, B_TORCH, 0, 0.0, 1);
    float a = f.held[0];
    lightfx_build(&f, d, 1.0f, B_TORCH, 0, 0.11, 1);
    CHECK(a != f.held[0]); /* on, it moves */
}

static void test_faces(void)
{
    lightfx_frame f = {0};
    build_at(&f, 0.5, 0, 0);
    /* Noon: the top faces the sun best, the bottom not at all; the north
     * face (-z) sees none of a southern sun. */
    float top = lightfx_face(&f, 2), bottom = lightfx_face(&f, 3);
    CHECK(top > lightfx_face(&f, 4) && top > lightfx_face(&f, 0));
    CHECK(close_to(bottom, 1.0f - LIGHTFX_SUN_SHADE * len3(f.sun), 1e-5f));
    CHECK(close_to(lightfx_face(&f, 5), bottom, 1e-5f));
    CHECK(close_to(lightfx_face(&f, 0), lightfx_face(&f, 1), 1e-5f));
    /* Morning: east (+x) is lit, west in shade. */
    build_at(&f, 0.3, 0, 0);
    CHECK(lightfx_face(&f, 0) > lightfx_face(&f, 1));
    /* Codes fit f_layer's top byte, and a face fully toward the sun
     * loses nothing. */
    for (int d = 0; d < 48; d++) {
        build_at(&f, d / 48.0, 0, 0);
        for (int face = 0; face < 6; face++) {
            float m = lightfx_face(&f, face);
            CHECK(m >= 1.0f - LIGHTFX_SUN_SHADE - 1e-5f && m <= 1.0f + 1e-5f);
            CHECK(lightfx_face_code(&f, face) < 256u);
        }
    }
    const lightfx_frame up = {{1.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 0.0f};
    CHECK(lightfx_face_code(&up, 2) == 0u);
    CHECK(lightfx_face_code(&up, 3) == 156u);
}

static void test_tone(void)
{
    /* The identity up to the knee, smooth and rising after it, and never
     * reaching white. */
    CHECK(lightfx_tone(0.0f) == 0.0f);
    CHECK(lightfx_tone(0.5f) == 0.5f);
    CHECK(close_to(lightfx_tone(LIGHTFX_TONE_KNEE), LIGHTFX_TONE_KNEE, 1e-6f));
    float prev = 0.0f;
    for (int i = 1; i <= 400; i++) {
        float y = lightfx_tone((float)i * 0.01f);
        CHECK(y > prev);
        CHECK(y < 1.0f);
        prev = y;
    }
    float e = 1e-3f;
    float slope = (lightfx_tone(LIGHTFX_TONE_KNEE + e) - lightfx_tone(LIGHTFX_TONE_KNEE)) / e;
    CHECK(close_to(slope, 1.0f, 0.01f));
    CHECK(lightfx_tone(1.3f) > 0.85f);
}

static void test_at(void)
{
    lightfx_frame f = {0};
    build_at(&f, 0.5, 0, 0);
    /* Full block light is gained by 1.3; the noon sky is about 1. */
    CHECK(close_to(lightfx_at(&f, LIGHT_PACK(0, 15, 15, 15), 0.0f), LIGHTFX_BLOCK_GAIN, 1e-3f));
    CHECK(close_to(lightfx_at(&f, LIGHT_OPEN_SKY, 0.0f), 1.0f, 0.03f));
    /* A dark cave: the floor lifts it, the held torch more, and less
     * the farther away. */
    float dark = lightfx_at(&f, 0, 0.0f);
    CHECK(dark < 0.06f);
    build_at(&f, 0.5, 0, 100);
    float floor = lightfx_at(&f, 0, 0.0f);
    CHECK(floor > dark * 2.0f);
    build_at(&f, 0.5, B_TORCH, 0);
    float near = lightfx_at(&f, 0, 0.3f), far = lightfx_at(&f, 0, 8.0f);
    CHECK(near > 0.4f);
    CHECK(far < near && far > dark);
}

/* The light on an entity per channel: torchlight is orange, the open sky
 * by day near white and by night the moon's blue, and the brightness
 * matches lightfx_at. */
static void test_colour(void)
{
    lightfx_frame f = {0};
    float rgb[3] = {0.0f, 0.0f, 0.0f};
    build_at(&f, 0.5, 0, 0);
    float b = lightfx_colour(&f, light_emission(B_TORCH), 0.0f, rgb);
    CHECK(close_to(b, lightfx_at(&f, light_emission(B_TORCH), 0.0f), 1e-5f));
    CHECK(rgb[0] == 1.0f && rgb[1] < 0.9f && rgb[2] < rgb[1]);
    b = lightfx_colour(&f, LIGHT_OPEN_SKY, 0.0f, rgb);
    CHECK(close_to(b, lightfx_at(&f, LIGHT_OPEN_SKY, 0.0f), 1e-5f));
    CHECK(rgb[0] > 0.85f && rgb[1] > 0.85f && rgb[2] > 0.85f);
    /* A blue evening sky (by full night the black level, as on the
     * terrain, outshines the moon and the colour goes grey). */
    lightfx_frame eve = {0};
    eve.sky[0] = 0.2f;
    eve.sky[1] = 0.3f;
    eve.sky[2] = 0.5f;
    lightfx_colour(&eve, LIGHT_OPEN_SKY, 0.0f, rgb);
    CHECK(rgb[2] == 1.0f && rgb[0] < 0.6f && rgb[1] < rgb[2]);
    /* A carried torch lights a dark cell orange nearby, not far off. */
    build_at(&f, 0.5, B_TORCH, 0);
    float near = lightfx_colour(&f, 0, 1.0f, rgb);
    CHECK(near > 0.3f && rgb[0] == 1.0f && rgb[2] < 0.8f);
    float far = lightfx_colour(&f, 0, 12.0f, rgb);
    CHECK(far < near);
    /* Black with no floor: a neutral colour, never a division by 0. */
    lightfx_frame none = {0};
    none.floor = -100.0f;
    none.held[0] = none.held[1] = none.held[2] = -100.0f;
    float z = lightfx_colour(&none, 0, 0.0f, rgb);
    CHECK(z >= 0.0f && rgb[0] <= 1.0f && rgb[1] <= 1.0f && rgb[2] <= 1.0f);
}

/* Coloured light multiplies into an instance's tint, except where the
 * part glows. */
static void test_shine(void)
{
    entity_instance e[3] = {0};
    e[1].tint = entity_tint(0x80FF40u);
    e[2].tint = entity_tint(0x80FF40u) | ENTITY_GLOWS;
    const float warm[3] = {1.0f, 0.5f, 0.25f};
    entity_shine(e, 3, warm);
    CHECK(e[0].tint == entity_tint(0xFF8040u));
    CHECK(e[1].tint == entity_tint(0x808010u));
    CHECK(e[2].tint == (entity_tint(0x80FF40u) | ENTITY_GLOWS));
    /* White leaves the colour as it was; out-of-range values are held to 0..1. */
    const float white[3] = {1.0f, 1.0f, 1.0f}, wild[3] = {2.0f, -1.0f, 1.0f};
    entity_instance w = {0};
    w.tint = entity_tint(0x123456u);
    entity_shine(&w, 1, white);
    CHECK(w.tint == entity_tint(0x123456u));
    entity_shine(&w, 1, wild);
    CHECK(w.tint == entity_tint(0x120056u));
}

static void test_pack(void)
{
    lightfx_frame f = {0};
    build_at(&f, 0.0, B_TORCH, 60);
    float sky[3] = {0.0f, 0.0f, 0.0f};
    uint32_t bits = lightfx_pack(&f, 0.5f, sky);
    for (int i = 0; i < 3; i++) {
        CHECK(close_to(sky[i], log2f(f.sky[i]) / LIGHT_CURVE_K, 1e-5f));
        CHECK(close_to((float)((bits >> (8 * i)) & 255u) / 255.0f, f.held[i], 0.5f / 255.0f + 1e-6f));
    }
    CHECK(close_to((float)(bits >> 24) / 255.0f, f.floor, 0.5f / 255.0f + 1e-6f));
    /* With no sky colour, daylight alone. */
    const lightfx_frame none = {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 0.0f};
    CHECK(lightfx_pack(&none, 0.5f, sky) == 0u);
    for (int i = 0; i < 3; i++) CHECK(close_to(sky[i], -1.0f / LIGHT_CURVE_K, 1e-5f));
    /* Overbright levels are clamped, not wrapped. */
    const lightfx_frame hot = {{1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f}, {2.0f, -1.0f, 0.5f}, 3.0f};
    bits = lightfx_pack(&hot, 1.0f, sky);
    CHECK((bits & 255u) == 255u);
    CHECK(((bits >> 8) & 255u) == 0u);
    CHECK((bits >> 24) == 255u);
}

static void test_fog(void)
{
    lightfx_frame f = {0};
    build_at(&f, 0.5, 0, 0);
    const float sky[3] = {0.5f, 0.7f, 1.0f};
    float out[3] = {0.0f, 0.0f, 0.0f};
    /* Under open sky the fog is the sky. */
    lightfx_fog(&f, sky, LIGHT_OPEN_SKY, out);
    for (int i = 0; i < 3; i++) CHECK(close_to(out[i], sky[i], 1e-5f));
    /* Deep in a cave it is dark, but not black. */
    lightfx_fog(&f, sky, 0, out);
    for (int i = 0; i < 3; i++) CHECK(out[i] < 0.07f && out[i] > 0.0f);
    /* By a fire it warms. */
    lightfx_fog(&f, sky, light_emission(B_CAMPFIRE), out);
    CHECK(out[0] > out[2]);
    CHECK(out[0] > 0.2f);
    /* Easing: no time no change, a long time the target, and two half
     * steps the same as one whole. */
    float cur[3] = {0.0f, 0.0f, 0.0f}, a[3] = {0.0f, 0.0f, 0.0f};
    const float to[3] = {1.0f, 0.5f, 0.25f};
    lightfx_ease(cur, to, 0.0f);
    CHECK(cur[0] == 0.0f);
    lightfx_ease(cur, to, -1.0f);
    CHECK(cur[0] == 0.0f);
    lightfx_ease(cur, to, 60.0f);
    CHECK(close_to(cur[0], 1.0f, 1e-4f) && close_to(cur[2], 0.25f, 1e-4f));
    lightfx_ease(a, to, 0.1f);
    lightfx_ease(a, to, 0.1f);
    float b[3] = {0.0f, 0.0f, 0.0f};
    lightfx_ease(b, to, 0.2f);
    for (int i = 0; i < 3; i++) CHECK(close_to(a[i], b[i], 1e-5f));
}

static void test_torch_model(void)
{
    entity_instance m[TORCH_PARTS] = {0};
    const float at[3] = {1.0f, 2.0f, 3.0f};
    CHECK(torch_model(at, quat_identity(), 1.0f, 5.0, 9u, 0.4f, m) == TORCH_PARTS);
    float top = 0.0f;
    for (int i = 0; i < TORCH_PARTS; i++) {
        for (int k = 0; k < 3; k++) CHECK(m[i].scale[k] > 0.0f);
        CHECK(m[i].pos[1] >= at[1]);
        top = fmaxf(top, m[i].pos[1] + 0.5f * m[i].scale[1] - at[1]);
        CHECK(fabsf(m[i].pos[0] - at[0]) < 0.1f && fabsf(m[i].pos[2] - at[2]) < 0.1f);
        float q = m[i].rot[0] * m[i].rot[0] + m[i].rot[1] * m[i].rot[1] + m[i].rot[2] * m[i].rot[2] +
                  m[i].rot[3] * m[i].rot[3];
        CHECK(close_to(q, 1.0f, 1e-4f));
    }
    CHECK(top <= TORCH_HEIGHT + 0.25f);
    /* The handle takes the cell's light, the head is charred darker, and
     * the ember, the flame and the sparks bring their own. */
    CHECK(m[0].light == 0.4f);
    CHECK(m[1].light < 0.4f && m[1].light > 0.0f);
    for (int i = 2; i < TORCH_PARTS; i++) CHECK(m[i].light == 1.0f);
    /* Only what glows is marked so; the light around passes it by. */
    CHECK(!(m[0].tint & ENTITY_GLOWS) && !(m[1].tint & ENTITY_GLOWS));
    for (int i = 2; i < TORCH_PARTS; i++) CHECK(m[i].tint & ENTITY_GLOWS);
    CHECK((m[0].tex & 255u) == T_LOG_SIDE && (m[0].tex & ENTITY_TEX_METRIC));
    CHECK((m[3].tex & 255u) == T_LAVA && (m[4].tex & 255u) == T_FIRE0);
    /* The handle is the bottom part and the flame's layers climb. */
    CHECK(m[0].pos[1] < m[1].pos[1] && m[1].pos[1] < m[3].pos[1] && m[3].pos[1] < m[5].pos[1]);
    /* The same time gives the same model; the flame moves over time; two
     * torches burn out of step. */
    entity_instance n[TORCH_PARTS] = {0};
    torch_model(at, quat_identity(), 1.0f, 5.0, 9u, 0.4f, n);
    CHECK(n[5].pos[0] == m[5].pos[0] && n[6].pos[1] == m[6].pos[1]);
    torch_model(at, quat_identity(), 1.0f, 5.3, 9u, 0.4f, n);
    CHECK(n[5].pos[0] != m[5].pos[0] || n[5].scale[1] != m[5].scale[1]);
    CHECK(n[6].pos[1] != m[6].pos[1]);
    torch_model(at, quat_identity(), 1.0f, 5.0, 10u, 0.4f, n);
    CHECK(n[6].pos[1] != m[6].pos[1]);
    /* Size scales it about its foot; a quarter turn about x lays it along +z. */
    const float origin[3] = {0.0f, 0.0f, 0.0f};
    torch_model(origin, quat_identity(), 1.0f, 5.0, 9u, 0.4f, m);
    torch_model(origin, quat_identity(), 0.5f, 5.0, 9u, 0.4f, n);
    CHECK(close_to(n[1].pos[1], m[1].pos[1] * 0.5f, 1e-5f));
    CHECK(close_to(n[0].scale[1], m[0].scale[1] * 0.5f, 1e-6f));
    torch_model(origin, quat_axis(1.0f, 0.0f, 0.0f, 1.5707963f), 1.0f, 5.0, 9u, 0.4f, n);
    CHECK(close_to(n[1].pos[2], m[1].pos[1], 1e-4f));
    CHECK(close_to(n[1].pos[1], 0.0f, 1e-4f));
    /* Sparks shrink as they rise. */
    float lo = 1.0f, hi = 0.0f, s_lo = 0.0f, s_hi = 0.0f;
    for (int i = 0; i < 100; i++) {
        torch_model(origin, quat_identity(), 1.0f, i * 0.014, 9u, 0.4f, n);
        if (n[6].pos[1] < lo) {
            lo = n[6].pos[1];
            s_lo = n[6].scale[0];
        }
        if (n[6].pos[1] > hi) {
            hi = n[6].pos[1];
            s_hi = n[6].scale[0];
        }
    }
    CHECK(hi - lo > 0.25f);
    CHECK(s_hi < s_lo);
}

static void test_sky(void)
{
    sky_star *a = mem_alloc(mem_array_size(SKY_STARS, sizeof *a));
    sky_star *b = mem_alloc(mem_array_size(SKY_STARS, sizeof *b));
    sky_stars(1u, a);
    sky_stars(1u, b);
    int same = 1, bright = 0, up = 0;
    for (int i = 0; i < SKY_STARS; i++) {
        same &= a[i].dir[0] == b[i].dir[0] && a[i].rgb[1] == b[i].rgb[1];
        CHECK(close_to(len3(a[i].dir), 1.0f, 1e-4f));
        CHECK(a[i].size >= 0.8f && a[i].size <= 2.0f);
        CHECK(a[i].phase >= 0.0f && a[i].phase < 1.0f);
        for (int k = 0; k < 3; k++) CHECK(a[i].rgb[k] > 0.0f && a[i].rgb[k] <= 1.0f);
        bright += a[i].rgb[1] > 0.5f;
        up += a[i].dir[1] > 0.0f;
    }
    CHECK(same);
    /* Most stars are faint, a few bright; both halves of the sky have some. */
    CHECK(bright > 10 && bright < SKY_STARS / 8);
    CHECK(up > SKY_STARS / 4 && up < SKY_STARS * 3 / 4);
    sky_stars(2u, b);
    CHECK(a[0].dir[0] != b[0].dir[0]);
    mem_free(a);
    mem_free(b);
    /* The field turns with the sun: rotating midnight's sun by the field's
     * turn at day d gives the sun at d. The pole stays put. */
    thermo *th = thermo_create(6);
    float s0[3] = {0.0f, 0.0f, 0.0f}, sd[3] = {0.0f, 0.0f, 0.0f}, r[3] = {0.0f, 0.0f, 0.0f};
    sun_at(th, 0.0, s0);
    for (int h = 1; h < 24; h += 5) {
        sun_at(th, h / 24.0, sd);
        quat_apply(sky_rotation(h / 24.0), s0, r);
        for (int k = 0; k < 3; k++) CHECK(close_to(r[k], sd[k], 2e-3f));
    }
    const float pole[3] = {0.0f, 0.6428f, -0.7660f};
    quat_apply(sky_rotation(0.37), pole, r);
    for (int k = 0; k < 3; k++) CHECK(close_to(r[k], pole[k], 1e-4f));
    /* Visible all night, gone by day, fading in between. */
    CHECK(sky_visibility(THERMO_NIGHT_LIGHT) == 1.0f);
    CHECK(sky_visibility(1.0f) == 0.0f);
    float prev = 1.0f;
    for (int i = 0; i <= 100; i++) {
        float v = sky_visibility(THERMO_NIGHT_LIGHT + (1.0f - THERMO_NIGHT_LIGHT) * (float)i / 100.0f);
        CHECK(v <= prev && v >= 0.0f);
        prev = v;
    }
    /* The moon stands opposite the sun and doesn't twinkle. */
    sun_at(th, 0.1, sd);
    sky_star m = sky_moon(sd);
    for (int k = 0; k < 3; k++) CHECK(m.dir[k] == -sd[k]);
    CHECK(m.phase < 0.0f && m.size > 10.0f);
    thermo_destroy(th);
}

void test_lightfx_all(void)
{
    test_sun();
    test_sky_colour();
    test_held();
    test_floor_and_flicker();
    test_faces();
    test_tone();
    test_at();
    test_colour();
    test_shine();
    test_pack();
    test_fog();
    test_torch_model();
    test_sky();
}
