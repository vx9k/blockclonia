#include "settings.h"
#include "save.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *key;
    size_t offset;
    int lo, hi;
} field;

#define F(name, lo, hi) {#name, offsetof(settings, name), lo, hi}
static const field FIELDS[] = {
    F(fov, SETTINGS_FOV_MIN, SETTINGS_FOV_MAX),
    F(sensitivity, SETTINGS_SENS_MIN, SETTINGS_SENS_MAX),
    F(invert_y, 0, 1),
    F(view_bob, 0, 1),
    F(fov_effects, 0, 1),
    F(sprint_toggle, 0, 1),
    F(gui_scale, 0, SETTINGS_GUI_MAX),
    F(vsync, 0, 1),
    F(render_distance, SETTINGS_RD_MIN, SETTINGS_RD_MAX),
    F(show_hints, 0, 1),
};
#undef F
#define NFIELDS ((int)(sizeof FIELDS / sizeof FIELDS[0]))

void settings_default(settings *s)
{
    s->fov = 70;
    s->sensitivity = 100;
    s->invert_y = 0;
    s->view_bob = 1;
    s->fov_effects = 1;
    s->sprint_toggle = 0;
    s->gui_scale = 0;
    s->vsync = 1;
    s->render_distance = 8;
    s->show_hints = 1;
}

static int *field_ptr(settings *s, const field *f) { return (int *)(void *)((char *)s + f->offset); }
static int field_get(const settings *s, const field *f)
{
    return *(const int *)(const void *)((const char *)s + f->offset);
}

/* One "key value" line; the value must be a plain decimal integer. */
static int apply_line(settings *s, const char *line)
{
    const char *sp = strchr(line, ' ');
    if (line[0] == '#' || !sp) return 0;
    size_t klen = (size_t)(sp - line);
    const char *v = sp + 1;
    if (!((*v >= '0' && *v <= '9') || (*v == '-' && v[1] >= '0' && v[1] <= '9'))) return 0;
    char *end;
    errno = 0;
    long n = strtol(v, &end, 10);
    if (errno || (*end != '\0' && *end != '\r')) return 0;
    for (int i = 0; i < NFIELDS; i++) {
        if (strlen(FIELDS[i].key) != klen || memcmp(FIELDS[i].key, line, klen) != 0) continue;
        if (n < FIELDS[i].lo || n > FIELDS[i].hi) return 0;
        *field_ptr(s, &FIELDS[i]) = (int)n;
        return 1;
    }
    return 0;
}

int settings_parse(settings *s, const char *text)
{
    int applied = 0;
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[96];
        if (len < sizeof line) {
            memcpy(line, p, len);
            line[len] = '\0';
            applied += apply_line(s, line);
        }
        if (!eol) break;
        p = eol + 1;
    }
    return applied;
}

int settings_format(const settings *s, char *out, int cap)
{
    if (cap <= 0) return 0;
    int len = snprintf(out, (size_t)cap, "# blockclonia settings\n");
    for (int i = 0; i < NFIELDS && len >= 0 && len < cap; i++)
        len += snprintf(out + len, (size_t)(cap - len), "%s %d\n", FIELDS[i].key, field_get(s, &FIELDS[i]));
    if (len < 0) len = 0;
    if (len >= cap) len = cap - 1;
    return len;
}

int settings_load(settings *s, const char *path)
{
    char buf[1024];
    long n = save_read_file(path, buf, sizeof buf - 1);
    if (n < 0) return -1;
    buf[n] = '\0';
    settings_parse(s, buf);
    return 0;
}

int settings_save(const settings *s, const char *path)
{
    char buf[1024];
    int n = settings_format(s, buf, (int)sizeof buf);
    return save_write_file(path, buf, (size_t)n);
}
