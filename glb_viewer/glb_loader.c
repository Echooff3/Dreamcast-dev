/*
 * glb_loader.c - GLB/GLTF model loader implementation
 *
 * Uses cgltf to parse GLB files and extracts mesh, texture,
 * skeleton, and animation data for Dreamcast PVR rendering.
 * Uses stb_image for decoding embedded PNG/JPEG textures.
 */

#define CGLTF_IMPLEMENTATION
#include "deps/cgltf.h"

/* stb_image for decoding embedded PNG/JPEG textures (self-contained,
 * no external zlib dependency). Only enable PNG and JPEG decoders. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "deps/stb_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dc/pvr.h>

#include "glb_loader.h"

/* -------------------------------------------------------------------------
 * Upload RGBA pixels to PVR VRAM as RGB565 or ARGB4444 texture
 * Textures must be power-of-2 dimensions.
 * ------------------------------------------------------------------------- */
static int next_power_of_2(int v)
{
    v--;
    v |= v >> 1; v |= v >> 2; v |= v >> 4;
    v |= v >> 8; v |= v >> 16;
    return v + 1;
}

static pvr_ptr_t upload_texture(const uint8_t *rgba, int w, int h, int has_alpha,
                                 int *out_w, int *out_h)
{
    int pw = next_power_of_2(w);
    int ph = next_power_of_2(h);
    if (pw < 8) pw = 8;
    if (ph < 8) ph = 8;

    /* Downscale source to fit PVR max texture size (1024x1024).
     * Uses simple 2x2 box filter for each halving step. */
    const uint8_t *src = rgba;
    uint8_t *scaled = NULL;
    int sw = w, sh = h;

    while (pw > 1024 || ph > 1024) {
        int nw = sw / 2;
        int nh = sh / 2;
        if (nw < 1) nw = 1;
        if (nh < 1) nh = 1;

        uint8_t *dst = malloc((size_t)nw * nh * 4);
        if (!dst) { free(scaled); return NULL; }

        for (int y = 0; y < nh; y++) {
            for (int x = 0; x < nw; x++) {
                int x0 = x * 2, y0 = y * 2;
                int x1 = (x0 + 1 < sw) ? x0 + 1 : x0;
                int y1 = (y0 + 1 < sh) ? y0 + 1 : y0;
                for (int c = 0; c < 4; c++) {
                    int sum = src[(y0 * sw + x0) * 4 + c]
                            + src[(y0 * sw + x1) * 4 + c]
                            + src[(y1 * sw + x0) * 4 + c]
                            + src[(y1 * sw + x1) * 4 + c];
                    dst[(y * nw + x) * 4 + c] = (uint8_t)(sum / 4);
                }
            }
        }

        free(scaled);
        scaled = dst;
        src = scaled;
        sw = nw;
        sh = nh;
        pw = next_power_of_2(sw);
        ph = next_power_of_2(sh);
        if (pw < 8) pw = 8;
        if (ph < 8) ph = 8;
    }

    /* Convert to 16-bit format */
    uint16_t *tex_data = malloc((size_t)pw * ph * 2);
    if (!tex_data) { free(scaled); return NULL; }

    for (int y = 0; y < ph; y++) {
        for (int x = 0; x < pw; x++) {
            int sx = (x < sw) ? x : sw - 1;
            int sy = (y < sh) ? y : sh - 1;
            int si = (sy * sw + sx) * 4;
            uint8_t r = src[si + 0];
            uint8_t g = src[si + 1];
            uint8_t b = src[si + 2];
            uint8_t a = src[si + 3];

            if (has_alpha) {
                /* ARGB4444 */
                tex_data[y * pw + x] = ((a >> 4) << 12) | ((r >> 4) << 8) |
                                        ((g >> 4) << 4) | (b >> 4);
            } else {
                /* RGB565 */
                tex_data[y * pw + x] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
            }
        }
    }

    free(scaled);

    pvr_ptr_t pvr_mem = pvr_mem_malloc((size_t)pw * ph * 2);
    if (pvr_mem) {
        /* Use raw load (no twiddling) - pair with PVR_TXRFMT_NONTWIDDLED
         * in the polygon context when rendering. */
        pvr_txr_load(tex_data, pvr_mem, (size_t)pw * ph * 2);
    }
    free(tex_data);

    *out_w = pw;
    *out_h = ph;
    return pvr_mem;
}

/* -------------------------------------------------------------------------
 * Load textures from GLB
 * ------------------------------------------------------------------------- */
static int load_textures(cgltf_data *data, glb_model_t *model)
{
    if (data->images_count == 0) {
        model->textures = NULL;
        model->texture_count = 0;
        return 0;
    }

    model->texture_count = (int)data->images_count;
    model->textures = calloc(model->texture_count, sizeof(glb_texture_t));
    if (!model->textures) return -1;

    for (cgltf_size i = 0; i < data->images_count; i++) {
        cgltf_image *img = &data->images[i];

        if (!img->buffer_view || !img->buffer_view->buffer->data) {
            printf("GLB: Image %d has no embedded data, skipping\n", (int)i);
            continue;
        }

        const uint8_t *img_data = (const uint8_t *)img->buffer_view->buffer->data
                                   + img->buffer_view->offset;
        size_t img_size = img->buffer_view->size;

        int w, h, channels;
        uint8_t *pixels = NULL;

        /* Decode PNG or JPEG using stb_image */
        pixels = stbi_load_from_memory(img_data, (int)img_size, &w, &h, &channels, 4);

        if (!pixels) {
            printf("GLB: Could not decode image %d\n", (int)i);
            continue;
        }

        /* Check if texture has meaningful alpha */
        int has_alpha = 0;
        for (int p = 0; p < w * h; p++) {
            if (pixels[p * 4 + 3] < 255) {
                has_alpha = 1;
                break;
            }
        }

        int tw = 0, th = 0;
        model->textures[i].pvr_mem = upload_texture(pixels, w, h, has_alpha, &tw, &th);
        model->textures[i].width = tw;
        model->textures[i].height = th;
        model->textures[i].has_alpha = has_alpha;
        stbi_image_free(pixels);

        printf("GLB: Loaded texture %d: %dx%d -> %dx%d (alpha=%d)\n",
               (int)i, w, h, tw, th, has_alpha);
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * Load meshes from GLB
 * ------------------------------------------------------------------------- */
static int load_meshes(cgltf_data *data, glb_model_t *model)
{
    /* Count total primitives across all meshes */
    int total_prims = 0;
    for (cgltf_size m = 0; m < data->meshes_count; m++)
        total_prims += (int)data->meshes[m].primitives_count;

    model->mesh_count = total_prims;
    model->meshes = calloc(total_prims, sizeof(glb_mesh_t));
    if (!model->meshes) return -1;

    int mesh_idx = 0;
    for (cgltf_size m = 0; m < data->meshes_count; m++) {
        cgltf_mesh *mesh = &data->meshes[m];

        for (cgltf_size p = 0; p < mesh->primitives_count; p++) {
            cgltf_primitive *prim = &mesh->primitives[p];
            glb_mesh_t *out = &model->meshes[mesh_idx++];

            /* Find accessors for standard attributes */
            cgltf_accessor *pos_acc = NULL, *nrm_acc = NULL, *uv_acc = NULL;
            cgltf_accessor *jnt_acc = NULL, *wgt_acc = NULL;

            for (cgltf_size a = 0; a < prim->attributes_count; a++) {
                switch (prim->attributes[a].type) {
                    case cgltf_attribute_type_position:
                        pos_acc = prim->attributes[a].data;
                        break;
                    case cgltf_attribute_type_normal:
                        nrm_acc = prim->attributes[a].data;
                        break;
                    case cgltf_attribute_type_texcoord:
                        if (prim->attributes[a].index == 0)
                            uv_acc = prim->attributes[a].data;
                        break;
                    case cgltf_attribute_type_joints:
                        if (prim->attributes[a].index == 0)
                            jnt_acc = prim->attributes[a].data;
                        break;
                    case cgltf_attribute_type_weights:
                        if (prim->attributes[a].index == 0)
                            wgt_acc = prim->attributes[a].data;
                        break;
                    default:
                        break;
                }
            }

            if (!pos_acc) {
                printf("GLB: Primitive has no POSITION attribute, skipping\n");
                continue;
            }

            int vcount = (int)pos_acc->count;
            out->vertex_count = vcount;
            out->vertices = calloc(vcount, sizeof(glb_vertex_t));
            if (!out->vertices) return -1;

            /* Read vertex data */
            for (int v = 0; v < vcount; v++) {
                float tmp[4];

                cgltf_accessor_read_float(pos_acc, v, tmp, 3);
                out->vertices[v].pos.x = tmp[0];
                out->vertices[v].pos.y = tmp[1];
                out->vertices[v].pos.z = tmp[2];

                if (nrm_acc) {
                    cgltf_accessor_read_float(nrm_acc, v, tmp, 3);
                    out->vertices[v].normal.x = tmp[0];
                    out->vertices[v].normal.y = tmp[1];
                    out->vertices[v].normal.z = tmp[2];
                } else {
                    out->vertices[v].normal.x = 0;
                    out->vertices[v].normal.y = 1;
                    out->vertices[v].normal.z = 0;
                }

                if (uv_acc) {
                    cgltf_accessor_read_float(uv_acc, v, tmp, 2);
                    out->vertices[v].u = tmp[0];
                    out->vertices[v].v = tmp[1];
                }

                if (jnt_acc) {
                    cgltf_uint jtmp[4];
                    cgltf_accessor_read_uint(jnt_acc, v, jtmp, 4);
                    out->vertices[v].joints[0] = (uint8_t)jtmp[0];
                    out->vertices[v].joints[1] = (uint8_t)jtmp[1];
                    out->vertices[v].joints[2] = (uint8_t)jtmp[2];
                    out->vertices[v].joints[3] = (uint8_t)jtmp[3];
                }

                if (wgt_acc) {
                    cgltf_accessor_read_float(wgt_acc, v, tmp, 4);
                    out->vertices[v].weights[0] = tmp[0];
                    out->vertices[v].weights[1] = tmp[1];
                    out->vertices[v].weights[2] = tmp[2];
                    out->vertices[v].weights[3] = tmp[3];
                }
            }

            /* Read indices */
            if (prim->indices) {
                int icount = (int)prim->indices->count;
                out->index_count = icount;
                out->indices = malloc(icount * sizeof(uint16_t));
                if (!out->indices) return -1;

                for (int i = 0; i < icount; i++) {
                    out->indices[i] = (uint16_t)cgltf_accessor_read_index(prim->indices, i);
                }
            }

            /* Material / texture reference */
            out->texture_index = -1;
            if (prim->material && prim->material->has_pbr_metallic_roughness) {
                cgltf_texture_view *tv = &prim->material->pbr_metallic_roughness.base_color_texture;
                if (tv->texture && tv->texture->image) {
                    out->texture_index = (int)(tv->texture->image - data->images);
                }
            }

            printf("GLB: Loaded mesh primitive: %d vertices, %d indices, tex=%d\n",
                   out->vertex_count, out->index_count, out->texture_index);
        }
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * Load skeleton from GLB
 * ------------------------------------------------------------------------- */
static int load_skeleton(cgltf_data *data, glb_model_t *model)
{
    if (data->skins_count == 0) {
        model->joints = NULL;
        model->joint_count = 0;
        return 0;
    }

    /* Build node-to-joint mapping for all nodes */
    model->node_count = (int)data->nodes_count;
    model->node_to_joint = malloc(data->nodes_count * sizeof(int));
    if (!model->node_to_joint) return -1;
    for (int i = 0; i < (int)data->nodes_count; i++)
        model->node_to_joint[i] = -1;

    /* Use first skin */
    cgltf_skin *skin = &data->skins[0];
    model->joint_count = (int)skin->joints_count;
    if (model->joint_count > GLB_MAX_JOINTS)
        model->joint_count = GLB_MAX_JOINTS;

    model->joints = calloc(model->joint_count, sizeof(glb_joint_t));
    if (!model->joints) return -1;

    /* Read inverse bind matrices */
    float ibm[16];
    for (int j = 0; j < model->joint_count; j++) {
        cgltf_node *node = skin->joints[j];
        int node_idx = (int)(node - data->nodes);
        model->node_to_joint[node_idx] = j;

        glb_joint_t *joint = &model->joints[j];
        strncpy(joint->name, node->name ? node->name : "joint", 31);
        joint->name[31] = '\0';

        /* Default transform */
        if (node->has_translation) {
            joint->translation.x = node->translation[0];
            joint->translation.y = node->translation[1];
            joint->translation.z = node->translation[2];
        }
        if (node->has_rotation) {
            joint->rotation.x = node->rotation[0];
            joint->rotation.y = node->rotation[1];
            joint->rotation.z = node->rotation[2];
            joint->rotation.w = node->rotation[3];
        } else {
            joint->rotation.w = 1.0f;
        }
        if (node->has_scale) {
            joint->scale.x = node->scale[0];
            joint->scale.y = node->scale[1];
            joint->scale.z = node->scale[2];
        } else {
            joint->scale.x = joint->scale.y = joint->scale.z = 1.0f;
        }

        /* Inverse bind matrix */
        if (skin->inverse_bind_matrices) {
            cgltf_accessor_read_float(skin->inverse_bind_matrices, j, ibm, 16);
            memcpy(joint->inv_bind.m, ibm, sizeof(float) * 16);
        } else {
            /* Identity */
            memset(joint->inv_bind.m, 0, sizeof(float) * 16);
            joint->inv_bind.m[0] = joint->inv_bind.m[5] = 1.0f;
            joint->inv_bind.m[10] = joint->inv_bind.m[15] = 1.0f;
        }

        /* Find parent */
        joint->parent = -1;
        if (node->parent) {
            int parent_node_idx = (int)(node->parent - data->nodes);
            if (model->node_to_joint[parent_node_idx] >= 0) {
                joint->parent = model->node_to_joint[parent_node_idx];
            }
        }
    }

    printf("GLB: Loaded skeleton with %d joints\n", model->joint_count);
    return 0;
}

/* -------------------------------------------------------------------------
 * Load animations from GLB
 * ------------------------------------------------------------------------- */
static int load_animations(cgltf_data *data, glb_model_t *model)
{
    if (data->animations_count == 0) {
        model->animations = NULL;
        model->animation_count = 0;
        return 0;
    }

    int anim_count = (int)data->animations_count;
    if (anim_count > GLB_MAX_ANIMATIONS) anim_count = GLB_MAX_ANIMATIONS;

    model->animation_count = anim_count;
    model->animations = calloc(anim_count, sizeof(glb_animation_t));
    if (!model->animations) return -1;

    for (int a = 0; a < anim_count; a++) {
        cgltf_animation *anim = &data->animations[a];
        glb_animation_t *out = &model->animations[a];

        strncpy(out->name, anim->name ? anim->name : "anim", 31);
        out->name[31] = '\0';

        int ch_count = (int)anim->channels_count;
        if (ch_count > GLB_MAX_CHANNELS) ch_count = GLB_MAX_CHANNELS;

        out->channel_count = ch_count;
        out->channels = calloc(ch_count, sizeof(glb_anim_channel_t));
        if (!out->channels) return -1;

        out->duration = 0;

        for (int c = 0; c < ch_count; c++) {
            cgltf_animation_channel *ch = &anim->channels[c];
            glb_anim_channel_t *och = &out->channels[c];

            /* Target joint */
            och->target_joint = -1;
            if (ch->target_node && model->node_to_joint) {
                int node_idx = (int)(ch->target_node - data->nodes);
                if (node_idx >= 0 && node_idx < model->node_count)
                    och->target_joint = model->node_to_joint[node_idx];
            }

            /* Path type */
            switch (ch->target_path) {
                case cgltf_animation_path_type_translation:
                    och->path = GLB_PATH_TRANSLATION; break;
                case cgltf_animation_path_type_rotation:
                    och->path = GLB_PATH_ROTATION; break;
                case cgltf_animation_path_type_scale:
                    och->path = GLB_PATH_SCALE; break;
                default:
                    och->path = GLB_PATH_TRANSLATION; break;
            }

            /* Interpolation */
            if (ch->sampler) {
                switch (ch->sampler->interpolation) {
                    case cgltf_interpolation_type_step:
                        och->interpolation = GLB_INTERP_STEP; break;
                    case cgltf_interpolation_type_linear:
                        och->interpolation = GLB_INTERP_LINEAR; break;
                    case cgltf_interpolation_type_cubic_spline:
                        och->interpolation = GLB_INTERP_CUBICSPLINE; break;
                    default:
                        och->interpolation = GLB_INTERP_LINEAR; break;
                }
            }

            /* Keyframe data */
            if (ch->sampler && ch->sampler->input && ch->sampler->output) {
                int kf_count = (int)ch->sampler->input->count;
                if (kf_count > GLB_MAX_KEYFRAMES)
                    kf_count = GLB_MAX_KEYFRAMES;

                och->keyframe_count = kf_count;
                och->times = malloc(kf_count * sizeof(float));
                if (!och->times) return -1;

                int value_components;
                if (och->path == GLB_PATH_ROTATION)
                    value_components = 4;
                else
                    value_components = 3;

                och->values = malloc(kf_count * value_components * sizeof(float));
                if (!och->values) return -1;

                for (int k = 0; k < kf_count; k++) {
                    cgltf_accessor_read_float(ch->sampler->input, k,
                                             &och->times[k], 1);
                    cgltf_accessor_read_float(ch->sampler->output, k,
                                             &och->values[k * value_components],
                                             value_components);
                }

                /* Track max duration */
                if (och->times[kf_count - 1] > out->duration)
                    out->duration = och->times[kf_count - 1];
            }
        }

        printf("GLB: Loaded animation '%s': %d channels, %.2fs\n",
               out->name, out->channel_count, out->duration);
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */
int glb_load(const char *path, glb_model_t *model)
{
    memset(model, 0, sizeof(glb_model_t));

    printf("GLB: Loading %s...\n", path);

    cgltf_options options = {0};
    cgltf_data *data = NULL;
    cgltf_result result;

    result = cgltf_parse_file(&options, path, &data);
    if (result != cgltf_result_success) {
        printf("GLB: Failed to parse %s (error %d)\n", path, (int)result);
        return -1;
    }

    result = cgltf_load_buffers(&options, data, path);
    if (result != cgltf_result_success) {
        printf("GLB: Failed to load buffers (error %d)\n", (int)result);
        cgltf_free(data);
        return -1;
    }

    /* Validate */
    result = cgltf_validate(data);
    if (result != cgltf_result_success) {
        printf("GLB: Validation warning (error %d), continuing...\n", (int)result);
    }

    printf("GLB: Parsed - %d meshes, %d images, %d skins, %d animations\n",
           (int)data->meshes_count, (int)data->images_count,
           (int)data->skins_count, (int)data->animations_count);

    /* Load skeleton first (needed for mesh joint mapping) */
    if (load_skeleton(data, model) < 0) {
        printf("GLB: Failed to load skeleton\n");
        cgltf_free(data);
        glb_free(model);
        return -1;
    }

    /* Load textures */
    if (load_textures(data, model) < 0) {
        printf("GLB: Failed to load textures\n");
        cgltf_free(data);
        glb_free(model);
        return -1;
    }

    /* Load meshes */
    if (load_meshes(data, model) < 0) {
        printf("GLB: Failed to load meshes\n");
        cgltf_free(data);
        glb_free(model);
        return -1;
    }

    /* Load animations */
    if (load_animations(data, model) < 0) {
        printf("GLB: Failed to load animations\n");
        cgltf_free(data);
        glb_free(model);
        return -1;
    }

    cgltf_free(data);
    printf("GLB: Load complete\n");
    return 0;
}

void glb_free(glb_model_t *model)
{
    if (model->meshes) {
        for (int i = 0; i < model->mesh_count; i++) {
            free(model->meshes[i].vertices);
            free(model->meshes[i].indices);
        }
        free(model->meshes);
    }

    if (model->textures) {
        for (int i = 0; i < model->texture_count; i++) {
            if (model->textures[i].pvr_mem)
                pvr_mem_free(model->textures[i].pvr_mem);
        }
        free(model->textures);
    }

    free(model->joints);

    if (model->animations) {
        for (int i = 0; i < model->animation_count; i++) {
            if (model->animations[i].channels) {
                for (int c = 0; c < model->animations[i].channel_count; c++) {
                    free(model->animations[i].channels[c].times);
                    free(model->animations[i].channels[c].values);
                }
                free(model->animations[i].channels);
            }
        }
        free(model->animations);
    }

    free(model->node_to_joint);
    memset(model, 0, sizeof(glb_model_t));
}
