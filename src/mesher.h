/* Greedy mesher for one 16^3 section, with baked ambient occlusion.
 *
 * Vertex format: one uint32 per vertex (4 bytes), decoded in block.vert:
 *   bits  0-4  x (0..16)     bits  5-9  y     bits 10-14 z
 *   bits 15-17 face (0..5: +X -X +Y -Y +Z -Z)
 *   bits 18-19 ambient occlusion (0 = darkest, 3 = unoccluded)
 *   bits 20-27 texture layer
 *   bits 28-31 vertical drop in 1/8 block (water surfaces)
 * Quads are 4 consecutive vertices, drawn with a shared index buffer. */
#ifndef MC_MESHER_H
#define MC_MESHER_H

#include <stdint.h>

#define MESH_PAD 18
#define MESH_PAD_VOL (MESH_PAD * MESH_PAD * MESH_PAD)
/* Every quad is one face of one block, so a section never exceeds 6 faces
 * per block. */
#define MESH_MAX_QUADS (16 * 16 * 16 * 6)

typedef struct {
    uint8_t blocks[MESH_PAD_VOL]; /* section plus a 1-block border */
    uint8_t meta[MESH_PAD_VOL];
} mesh_input;

static inline int mesh_pidx(int x, int y, int z)
{
    return ((y + 1) * MESH_PAD + (z + 1)) * MESH_PAD + (x + 1);
}

/* Builds lookup tables; call once before any worker thread meshes. */
void mesher_init(void);

typedef struct {
    uint32_t opaque, trans;
    /* Opaque quads are grouped by face (+X -X +Y -Y +Z -Z); group f ends
     * at quad face_end[f], so the renderer can skip groups facing away. */
    uint16_t face_end[6];
} mesh_counts;

/* Writes opaque quads first, then translucent ones. `out` must hold
 * MESH_MAX_QUADS * 4 vertices. Returns total quad count. */
uint32_t mesh_section_counts(const mesh_input *in, uint32_t *out, mesh_counts *counts);
uint32_t mesh_section(const mesh_input *in, uint32_t *out, uint32_t *opaque_quads,
                      uint32_t *trans_quads);

#endif
