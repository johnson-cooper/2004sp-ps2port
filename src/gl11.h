#pragma once

// entry/client.c includes client.h (and therefore clientstream.h) before it reaches gl11.h, while
// world3d.c reaches client.h transitively through gl11.h. Remember that distinction before the
// include so only client-side scene rebuild calls get the PS2 lazy-minlevel policy below; the real
// world3d_set_minlevel() definition in world3d.c must remain untouched.
#if defined(__PS2__) && defined(RS2_CLIENTSTREAM_H_INCLUDED)
#define RS2_GL11_CLIENT_ALREADY_INCLUDED 1
#endif

#ifdef __PS2__
#define RS2_CLIENT_INCLUDED_FROM_GL11 1
#endif
#include "client.h"
#ifdef __PS2__
#undef RS2_CLIENT_INCLUDED_FROM_GL11
#endif
#include "model.h"

#if defined(__PS2__) && defined(RS2_GL11_CLIENT_ALREADY_INCLUDED)
// The stock world3d_set_minlevel() eagerly replaces/allocates every Ground in a full 104x104 level.
// That defeats PS2_TERRAIN_MIN/MAX_TILE and creates >10k Ground nodes before the bounded terrain
// build even begins. On the PS2 client rebuild path, setting the level should be metadata-only;
// world3d_set_tile()/entity/loc insertion then materialise only tiles that are actually resident.
#define world3d_set_minlevel(world3d, level) ((world3d)->minLevel = (level))
#undef RS2_GL11_CLIENT_ALREADY_INCLUDED
#endif

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
