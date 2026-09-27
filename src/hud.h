/* The health UI: an always-on HUD (vitals strip, needs, air temperature and
 * wetness, alerts, screen effects), the H panel (body diagram, per-part
 * status, organs, a bedside monitor with live ECG, arterial, pleth and
 * capnography traces, needs and supplies) and the death screen. Draws into
 * a ui batch.
 *
 * Alerts animate when hud_update runs each frame: a new one slides in from
 * the left, one that clears fades out, and the rest glide to their new
 * rows. Without hud_update they are drawn statically. */
#ifndef MC_HUD_H
#define MC_HUD_H

#include "health.h"
#include "ui.h"

#define HUD_ALERTS 12

typedef struct {
    char text[64];
    uint32_t key;           /* identity across frames (the text may change) */
    int sev;
    float slide;            /* 1 off-screen to the left .. 0 in place */
    float alpha;            /* fades to 0 once the condition has cleared */
    float row;              /* animated row, in alert lines */
    int live;               /* the condition still holds */
} hud_alert;

typedef struct {
    int panel;              /* H panel open */
    int sel;                /* selected body part in the panel */
    char msg[128];          /* result of the last treatment */
    float msg_age;          /* s since msg was set */
    const char *held;       /* name of the block in hand */
    int flying;
    int debug;              /* F3 overlay is up: keep clear of its left column */
    const inventory *inv;   /* supplies for the panel, and the hotbar */
    int hide_hints;         /* the key hints setting is off */
    /* Alert animation, owned by hud_update. */
    hud_alert alerts[HUD_ALERTS];
    int alert_count;
    int animated;           /* hud_update has run: draw the animated list */
} hud_state;

/* Advances the alert animation by dt seconds of real time. */
void hud_update(hud_state *s, const health *h, float dt);

/* The HUD, plus the H panel or the death screen when they are showing. */
void hud_draw(ui *u, const health *h, const hud_state *s);

#endif
