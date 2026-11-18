#include "gamemode.h"

static const char *const NAMES[MODE_COUNT] = {"Survival", "Creative", "Hardcore", "Spectator"};

const char *mode_name(int mode) { return mode >= 0 && mode < MODE_COUNT ? NAMES[mode] : "?"; }

int mode_valid(int64_t mode) { return mode >= 0 && mode < MODE_COUNT; }

int mode_body(int mode) { return mode == MODE_SURVIVAL || mode == MODE_HARDCORE; }

int mode_creative(int mode) { return mode == MODE_CREATIVE; }

int mode_can_fly(int mode) { return mode == MODE_CREATIVE || mode == MODE_SPECTATOR; }

int mode_hands(int mode) { return mode != MODE_SPECTATOR; }

int mode_noclip(int mode) { return mode == MODE_SPECTATOR; }

world_rules rules_new(int mode, int cheats)
{
    world_rules r = {mode_valid(mode) ? mode : MODE_SURVIVAL, cheats != 0, 0};
    if (r.mode == MODE_HARDCORE) {
        r.hardcore = 1;
        r.cheats = 0;
    }
    return r;
}

world_rules rules_load(save_db *db)
{
    int64_t mode = MODE_SURVIVAL, cheats = 0, hardcore = 0;
    if (db) {
        save_db_get_meta(db, "mode", &mode);
        save_db_get_meta(db, "cheats", &cheats);
        save_db_get_meta(db, "hardcore", &hardcore);
    }
    if (!mode_valid(mode) || (cheats != 0 && cheats != 1) || (hardcore != 0 && hardcore != 1))
        return rules_new(MODE_SURVIVAL, 0);
    /* A hardcore world stays hardcore or, once its life is spent, spectator,
     * and never has cheats, whatever the file says. */
    world_rules r = {(int)mode, (int)cheats, (int)hardcore};
    if (r.hardcore) {
        r.cheats = 0;
        if (r.mode != MODE_SPECTATOR) r.mode = MODE_HARDCORE;
    } else if (r.mode == MODE_HARDCORE) {
        r.mode = MODE_SURVIVAL;
    }
    return r;
}

int rules_save(save_db *db, const world_rules *r)
{
    if (!db || !r) return -1;
    int bad = save_db_set_meta(db, "mode", r->mode) != 0;
    bad |= save_db_set_meta(db, "cheats", r->cheats) != 0;
    bad |= save_db_set_meta(db, "hardcore", r->hardcore) != 0;
    return bad ? -1 : 0;
}

int rules_next_mode(const world_rules *r, int from)
{
    if (!r || !r->cheats || r->hardcore) return from;
    return from == MODE_SURVIVAL ? MODE_CREATIVE : from == MODE_CREATIVE ? MODE_SPECTATOR : MODE_SURVIVAL;
}

void gamemode_apply(player *p, int mode)
{
    p->mode = mode_valid(mode) ? mode : MODE_SURVIVAL;
    if (mode_noclip(p->mode)) p->flying = 1;
    if (!mode_can_fly(p->mode)) p->flying = 0;
    if (!p->flying) p->sprinting = 0;
}

int gamemode_toggle_fly(player *p)
{
    /* A spectator has nothing to stand on: it always flies. */
    if (mode_can_fly(p->mode) && !mode_noclip(p->mode)) {
        p->flying = !p->flying;
        p->vel = dv3(0.0, 0.0, 0.0);
    }
    return p->flying;
}

int gamemode_after_death(int mode) { return mode == MODE_HARDCORE ? MODE_SPECTATOR : mode; }
