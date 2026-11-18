/* The air: its density and speed of sound from altitude and temperature,
 * and the wind. Anything that flies reads it: bullets now, aircraft,
 * parachutes and thrown things later. Deterministic: the wind is a
 * function of a seed and the simulated time, never the clock.
 *
 * It holds no forces on bodies (ballistics.c, later the vehicles'
 * aerodynamics) and no weather beyond wind. No window or GPU. */
#ifndef MC_ATMOS_H
#define MC_ATMOS_H

#include "mathlib.h"
#include "world.h"

#include <stdint.h>

typedef struct {
    double rho;      /* density, kg/m^3 */
    double sound;    /* speed of sound, m/s */
    dvec3 wind;      /* m/s */
    double gravity;  /* m/s^2, down */
} atmos_air;

/* Still air at world height y (SEA_LEVEL is sea level) and temp_c:
 * pressure from the standard atmosphere, density from the gas law. */
atmos_air atmos_at(double y, float temp_c);

/* The wind in the open at time t (s of simulated time), from a seed:
 * a direction that turns over hours, a speed of 0 to 9 m/s that changes
 * over half hours, and gusts over seconds. */
dvec3 atmos_wind(uint32_t seed, double t);

/* The wind at a point: the open wind, or none under solid ground (in a
 * cave or a tunnel) or where the column isn't loaded. */
dvec3 atmos_wind_at(const world *w, uint32_t seed, double t, dvec3 pos);

#endif
