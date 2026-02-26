/*
 * glb_anim.c - Animation evaluation and blending for GLB models
 *
 * Handles keyframe interpolation, pose evaluation, animation blending,
 * and vertex skinning using the SH4 FPU.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "glb_anim.h"

/* Small epsilon for float comparisons (length checks, weight thresholds) */
#define GLB_EPSILON 0.0001f

/* -------------------------------------------------------------------------
 * Quaternion / vector math helpers
 * ------------------------------------------------------------------------- */
static float vec3_lerp_comp(float a, float b, float t) { return a + (b - a) * t; }

static glb_vec3_t vec3_lerp(glb_vec3_t a, glb_vec3_t b, float t)
{
    glb_vec3_t r;
    r.x = vec3_lerp_comp(a.x, b.x, t);
    r.y = vec3_lerp_comp(a.y, b.y, t);
    r.z = vec3_lerp_comp(a.z, b.z, t);
    return r;
}

/* Quaternion spherical linear interpolation (slerp) */
static glb_vec4_t quat_slerp(glb_vec4_t a, glb_vec4_t b, float t)
{
    float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;

    /* Ensure shortest path */
    if (dot < 0.0f) {
        b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w;
        dot = -dot;
    }

    glb_vec4_t r;
    if (dot > 0.9995f) {
        /* Very close, use linear interpolation */
        r.x = a.x + (b.x - a.x) * t;
        r.y = a.y + (b.y - a.y) * t;
        r.z = a.z + (b.z - a.z) * t;
        r.w = a.w + (b.w - a.w) * t;
    } else {
        float theta = acosf(dot);
        float sin_theta = sinf(theta);
        float wa = sinf((1.0f - t) * theta) / sin_theta;
        float wb = sinf(t * theta) / sin_theta;
        r.x = wa * a.x + wb * b.x;
        r.y = wa * a.y + wb * b.y;
        r.z = wa * a.z + wb * b.z;
        r.w = wa * a.w + wb * b.w;
    }

    /* Normalize */
    float len = sqrtf(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
    if (len > GLB_EPSILON) {
        float inv = 1.0f / len;
        r.x *= inv; r.y *= inv; r.z *= inv; r.w *= inv;
    }
    return r;
}

/* -------------------------------------------------------------------------
 * Matrix helpers
 * ------------------------------------------------------------------------- */

/* Build a 4x4 TRS matrix from translation, rotation (quaternion), scale */
static void mat4_from_trs(glb_mat4_t *out, glb_vec3_t t, glb_vec4_t q, glb_vec3_t s)
{
    float *m = out->m;

    /* Rotation from quaternion */
    float x2 = q.x + q.x, y2 = q.y + q.y, z2 = q.z + q.z;
    float xx = q.x * x2, xy = q.x * y2, xz = q.x * z2;
    float yy = q.y * y2, yz = q.y * z2, zz = q.z * z2;
    float wx = q.w * x2, wy = q.w * y2, wz = q.w * z2;

    m[0]  = (1.0f - (yy + zz)) * s.x;
    m[1]  = (xy + wz) * s.x;
    m[2]  = (xz - wy) * s.x;
    m[3]  = 0.0f;

    m[4]  = (xy - wz) * s.y;
    m[5]  = (1.0f - (xx + zz)) * s.y;
    m[6]  = (yz + wx) * s.y;
    m[7]  = 0.0f;

    m[8]  = (xz + wy) * s.z;
    m[9]  = (yz - wx) * s.z;
    m[10] = (1.0f - (xx + yy)) * s.z;
    m[11] = 0.0f;

    m[12] = t.x;
    m[13] = t.y;
    m[14] = t.z;
    m[15] = 1.0f;
}

/* 4x4 matrix multiply: out = a * b (column-major) */
static void mat4_multiply(glb_mat4_t *out, const glb_mat4_t *a, const glb_mat4_t *b)
{
    float r[16];
    for (int c = 0; c < 4; c++) {
        for (int row = 0; row < 4; row++) {
            r[c * 4 + row] =
                a->m[0 * 4 + row] * b->m[c * 4 + 0] +
                a->m[1 * 4 + row] * b->m[c * 4 + 1] +
                a->m[2 * 4 + row] * b->m[c * 4 + 2] +
                a->m[3 * 4 + row] * b->m[c * 4 + 3];
        }
    }
    memcpy(out->m, r, sizeof(float) * 16);
}

/* Transform a vec3 by a 4x4 matrix (w=1 for position, w=0 for direction) */
static glb_vec3_t mat4_transform_point(const glb_mat4_t *m, glb_vec3_t v)
{
    glb_vec3_t r;
    r.x = m->m[0] * v.x + m->m[4] * v.y + m->m[8]  * v.z + m->m[12];
    r.y = m->m[1] * v.x + m->m[5] * v.y + m->m[9]  * v.z + m->m[13];
    r.z = m->m[2] * v.x + m->m[6] * v.y + m->m[10] * v.z + m->m[14];
    return r;
}

static glb_vec3_t mat4_transform_dir(const glb_mat4_t *m, glb_vec3_t v)
{
    glb_vec3_t r;
    r.x = m->m[0] * v.x + m->m[4] * v.y + m->m[8]  * v.z;
    r.y = m->m[1] * v.x + m->m[5] * v.y + m->m[9]  * v.z;
    r.z = m->m[2] * v.x + m->m[6] * v.y + m->m[10] * v.z;
    return r;
}

/* -------------------------------------------------------------------------
 * Animation state management
 * ------------------------------------------------------------------------- */
void glb_anim_init(glb_anim_state_t *state, int anim_index, float speed, int looping)
{
    state->anim_index = anim_index;
    state->time = 0.0f;
    state->speed = speed;
    state->looping = looping;
}

void glb_anim_update(glb_anim_state_t *state, const glb_model_t *model, float dt)
{
    if (state->anim_index < 0 || state->anim_index >= model->animation_count)
        return;

    const glb_animation_t *anim = &model->animations[state->anim_index];
    state->time += dt * state->speed;

    if (state->looping && anim->duration > 0.0f) {
        while (state->time >= anim->duration)
            state->time -= anim->duration;
        while (state->time < 0.0f)
            state->time += anim->duration;
    } else {
        if (state->time > anim->duration) state->time = anim->duration;
        if (state->time < 0.0f) state->time = 0.0f;
    }
}

/* -------------------------------------------------------------------------
 * Keyframe sampling
 * ------------------------------------------------------------------------- */
static void sample_channel(const glb_anim_channel_t *ch, float time,
                           float *out, int components)
{
    if (ch->keyframe_count == 0) return;
    if (ch->keyframe_count == 1 || time <= ch->times[0]) {
        memcpy(out, ch->values, components * sizeof(float));
        return;
    }

    int last = ch->keyframe_count - 1;
    if (time >= ch->times[last]) {
        memcpy(out, &ch->values[last * components], components * sizeof(float));
        return;
    }

    /* Find the two keyframes to interpolate between */
    int k0 = 0;
    for (int k = 0; k < last; k++) {
        if (time < ch->times[k + 1]) {
            k0 = k;
            break;
        }
    }
    int k1 = k0 + 1;

    float t0 = ch->times[k0];
    float t1 = ch->times[k1];
    float t = (t1 > t0) ? (time - t0) / (t1 - t0) : 0.0f;

    if (ch->interpolation == GLB_INTERP_STEP) {
        memcpy(out, &ch->values[k0 * components], components * sizeof(float));
    } else {
        /* Linear interpolation */
        const float *v0 = &ch->values[k0 * components];
        const float *v1 = &ch->values[k1 * components];

        if (components == 4) {
            /* Quaternion: use slerp */
            glb_vec4_t qa = {v0[0], v0[1], v0[2], v0[3]};
            glb_vec4_t qb = {v1[0], v1[1], v1[2], v1[3]};
            glb_vec4_t qr = quat_slerp(qa, qb, t);
            out[0] = qr.x; out[1] = qr.y; out[2] = qr.z; out[3] = qr.w;
        } else {
            for (int i = 0; i < components; i++)
                out[i] = v0[i] + (v1[i] - v0[i]) * t;
        }
    }
}

/* -------------------------------------------------------------------------
 * Pose evaluation
 * ------------------------------------------------------------------------- */
void glb_anim_evaluate(const glb_model_t *model, const glb_anim_state_t *state,
                        glb_pose_t *pose)
{
    /* Initialize pose to bind pose (default transforms from joints) */
    for (int j = 0; j < model->joint_count; j++) {
        pose->translations[j] = model->joints[j].translation;
        pose->rotations[j] = model->joints[j].rotation;
        pose->scales[j] = model->joints[j].scale;
    }

    if (state->anim_index < 0 || state->anim_index >= model->animation_count)
        return;

    const glb_animation_t *anim = &model->animations[state->anim_index];

    /* Apply animation channels */
    for (int c = 0; c < anim->channel_count; c++) {
        const glb_anim_channel_t *ch = &anim->channels[c];
        if (ch->target_joint < 0 || ch->target_joint >= model->joint_count)
            continue;

        int j = ch->target_joint;
        float tmp[4];

        switch (ch->path) {
            case GLB_PATH_TRANSLATION:
                sample_channel(ch, state->time, tmp, 3);
                pose->translations[j].x = tmp[0];
                pose->translations[j].y = tmp[1];
                pose->translations[j].z = tmp[2];
                break;
            case GLB_PATH_ROTATION:
                sample_channel(ch, state->time, tmp, 4);
                pose->rotations[j].x = tmp[0];
                pose->rotations[j].y = tmp[1];
                pose->rotations[j].z = tmp[2];
                pose->rotations[j].w = tmp[3];
                break;
            case GLB_PATH_SCALE:
                sample_channel(ch, state->time, tmp, 3);
                pose->scales[j].x = tmp[0];
                pose->scales[j].y = tmp[1];
                pose->scales[j].z = tmp[2];
                break;
        }
    }
}

/* -------------------------------------------------------------------------
 * Pose blending
 * ------------------------------------------------------------------------- */
void glb_anim_blend(const glb_pose_t *a, const glb_pose_t *b, float factor,
                     int joint_count, glb_pose_t *out)
{
    for (int j = 0; j < joint_count; j++) {
        out->translations[j] = vec3_lerp(a->translations[j], b->translations[j], factor);
        out->rotations[j] = quat_slerp(a->rotations[j], b->rotations[j], factor);
        out->scales[j] = vec3_lerp(a->scales[j], b->scales[j], factor);
    }
}

/* -------------------------------------------------------------------------
 * Skinning matrix computation
 * ------------------------------------------------------------------------- */
void glb_anim_compute_skin(const glb_model_t *model, const glb_pose_t *pose,
                            glb_skin_matrices_t *skin)
{
    /* Build local -> world matrices for each joint */
    glb_mat4_t world[GLB_MAX_JOINTS];

    for (int j = 0; j < model->joint_count; j++) {
        glb_mat4_t local;
        mat4_from_trs(&local, pose->translations[j], pose->rotations[j], pose->scales[j]);

        if (model->joints[j].parent >= 0) {
            mat4_multiply(&world[j], &world[model->joints[j].parent], &local);
        } else {
            world[j] = local;
        }

        /* Final skinning matrix = world * inverse_bind */
        mat4_multiply(&skin->matrices[j], &world[j], &model->joints[j].inv_bind);
    }
}

/* -------------------------------------------------------------------------
 * Vertex skinning
 * ------------------------------------------------------------------------- */
void glb_anim_skin_vertices(const glb_model_t *model, int mesh_index,
                             const glb_skin_matrices_t *skin,
                             glb_vec3_t *out_positions, glb_vec3_t *out_normals)
{
    if (mesh_index < 0 || mesh_index >= model->mesh_count)
        return;

    const glb_mesh_t *mesh = &model->meshes[mesh_index];

    for (int v = 0; v < mesh->vertex_count; v++) {
        const glb_vertex_t *vert = &mesh->vertices[v];

        glb_vec3_t pos = {0, 0, 0};
        glb_vec3_t nrm = {0, 0, 0};

        for (int w = 0; w < 4; w++) {
            float weight = vert->weights[w];
            if (weight < GLB_EPSILON) continue;

            int joint_idx = vert->joints[w];
            if (joint_idx >= model->joint_count) continue;

            const glb_mat4_t *mat = &skin->matrices[joint_idx];

            glb_vec3_t tp = mat4_transform_point(mat, vert->pos);
            glb_vec3_t tn = mat4_transform_dir(mat, vert->normal);

            pos.x += tp.x * weight;
            pos.y += tp.y * weight;
            pos.z += tp.z * weight;

            nrm.x += tn.x * weight;
            nrm.y += tn.y * weight;
            nrm.z += tn.z * weight;
        }

        /* Normalize the normal */
        float len = sqrtf(nrm.x * nrm.x + nrm.y * nrm.y + nrm.z * nrm.z);
        if (len > GLB_EPSILON) {
            float inv = 1.0f / len;
            nrm.x *= inv; nrm.y *= inv; nrm.z *= inv;
        }

        out_positions[v] = pos;
        out_normals[v] = nrm;
    }
}
