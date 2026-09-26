/* Thermodynamics: the day, the air, and heat moving through blocks.
 *
 * - A day clock on the survival clock (HEALTH_CLOCK x real time): one day
 *   is 20 real minutes. Daylight and sky colour follow the sun.
 * - Ambient air temperature from altitude (the terrain is vertically
 *   compressed, so the lapse rate is scaled to put 0 C at the snow line),
 *   time of day and shelter (enclosed spaces sit near the ground's mean).
 * - A sparse heat field: only cells that differ from ambient are kept.
 *   Heat conducts between neighbouring cells with each material's
 *   conductivity and heat capacity (blocks.c), air cells also carry heat
 *   upward by convection and lose it to the open air.
 * - Campfires are heat sources that burn their fuel (block meta) and turn
 *   to ash when it runs out.
 * - Phase changes with latent heat: snow and ice melt into water (volume
 *   conserving: snow gives less water than ice), still surface water
 *   exposed to freezing air freezes.
 * - What the body feels: air temperature at a point including local heat,
 *   radiant flux from fires (Stefan-Boltzmann, falling off with the square
 *   of distance), and the temperature of a surface.
 *
 * Slow thermal processes (conduction through solids, melting, fuel) run on
 * the survival clock like healing and thirst; radiant heat on the body is
 * immediate. No window or GPU. */
#ifndef MC_THERMO_H
#define MC_THERMO_H

#include <stdint.h>
#include "mathlib.h"
#include "world.h"

#define THERMO_MAX_CELLS 16384
#define THERMO_DAY_START 0.33 /* 08:00 */

typedef struct thermo thermo;

thermo *thermo_create(uint32_t seed);
void thermo_destroy(thermo *t);

/* ---------------------------------------------------------- the sun */

double thermo_day_time(const thermo *t);   /* 0..1, 0 = midnight, 0.5 = noon */
void thermo_set_day_time(thermo *t, double day);
float thermo_daylight(const thermo *t);    /* 0.18 at night .. 1 at noon, smooth at dawn/dusk */
void thermo_sky(const thermo *t, float rgb[3]); /* sky/fog colour, sRGB 0..1 */

/* ---------------------------------------------------------- queries */

/* Air temperature at a point with no local heat sources, degrees C. */
float thermo_ambient(const thermo *t, const world *w, double x, double y, double z);
/* Air temperature including the heat field (a fire warms the air above
 * and around it), degrees C. */
float thermo_air(const thermo *t, const world *w, double x, double y, double z);
/* Temperature of the block in a cell (its surface for the feet, its body
 * for melting), degrees C. Burning campfires report their flame. */
float thermo_block_temp(const thermo *t, const world *w, int x, int y, int z);
/* Radiant heat from burning campfires onto a body at p, W/m^2 of skin
 * facing them (0 with no fire in sight). */
double thermo_radiant(const thermo *t, const world *w, dvec3 p);
/* 1 if a burning campfire is within r metres of p (crafting glass, brick). */
int thermo_near_fire(const thermo *t, const world *w, dvec3 p, double r);

/* ---------------------------------------------------------- updates */

/* Hook for world block changes (chain it with the physics hook): wakes the
 * heat field around the change and registers or forgets campfires. */
void thermo_on_block_changed(thermo *t, const world *w, int x, int y, int z, uint8_t old_id, uint8_t new_id);

/* Advances dt real seconds: the day clock and fires, conduction, melting
 * and freezing near the player. Edits the world (melting, fuel, ash). */
void thermo_step(thermo *t, world *w, double dt, dvec3 player);

/* Adds fuel to the campfire at a cell: returns 1 if it took it. units is
 * in fuel units (a stick 1, planks 2, a log 4). */
int thermo_add_fuel(thermo *t, world *w, int x, int y, int z, int units);

/* Cells in the heat field and burning fires, for the F3 overlay. */
int thermo_cell_count(const thermo *t);
int thermo_fire_count(const thermo *t);

#endif
