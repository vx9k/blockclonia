/* Menus: the title screen, the pause menu, settings and the controls
 * reference. Immediate mode like the rest of the UI: each frame lays the
 * screen out, hit-tests the mouse against it and draws it in one pass, so
 * it needs no window or GPU and runs in the unit tests.
 *
 * Mouse and keyboard both work: arrows move the focus, Enter activates,
 * Left/Right change a setting and Esc goes back. */
#ifndef MC_MENU_H
#define MC_MENU_H

#include "settings.h"
#include "ui.h"

typedef enum { SCREEN_NONE, SCREEN_TITLE, SCREEN_PAUSE, SCREEN_SETTINGS, SCREEN_CONTROLS } screen_id;

typedef enum {
    MENU_NONE,
    MENU_PLAY,         /* title: start playing */
    MENU_RESUME,       /* pause: back to the game */
    MENU_TO_TITLE,     /* pause: save and return to the title screen */
    MENU_QUIT,         /* quit the game */
    MENU_SETTINGS,     /* a setting changed (apply it; saved when leaving) */
} menu_action;

typedef struct {
    float mx, my;      /* cursor in logical pixels; negative when outside */
    int mouse_down;    /* left button held */
    int click;         /* left button pressed this frame */
    int up, down, left, right, enter, back; /* key presses this frame */
    float dt;
} menu_input;

#define MENU_MAX_ITEMS 16

typedef struct {
    int screen;
    int back_to;       /* settings/controls return here */
    float age;         /* s on this screen, drives the entry animation */
    double time;       /* s since start, drives idle animations */
    int focus;         /* keyboard focus */
    int drag;          /* slider being dragged, -1 none */
    int items;         /* focusable items laid out last frame */
    float hover[MENU_MAX_ITEMS];
    float last_mx, last_my;
    char footer[160];  /* title screen, bottom left: versions and seed */
    char status[160];  /* pause screen: a line about the survivor */
} menu;

void menu_init(menu *m);
void menu_open(menu *m, int screen);

/* Lays out, handles input and draws the current screen. Returns what the
 * player asked for this frame. */
menu_action menu_frame(menu *m, ui *u, const menu_input *in, settings *s);

#endif
