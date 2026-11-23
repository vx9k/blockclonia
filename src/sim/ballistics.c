#include "ballistics.h"

#include <math.h>

/* lb/in^2 to kg/m^2, for ballistic coefficients as published. */
#define BC_SI 703.0696

typedef struct {
    double mach, cd;
} drag_point;

/* The standard reference curves from the US Army's Ballistic Research
 * Laboratory: G1 for flat-based bullets and round balls, G7 for long
 * boat-tails. */
/* clang-format off */
static const drag_point G1[] = {
    {0.000, 0.2629}, {0.050, 0.2558}, {0.100, 0.2487}, {0.150, 0.2413}, {0.200, 0.2344}, {0.250, 0.2278},
    {0.300, 0.2214}, {0.350, 0.2155}, {0.400, 0.2104}, {0.450, 0.2061}, {0.500, 0.2032}, {0.550, 0.2020},
    {0.600, 0.2034}, {0.700, 0.2165}, {0.725, 0.2230}, {0.750, 0.2313}, {0.775, 0.2417}, {0.800, 0.2546},
    {0.825, 0.2706}, {0.850, 0.2901}, {0.875, 0.3136}, {0.900, 0.3415}, {0.925, 0.3734}, {0.950, 0.4084},
    {0.975, 0.4448}, {1.000, 0.4805}, {1.025, 0.5136}, {1.050, 0.5427}, {1.075, 0.5677}, {1.100, 0.5883},
    {1.125, 0.6053}, {1.150, 0.6191}, {1.200, 0.6393}, {1.250, 0.6518}, {1.300, 0.6589}, {1.350, 0.6621},
    {1.400, 0.6625}, {1.450, 0.6607}, {1.500, 0.6573}, {1.550, 0.6528}, {1.600, 0.6474}, {1.650, 0.6413},
    {1.700, 0.6347}, {1.750, 0.6280}, {1.800, 0.6210}, {1.850, 0.6141}, {1.900, 0.6072}, {1.950, 0.6003},
    {2.000, 0.5934}, {2.050, 0.5867}, {2.100, 0.5804}, {2.150, 0.5743}, {2.200, 0.5685}, {2.250, 0.5630},
    {2.300, 0.5577}, {2.350, 0.5527}, {2.400, 0.5481}, {2.450, 0.5438}, {2.500, 0.5397}, {2.600, 0.5325},
    {2.700, 0.5264}, {2.800, 0.5211}, {2.900, 0.5168}, {3.000, 0.5133}, {3.100, 0.5105}, {3.200, 0.5084},
    {3.300, 0.5067}, {3.400, 0.5054}, {3.500, 0.5040}, {3.600, 0.5030}, {3.700, 0.5022}, {3.800, 0.5016},
    {3.900, 0.5010}, {4.000, 0.5006}, {4.200, 0.4998}, {4.400, 0.4995}, {4.600, 0.4992}, {4.800, 0.4990},
    {5.000, 0.4988}
};
static const drag_point G7[] = {
    {0.000, 0.1198}, {0.050, 0.1197}, {0.100, 0.1196}, {0.150, 0.1194}, {0.200, 0.1193}, {0.250, 0.1194},
    {0.300, 0.1194}, {0.350, 0.1194}, {0.400, 0.1193}, {0.450, 0.1193}, {0.500, 0.1194}, {0.550, 0.1193},
    {0.600, 0.1194}, {0.650, 0.1197}, {0.700, 0.1202}, {0.725, 0.1207}, {0.750, 0.1215}, {0.775, 0.1226},
    {0.800, 0.1242}, {0.825, 0.1266}, {0.850, 0.1306}, {0.875, 0.1368}, {0.900, 0.1464}, {0.925, 0.1660},
    {0.950, 0.2054}, {0.975, 0.2993}, {1.000, 0.3803}, {1.025, 0.4015}, {1.050, 0.4043}, {1.075, 0.4034},
    {1.100, 0.4014}, {1.125, 0.3987}, {1.150, 0.3955}, {1.200, 0.3884}, {1.250, 0.3810}, {1.300, 0.3732},
    {1.350, 0.3657}, {1.400, 0.3580}, {1.500, 0.3440}, {1.550, 0.3376}, {1.600, 0.3315}, {1.650, 0.3260},
    {1.700, 0.3209}, {1.750, 0.3160}, {1.800, 0.3117}, {1.850, 0.3078}, {1.900, 0.3042}, {1.950, 0.3010},
    {2.000, 0.2980}, {2.050, 0.2951}, {2.100, 0.2922}, {2.150, 0.2892}, {2.200, 0.2864}, {2.250, 0.2835},
    {2.300, 0.2807}, {2.350, 0.2779}, {2.400, 0.2752}, {2.450, 0.2725}, {2.500, 0.2697}, {2.550, 0.2670},
    {2.600, 0.2643}, {2.650, 0.2615}, {2.700, 0.2588}, {2.750, 0.2561}, {2.800, 0.2533}, {2.850, 0.2506},
    {2.900, 0.2479}, {2.950, 0.2451}, {3.000, 0.2424}, {3.100, 0.2368}, {3.200, 0.2313}, {3.300, 0.2258},
    {3.400, 0.2205}, {3.500, 0.2154}, {3.600, 0.2106}, {3.700, 0.2060}, {3.800, 0.2017}, {3.900, 0.1975},
    {4.000, 0.1935}, {4.200, 0.1861}, {4.400, 0.1793}, {4.600, 0.1730}, {4.800, 0.1672}, {5.000, 0.1618}
};
/* clang-format on */

/* Charges are the published loads: 70 grains of black powder; 46 grains
 * of smokeless in the ball and tracer rounds, 42 in the match. */
static const ammo_def AMMO[AMMO_COUNT] = {
    /* A .54 patched ball: round, so it sheds speed fast. */
    [AMMO_ROUND_BALL] = {"Round ball", 0.01372f, 0.01426f, 550.0f, 0.068f, DRAG_G1, BUILD_LEAD, 1, 0.0f, 0.00454f},
    /* A .45 405-grain lead bullet over black powder: slow and heavy. */
    [AMMO_BLACK_LEAD] = {"Black-powder lead", 0.01163f, 0.02624f, 400.0f, 0.214f, DRAG_G1, BUILD_LEAD, 1, 0.0f,
                         0.00454f},
    /* 7.62 mm, 9.5 g full metal jacket at 840 m/s. */
    [AMMO_SMOKELESS] = {"7.62 mm ball", 0.00782f, 0.00953f, 840.0f, 0.200f, DRAG_G7, BUILD_FMJ, 0, 0.0f, 0.00298f},
    /* 7.62 mm, 11.3 g hollow-point boat-tail at 790 m/s. */
    [AMMO_MATCH] = {"7.62 mm match", 0.00782f, 0.01134f, 790.0f, 0.243f, DRAG_G7, BUILD_HOLLOW_POINT, 0, 0.0f,
                    0.00272f},
    /* 7.62 mm, 9.3 g, a pellet of strontium and magnesium in its base that
     * burns red out to about 900 m. */
    [AMMO_TRACER] = {"7.62 mm tracer", 0.00782f, 0.00933f, 838.0f, 0.190f, DRAG_G7, BUILD_FMJ, 0, 1.9f, 0.00298f},
};

const ammo_def *ammo_get(int kind)
{
    if (kind < 0 || kind >= AMMO_COUNT) return NULL;
    return &AMMO[kind];
}

double ballistics_cd(int model, double mach)
{
    const drag_point *t = model == DRAG_G7 ? G7 : G1;
    int n = model == DRAG_G7 ? (int)(sizeof G7 / sizeof G7[0]) : (int)(sizeof G1 / sizeof G1[0]);
    if (!(mach > t[0].mach)) return t[0].cd;
    if (mach >= t[n - 1].mach) return t[n - 1].cd;
    int lo = 0, hi = n - 1;
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (t[mid].mach <= mach) lo = mid;
        else hi = mid;
    }
    double f = (mach - t[lo].mach) / (t[hi].mach - t[lo].mach);
    return t[lo].cd + f * (t[hi].cd - t[lo].cd);
}

double ballistics_drag(const ammo_def *a, const atmos_air *air, double v)
{
    /* 0.5 rho v^2 Cd A / m, where the bullet's Cd is the reference curve's
     * times its form factor i and bc = m / (i d^2): d^2 and i fold into the
     * coefficient, leaving pi/8. */
    double cd = ballistics_cd(a->drag, v / air->sound);
    return MC_PI / 8.0 * air->rho * v * v * cd / ((double)a->bc * BC_SI);
}

static dvec3 accel(const ammo_def *a, const atmos_air *air, dvec3 vel)
{
    dvec3 rel = dv3(vel.x - air->wind.x, vel.y - air->wind.y, vel.z - air->wind.z);
    double v = dv3_len(rel);
    double k = v > 1e-9 ? ballistics_drag(a, air, v) / v : 0.0;
    return dv3(-rel.x * k, -rel.y * k - air->gravity, -rel.z * k);
}

void ballistics_step(const ammo_def *a, const atmos_air *air, dvec3 *pos, dvec3 *vel, double dt)
{
    /* The midpoint method: at the muzzle drag changes by several percent
     * within one step, more than plain Euler follows. */
    dvec3 v0 = *vel;
    dvec3 a0 = accel(a, air, v0);
    dvec3 vm = dv3(v0.x + a0.x * dt * 0.5, v0.y + a0.y * dt * 0.5, v0.z + a0.z * dt * 0.5);
    dvec3 am = accel(a, air, vm);
    *vel = dv3(v0.x + am.x * dt, v0.y + am.y * dt, v0.z + am.z * dt);
    *pos = dv3(pos->x + (v0.x + vel->x) * 0.5 * dt, pos->y + (v0.y + vel->y) * 0.5 * dt,
               pos->z + (v0.z + vel->z) * 0.5 * dt);
}
