#ifdef __PS2__

#undef client
#include <gsKit.h>

#include <stdint.h>
#include <stdlib.h>

#include "defines.h"
#include "pix3d.h"
#include "ps2_gs_raster.h"

#define PS2_GS_RUNTIME_CODE __attribute__((section(".ps2_runtime_text"), noinline))
#define PS2_GS_RUNTIME_DATA __attribute__((section(".ps2_runtime_data"), used, aligned(16)))

#define PS2_GS_TRI_CAPACITY 4096
#define PS2_GS_STATE_MAGIC 0x47535231u

typedef struct {
    int16_t x1, y1;
    int16_t x2, y2;
    int16_t x3, y3;
    uint8_t kind;
    uint8_t alpha;
    uint16_t reserved;
    uint32_t color1;
    uint32_t color2;
    uint32_t color3;
} Ps2GsTriangle;

typedef struct {
    uint32_t magic;
    Ps2GsTriangle *triangles;
    int count;
    int overflow;
} Ps2GsRasterState;

static Ps2GsRasterState ps2_gs_state PS2_GS_RUNTIME_DATA = {
    PS2_GS_STATE_MAGIC, NULL, 0, 0
};

extern Pix3D _Pix3D;

PS2_GS_RUNTIME_CODE
static int16_t ps2_gs_coord(int value) {
    if (value < -8192) return -8192;
    if (value > 8191) return 8191;
    return (int16_t)value;
}

PS2_GS_RUNTIME_CODE
static uint32_t ps2_gs_palette_rgb(int color) {
    if (color < 0) color = 0;
    if (color > 65535) color = 65535;
    return (uint32_t)_Pix3D.palette[color] & 0x00ffffffU;
}

PS2_GS_RUNTIME_CODE
static bool ps2_gs_ensure_queue(void) {
    if (ps2_gs_state.magic != PS2_GS_STATE_MAGIC) {
        ps2_gs_state.magic = PS2_GS_STATE_MAGIC;
        ps2_gs_state.triangles = NULL;
        ps2_gs_state.count = 0;
        ps2_gs_state.overflow = 0;
    }
    if (!ps2_gs_state.triangles) {
        ps2_gs_state.triangles = malloc(sizeof(Ps2GsTriangle) * PS2_GS_TRI_CAPACITY);
        if (!ps2_gs_state.triangles) {
            return false;
        }
    }
    return true;
}

PS2_GS_RUNTIME_CODE
static bool ps2_gs_push(int x1, int y1, int x2, int y2, int x3, int y3,
                        uint32_t color1, uint32_t color2, uint32_t color3,
                        int alpha, int kind) {
    int area = (x2 - x1) * (y3 - y1) - (y2 - y1) * (x3 - x1);
    if (area == 0) {
        return true;
    }
    if (!ps2_gs_ensure_queue()) {
        return false;
    }
    if (ps2_gs_state.count >= PS2_GS_TRI_CAPACITY) {
        ps2_gs_state.overflow++;
        return false;
    }

    Ps2GsTriangle *tri = &ps2_gs_state.triangles[ps2_gs_state.count++];
    tri->x1 = ps2_gs_coord(x1);
    tri->y1 = ps2_gs_coord(y1);
    tri->x2 = ps2_gs_coord(x2);
    tri->y2 = ps2_gs_coord(y2);
    tri->x3 = ps2_gs_coord(x3);
    tri->y3 = ps2_gs_coord(y3);
    tri->kind = (uint8_t)kind;
    if (alpha < 0) alpha = 0;
    if (alpha > 255) alpha = 255;
    tri->alpha = (uint8_t)alpha;
    tri->reserved = 0;
    tri->color1 = color1 & 0x00ffffffU;
    tri->color2 = color2 & 0x00ffffffU;
    tri->color3 = color3 & 0x00ffffffU;
    return true;
}

PS2_GS_RUNTIME_CODE
bool ps2_gs_raster_queue_flat(int x1, int y1, int x2, int y2, int x3, int y3, int rgb, int alpha) {
    uint32_t color = (uint32_t)rgb & 0x00ffffffU;
    return ps2_gs_push(x1, y1, x2, y2, x3, y3, color, color, color, alpha, 0);
}

PS2_GS_RUNTIME_CODE
bool ps2_gs_raster_queue_gouraud(int x1, int y1, int x2, int y2, int x3, int y3,
                                 int color1, int color2, int color3, int alpha) {
    return ps2_gs_push(x1, y1, x2, y2, x3, y3,
                       ps2_gs_palette_rgb(color1), ps2_gs_palette_rgb(color2),
                       ps2_gs_palette_rgb(color3), alpha, 1);
}

PS2_GS_RUNTIME_CODE
bool ps2_gs_raster_has_pending(void) {
    return ps2_gs_state.magic == PS2_GS_STATE_MAGIC && ps2_gs_state.count > 0;
}

PS2_GS_RUNTIME_CODE
static u64 ps2_gs_color(uint32_t rgb, uint8_t rs_alpha) {
    // RuneScape alpha is inverse opacity: 0 = opaque, 255 = almost transparent.
    int opacity = 256 - (int)rs_alpha;
    int gs_alpha = (opacity * 0x80 + 127) >> 8;
    if (gs_alpha < 0) gs_alpha = 0;
    if (gs_alpha > 0x80) gs_alpha = 0x80;
    return GS_SETREG_RGBAQ((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff, gs_alpha, 0x00);
}

PS2_GS_RUNTIME_CODE
void ps2_gs_raster_flush(void *gs_global, float view_x, float view_y, float view_w, float view_h) {
    if (!gs_global || !ps2_gs_raster_has_pending() || view_w <= 0.0f || view_h <= 0.0f) {
        return;
    }

    GSGLOBAL *gs = (GSGLOBAL *)gs_global;
    const float scale_x = view_w / (float)PS2_3D_RENDER_WIDTH;
    const float scale_y = view_h / (float)PS2_3D_RENDER_HEIGHT;
    const float vx1 = view_x + view_w;
    const float vy1 = view_y + view_h;

    int sx0 = (int)view_x;
    int sy0 = (int)view_y;
    int sx1 = (int)vx1 - 1;
    int sy1 = (int)vy1 - 1;
    if (sx0 < 0) sx0 = 0;
    if (sy0 < 0) sy0 = 0;
    if (sx1 >= gs->Width) sx1 = gs->Width - 1;
    if (sy1 >= gs->Height) sy1 = gs->Height - 1;

    gsKit_set_scissor(gs, GS_SETREG_SCISSOR(sx0, sx1, sy0, sy1));
    gsKit_set_primalpha(gs, GS_BLEND_BACK2FRONT, 0);

    int saved_alpha_enable = gs->PrimAlphaEnable;
    gs->PrimAlphaEnable = GS_SETTING_OFF;

    // The software viewport uses this same colour as its key. Recreate the sky directly on the GS
    // before the ordered scene triangles, then let the keyed CPU/UI texture overlay it afterward.
    gsKit_prim_sprite(gs, view_x, view_y, vx1, vy1, 1,
                      GS_SETREG_RGBAQ((PS2_VIEWPORT_SKY_RGB >> 16) & 0xff,
                                     (PS2_VIEWPORT_SKY_RGB >> 8) & 0xff,
                                     PS2_VIEWPORT_SKY_RGB & 0xff, 0x80, 0x00));

    for (int i = 0; i < ps2_gs_state.count; i++) {
        Ps2GsTriangle *tri = &ps2_gs_state.triangles[i];

        float x1 = view_x + (float)tri->x1 * scale_x;
        float y1 = view_y + (float)tri->y1 * scale_y;
        float x2 = view_x + (float)tri->x2 * scale_x;
        float y2 = view_y + (float)tri->y2 * scale_y;
        float x3 = view_x + (float)tri->x3 * scale_x;
        float y3 = view_y + (float)tri->y3 * scale_y;

        gs->PrimAlphaEnable = tri->alpha ? GS_SETTING_ON : GS_SETTING_OFF;

        if (tri->kind) {
            gsKit_prim_triangle_gouraud_3d(gs,
                                           x1, y1, 2,
                                           x2, y2, 2,
                                           x3, y3, 2,
                                           ps2_gs_color(tri->color1, tri->alpha),
                                           ps2_gs_color(tri->color2, tri->alpha),
                                           ps2_gs_color(tri->color3, tri->alpha));
        } else {
            gsKit_prim_triangle_3d(gs,
                                   x1, y1, 2,
                                   x2, y2, 2,
                                   x3, y3, 2,
                                   ps2_gs_color(tri->color1, tri->alpha));
        }
    }

    gs->PrimAlphaEnable = saved_alpha_enable;
    gsKit_set_scissor(gs, GS_SCISSOR_RESET);

    ps2_gs_state.count = 0;
    ps2_gs_state.overflow = 0;
}

PS2_GS_RUNTIME_CODE
void ps2_gs_raster_discard(void) {
    if (ps2_gs_state.magic == PS2_GS_STATE_MAGIC) {
        ps2_gs_state.count = 0;
        ps2_gs_state.overflow = 0;
    }
}

PS2_GS_RUNTIME_CODE
void ps2_gs_raster_shutdown(void) {
    if (ps2_gs_state.magic != PS2_GS_STATE_MAGIC) {
        return;
    }
    free(ps2_gs_state.triangles);
    ps2_gs_state.triangles = NULL;
    ps2_gs_state.count = 0;
    ps2_gs_state.overflow = 0;
}

#endif
