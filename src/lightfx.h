/* The frame's lighting: what the block shaders get each frame besides the
 * light texture. The colour of daylight and moonlight and the direction
 * they come from, the light the player carries, the darkest level the
 * brightness setting allows, the fog's tint and the tone curve.
 *
 * The renderer packs a lightfx_frame into push constants, and
 * shaders/block.vert and block.frag repeat the curves below; the functions
 * here are their CPU twins, for the things drawn with one light value each
 * (the first-person hand) and for the tests. Pure functions of their
 * inputs: no world, GPU or clock. */
#ifndef MC_LIGHTFX_H
#define MC_LIGHTFX_H

#include <stdint.h>

struct world;

/* A face turned from the sun keeps 1 - LIGHTFX_SUN_SHADE of the sky's
 * light at full contrast (block.vert's SUN_SHADE). */
#define LIGHTFX_SUN_SHADE 0.4f
/* How strongly the moon shades faces, as a share of the sun's. */
#define LIGHTFX_MOON_CONTRAST 0.5f
/* block.vert hands the fragment shader each face's sun shading as a shift
 * of the sky's level, in steps of 1/1024 level in f_layer's top byte
 * (block.frag's SUN_STEP); the darkest, log2(0.6) / LIGHT_CURVE_K, is
 * step 157. */
#define LIGHTFX_SUN_STEP (1.0f / 1024.0f)
/* Block light is 1.3 times as bright as before the tone curve: a lit wall
 * by a lamp is brighter than white paper at noon, and the curve rolls it
 * off instead of clipping (block.frag's BLOCK_GAIN, as a level shift). */
#define LIGHTFX_BLOCK_GAIN 1.3f
/* The tone curve is the identity up to LIGHTFX_TONE_KNEE, then rolls off
 * toward 1 (block.frag's TONE_KNEE). */
#define LIGHTFX_TONE_KNEE 0.75f
/* At brightness 100 the darkest light is level 4.5 of 15. */
#define LIGHTFX_FLOOR_MAX 0.3f

typedef struct {
    float sky[3];  /* sky light's brightness per channel: daylight times its colour, 0..1 */
    float sun[3];  /* toward the sun by day and the moon by night, times its contrast 0..1 */
    float held[3]; /* the carried light's level at the eye, 0..1 per channel; 0 without one */
    float floor;   /* the darkest level, 0..1 */
} lightfx_frame;

/* Fills f for a sun at sun_dir (thermo_sun), daylight 0..1
 * (thermo_daylight), the held item (0 for none), the brightness setting
 * 0..100, the time in seconds and whether flames flicker (the setting;
 * off, a flame holds LIGHTFX_STEADY). */
void lightfx_build(lightfx_frame *f, const float sun_dir[3], float daylight, uint32_t held_item, int brightness,
                   double time, int flicker);

/* Adds a muzzle flash at `flash` 0..1 of its full brightness to the held
 * light: a warm white at level 15 at the eye that falls off with distance
 * as a torch's does, so the walls and ground nearby light up for a frame
 * or two. A brighter light already held stays. */
void lightfx_flash(lightfx_frame *f, float flash);

/* The light level 0..1 at a world position as an eye there meets it: the
 * cell's sky light shifted by daylight, its block light, and what the
 * body carries (held_item, 0 for none), whichever is brightest. Levels,
 * not brightness, because the curve has a floor: a moonlit field and a
 * sealed cave differ by a level but barely in brightness. *fire gets how
 * much of it is firelight, 0..1: warm light, red over blue, which
 * campfires, torches, lamps and lava give and foxfire does not. */
float lightfx_seen(const struct world *w, double x, double y, double z, float daylight, uint32_t held_item,
                   float *fire);

/* How dark it is for a light level from lightfx_seen, 0 (enough to see
 * by) .. 1 (pitch black): a moonlit field is about 0.9. */
float lightfx_dark(float level);

/* What a flame flickers by at time t: 0.9..1, the same at any frame rate.
 * LIGHTFX_STEADY is its mean, for when flicker is off. */
#define LIGHTFX_STEADY 0.95f
float lightfx_flicker(double t);

/* The share of the sky's light that reaches face 0..5 (+x, -x, +y, -y,
 * +z, -z) from f's sun: 1 facing it, down to 1 - LIGHTFX_SUN_SHADE. */
float lightfx_face(const lightfx_frame *f, int face);
/* That as block.vert sends it: the level shift in LIGHTFX_SUN_STEPs. */
uint32_t lightfx_face_code(const lightfx_frame *f, int face);

/* The tone curve on one channel: x up to the knee, then toward 1. */
float lightfx_tone(float x);

/* The brightness, before the tone curve, of something in a cell with
 * light l, dist metres from the eye, as block.frag computes it for a face
 * fully turned to the sun: the brightest of block light, sky light, the
 * held light and the floor, by their brightest channel. */
float lightfx_at(const lightfx_frame *f, uint16_t l, float dist);

/* block.frag's light push constants (shaders/push_frag.glsl): writes the
 * sky's level shift per channel, log2(brightness) / LIGHT_CURVE_K, to sky
 * and returns the held light and the floor as packUnorm4x8 does. A frame
 * with no sky colour (all zero) lights the sky by daylight alone. */
uint32_t lightfx_pack(const lightfx_frame *f, float daylight, float sky[3]);

/* The fog's colour from the sky's and the light where the eye is: dark in
 * a cave, warm by a fire. sky and out are linear 0..1. */
void lightfx_fog(const lightfx_frame *f, const float sky[3], uint16_t eye, float out[3]);
/* Moves cur toward target with a 1.5 s time constant, so the fog changes
 * gradually when the eye passes into a cave, at any frame rate. */
void lightfx_ease(float cur[3], const float target[3], float dt);

#endif
