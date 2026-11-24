/* The world's own state outside its columns and players, as one tagged
 * record (tagged.h) in the database's "state" blob: the clock (the time of
 * day and the days passed), and a section for each system that keeps
 * state of its own as those systems learn to save it.
 *
 * Systems planned but not built yet (vehicles, machines, weather,
 * animals) are written as empty sections already. A world saved today
 * then opens in a build that has them, each starting from its defaults,
 * and no save format has to change when one lands: its section gains
 * fields. It deliberately holds nothing per player; that is the players
 * table (survival.h). No window or GPU. */
#ifndef MC_WORLD_STATE_H
#define MC_WORLD_STATE_H

#include "save_db.h"
#include "thermo.h"

#include <stddef.h>
#include <stdint.h>

#define WORLD_STATE_MAX 4096 /* bytes of the record, for now */

/* "MCWS" and the record. Returns the bytes written, 0 if cap is too small. */
size_t world_state_encode(const thermo *th, uint8_t *out, size_t cap);
/* Reads a record into th; sections it lacks leave th as it is. Returns 0,
 * or -1 with th untouched for malformed data. */
int world_state_decode(thermo *th, const uint8_t *in, size_t len);

/* save adds the record to a batch for the database's writer (0, or -1 if
 * it could not be encoded). load reads it from db: 1 if one was read, 0
 * if the world has none (a new world, or one from before it existed), -1
 * if it is corrupt or unreadable (th untouched). */
struct save_batch;
int world_state_save(struct save_batch *b, const thermo *th);
int world_state_load(save_db *db, thermo *th);

#endif
