#pragma once

// world3d.c includes gl11.h before client.h. Mark that transitive client.h include so the PS2
// client-only lazy world3d_set_minlevel alias in client.h does not rewrite world3d.c's real function
// definition. client.c includes client.h directly first, so its scene-build calls still use the lazy
// PS2 path. This avoids touching the large client/world3d translation units just to change one
// platform policy.
#ifdef __PS2__
#define RS2_CLIENT_INCLUDED_FROM_GL11 1
#endif
#include "client.h"
#ifdef __PS2__
#undef RS2_CLIENT_INCLUDED_FROM_GL11
#endif
#include "model.h"

typedef struct {
    float uA, uB, uC;
    float vA, vB, vC;
} UV;

void gl_load(void);
void gl_start_frame(void);
void gl_end_frame(void);
void gl_start_drawscene(void);
void gl_end_drawscene(Client *c);
void gl_set_brightness(void);
void gl_gouraud_triangle(int xA, int xB, int xC, int yA, int yB, int yC, int zA, int zB, int zC, int colorA, int colorB, int colorC, int alpha);
void gl_texture_triangle(int xA, int xB, int xC, int yA, int yB, int yC, int zA, int zB, int zC, int shadeA, int shadeB, int shadeC, UV uv, int texture);
void gl_start_model(Model *model, int sceneX, int sceneY, int sceneZ, int yaw);
