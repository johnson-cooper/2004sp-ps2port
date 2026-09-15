#pragma once

// entry/client.c includes client.h (and therefore clientstream.h) before it reaches gl11.h, while
// world3d.c reaches client.h transitively through gl11.h. Remember that distinction before the
// include so only client-side scene rebuild/submission calls get the PS2 policies below; the real
// World3D function definitions must remain untouched.
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
// build even begins. On the PS2 client rebuild path, setting the level is metadata-only;
// world3d_set_tile()/entity/loc insertion then materialise only tiles that are actually resident.
#define world3d_set_minlevel(world3d, level) ((world3d)->minLevel = (level))

// Dynamic entities remain authoritative in the network/update lists, but only nearby entities earn
// World3D residency. This stops a crowded town from allocating/submitting every visible protocol
// entity before the tiny software-render window ever gets a chance to cull it.
static inline bool ps2_dynamic_scene_admit(Client *c, int sceneX, int sceneZ, int bitset) {
    if (!c || !c->local_player) {
        return false;
    }

    int tileX = sceneX >> 7;
    int tileZ = sceneZ >> 7;
    int localX = c->local_player->pathing_entity.x >> 7;
    int localZ = c->local_player->pathing_entity.z >> 7;
    int dx = tileX - localX;
    int dz = tileZ - localZ;
    // Match PS2_RENDER_RADIUS: do not allocate an entity in a ring World3D will not traverse.
    if (dx < -PS2_RENDER_RADIUS || dx > PS2_RENDER_RADIUS || dz < -PS2_RENDER_RADIUS || dz > PS2_RENDER_RADIUS) {
        return false;
    }

    static int last_scene_cycle = -1;
    static int players_this_frame = 0;
    static int npcs_this_frame = 0;
    static int misc_this_frame = 0;
    if (last_scene_cycle != c->scene_cycle) {
        last_scene_cycle = c->scene_cycle;
        players_this_frame = 0;
        npcs_this_frame = 0;
        misc_this_frame = 0;
    }

    if (bitset == -1) {
        if (misc_this_frame >= 8) {
            return false;
        }
        misc_this_frame++;
        return true;
    }
    if ((bitset & 0x20000000) != 0) {
        if (npcs_this_frame >= 24) {
            return false;
        }
        npcs_this_frame++;
        return true;
    }
    if (players_this_frame >= 16) {
        return false;
    }
    players_this_frame++;
    return true;
}

// Parenthesising the underlying function name prevents these function-like macros from recursively
// expanding themselves. They only exist in entry/client.c's include context, not world3d.c.
#define world3d_add_temporary(world3d, level, x, y, z, model, entity, bitset, yaw, padding, forwardPadding) \
    (ps2_dynamic_scene_admit(c, (x), (z), (bitset)) ? \
         (world3d_add_temporary)((world3d), (level), (x), (y), (z), (model), (entity), (bitset), (yaw), (padding), (forwardPadding)) : false)

#define world3d_add_temporary2(world3d, level, x, y, z, minTileX, minTileZ, maxTileX, maxTileZ, model, entity, bitset, yaw) \
    (ps2_dynamic_scene_admit(c, (x), (z), (bitset)) ? \
         (world3d_add_temporary2)((world3d), (level), (x), (y), (z), (minTileX), (minTileZ), (maxTileX), (maxTileZ), (model), (entity), (bitset), (yaw)) : false)

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
