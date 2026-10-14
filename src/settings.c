#include "settings.h"
#include "mem.h"
#include "os.h"
#include "save.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *key;
    int lo, hi;
} field;

/* In the order of field_ptr's cases. */
static const field FIELDS[] = {
    {"fov", SETTINGS_FOV_MIN, SETTINGS_FOV_MAX},
    {"sensitivity", SETTINGS_SENS_MIN, SETTINGS_SENS_MAX},
    {"invert_y", 0, 1},
    {"view_bob", 0, 1},
    {"fov_effects", 0, 1},
    {"sprint_toggle", 0, 1},
    {"gui_scale", 0, SETTINGS_GUI_MAX},
    {"vsync", 0, 1},
    {"render_distance", SETTINGS_RD_MIN, SETTINGS_RD_MAX},
    {"show_hints", 0, 1},
    {"filtering", 0, 1},
    {"volume_master", 0, 100},
    {"volume_effects", 0, 100},
    {"volume_ambient", 0, 100},
    {"fullscreen", 0, 1},
    {"max_fps", 0, SETTINGS_FPS_MAX},
    {"brightness", 0, 100},
    {"volume_ui", 0, 100},
    {"volume_body", 0, 100},
    {"mute_unfocused", 0, 1},
    {"discord", 0, 1},
};
#define NFIELDS ((int)(sizeof FIELDS / sizeof FIELDS[0]))
_Static_assert(sizeof FIELDS / sizeof FIELDS[0] * sizeof(int) == sizeof(settings), "every setting needs a FIELDS row");

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
    s->filtering = 1;
    s->volume_master = 80;
    s->volume_effects = 100;
    s->volume_ambient = 70;
    s->fullscreen = 0;
    s->max_fps = 0;
    s->brightness = 0;
    s->volume_ui = 80;
    s->volume_body = 100;
    s->mute_unfocused = 1;
    s->discord = 0; /* opt-in: nothing leaves the machine unasked */
}

static int *field_ptr(settings *s, int i)
{
    switch (i) {
    case 0: return &s->fov;
    case 1: return &s->sensitivity;
    case 2: return &s->invert_y;
    case 3: return &s->view_bob;
    case 4: return &s->fov_effects;
    case 5: return &s->sprint_toggle;
    case 6: return &s->gui_scale;
    case 7: return &s->vsync;
    case 8: return &s->render_distance;
    case 9: return &s->show_hints;
    case 10: return &s->filtering;
    case 11: return &s->volume_master;
    case 12: return &s->volume_effects;
    case 13: return &s->volume_ambient;
    case 14: return &s->fullscreen;
    case 15: return &s->max_fps;
    case 16: return &s->brightness;
    case 17: return &s->volume_ui;
    case 18: return &s->volume_body;
    case 19: return &s->mute_unfocused;
    default: return &s->discord;
    }
}

static int field_get(const settings *s, int i)
{
    settings t = *s;
    return *field_ptr(&t, i);
}

/* One "key value" line; the value must be a plain decimal integer. */
static int apply_line(settings *s, const char *line)
{
    const char *sp = strchr(line, ' ');
    if (line[0] == '#' || !sp) return 0;
    size_t klen = (size_t)(sp - line);
    const char *v = sp + 1;
    if (!((*v >= '0' && *v <= '9') || (*v == '-' && v[1] >= '0' && v[1] <= '9'))) return 0;
    char *end = NULL;
    errno = 0;
    long n = strtol(v, &end, 10);
    if (errno || (*end != '\0' && *end != '\r')) return 0;
    for (int i = 0; i < NFIELDS; i++) {
        if (strlen(FIELDS[i].key) != klen || memcmp(FIELDS[i].key, line, klen) != 0) continue;
        if (n < FIELDS[i].lo || n > FIELDS[i].hi) return 0;
        *field_ptr(s, i) = (int)n;
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
        char line[96] = {0};
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
        len += snprintf(out + len, (size_t)(cap - len), "%s %d\n", FIELDS[i].key, field_get(s, i));
    if (len < 0) len = 0;
    if (len >= cap) len = cap - 1;
    return len;
}

int settings_load(settings *s, const char *path)
{
    char *buf = mem_alloc(SETTINGS_MAX_FILE + 1);
    long n = save_read_file(path, buf, SETTINGS_MAX_FILE);
    int r = 0;
    if (n < 0) {
        r = os_path_kind(path) == OS_MISSING ? SETTINGS_MISSING : SETTINGS_UNUSABLE;
    } else {
        buf[n] = '\0';
        r = settings_parse(s, buf);
        if (r == 0 && n > 0) r = SETTINGS_UNUSABLE;
    }
    mem_free(buf);
    return r;
}

int settings_save(const settings *s, const char *path)
{
    char buf[4096] = {0}; /* the file is ~300 bytes; room for comments a player adds */
    int n = settings_format(s, buf, (int)sizeof buf);
    return save_write_file(path, buf, (size_t)n);
}
