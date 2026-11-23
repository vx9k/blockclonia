/* The interface palette: dark translucent panels with thin slate borders,
 * light grey text and the bedside-monitor trace colours as accents. Shared
 * by the HUD, the health panel, the menus, the inventory and the F3 view. */
#ifndef MC_PALETTE_H
#define MC_PALETTE_H

#include "ui.h"

#define C_TEXT ui_rgba(232, 232, 232, 255)
#define C_DIM ui_rgba(150, 156, 165, 255)
#define C_HEAD ui_rgba(255, 255, 255, 255)
#define C_PANEL ui_rgba(14, 16, 20, 232)
#define C_BORDER ui_rgba(72, 82, 98, 255)
#define C_BACK ui_rgba(40, 44, 52, 255)
#define C_ECG ui_rgba(40, 235, 100, 255)
#define C_ART ui_rgba(245, 70, 70, 255)
#define C_PLETH ui_rgba(70, 200, 245, 255)
#define C_CO2 ui_rgba(245, 220, 70, 255)
#define C_HINT ui_rgba(250, 235, 150, 255)

/* Severity: green, yellow, orange, red (packed little-endian RGBA). */
#define PAL_SEV0 0xff5ac85au
#define PAL_SEV1 0xff3cc8e6u
#define PAL_SEV2 0xff2882f0u
#define PAL_SEV3 0xff3232e6u

/* Linear blend of two packed colours, t in 0..1. */
static inline uint32_t pal_mix(uint32_t a, uint32_t b, float t)
{
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    uint32_t r = 0;
    for (int s = 0; s < 32; s += 8) {
        float ca = (float)((a >> s) & 255u), cb = (float)((b >> s) & 255u);
        r |= (uint32_t)(ca + (cb - ca) * t + 0.5f) << s;
    }
    return r;
}

#endif
