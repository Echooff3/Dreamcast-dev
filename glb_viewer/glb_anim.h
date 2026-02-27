/*
 * glb_anim.h - Animation evaluation and blending for GLB models
 */

#ifndef GLB_ANIM_H
#define GLB_ANIM_H

#include "glb_loader.h"

/* -------------------------------------------------------------------------
 * Animation state for playback
 * ------------------------------------------------------------------------- */
typedef struct {
    int   anim_index;       /* which animation in the model */
    float time;             /* current playback time */
    float speed;            /* playback speed multiplier */
    int   looping;          /* 1 = loop, 0 = clamp */
} glb_anim_state_t;

/* -------------------------------------------------------------------------
 * Pose: per-joint local transforms after animation evaluation
 * ------------------------------------------------------------------------- */
typedef struct {
    glb_vec3_t translations[GLB_MAX_JOINTS];
    glb_vec4_t rotations[GLB_MAX_JOINTS];
    glb_vec3_t scales[GLB_MAX_JOINTS];
} glb_pose_t;

/* -------------------------------------------------------------------------
 * Skinning matrices: final world-space joint matrices for vertex skinning
 * ------------------------------------------------------------------------- */
typedef struct {
    glb_mat4_t matrices[GLB_MAX_JOINTS];
} glb_skin_matrices_t;

/* -------------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------------- */

/* Initialize an animation state. */
void glb_anim_init(glb_anim_state_t *state, int anim_index, float speed, int looping);

/* Advance animation time by dt seconds. */
void glb_anim_update(glb_anim_state_t *state, const glb_model_t *model, float dt);

/* Evaluate the current pose from an animation state. */
void glb_anim_evaluate(const glb_model_t *model, const glb_anim_state_t *state,
                        glb_pose_t *pose);

/* Blend two poses: out = lerp(a, b, factor). factor=0 gives a, factor=1 gives b. */
void glb_anim_blend(const glb_pose_t *a, const glb_pose_t *b, float factor,
                     int joint_count, glb_pose_t *out);

/* Compute skinning matrices from a pose.
 * Combines local transforms into world transforms, then applies inverse bind matrices. */
void glb_anim_compute_skin(const glb_model_t *model, const glb_pose_t *pose,
                            glb_skin_matrices_t *skin);

/* Apply skinning to mesh vertices, writing transformed positions and normals. */
void glb_anim_skin_vertices(const glb_model_t *model, int mesh_index,
                             const glb_skin_matrices_t *skin,
                             glb_vec3_t *out_positions, glb_vec3_t *out_normals);

#endif /* GLB_ANIM_H */
