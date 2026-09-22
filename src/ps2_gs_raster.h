#pragma once

#ifdef __PS2__

#include <stdbool.h>

bool ps2_gs_raster_queue_flat(int x1, int y1, int x2, int y2, int x3, int y3, int rgb, int alpha);
bool ps2_gs_raster_queue_gouraud(int x1, int y1, int x2, int y2, int x3, int y3,
                                 int color1, int color2, int color3, int alpha);
bool ps2_gs_raster_queue_textured(
    int x1, int y1, int x2, int y2, int x3, int y3,
    int shade1, int shade2, int shade3,
    int u, int u_stride, int u_step_y,
    int v, int v_stride, int v_step_y,
    int w, int w_stride, int w_step_y,
    int center_x, int center_y,
    int texture_id, int fallback_rgb, int alpha);
void ps2_gs_raster_invalidate_texture(int texture_id);
bool ps2_gs_raster_has_pending(void);
void ps2_gs_raster_flush(void *gs_global, float view_x, float view_y, float view_w, float view_h);
void ps2_gs_raster_discard(void);
void ps2_gs_raster_shutdown(void);

#endif
