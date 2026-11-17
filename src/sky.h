/* The night sky: a fixed field of stars, with a band of the Milky Way,
 * that turns about the celestial pole once a day; the moon opposite the
 * sun; and how much of either shows. Builds the instances and the numbers;
 * renderer.c draws them (shaders/star.vert, star.frag) as small quads at
 * the far plane where nothing else was drawn. No GPU or clock here. */
#ifndef MC_SKY_H
#define MC_SKY_H

#include "entity.h"
#include <stdint.h>

#define SKY_STARS 1800

/* One star or the moon, as star.vert reads it: 32 bytes. */
typedef struct {
    float dir[3]; /* unit vector in the star field's own frame (world frame for the moon) */
    float size;   /* half the quad's side, in pixels of a 1080-line screen */
    float rgb[3]; /* colour times brightness */
    float phase;  /* twinkle phase 0..1; negative: steady (the moon) */
} sky_star;

/* Fills out[0 .. SKY_STARS) from seed; the same seed gives the same sky. */
void sky_stars(uint32_t seed, sky_star *out);

/* The star field's orientation at day 0..1 (thermo_day_time): a turn a day
 * about the celestial pole, which stands 40 degrees above the northern
 * horizon (-z), in the same sense as the sun. */
quat sky_rotation(double day);

/* How much of the night sky shows at daylight (thermo_daylight,
 * THERMO_NIGHT_LIGHT at night): none by day, coming out through dusk, all at night. */
float sky_visibility(float daylight);

/* The moon, opposite the sun (sun_dir from thermo_sun), as an instance. */
sky_star sky_moon(const float sun_dir[3]);

#endif
