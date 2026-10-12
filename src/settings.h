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
    int filtering;        /* trilinear, and anisotropic where the GPU has it, filtering of distant textures */
    int volume_master;    /* percent */
    int volume_effects;   /* percent */
    int volume_ambient;   /* percent */
    int fullscreen;       /* borderless on the monitor the window is on */
    int max_fps;          /* frame-rate limit with VSync off; 0: none */
    int brightness;       /* percent: 0 keeps nights dark, 100 lifts them toward dusk */
    int volume_ui;        /* percent: menu clicks and the inventory */
    int volume_body;      /* percent: heartbeat and laboured breathing */
    int mute_unfocused;   /* silence while the window is in the background */
} settings;

#define SETTINGS_FOV_MIN 50
#define SETTINGS_FOV_MAX 110
#define SETTINGS_SENS_MIN 10
#define SETTINGS_SENS_MAX 300
#define SETTINGS_GUI_MAX 4
#define SETTINGS_RD_MIN 2
#define SETTINGS_RD_MAX 32
/* Values below SETTINGS_FPS_MIN (except 0, no limit) are raised to it. */
#define SETTINGS_FPS_MIN 20
#define SETTINGS_FPS_MAX 360

/* The values a fresh install starts with. */
void settings_default(settings *s);
/* Parses text into s (fields not mentioned keep their value). Returns the
 * number of keys applied. */
int settings_parse(settings *s, const char *text);
/* Writes the whole set as text; returns the length (always < cap). */
int settings_format(const settings *s, char *out, int cap);

/* The largest settings file read: the game writes ~300 bytes, and the
 * rest is room for comments a player adds. */
#define SETTINGS_MAX_FILE 65535

/* Reads path into s and returns the number of keys applied. Otherwise s
 * is unchanged and the result is SETTINGS_MISSING when there is no file
 * (saving creates one), or SETTINGS_UNUSABLE when there is one this build
 * cannot use: not a regular file (a symlink, say), unreadable, larger
 * than SETTINGS_MAX_FILE, or not empty yet without a single setting.
 * Saving must leave such a file alone: it is somebody's, and writing the
 * defaults over it would lose it. */
#define SETTINGS_MISSING (-1)
#define SETTINGS_UNUSABLE (-2)
int settings_load(settings *s, const char *path);
int settings_save(const settings *s, const char *path);

#endif
