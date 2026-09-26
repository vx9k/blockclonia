/* The player's inventory: a 9-slot hotbar and 27 more slots, a stack held
 * by the mouse while the inventory screen is open, and crafting. Pure
 * data, so it is unit-tested and saved as a few bytes. */
#ifndef MC_INVENTORY_H
#define MC_INVENTORY_H

#include <stddef.h>
#include <stdint.h>
#include "item.h"

#define INV_HOTBAR 9
#define INV_SLOTS 36     /* 0..8 hotbar, 9..35 the rest */

typedef struct {
    item_stack slot[INV_SLOTS];
    item_stack cursor;   /* held by the mouse on the inventory screen */
    int selected;        /* hotbar slot in hand */
} inventory;

void inv_init(inventory *inv);
/* The survival kit a new player starts with. */
void inv_starting_kit(inventory *inv);

/* Adds items, filling matching stacks first, then empty slots (hotbar
 * first). Returns how many did not fit. */
int inv_add(inventory *inv, int id, int count);
int inv_count(const inventory *inv, int id);
/* Removes up to count, from the last slot backwards so the hotbar is used
 * last. Returns how many were removed. */
int inv_take(inventory *inv, int id, int count);

static inline item_stack *inv_held(inventory *inv) { return &inv->slot[inv->selected]; }
/* Uses up n of the held stack. */
void inv_use_held(inventory *inv, int n);
/* Replaces the held stack (a bucket filling up). */
void inv_set_held(inventory *inv, int id, int count);

/* Slot clicks on the inventory screen. button 0: left (take, place, merge,
 * swap), 1: right (take half, place one). shift moves the whole stack
 * between the hotbar and the rest. */
void inv_click(inventory *inv, int slot, int button, int shift);
/* Puts the cursor stack back; returns what did not fit (to drop). */
item_stack inv_return_cursor(inventory *inv);

/* ------------------------------------------------------------ crafting */

typedef struct {
    item_stack out;
    item_stack in[3];    /* id 0 ends the list */
    uint8_t needs_fire;  /* only next to a burning campfire */
} recipe;

extern const recipe g_recipes[];
extern const int g_recipe_count;

/* 1 if the ingredients are there (and fire, when needed). */
int inv_can_craft(const inventory *inv, int r, int near_fire);
/* Crafts once; returns 1 on success. The result goes into the inventory,
 * or fails without using anything if there is no room. */
int inv_craft(inventory *inv, int r, int near_fire);

/* ---------------------------------------------------------- persistence */

#define INV_ENCODED_SIZE (8 + 2 * (INV_SLOTS + 1))
size_t inv_encode(const inventory *inv, uint8_t *out, size_t cap);
/* 0 on success; -1 (inventory untouched) if the bytes are malformed. */
int inv_decode(inventory *inv, const uint8_t *in, size_t len);

#endif
