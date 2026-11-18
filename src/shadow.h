/* Sun and moon shadows: the parts of the renderer's shadow map that need
 * no GPU. The renderer draws the terrain's depth as seen from the sun (by
 * night the moon) into one SHADOW_SIZE square map, an orthographic view
 * SHADOW_RADIUS around the eye, and block.frag compares against it: a
 * face the sun can't reach gets the sky light of a face turned away from
 * the sun (lightfx.h), so shadows are as deep as the sun's own shading
 * and fade with it at dusk. Under the map's edge the flood-filled sky
 * light still darkens what is overhung.
 *
 * Here: the map's matrix, which keeps whole texels fixed in the world as
 * the eye moves (no shimmering edges) and turns with the sun in steps;
 * which sections and face groups it draws; and the `shadows` setting. It
 * doesn't draw or read the clock; Auto reuses bloom's measurement. */
#ifndef MC_SHADOW_H
#define MC_SHADOW_H

#include "mathlib.h"

#include <stdint.h>

/* The `shadows` setting, as bloom's. */
#define SHADOW_OFF 0
#define SHADOW_ON 1
#define SHADOW_AUTO 2

/* 2048 texels over 128 m: 6 cm, a texel a sixteenth of a block, the
 * textures' own pixel. Beyond the radius only the flood-filled sky light
 * shades. */
#define SHADOW_SIZE 2048
#define SHADOW_RADIUS 64.0f
/* Metres of depth either side of the eye along the sun: the world's
 * height and more, so a peak above the eye still shades it. 16-bit depth
 * then resolves 1 cm. */
#define SHADOW_DEPTH 400.0f
/* The map follows the sun once it has moved a quarter of a degree (a
 * minute of a 24-minute day), so its texels don't crawl every frame. */
#define SHADOW_TURN 0.0044f
/* Auto keeps shadows when drawing the map costs under this: on the Intel
 * N-series iGPU the shaders are tuned on, it doesn't. */
#define SHADOW_BUDGET_MS 0.5f
/* Columns around the eye whose sections can cast into the map. */
#define SHADOW_REACH ((int)(2.0f * SHADOW_RADIUS) / 16 + 1)

/* Points `kept` (unit) toward the light in sun (lightfx_frame.sun: its
 * direction times a contrast that is 0 in twilight). Leaves it while the
 * light is within SHADOW_TURN of it. Returns 0 when the light is too low
 * to cast shadows; kept then is unchanged. */
int shadow_aim(float kept[3], const float sun[3]);

/* The map's matrix (column-major, as mat4): camera-relative world to
 * clip space, x and y -1..1 across the map, z 0 nearest the light to 1.
 * dir is the unit direction toward the light, eye the camera's world
 * position: the map's texel grid is fixed in the world. */
void shadow_matrix(const float dir[3], dvec3 eye, float m[16]);

/* 1 if the cube from lo (camera-relative) with edge `size` metres can
 * reach the map. */
int shadow_box(const float m[16], const float lo[3], float size);

/* The face groups (mesher order +X, -X, +Y, -Y, +Z, -Z, as bits) turned
 * from the light: the map holds only those. The far side of everything
 * casts the shadow, so a sunlit floor is never in the map and can't
 * shadow itself. */
uint32_t shadow_groups(const float dir[3]);

#endif
