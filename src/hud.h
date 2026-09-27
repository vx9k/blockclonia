/* The health UI: an always-on HUD (vitals strip, needs, alerts, screen
 * effects), the H panel (body diagram, per-part status, organs, a bedside
 * monitor with live ECG, arterial, pleth and capnography traces, needs and
 * supplies) and the death screen. Draws into a ui batch. */
#ifndef MC_HUD_H
#define MC_HUD_H

#include "health.h"
#include "ui.h"

typedef struct {
    int panel;              /* H panel open */
    int sel;                /* selected body part in the panel */
    char msg[128];          /* result of the last treatment */
    float msg_age;          /* s since msg was set */
    const char *held;       /* name of the block in hand */
    int flying;
} hud_state;

void hud_draw(ui *u, const health *h, const hud_state *s);

#endif
