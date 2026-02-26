/*
 * glb_loader.h - GLB/GLTF model loader for Dreamcast
 *
 * Loads meshes, textures, skeleton, and animations from GLB files
 * using the cgltf library. Data is stored in structures suitable
 * for PVR rendering on the Dreamcast.
 */

#ifndef GLB_LOADER_H
#define GLB_LOADER_H

#include <dc/pvr.h>

/* -------------------------------------------------------------------------
 * Limits
 * ------------------------------------------------------------------------- */
#define GLB_MAX_JOINTS      64
#define GLB_MAX_ANIMATIONS  16
#define GLB_MAX_KEYFRAMES   256
#define GLB_MAX_CHANNELS    64

/* -------------------------------------------------------------------------
 * Math types
 * ------------------------------------------------------------------------- */
typedef struct {
    float x, y, z;
} glb_vec3_t;

typedef struct {
    float x, y, z, w;
} glb_vec4_t;

typedef struct {
    float m[16]; /* column-major 4x4 */
} glb_mat4_t;

/* -------------------------------------------------------------------------
 * Vertex data (ready for PVR rendering after skinning/transform)
 * ------------------------------------------------------------------------- */
typedef struct {
    glb_vec3_t pos;
    glb_vec3_t normal;
    float u, v;
    uint8_t joints[4];
    float weights[4];
} glb_vertex_t;

/* -------------------------------------------------------------------------
 * Mesh
 * ------------------------------------------------------------------------- */
typedef struct {
    glb_vertex_t *vertices;
    uint16_t     *indices;
    int           vertex_count;
    int           index_count;
    int           texture_index; /* -1 = no texture */
} glb_mesh_t;

/* -------------------------------------------------------------------------
 * Texture
 * ------------------------------------------------------------------------- */
typedef struct {
    pvr_ptr_t  pvr_mem;   /* PVR VRAM pointer */
    int        width;
    int        height;
    int        has_alpha;
} glb_texture_t;

/* -------------------------------------------------------------------------
 * Skeleton / Joint
 * ------------------------------------------------------------------------- */
typedef struct {
    char        name[32];
    int         parent;        /* -1 for root */
    glb_vec3_t  translation;
    glb_vec4_t  rotation;      /* quaternion (x,y,z,w) */
    glb_vec3_t  scale;
    glb_mat4_t  inv_bind;      /* inverse bind matrix */
} glb_joint_t;

/* -------------------------------------------------------------------------
 * Animation keyframe channel
 * ------------------------------------------------------------------------- */
typedef enum {
    GLB_PATH_TRANSLATION,
    GLB_PATH_ROTATION,
    GLB_PATH_SCALE
} glb_anim_path_t;

typedef enum {
    GLB_INTERP_STEP,
    GLB_INTERP_LINEAR,
    GLB_INTERP_CUBICSPLINE
} glb_interpolation_t;

typedef struct {
    int                 target_joint;
    glb_anim_path_t    path;
    glb_interpolation_t interpolation;
    float              *times;
    float              *values;      /* vec3 or vec4 packed sequentially */
    int                 keyframe_count;
} glb_anim_channel_t;

typedef struct {
    char                name[32];
    glb_anim_channel_t *channels;
    int                 channel_count;
    float               duration;
} glb_animation_t;

/* -------------------------------------------------------------------------
 * Complete loaded model
 * ------------------------------------------------------------------------- */
typedef struct {
    glb_mesh_t      *meshes;
    int              mesh_count;

    glb_texture_t   *textures;
    int              texture_count;

    glb_joint_t     *joints;
    int              joint_count;

    glb_animation_t *animations;
    int              animation_count;

    /* Node-to-joint mapping (internal) */
    int             *node_to_joint;
    int              node_count;
} glb_model_t;

/* -------------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------------- */

/* Load a GLB file from the filesystem (e.g., "/rd/model.glb").
 * Returns 0 on success, -1 on error. */
int glb_load(const char *path, glb_model_t *model);

/* Free all resources associated with a loaded model. */
void glb_free(glb_model_t *model);

#endif /* GLB_LOADER_H */
