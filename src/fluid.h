/* Fluids: liquid blocks (BF_FLUID) flowing through the world.
 *
 * - A liquid cell holds a level 1..FLUID_FULL in its meta (world.h). Each
 *   tick a cell pours what it can into the cell below, then moves sideways:
 *   all of it over a ledge if a neighbour has a drop under it, otherwise
 *   levelling with its lower neighbours. Volume is conserved exactly.
 * - Only cells that may move are simulated: a change to a cell wakes it,
 *   its six neighbours and, when it has room, the cells diagonally above
 *   that may drain into it (fluid_wake). A cell that moved wakes its
 *   neighbours through the world's change hook. A still sea costs nothing.
 * - Ticks run at 10 Hz with a fixed budget of cells, so a flood slows
 *   down rather than stalls the frame, the same on every machine.
 *
 * Deliberately not here: buoyancy, swimming and drag (physics), heat,
 * freezing and boiling (thermo), drinking (survival). No window or GPU. */
#ifndef MC_FLUID_H
#define MC_FLUID_H

#include "world.h"
#include <stdint.h>

#define FLUID_TICK_EVERY 6 /* physics steps per fluid tick: 10 Hz */

/* Cells waiting for their turn, each at most once. */
typedef struct {
    ipos *items;
    int count, cap;
    uint32_t *set; /* open-addressing dedupe of items (index + 1) */
    uint32_t set_cap;
} fluid_queue;

typedef struct {
    world *w;
    fluid_queue now, next; /* this tick's cells, and those woken for the next */
    fluid_queue spare;     /* empty; where begin_tick lines the two up */
    int read, end;         /* this tick's cells: now.items[read, end) */
    int per_step;          /* a tick's work is spread over FLUID_TICK_EVERY steps */
    uint32_t order;        /* the tick's number, which turns the order cells share sideways */
    int updates;           /* cells the last tick took on (HUD, bench) */
} fluids;

/* Allocates the queues; w must outlive f. */
void fluid_init(fluids *f, world *w);
void fluid_destroy(fluids *f);

/* A cell changed: it and its six neighbours may flow next tick. */
void fluid_wake(fluids *f, int x, int y, int z);
/* Cells waiting for the next tick. */
int fluid_pending(const fluids *f);
/* Forgets every waiting cell (tests that build a still pool). */
void fluid_forget(fluids *f);

/* Starts tick number `order`: the woken cells, up to the budget, become
 * this tick's work. */
void fluid_begin_tick(fluids *f, uint32_t order);
/* Runs up to n of this tick's cells. */
void fluid_run(fluids *f, int n);
/* A whole tick at once (tests and --bench). */
void fluid_tick(fluids *f, uint32_t order);

/* Writes level (0 = air) of liquid id into a cell. */
void fluid_set(world *w, int x, int y, int z, uint8_t id, int level);
/* A solid took the cell at (x, y, z), which held `level` of liquid `id`:
 * pushes it into the cell above when that holds air or the same liquid,
 * rather than deleting it. What does not fit is lost to the
 * surroundings. A player's edit (player = 1) is re-meshed first. */
void fluid_displace(world *w, int x, int y, int z, uint8_t id, int level, int player);

/* How the world looks from inside liquid id: the colour the view fades to
 * and the distance (m) at which it has faded completely. Water's for
 * anything that isn't a liquid. */
void fluid_view(uint8_t id, float rgb[3], float *range);

#endif
