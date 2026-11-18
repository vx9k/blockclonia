/* Item models: every item that isn't a block as a few boxes at its real
 * size in metres, so the dropped item, the one in the hand and the
 * inventory icon are one object seen three ways. Boxes take a material
 * layer (T_MAT_*, greyscale) and a tint, so one layer serves every metal.
 * Builders cover the families (tools by kind and tier, ingots, ore lumps,
 * bottles, buckets) and the one-offs.
 *
 * The models are data built on demand; this module doesn't draw. It turns
 * a model into entity instances (the renderer's cubes) and into an icon's
 * flat-shaded quads (invui draws them), and it never allocates. */
#ifndef MC_ITEMMODEL_H
#define MC_ITEMMODEL_H

#include "entity.h"
#include "item.h"

#define IM_MAX_BOXES 16
#define IM_ICON_QUADS (IM_MAX_BOXES * 3) /* a box shows at most three faces */

typedef struct {
    float pos[3];  /* centre, in the model's frame: metres, y up */
    float size[3]; /* along the box's own axes */
    quat rot;
    uint8_t layer; /* texture layer, stretched over each face */
    uint8_t glow;  /* lights itself (a torch's flame) */
    uint32_t rgb;  /* tint 0xRRGGBB, multiplied into the layer */
} im_box;

/* How the hand takes it: a tool by the end of its handle, a gun at the
 * wrist of the stock, anything else resting in the palm. */
enum { IM_HOLD_PALM, IM_HOLD_TOOL, IM_HOLD_GUN };

typedef struct {
    int n;
    im_box box[IM_MAX_BOXES];
    int hold;
    float grip[3];         /* where the hand holds it */
    int bolt;              /* first of a rifle's two bolt-handle boxes, else 0 (box 0 never is) */
    float lo[3], hi[3];    /* bounds of every box's corners */
} item_model;

/* Builds the model of an item. 0 (and n = 0) for ids drawn as blocks;
 * the torch, a block, has its model too. */
int itemmodel_build(item_id id, item_model *m);

/* Centre of the bounds. */
void itemmodel_centre(const item_model *m, float out[3]);
/* Longest side of the bounds. */
float itemmodel_extent(const item_model *m);

/* The model's instances: the model point `origin` goes to `at`, then the
 * model is turned by rot and scaled. Boxes that glow ignore `light`.
 * Returns the number written, at most max. */
int itemmodel_place(const item_model *m, const float origin[3], const float at[3], quat rot, float scale, float light,
                    entity_instance *out, int max);

/* 1 for layers with see-through texels, which need blending. */
int itemmodel_layer_clear(uint8_t layer);

/* An icon's face: corners in a unit square (y down, clockwise), colour
 * as ui_rgba packs it (alpha in the top byte). */
typedef struct {
    float xy[8];
    uint32_t rgba;
} im_quad;

/* The model from the icon's angle: tools and guns diagonal, handle to the
 * lower left; anything else from above a front corner. Faces facing the
 * eye, flat-shaded, far ones first. Returns the count, at most max. */
int itemmodel_icon(const item_model *m, im_quad *out, int max);

#endif
