/*
 * GLB Model Viewer - Dreamcast Demo
 *
 * Loads a GLB model from the romdisk and displays it with:
 * - Textured mesh rendering via PVR
 * - Skeletal animation playback
 * - Animation cycling (A button)
 * - Animation blending (smooth transitions between animations)
 * - Camera rotation (D-pad)
 *
 * Controls:
 *   A      - Cycle to next animation
 *   B      - Toggle animation blending on/off
 *   D-Pad  - Rotate camera around model
 *   Start  - Exit
 */

#include <kos.h>
#include <dc/pvr.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "glb_loader.h"
#include "glb_anim.h"

extern uint8 romdisk[];
KOS_INIT_ROMDISK(romdisk);

/* -------------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------------- */
#define SCREEN_W    640
#define SCREEN_H    480
#define NEAR_PLANE  0.1f
#define FAR_PLANE   100.0f
#define FOV_Y       60.0f
#define BLEND_SPEED 3.0f   /* Blend factor increase per second */

/* -------------------------------------------------------------------------
 * Camera state
 * ------------------------------------------------------------------------- */
static float cam_angle_y = 0.0f;   /* Horizontal rotation (radians) */
static float cam_angle_x = 0.3f;   /* Vertical tilt (radians) */
static float cam_distance = 4.0f;  /* Distance from center */

/* -------------------------------------------------------------------------
 * Perspective projection helper
 * Builds a column-major 4x4 perspective matrix.
 * ------------------------------------------------------------------------- */
static void build_perspective(float *m, float fov_deg, float aspect,
                               float znear, float zfar)
{
    float fov_rad = fov_deg * (float)M_PI / 180.0f;
    float f = 1.0f / tanf(fov_rad / 2.0f);
    float range_inv = 1.0f / (znear - zfar);

    memset(m, 0, 16 * sizeof(float));
    m[0]  = f / aspect;
    m[5]  = f;
    m[10] = (zfar + znear) * range_inv;
    m[11] = -1.0f;
    m[14] = 2.0f * zfar * znear * range_inv;
}

/* -------------------------------------------------------------------------
 * View matrix: look at origin from a spherical camera position
 * ------------------------------------------------------------------------- */
static void build_view(float *m, float angle_y, float angle_x, float dist)
{
    /* Camera position in world space */
    float cx = dist * cosf(angle_x) * sinf(angle_y);
    float cy = dist * sinf(angle_x);
    float cz = dist * cosf(angle_x) * cosf(angle_y);

    /* Forward (camera looks at origin) */
    float fx = -cx, fy = -cy, fz = -cz;
    float flen = sqrtf(fx * fx + fy * fy + fz * fz);
    fx /= flen; fy /= flen; fz /= flen;

    /* Right = forward x up(0,1,0) */
    float rx = fz, ry = 0, rz = -fx;
    float rlen = sqrtf(rx * rx + rz * rz);
    if (rlen > 0.0001f) { rx /= rlen; rz /= rlen; }

    /* True up = right x forward */
    float ux = ry * fz - rz * fy;
    float uy = rz * fx - rx * fz;
    float uz = rx * fy - ry * fx;

    /* Column-major view matrix */
    m[0]  = rx;  m[1]  = ux;  m[2]  = -fx; m[3]  = 0;
    m[4]  = ry;  m[5]  = uy;  m[6]  = -fy; m[7]  = 0;
    m[8]  = rz;  m[9]  = uz;  m[10] = -fz; m[11] = 0;
    m[12] = -(rx * cx + ry * cy + rz * cz);
    m[13] = -(ux * cx + uy * cy + uz * cz);
    m[14] = -(-fx * cx + -fy * cy + -fz * cz);
    m[15] = 1;
}

/* -------------------------------------------------------------------------
 * Matrix multiply (column-major 4x4)
 * ------------------------------------------------------------------------- */
static void mat4_mul(float *out, const float *a, const float *b)
{
    float r[16];
    for (int c = 0; c < 4; c++) {
        for (int row = 0; row < 4; row++) {
            r[c * 4 + row] =
                a[0 * 4 + row] * b[c * 4 + 0] +
                a[1 * 4 + row] * b[c * 4 + 1] +
                a[2 * 4 + row] * b[c * 4 + 2] +
                a[3 * 4 + row] * b[c * 4 + 3];
        }
    }
    memcpy(out, r, 16 * sizeof(float));
}

/* -------------------------------------------------------------------------
 * Project a 3D point to screen coordinates using MVP matrix
 * Returns 0 if behind camera (should be clipped).
 * ------------------------------------------------------------------------- */
static int project_vertex(const float *mvp, float x, float y, float z,
                           float *sx, float *sy, float *sz)
{
    float cx = mvp[0]*x + mvp[4]*y + mvp[8]*z  + mvp[12];
    float cy = mvp[1]*x + mvp[5]*y + mvp[9]*z  + mvp[13];
    float cw = mvp[3]*x + mvp[7]*y + mvp[11]*z + mvp[15];

    if (cw < 0.001f) return 0; /* Behind camera */

    float inv_w = 1.0f / cw;
    float nx = cx * inv_w;
    float ny = cy * inv_w;

    *sx = (nx * 0.5f + 0.5f) * SCREEN_W;
    *sy = (0.5f - ny * 0.5f) * SCREEN_H;  /* Flip Y for screen coords */
    *sz = inv_w;  /* Use 1/w for PVR Z (nearer = larger) */
    return 1;
}

/* -------------------------------------------------------------------------
 * Simple directional lighting
 * Returns intensity 0..1 for a given normal.
 * ------------------------------------------------------------------------- */
static float compute_lighting(float nx, float ny, float nz)
{
    /* Light direction (normalized): upper-right-front */
    const float lx = 0.577f, ly = 0.577f, lz = 0.577f;
    float dot = nx * lx + ny * ly + nz * lz;
    if (dot < 0.0f) dot = 0.0f;
    /* Ambient + diffuse */
    return 0.3f + 0.7f * dot;
}

/* -------------------------------------------------------------------------
 * Render a mesh to PVR
 * ------------------------------------------------------------------------- */
static void render_mesh(const glb_model_t *model, int mesh_index,
                         const glb_vec3_t *positions, const glb_vec3_t *normals,
                         const float *mvp)
{
    const glb_mesh_t *mesh = &model->meshes[mesh_index];
    if (!mesh->vertices || mesh->index_count == 0) return;

    /* Set up polygon header */
    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;

    int tex_idx = mesh->texture_index;
    if (tex_idx >= 0 && tex_idx < model->texture_count &&
        model->textures[tex_idx].pvr_mem) {
        glb_texture_t *tex = &model->textures[tex_idx];
        int fmt = tex->has_alpha ? PVR_TXRFMT_ARGB4444 : PVR_TXRFMT_RGB565;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, fmt | PVR_TXRFMT_NONTWIDDLED,
                          tex->width, tex->height, tex->pvr_mem,
                          PVR_FILTER_BILINEAR);
    } else {
        pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    }

    cxt.gen.culling = PVR_CULLING_NONE; /* Show both sides */
    pvr_poly_compile(&hdr, &cxt);
    pvr_prim(&hdr, sizeof(hdr));

    /* Submit triangles */
    for (int i = 0; i < mesh->index_count; i += 3) {
        pvr_vertex_t verts[3];
        int valid = 1;

        for (int v = 0; v < 3; v++) {
            int idx = mesh->indices[i + v];
            if (idx >= mesh->vertex_count) { valid = 0; break; }

            float sx, sy, sz;
            if (!project_vertex(mvp,
                                positions[idx].x, positions[idx].y, positions[idx].z,
                                &sx, &sy, &sz)) {
                valid = 0;
                break;
            }

            /* Simple lighting */
            float light = compute_lighting(normals[idx].x, normals[idx].y, normals[idx].z);

            verts[v].x = sx;
            verts[v].y = sy;
            verts[v].z = sz;
            verts[v].u = mesh->vertices[idx].u;
            verts[v].v = mesh->vertices[idx].v;
            verts[v].argb = PVR_PACK_COLOR(1.0f, light, light, light);
            verts[v].oargb = 0;
        }

        if (!valid) continue;

        /* PVR wants triangle strips; we submit individual triangles
         * as 3-vertex strips using VERTEX, VERTEX, VERTEX_EOL */
        verts[0].flags = PVR_CMD_VERTEX;
        verts[1].flags = PVR_CMD_VERTEX;
        verts[2].flags = PVR_CMD_VERTEX_EOL;

        pvr_prim(&verts[0], sizeof(pvr_vertex_t));
        pvr_prim(&verts[1], sizeof(pvr_vertex_t));
        pvr_prim(&verts[2], sizeof(pvr_vertex_t));
    }
}

/* -------------------------------------------------------------------------
 * Draw HUD text showing current state
 * ------------------------------------------------------------------------- */
static void draw_hud(const glb_model_t *model, int anim_index, int blending,
                      float blend_factor)
{
    char buf[128];
    int y = 20;

    bfont_set_encoding(BFONT_CODE_ISO8859_1);

    bfont_draw_str(vram_s + (y * SCREEN_W + 20), SCREEN_W, 0, "GLB Model Viewer");
    y += 30;

    if (model->animation_count > 0) {
        snprintf(buf, sizeof(buf), "Anim [A]: %s (%d/%d)",
                 model->animations[anim_index].name,
                 anim_index + 1, model->animation_count);
        bfont_draw_str(vram_s + (y * SCREEN_W + 20), SCREEN_W, 0, buf);
        y += 24;

        snprintf(buf, sizeof(buf), "Blend [B]: %s (%.0f%%)",
                 blending ? "ON" : "OFF", blend_factor * 100.0f);
        bfont_draw_str(vram_s + (y * SCREEN_W + 20), SCREEN_W, 0, buf);
        y += 24;
    } else {
        bfont_draw_str(vram_s + (y * SCREEN_W + 20), SCREEN_W, 0, "No animations");
        y += 24;
    }

    snprintf(buf, sizeof(buf), "Meshes: %d  Joints: %d  Textures: %d",
             model->mesh_count, model->joint_count, model->texture_count);
    bfont_draw_str(vram_s + (y * SCREEN_W + 20), SCREEN_W, 0, buf);
    y += 24;

    bfont_draw_str(vram_s + (y * SCREEN_W + 20), SCREEN_W, 0,
                   "D-Pad: rotate  Start: exit");
}

/* -------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */
int main(void)
{
    maple_device_t  *cont_dev;
    cont_state_t    *state;
    uint32_t         prev_buttons = 0;
    int              done = 0;
    float            dt = 1.0f / 60.0f; /* Approximate frame time */

    /* -- Video / PVR init ------------------------------------------------- */
    vid_set_mode(DM_640x480, PM_RGB565);
    pvr_init_defaults();

    printf("=== GLB Model Viewer ===\n");

    /* -- Load model ------------------------------------------------------- */
    glb_model_t model;
    if (glb_load("/rd/model.glb", &model) < 0) {
        printf("ERROR: Failed to load model!\n");
        /* Show error on screen */
        while (!done) {
            pvr_wait_ready();
            pvr_scene_begin();
            pvr_list_begin(PVR_LIST_OP_POLY);
            pvr_list_finish();
            pvr_scene_finish();
            bfont_draw_str(vram_s + (200 * SCREEN_W + 100), SCREEN_W, 0,
                           "Error: Could not load model.glb");
            cont_dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
            if (cont_dev) {
                state = (cont_state_t *)maple_dev_status(cont_dev);
                if (state && (state->buttons & CONT_START))
                    done = 1;
            }
        }
        return 1;
    }

    /* -- Animation state -------------------------------------------------- */
    int current_anim = 0;
    int blending_enabled = 1;
    float blend_factor = 1.0f; /* 1.0 = fully current, 0.0 = fully previous */

    glb_anim_state_t anim_state;
    glb_anim_state_t prev_anim_state;
    glb_anim_init(&anim_state, 0, 1.0f, 1);
    glb_anim_init(&prev_anim_state, 0, 1.0f, 1);

    glb_pose_t pose_current, pose_prev, pose_blended;
    glb_skin_matrices_t skin;

    /* Allocate skinned vertex buffers */
    int max_verts = 0;
    for (int m = 0; m < model.mesh_count; m++) {
        if (model.meshes[m].vertex_count > max_verts)
            max_verts = model.meshes[m].vertex_count;
    }
    glb_vec3_t *skinned_pos = malloc(max_verts * sizeof(glb_vec3_t));
    glb_vec3_t *skinned_nrm = malloc(max_verts * sizeof(glb_vec3_t));
    if (!skinned_pos || !skinned_nrm) {
        printf("ERROR: Failed to allocate vertex buffers!\n");
        free(skinned_pos);
        free(skinned_nrm);
        glb_free(&model);
        return 1;
    }

    /* -- Projection matrix (constant) ------------------------------------- */
    float proj[16];
    build_perspective(proj, FOV_Y, (float)SCREEN_W / (float)SCREEN_H,
                       NEAR_PLANE, FAR_PLANE);

    /* -- Main loop -------------------------------------------------------- */
    while (!done) {
        /* -- Input -------------------------------------------------------- */
        cont_dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
        if (cont_dev) {
            state = (cont_state_t *)maple_dev_status(cont_dev);
            if (state) {
                uint32_t buttons = state->buttons;
                uint32_t newly_pressed = buttons & ~prev_buttons;

                /* Start: exit */
                if (newly_pressed & CONT_START)
                    done = 1;

                /* A: cycle animation */
                if ((newly_pressed & CONT_A) && model.animation_count > 0) {
                    prev_anim_state = anim_state;
                    current_anim = (current_anim + 1) % model.animation_count;
                    glb_anim_init(&anim_state, current_anim, 1.0f, 1);
                    if (blending_enabled)
                        blend_factor = 0.0f; /* Start blending from previous */
                    else
                        blend_factor = 1.0f;
                }

                /* B: toggle blending */
                if (newly_pressed & CONT_B)
                    blending_enabled = !blending_enabled;

                /* D-pad: rotate camera */
                if (buttons & CONT_DPAD_LEFT)
                    cam_angle_y -= 2.0f * dt;
                if (buttons & CONT_DPAD_RIGHT)
                    cam_angle_y += 2.0f * dt;
                if (buttons & CONT_DPAD_UP)
                    cam_angle_x += 1.0f * dt;
                if (buttons & CONT_DPAD_DOWN)
                    cam_angle_x -= 1.0f * dt;

                /* Clamp vertical angle */
                if (cam_angle_x > 1.4f) cam_angle_x = 1.4f;
                if (cam_angle_x < -0.5f) cam_angle_x = -0.5f;

                prev_buttons = buttons;
            }
        }

        /* -- Update animations -------------------------------------------- */
        glb_anim_update(&anim_state, &model, dt);

        /* Advance blend factor */
        if (blend_factor < 1.0f) {
            blend_factor += BLEND_SPEED * dt;
            if (blend_factor > 1.0f) blend_factor = 1.0f;
            glb_anim_update(&prev_anim_state, &model, dt);
        }

        /* Evaluate poses */
        glb_anim_evaluate(&model, &anim_state, &pose_current);

        glb_pose_t *final_pose;
        if (blend_factor < 1.0f && blending_enabled) {
            glb_anim_evaluate(&model, &prev_anim_state, &pose_prev);
            glb_anim_blend(&pose_prev, &pose_current, blend_factor,
                           model.joint_count, &pose_blended);
            final_pose = &pose_blended;
        } else {
            final_pose = &pose_current;
        }

        /* Compute skinning matrices */
        glb_anim_compute_skin(&model, final_pose, &skin);

        /* -- Build MVP matrix --------------------------------------------- */
        float view[16], mvp[16];
        build_view(view, cam_angle_y, cam_angle_x, cam_distance);
        mat4_mul(mvp, proj, view);

        /* -- Rendering ---------------------------------------------------- */
        pvr_wait_ready();
        pvr_scene_begin();
        pvr_list_begin(PVR_LIST_OP_POLY);

        for (int m = 0; m < model.mesh_count; m++) {
            /* Skin vertices */
            if (model.joint_count > 0) {
                glb_anim_skin_vertices(&model, m, &skin, skinned_pos, skinned_nrm);
            } else {
                /* No skeleton: use original positions */
                for (int v = 0; v < model.meshes[m].vertex_count; v++) {
                    skinned_pos[v] = model.meshes[m].vertices[v].pos;
                    skinned_nrm[v] = model.meshes[m].vertices[v].normal;
                }
            }

            render_mesh(&model, m, skinned_pos, skinned_nrm, mvp);
        }

        pvr_list_finish();
        pvr_scene_finish();

        /* Draw HUD directly to VRAM after PVR scene */
        draw_hud(&model, current_anim, blending_enabled, blend_factor);
    }

    /* -- Cleanup ---------------------------------------------------------- */
    free(skinned_pos);
    free(skinned_nrm);
    glb_free(&model);

    return 0;
}
