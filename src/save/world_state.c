#include "world_state.h"
#include "tagged.h"

#include <string.h>

typedef struct {
    double day;   /* 0..1, 0 = midnight */
    int64_t days; /* whole days passed */
} clock_state;

static const tag_field CLOCK_F[] = {TAG_F64R(clock_state, day, 0.0, 1.0), TAG_I64R(clock_state, days, 0, 1e15)};
static const tag_table CLOCK = TAG_TABLE(CLOCK_F);

/* The sections of systems not built yet: no fields so far. */
static const tag_table PLANNED = {NULL, 0};
static const char *const PLANNED_NAMES[] = {"vehicles", "machines", "weather", "animals"};
#define NPLANNED (sizeof PLANNED_NAMES / sizeof PLANNED_NAMES[0])

static const uint8_t MAGIC[4] = {'M', 'C', 'W', 'S'};

size_t world_state_encode(const thermo *th, uint8_t *out, size_t cap)
{
    clock_state c = {thermo_day_time(th), thermo_days(th)};
    tag_section sec[1 + NPLANNED] = {{"clock", &c, NULL, &CLOCK}};
    for (size_t i = 0; i < NPLANNED; i++) sec[1 + i] = (tag_section){PLANNED_NAMES[i], &PLANNED, NULL, &PLANNED};
    wire_w w = wire_writer(out, cap);
    wire_bytes(&w, MAGIC, sizeof MAGIC);
    tag_put_sections(&w, sec, 1 + NPLANNED);
    return w.bad ? 0 : w.len;
}

int world_state_decode(thermo *th, const uint8_t *in, size_t len)
{
    if (!in || len < sizeof MAGIC || memcmp(in, MAGIC, sizeof MAGIC) != 0) return -1;
    clock_state c = {thermo_day_time(th), thermo_days(th)};
    tag_section sec[] = {{"clock", NULL, &c, &CLOCK}};
    wire_r r = wire_reader(in + sizeof MAGIC, len - sizeof MAGIC);
    tag_get_sections(&r, sec, 1);
    if (r.bad || r.pos != r.len) return -1;
    thermo_set_day_time(th, c.day);
    thermo_set_days(th, c.days);
    return 0;
}

int world_state_save(save_db *db, const thermo *th)
{
    uint8_t buf[WORLD_STATE_MAX] = {0};
    size_t n = world_state_encode(th, buf, sizeof buf);
    return n && save_db_put_blob(db, "state", buf, n) == 0 ? 0 : -1;
}

int world_state_load(save_db *db, thermo *th)
{
    uint8_t buf[WORLD_STATE_MAX + 1] = {0};
    long n = save_db_get_blob(db, "state", buf, sizeof buf);
    if (n == -1) return 0;
    if (n < 0) return -1;
    return world_state_decode(th, buf, (size_t)n) == 0 ? 1 : -1;
}
