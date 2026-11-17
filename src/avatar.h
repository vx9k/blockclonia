/* The players' figure: a skeleton of boxes at adult proportions (segment
 * lengths as fractions of a 1.75 m height, after Drillis & Contini) in a
 * bare linen base layer, posed from joint angles measured in human gait
 * studies. It walks, runs and sneaks at the speed it is moved, with the
 * stride growing with speed so the cadence stays human, and stands on its
 * lowest point. Clothing, held items and the first-person arm are not
 * here; this only draws the body. No window or GPU. */
#ifndef MC_AVATAR_H
#define MC_AVATAR_H

#include "entity.h"

/* The bones, parents before children; arms and legs run right then left. */
enum {
    AV_PELVIS = 0,
    AV_SPINE = 1,
    AV_CHEST = 2,
    AV_NECK = 3,
    AV_HEAD = 4,
    AV_SHOULDER_R = 5, /* then elbow, wrist, fingers, thumb */
    AV_SHOULDER_L = 10,
    AV_HIP_R = 15, /* then knee, ankle */
    AV_HIP_L = 18,
    AVATAR_BONES = 21
};
#define AVATAR_BOXES 27
#define AVATAR_HEIGHT 1.75f /* m, to the crown */

/* How a figure is moving, kept between frames. */
typedef struct {
    float phase;   /* 0..1 through the stride; 0 is the right heel striking */
    float speed;   /* smoothed ground speed, m/s */
    float sneak;   /* 0..1, eased toward sneaking */
    float time;    /* s, for breathing; wraps */
    double x, z;   /* feet last frame */
    int seen;      /* x, z hold a position */
} avatar_anim;

/* Moves the stride on by how far the feet went since the last call (a jump
 * of more than a few metres is a teleport and restarts it), so the legs
 * keep pace with the ground at any frame rate. Flying figures stand. */
void avatar_anim_update(avatar_anim *a, double x, double z, int sneaking, int flying, float dt);

/* The figure with its feet at feet (relative to the camera), facing yaw
 * (radians; 0 looks toward -z, turning toward +x) with the head pitched up
 * by pitch, lying on its front when dead. Writes AVATAR_BOXES instances
 * (fewer if max is smaller) and returns how many. */
int avatar_build(const avatar_anim *a, const float feet[3], float yaw, float pitch, int dead, float light,
                 entity_instance *out, int max);

/* The bone box i hangs from, and a bone's parent (-1 for the pelvis); for
 * the tests that look for boxes passing into each other. */
int avatar_box_bone(int box);
int avatar_bone_parent(int bone);

/* Metres per stride (two steps) at a ground speed, for sneaking 0..1. */
float avatar_stride(float speed, float sneak);

#endif
