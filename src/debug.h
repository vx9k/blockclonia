/* The F3 debug overlay: where you are and what you are looking at on the
 * left; the engine (frame times, memory, renderer, physics) on the right;
 * a frame-time graph and an axis gizmo. The game fills a debug_info each
 * frame; drawing needs nothing else, so it runs in the unit tests. */
#ifndef MC_DEBUG_H
#define MC_DEBUG_H

#include "ui.h"
#include <stddef.h>
#include <stdint.h>

#define DEBUG_HIST 240
/* The left column ends above this (logical pixels) even with every
 * optional line showing, so the HUD can start below it and stay put. */
#define DEBUG_LEFT_BOTTOM 142.0f

typedef struct {
    float ms[DEBUG_HIST];      /* frame times, ring buffer */
    int pos;
    float fps;                 /* smoothed */
} debug_frames;

/* Records one frame time and updates the smoothed fps. */
void debug_frames_push(debug_frames *f, float ms);

typedef struct {
    const debug_frames *frames;
    float phys_ms;             /* physics + health per frame */

    double x, y, z;
    float yaw, pitch;          /* radians */
    double vx, vy, vz;
    int on_ground, flying, sprinting, sneaking;
    double submerged;
    float air_temp, body_temp, feels_like; /* degrees C */
    double day_time;           /* 0..1, 0 = midnight */

    int has_target;
    int tx, ty, tz;
    uint8_t target_id;
    float target_temp;         /* degrees C of the looked-at block */
    int target_level;          /* water level, 0 if not water */
    float break_progress;      /* 0..1 */

    uint32_t seed;
    int radius, threads;
    int bodies, items, particles, fluid_updates, last_collapse, heat_cells, fires;

    int draw_calls, sections;
    uint32_t quads, pool_used_kb, pool_total_kb;
    int ui_quads;
    size_t rss, commit;        /* bytes; 0 if unknown */
    const char *gpu;
    const char *versions;      /* "mimalloc 3.5.3, GLFW 3.5.1 ..." */
    const char *api;           /* "Vulkan 1.4: ..." (renderer features in use) */
    const char *audio;         /* output device, or "off" */
    int snd_voices;
    float snd_load;            /* mixer time / real time */
    float gpu_ms;              /* GPU time of a frame, 0 if unknown */
    uint32_t vram_used_mb, vram_budget_mb;
    int width, height;
} debug_info;

/* Draws the whole overlay from d (renderer lines it has no data for are left out). */
void debug_draw(ui *u, const debug_info *d);

/* "north", "south-east", ... for a yaw in radians (0 = -Z = north). */
const char *debug_facing(float yaw);

#endif
