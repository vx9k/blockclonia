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

typedef enum {
    SCREEN_NONE,
    SCREEN_TITLE,
    SCREEN_PAUSE,
    SCREEN_SETTINGS,
    SCREEN_CONTROLS,
    SCREEN_DEBUG_HEALTH,
    SCREEN_WORLDS,       /* the player's worlds */
    SCREEN_CREATE_WORLD,
    SCREEN_RENAME_WORLD,
    SCREEN_DELETE_WORLD  /* the confirmation */
} screen_id;

typedef enum {
    MENU_NONE,
    MENU_WORLDS,       /* the world list opened: fill worlds/world_count */
    MENU_WORLD_PLAY,   /* play worlds[world_sel] */
    MENU_WORLD_CREATE, /* create text_name with the seed in text_seed ("" for a random one) */
    MENU_WORLD_RENAME, /* rename worlds[world_sel] to text_name */
    MENU_WORLD_DELETE, /* delete worlds[world_sel]; the player has confirmed */
    MENU_RESUME,       /* pause: back to the game */
    MENU_TO_TITLE,     /* pause: save and return to the title screen */
    MENU_QUIT,         /* quit the game */
    MENU_SETTINGS,     /* a setting changed (apply it; saved when leaving) */
    MENU_DEBUG_INJURE, /* debug menu: apply debug_kind to debug_part (see below) */
    MENU_DEBUG_RESET,  /* debug menu: heal the body back to new */
} menu_action;

typedef struct {
    float mx, my;      /* cursor in logical pixels; negative when outside */
    int mouse_down;    /* left button held */
    int click;         /* left button pressed this frame */
    int up, down, left, right, enter, back; /* key presses this frame */
    int letter_nav;    /* those came from W/A/S/D/Space, which a text field types instead */
    char text[16];     /* printable ASCII typed this frame */
    int text_len;
    int backspace;
    float dt;
} menu_input;

#define MENU_NAME_MAX 32 /* SAVE_NAME_MAX */
#define MENU_SEED_MAX 20

/* One row of the world list, as main.c describes it. */
typedef struct {
    char name[MENU_NAME_MAX + 1];
    char detail[80];   /* seed, when last played, or why it cannot be played */
    int playable;
} menu_world;

#define MENU_MAX_ITEMS 32
#define SETTINGS_TABS 4 /* the debug health screen lists every HEALTH_DEBUG_KINDS entry */

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
    int tab;           /* settings: 0 video, 1 display, 2 controls, 3 audio */
    int clicked;       /* a button or setting was used this frame (UI sound) */
    int hovered;       /* the focus moved to another item this frame (UI sound) */
    /* Debug health screen: the part cycled to, and what MENU_DEBUG_INJURE
     * asks the caller to apply. menu.c never sees a health struct; it
     * only hands back which of HEALTH_DEBUG_KINDS was clicked. */
    int debug_part;
    int debug_kind;
    /* World screens: the list main.c fills on MENU_WORLDS, the selected
     * row, the first row shown, the text being typed and a line saying
     * what went wrong last. */
    const menu_world *worlds;
    int world_count, world_sel, world_scroll;
    char text_name[MENU_NAME_MAX + 1];
    char text_seed[MENU_SEED_MAX + 1];
    char note[96];
    int text_focus;    /* a text field had the focus last frame */
} menu;

void menu_init(menu *m);
void menu_open(menu *m, int screen);

/* Lays out, handles input and draws the current screen. Returns what the
 * player asked for this frame. */
menu_action menu_frame(menu *m, ui *u, const menu_input *in, settings *s);

#endif
