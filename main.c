/* Dreamcast Demo - Hello Dreamcast!
 *
 * Demonstrates basic PVR graphics, controller input, and color cycling.
 * Press A to cycle through background colors.
 * Press Start to exit.
 */

#include <kos.h>
#include <dc/pvr.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>

/* KOS declaration for romdisk (not used here, but required by the linker) */
extern uint8 romdisk[];
KOS_INIT_ROMDISK(romdisk);

/* -------------------------------------------------------------------------
 * Color table that the A button cycles through
 * Values are PVR packed-color ARGB (alpha=1.0 implied by clear color)
 * ------------------------------------------------------------------------- */
typedef struct {
    const char *name;
    float r, g, b;
} bg_color_t;

static const bg_color_t colors[] = {
    { "Black",  0.0f, 0.0f, 0.0f },
    { "Blue",   0.0f, 0.0f, 0.8f },
    { "Red",    0.8f, 0.0f, 0.0f },
    { "Green",  0.0f, 0.8f, 0.0f },
    { "Purple", 0.5f, 0.0f, 0.5f },
    { "Cyan",   0.0f, 0.8f, 0.8f },
};
#define NUM_COLORS (sizeof(colors) / sizeof(colors[0]))

/* -------------------------------------------------------------------------
 * Draw a simple colored rectangle that fills the screen background.
 * We submit one opaque vertex polygon to the PVR.
 * ------------------------------------------------------------------------- */
static void draw_background(float r, float g, float b)
{
    pvr_vertex_t vert;
    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;

    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    pvr_poly_compile(&hdr, &cxt);
    pvr_prim(&hdr, sizeof(hdr));

    vert.argb  = PVR_PACK_COLOR(1.0f, r, g, b);
    vert.oargb = 0;
    vert.flags = PVR_CMD_VERTEX;
    vert.z     = 1.0f;

    vert.x = 0.0f;   vert.y = 480.0f; pvr_prim(&vert, sizeof(vert));
    vert.x = 0.0f;   vert.y = 0.0f;   pvr_prim(&vert, sizeof(vert));
    vert.x = 640.0f; vert.y = 480.0f; pvr_prim(&vert, sizeof(vert));
    vert.flags = PVR_CMD_VERTEX_EOL;
    vert.x = 640.0f; vert.y = 0.0f;   pvr_prim(&vert, sizeof(vert));
}

/* -------------------------------------------------------------------------
 * Draw "Hello Dreamcast!" as a white banner using bfont
 * ------------------------------------------------------------------------- */
static void draw_text(void)
{
    bfont_set_encoding(BFONT_CODE_ISO8859_1);
    /* vram_s offset: row 200 * stride 640 + column 160 (centers text roughly) */
    bfont_draw_str(vram_s + (200 * 640 + 160), 640, 0, "Hello Dreamcast!");
}

/* -------------------------------------------------------------------------
 * Main entry point
 * ------------------------------------------------------------------------- */
int main(void)
{
    maple_device_t  *cont_dev;
    cont_state_t    *state;
    uint32_t         prev_buttons = 0;
    int              color_index  = 0;   /* start with black */
    int              done         = 0;

    /* -- Video / PVR init ------------------------------------------------- */
    vid_set_mode(DM_640x480, PM_RGB565);

    pvr_init_defaults();

    /* -- Main loop -------------------------------------------------------- */
    while (!done) {
        /* Controller polling */
        cont_dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
        if (cont_dev) {
            state = (cont_state_t *)maple_dev_status(cont_dev);
            if (state) {
                uint32_t buttons = state->buttons;
                uint32_t newly_pressed = buttons & ~prev_buttons;

                /* A button: cycle to next color */
                if (newly_pressed & CONT_A) {
                    color_index = (color_index + 1) % NUM_COLORS;
                }

                /* Start button: quit */
                if (newly_pressed & CONT_START) {
                    done = 1;
                }

                prev_buttons = buttons;
            }
        }

        /* -- Rendering ---------------------------------------------------- */
        pvr_wait_ready();
        pvr_scene_begin();
        pvr_list_begin(PVR_LIST_OP_POLY);

        draw_background(colors[color_index].r,
                        colors[color_index].g,
                        colors[color_index].b);

        pvr_list_finish();
        pvr_scene_finish();

        /* Draw text directly to VRAM after PVR scene is complete */
        draw_text();
    }

    return 0;
}
