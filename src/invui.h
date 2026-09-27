/* Inventory interface: item icons, the hotbar at the bottom of the screen
 * and the inventory screen (Tab) with crafting. Icons come from the same
 * procedural textures as the world: blocks as small shaded isometric cubes,
 * items as their 16x16 sprites. No window or GPU. */
#ifndef MC_INVUI_H
#define MC_INVUI_H

#include "inventory.h"
#include "ui.h"
#include <stddef.h>

typedef struct {
    float open_t;        /* s since the screen opened */
    float sel_x;         /* animated hotbar highlight, slot units */
    float name_age;      /* s since the item in hand changed */
    int last_sel, last_id;
    float pop[INV_SLOTS];/* per-slot pulse when a stack grows (pickup) */
    uint8_t last_count[INV_SLOTS];
    uint8_t last_slot_id[INV_SLOTS];
} invui;

typedef struct {
    float mx, my;        /* cursor, logical pixels */
    int click, rclick;   /* pressed this frame */
    int shift;
    int near_fire;       /* recipes that need a fire can be made */
    float dt;
} invui_input;

typedef struct {
    int changed;         /* slots or crafting changed something */
    item_stack drop;     /* the cursor stack was dropped outside the panel */
    int crafted;         /* recipe index crafted this frame, -1 none */
} invui_result;

void invui_init(invui *s);

/* One item icon, size x size logical pixels (16 is native). */
void invui_icon(ui *u, int id, float x, float y, float size, float alpha);

/* The hotbar, the held item's name after switching, and pickup pulses. */
void invui_hotbar(ui *u, invui *s, const inventory *inv, float dt);

/* The inventory screen. Handles slot clicks, crafting and tooltips. */
invui_result invui_screen(ui *u, invui *s, inventory *inv, const invui_input *in);

#endif
