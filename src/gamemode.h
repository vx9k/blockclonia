/* Game modes and a world's rules. Survival simulates the body and makes
 * every block cost something; creative flies, builds from nothing and
 * leaves the body alone; hardcore is survival with one life, after which
 * the player can only watch; spectator flies through blocks with no hands.
 * A world is made in one mode, and only a world made with cheats allowed
 * can switch later. This module says what each mode permits and keeps the
 * rules in world.db; it does not draw menus or move players. */
#ifndef MC_GAMEMODE_H
#define MC_GAMEMODE_H
#include "physics.h"
#include "save_db.h"

enum { MODE_SURVIVAL, MODE_CREATIVE, MODE_HARDCORE, MODE_SPECTATOR, MODE_COUNT };

typedef struct {
    int mode;     /* MODE_*: what new players and a respawn get */
    int cheats;   /* the mode may be switched (and, later, commands used) */
    int hardcore; /* made in hardcore: never cheats, and one life each */
} world_rules;

const char *mode_name(int mode); /* "Survival", ...; "?" out of range */
int mode_valid(int64_t mode);
int mode_body(int mode);     /* the body is simulated: hunger, injuries, death */
int mode_creative(int mode); /* blocks cost nothing and break at once, nothing drops */
int mode_can_fly(int mode);
int mode_hands(int mode);    /* may break, place, use and strike */
int mode_noclip(int mode);   /* passes through blocks */

/* A new world's rules: hardcore never has cheats. */
world_rules rules_new(int mode, int cheats);
/* Reads the rules; a world without them (or with values out of range, as
 * world files are shared) is survival without cheats. */
world_rules rules_load(save_db *db);
int rules_save(save_db *db, const world_rules *r); /* 0 on success */
/* The modes this world may be switched to from mode `from`: none without
 * cheats; never into hardcore. Returns the next one after `from` in the
 * cycle survival, creative, spectator, or `from` if switching is off. */
int rules_next_mode(const world_rules *r, int from);

/* Puts a player in a mode: spectators always fly, survival never does. */
void gamemode_apply(player *p, int mode);
/* The fly key: toggles flight where the mode allows it. Returns flying. */
int gamemode_toggle_fly(player *p);
/* After a death: the mode the player continues in (hardcore -> spectator). */
int gamemode_after_death(int mode);
#endif
