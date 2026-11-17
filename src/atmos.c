#include "atmos.h"
#include "noise.h"
#include "physics.h"

#include <math.h>

atmos_air atmos_at(double y, float temp_c)
{
    /* The standard atmosphere's pressure, good to 11 km; the world spans
     * a few hundred metres either side of sea level. */
    double h = fmin(fmax(y - SEA_LEVEL, -1000.0), 11000.0);
    double p = 101325.0 * pow(1.0 - 2.25577e-5 * h, 5.25588);
    double t = isfinite(temp_c) ? fmax((double)temp_c + 273.15, 150.0) : 288.15;
    atmos_air a = {0};
    a.rho = p / (287.05 * t);
    a.sound = sqrt(1.4 * 287.05 * t);
    a.gravity = GRAVITY;
    return a;
}

/* Smooth noise in [0, 1] along one axis: hashed values at whole x, eased
 * between. */
static double noise1(uint32_t seed, double x)
{
    double f = floor(x);
    uint32_t i = (uint32_t)(int64_t)f;
    double a = (double)(hash_u32(seed ^ (i * 2654435761u)) >> 8) / 16777216.0;
    double b = (double)(hash_u32(seed ^ ((i + 1u) * 2654435761u)) >> 8) / 16777216.0;
    double u = x - f;
    u = u * u * (3.0 - 2.0 * u);
    return a + (b - a) * u;
}

dvec3 atmos_wind(uint32_t seed, double t)
{
    double dir = 4.0 * MC_PI * noise1(seed ^ 0x51A7u, t / 7200.0);
    double speed = 9.0 * noise1(seed ^ 0x3E11u, t / 1800.0);
    double gust = 1.0 + 0.35 * (2.0 * noise1(seed ^ 0x6057u, t / 6.0) - 1.0);
    return dv3(cos(dir) * speed * gust, 0.0, sin(dir) * speed * gust);
}

dvec3 atmos_wind_at(const world *w, uint32_t seed, double t, dvec3 pos)
{
    int top = world_surface_y(w, (int)floor(pos.x), (int)floor(pos.z));
    if (top < 0 || pos.y < (double)top) return dv3(0.0, 0.0, 0.0);
    return atmos_wind(seed, t);
}
