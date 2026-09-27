/* Player settings: view, controls and interface options, kept in a small
 * text file ("key value" per line). Unknown keys and out-of-range values
 * are ignored, so an old or hand-edited file never breaks the game. */
#ifndef MC_SETTINGS_H
#define MC_SETTINGS_H

typedef struct {
    int fov;              /* degrees, vertical */
    int sensitivity;      /* percent of the default mouse speed */
    int invert_y;
    int view_bob;         /* head bob, landing dip, hurt shake */
    int fov_effects;      /* wider view while sprinting */
    int sprint_toggle;    /* 0: hold to sprint, 1: press to toggle */
    int gui_scale;        /* 0: automatic, else screen pixels per UI pixel */
    int vsync;
    int render_distance;  /* columns; applies on the next start */
    int show_hints;       /* key hints on the HUD */
    int filtering;        /* anisotropic filtering of distant textures (where the GPU has it) */
    int volume_master;    /* percent */
    int volume_effects;   /* percent */
    int volume_ambient;   /* percent */
} settings;

#define SETTINGS_FOV_MIN 50
#define SETTINGS_FOV_MAX 110
#define SETTINGS_SENS_MIN 10
#define SETTINGS_SENS_MAX 300
#define SETTINGS_GUI_MAX 4
#define SETTINGS_RD_MIN 2
#define SETTINGS_RD_MAX 32

void settings_default(settings *s);
/* Parses text into s (fields not mentioned keep their value). Returns the
 * number of keys applied. */
int settings_parse(settings *s, const char *text);
/* Writes the whole set as text; returns the length (always < cap). */
int settings_format(const settings *s, char *out, int cap);

/* 0 on success; a missing or unreadable file leaves s unchanged. */
int settings_load(settings *s, const char *path);
int settings_save(const settings *s, const char *path);

#endif
