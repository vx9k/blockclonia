/* Vulkan 1.0 renderer tuned for low-end GPUs: 4-byte vertices in a few
 * large vertex pool buffers, one draw call per section with back-facing
 * face groups skipped, push constants instead of uniform buffers,
 * reversed-Z depth, no discard, CPU frustum culling, fog. */
#ifndef MC_RENDERER_H
#define MC_RENDERER_H

#include <stdint.h>
#include "mathlib.h"
#include "physics.h"
#include "ui.h"
#include "world.h"

typedef struct GLFWwindow GLFWwindow;

typedef struct {
    int vsync;
    int validate;
    int render_radius;
    int gpu_index;       /* -1: pick automatically */
    uint32_t pool_mb;    /* first vertex pool block; 0: derive from render radius */
    int screenshots;     /* a screenshot is planned: readable swapchain from the start */
} render_opts;

typedef struct {
    dvec3 eye;
    float yaw, pitch, fov;
    int has_selection;
    ipos selection;
    int underwater;
    int hide_crosshair;
    int ui_quads;        /* overlay quads written to renderer_ui_buffer() this frame */
} render_view;

typedef struct {
    int sections_drawn;
    int draw_calls;
    uint32_t quads;
    uint32_t pool_used_kb, pool_total_kb;
} render_stats;

typedef struct renderer renderer;

renderer *renderer_create(GLFWwindow *win, const render_opts *o);
void renderer_destroy(renderer *r);

/* Installs the mesh upload/free hooks on the world. */
void renderer_bind_world(renderer *r, world *w);

/* Waits for the frame slot and acquires an image. Returns 0 when the frame
 * should be skipped (minimised window, swapchain rebuilt). Mesh uploads
 * (jobs_poll) must happen between begin and end. */
int renderer_begin_frame(renderer *r);
void renderer_end_frame(renderer *r, const world *w, const physics *ph, const render_view *v,
                        double alpha);

/* The overlay vertex buffer for the current frame (mapped, write-only), valid
 * between a successful renderer_begin_frame() and renderer_end_frame().
 * Returns NULL, with *max_quads 0, when no frame is active. */
ui_vertex *renderer_ui_buffer(renderer *r, int *max_quads, int *fb_w, int *fb_h);

void renderer_request_screenshot(renderer *r, const char *path);
void renderer_on_resize(renderer *r);
render_stats renderer_stats(const renderer *r);

#endif
