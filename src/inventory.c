#include "inventory.h"

#include <string.h>

void inv_init(inventory *inv) { memset(inv, 0, sizeof *inv); }

void inv_starting_kit(inventory *inv)
{
    inv_init(inv);
    inv_add(inv, I_BANDAGE, 3);
    inv_add(inv, I_SPLINT, 1);
    inv_add(inv, I_ANTISEPTIC, 2);
    inv_add(inv, I_PAINKILLER, 4);
    inv_add(inv, I_ANTIBIOTIC, 1);
    inv_add(inv, I_APPLE, 3);
    inv_add(inv, I_DEFIBRILLATOR, 1);
    /* The kit lives in the backpack; the hand starts empty. */
    for (int i = 0; i < INV_HOTBAR; i++) {
        if (!inv->slot[i].count) continue;
        inv->slot[INV_HOTBAR + i] = inv->slot[i];
        inv->slot[i] = (item_stack){0, 0, 0};
    }
}

static int max_stack(int id) { return item_get(id)->stack; }

int inv_add(inventory *inv, int id, int count)
{
    if (!item_valid(id) || count <= 0) return count < 0 ? 0 : count;
    int ms = max_stack(id);
    for (int i = 0; i < INV_SLOTS && count > 0; i++) {
        item_stack *s = &inv->slot[i];
        if (s->id != id || !s->count || s->count >= ms) continue;
        int room = ms - s->count, put = count < room ? count : room;
        s->count = (uint8_t)(s->count + put);
        count -= put;
    }
    for (int i = 0; i < INV_SLOTS && count > 0; i++) {
        item_stack *s = &inv->slot[i];
        if (s->count) continue;
        int put = count < ms ? count : ms;
        *s = (item_stack){(uint8_t)id, (uint8_t)put, 0};
        count -= put;
    }
    return count;
}

int inv_count(const inventory *inv, int id)
{
    int n = 0;
    for (int i = 0; i < INV_SLOTS; i++)
        if (inv->slot[i].id == id) n += inv->slot[i].count;
    return n;
}

int inv_take(inventory *inv, int id, int count)
{
    int got = 0;
    for (int i = INV_SLOTS - 1; i >= 0 && got < count; i--) {
        item_stack *s = &inv->slot[i];
        if (s->id != id || !s->count) continue;
        int t = count - got < s->count ? count - got : s->count;
        s->count = (uint8_t)(s->count - t);
        if (!s->count) s->id = 0;
        got += t;
    }
    return got;
}

void inv_use_held(inventory *inv, int n)
{
    item_stack *s = inv_held(inv);
    s->count = (uint8_t)(n >= s->count ? 0 : s->count - n);
    if (!s->count) s->id = 0;
}

void inv_set_held(inventory *inv, int id, int count)
{ *inv_held(inv) = count > 0 && item_valid(id) ? (item_stack){(uint8_t)id, (uint8_t)count, 0} : (item_stack){0, 0, 0}; }

/* Moves a whole stack into [lo, hi): merging first, then an empty slot. */
static void move_stack(inventory *inv, int from, int lo, int hi)
{
    item_stack *s = &inv->slot[from];
    int ms = max_stack(s->id);
    for (int i = lo; i < hi && s->count; i++) {
        item_stack *d = &inv->slot[i];
        if (d->id != s->id || !d->count || d->count >= ms) continue;
        int t = ms - d->count < s->count ? ms - d->count : s->count;
        d->count = (uint8_t)(d->count + t);
        s->count = (uint8_t)(s->count - t);
    }
    for (int i = lo; i < hi && s->count; i++) {
        if (inv->slot[i].count) continue;
        inv->slot[i] = *s;
        s->count = 0;
    }
    if (!s->count) s->id = 0;
}

void inv_click(inventory *inv, int slot, int button, int shift)
{
    if (slot < 0 || slot >= INV_SLOTS) return;
    item_stack *s = &inv->slot[slot], *c = &inv->cursor;
    if (shift) {
        if (!s->count) return;
        if (slot < INV_HOTBAR) move_stack(inv, slot, INV_HOTBAR, INV_SLOTS);
        else move_stack(inv, slot, 0, INV_HOTBAR);
        return;
    }
    if (button == 0) {
        if (!c->count) {
            *c = *s;
            *s = (item_stack){0, 0, 0};
        } else if (!s->count) {
            *s = *c;
            *c = (item_stack){0, 0, 0};
        } else if (s->id == c->id) {
            int ms = max_stack(s->id), t = ms - s->count < c->count ? ms - s->count : c->count;
            s->count = (uint8_t)(s->count + t);
            c->count = (uint8_t)(c->count - t);
            if (!c->count) c->id = 0;
        } else {
            item_stack t = *s;
            *s = *c;
            *c = t;
        }
    } else {
        if (!c->count) {
            if (!s->count) return;
            int half = (s->count + 1) / 2;
            *c = (item_stack){s->id, (uint8_t)half, s->wear}; /* a split never mends a tool */
            s->count = (uint8_t)(s->count - half);
            if (!s->count) s->id = 0;
        } else if (!s->count || (s->id == c->id && s->count < max_stack(s->id))) {
            if (!s->count) *s = (item_stack){c->id, 0, c->wear};
            s->count++;
            c->count--;
            if (!c->count) c->id = 0;
        } else if (s->id != c->id) {
            item_stack t = *s;
            *s = *c;
            *c = t;
        }
    }
}

item_stack inv_return_cursor(inventory *inv)
{
    item_stack c = inv->cursor;
    inv->cursor = (item_stack){0, 0, 0};
    if (!c.count) return c;
    int left = inv_add_stack(inv, c);
    return left ? (item_stack){c.id, (uint8_t)left, c.wear} : (item_stack){0, 0, 0};
}

/* ------------------------------------------------------------ crafting */

const recipe g_recipes[] = {
    {{B_PLANKS, 4}, {{B_LOG, 1}}, 0},
    {{I_STICK, 4}, {{B_PLANKS, 2}}, 0},
    {{I_SPLINT, 1}, {{I_STICK, 2}, {I_FIBRE, 1}}, 0},
    {{I_BUCKET, 1}, {{B_PLANKS, 3}, {I_FIBRE, 1}}, 0},
    {{B_GLASS, 1}, {{B_SAND, 2}}, STATION_FIRE},
    {{B_BRICK, 1}, {{B_DIRT, 2}, {B_SAND, 1}}, STATION_FIRE},
    {{B_CAMPFIRE, 1}, {{I_STICK, 4}, {B_LOG, 1}}, 0},
    {{I_COOKED_MEAT, 1}, {{I_RAW_MEAT, 1}}, STATION_FIRE},
    {{I_GUNPOWDER, 2}, {{I_COAL, 2}, {B_ASH, 1}}, 0},
    {{I_CARTRIDGE, 4}, {{I_COPPER_INGOT, 1}, {I_GUNPOWDER, 1}}, 0},
    {{I_RIFLE, 1}, {{I_IRON_INGOT, 4}, {B_PLANKS, 2}}, STATION_FURNACE},
    {{B_FURNACE, 1}, {{B_STONE, 8}}, 0},
    /* A campfire cannot reach the 1085-1540 C these metals need; a
     * furnace burning the coal can. */
    {{I_COPPER_INGOT, 1}, {{I_RAW_COPPER, 1}, {I_COAL, 1}}, STATION_FURNACE},
    {{I_IRON_INGOT, 1}, {{I_RAW_IRON, 1}, {I_COAL, 2}}, STATION_FURNACE},
/* A head of the material on a stick handle. */
#define TOOL_RECIPE(kind, tier, mat, n, sticks) \
    {{I_TOOL_FIRST + (kind) * TIER_COUNT + (tier), 1}, {{mat, n}, {I_STICK, sticks}}, 0}
    TOOL_RECIPE(TOOL_PICKAXE, TIER_WOOD, B_PLANKS, 3, 2),
    TOOL_RECIPE(TOOL_PICKAXE, TIER_STONE, B_STONE, 3, 2),
    TOOL_RECIPE(TOOL_PICKAXE, TIER_COPPER, I_COPPER_INGOT, 3, 2),
    TOOL_RECIPE(TOOL_PICKAXE, TIER_IRON, I_IRON_INGOT, 3, 2),
    TOOL_RECIPE(TOOL_AXE, TIER_WOOD, B_PLANKS, 3, 2),
    TOOL_RECIPE(TOOL_AXE, TIER_STONE, B_STONE, 3, 2),
    TOOL_RECIPE(TOOL_AXE, TIER_COPPER, I_COPPER_INGOT, 3, 2),
    TOOL_RECIPE(TOOL_AXE, TIER_IRON, I_IRON_INGOT, 3, 2),
    TOOL_RECIPE(TOOL_SHOVEL, TIER_WOOD, B_PLANKS, 1, 2),
    TOOL_RECIPE(TOOL_SHOVEL, TIER_STONE, B_STONE, 1, 2),
    TOOL_RECIPE(TOOL_SHOVEL, TIER_COPPER, I_COPPER_INGOT, 1, 2),
    TOOL_RECIPE(TOOL_SHOVEL, TIER_IRON, I_IRON_INGOT, 1, 2),
    TOOL_RECIPE(TOOL_KNIFE, TIER_WOOD, B_PLANKS, 1, 1),
    TOOL_RECIPE(TOOL_KNIFE, TIER_STONE, B_STONE, 1, 1),
    TOOL_RECIPE(TOOL_KNIFE, TIER_COPPER, I_COPPER_INGOT, 1, 1),
    TOOL_RECIPE(TOOL_KNIFE, TIER_IRON, I_IRON_INGOT, 1, 1),
    TOOL_RECIPE(TOOL_SWORD, TIER_WOOD, B_PLANKS, 2, 1),
    TOOL_RECIPE(TOOL_SWORD, TIER_STONE, B_STONE, 2, 1),
    TOOL_RECIPE(TOOL_SWORD, TIER_COPPER, I_COPPER_INGOT, 2, 1),
    TOOL_RECIPE(TOOL_SWORD, TIER_IRON, I_IRON_INGOT, 2, 1),
#undef TOOL_RECIPE
};
const int g_recipe_count = (int)(sizeof g_recipes / sizeof g_recipes[0]);

int inv_can_craft(const inventory *inv, int r, int stations)
{
    if (r < 0 || r >= g_recipe_count) return 0;
    const recipe *rc = &g_recipes[r];
    if ((rc->station & stations) != rc->station) return 0;
    for (int k = 0; k < 3 && rc->in[k].id; k++)
        if (inv_count(inv, rc->in[k].id) < rc->in[k].count) return 0;
    return 1;
}

int inv_craft(inventory *inv, int r, int stations)
{
    if (!inv_can_craft(inv, r, stations)) return 0;
    const recipe *rc = &g_recipes[r];
    inventory trial = *inv;
    for (int k = 0; k < 3 && rc->in[k].id; k++) inv_take(&trial, rc->in[k].id, rc->in[k].count);
    if (inv_add(&trial, rc->out.id, rc->out.count) != 0) return 0; /* no room */
    *inv = trial;
    return 1;
}

/* ---------------------------------------------------------- persistence */

/* "INV2", selected, 3 reserved bytes, then (id, count, wear) for every slot
 * and the cursor. INV1, read but no longer written, had no wear byte. */
size_t inv_encode(const inventory *inv, uint8_t *out, size_t cap)
{
    if (cap < INV_ENCODED_SIZE) return 0;
    memcpy(out, "INV2", 4);
    out[4] = (uint8_t)inv->selected;
    out[5] = out[6] = out[7] = 0;
    uint8_t *p = out + 8;
    for (int i = 0; i <= INV_SLOTS; i++) {
        const item_stack *s = i < INV_SLOTS ? &inv->slot[i] : &inv->cursor;
        *p++ = s->count ? s->id : 0;
        *p++ = s->count;
        *p++ = s->count ? s->wear : 0;
    }
    return INV_ENCODED_SIZE;
}

int inv_decode(inventory *inv, const uint8_t *in, size_t len)
{
    /* Every INV1/INV2 ever written has zero reserved bytes: anything else is
     * a layout this build does not know. */
    int v2 = len == INV_ENCODED_SIZE && memcmp(in, "INV2", 4) == 0;
    int v1 = len == INV1_ENCODED_SIZE && memcmp(in, "INV1", 4) == 0;
    if (!(v1 || v2) || in[4] >= INV_HOTBAR || in[5] || in[6] || in[7]) return -1;
    inventory t = {0};
    inv_init(&t);
    t.selected = in[4];
    const uint8_t *p = in + 8;
    for (int i = 0; i <= INV_SLOTS; i++, p += v2 ? 3 : 2) {
        item_stack s = {p[0], p[1], v2 ? p[2] : 0};
        if ((s.id == 0) != (s.count == 0)) return -1;
        if (s.count && (!item_valid(s.id) || s.count > max_stack(s.id))) return -1;
        /* Wear only on things that wear, and never past worn out. */
        if (s.wear && (!s.count || s.wear >= item_durability(s.id))) return -1;
        if (i < INV_SLOTS) t.slot[i] = s;
        else t.cursor = s;
    }
    *inv = t;
    return 0;
}

int inv_add_stack(inventory *inv, item_stack s)
{
    if (!s.wear) return inv_add(inv, s.id, s.count);
    if (!item_valid(s.id) || !s.count) return s.count;
    for (int i = 0; i < INV_SLOTS; i++)
        if (!inv->slot[i].count) {
            inv->slot[i] = (item_stack){s.id, 1, s.wear}; /* worn things do not stack */
            return s.count - 1;
        }
    return s.count;
}

int inv_accept(void *inv, item_stack s) { return s.count - inv_add_stack(inv, s); }

int inv_wear_held(inventory *inv, int uses)
{
    item_stack *s = inv_held(inv);
    int dur = s->count ? item_durability(s->id) : 0;
    if (!dur) return 0;
    if (s->wear + uses < dur) {
        s->wear = (uint8_t)(s->wear + uses);
        return 0;
    }
    *s = (item_stack){0, 0, 0};
    return 1;
}
