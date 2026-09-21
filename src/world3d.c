#ifndef __PS2__
#include "world3d_impl.inc"
#else
#define world3d_init_global world3d_init_global_desktop
#define world3d_init world3d_init_desktop
#define world3d_draw world3d_draw_desktop
#define world3d_add_occluder world3d_add_occluder_desktop
#define world3d_update_activeoccluders world3d_update_activeoccluders_desktop
#define world3d_clear_temporarylocs world3d_clear_temporarylocs_desktop
#include "world3d_impl.inc"
#undef world3d_clear_temporarylocs
#undef world3d_update_activeoccluders
#undef world3d_add_occluder
#undef world3d_draw
#undef world3d_init
#undef world3d_init_global

// The PS2 client already exposes its one lifetime Client instance for the crash/checkpoint path.
// Reuse that pointer here so the local player can be admitted into World3D without reopening the
// huge client draw path: the normal PS2 pushPlayers() still skips LOCAL_PLAYER_INDEX, while this
// renderer-local bridge inserts that one actor before tile traversal/sorting.
#include "client.h"

// Keep a single 51x51 PS2 visibility map instead of the desktop 8x32x51x51 table.
// Rebuild this tiny map from camera yaw each rendered frame: the old PS2 square admitted
// many tiles behind/off-screen, so a camera-facing wedge can extend the forward horizon
// without increasing the expensive World3D/model workload.
static bool ps2_visibility_map[51][51];

__attribute__((section(".ps2_runtime_text"), noinline))
static int ps2_runtime_render_radius(void) {
    int radius = CONTROLLER_RENDER_RADIUS_DEFAULT;
    Client *c = ps2_crash_client;
    if (c) {
        radius = c->controller_render_radius;
    }
    if (radius < CONTROLLER_RENDER_RADIUS_MIN) radius = CONTROLLER_RENDER_RADIUS_MIN;
    if (radius > PS2_RENDER_RADIUS) radius = PS2_RENDER_RADIUS;
    return radius;
}

__attribute__((section(".ps2_runtime_text"), noinline))
static void ps2_update_visibility_map(int sinEyeYaw, int cosEyeYaw, int drawRadius) {
    const int backMargin = PS2_RENDER_BACK_MARGIN << 16;
    const int sideMargin = PS2_RENDER_SIDE_MARGIN << 16;

    for (int x = 0; x < 51; x++) {
        int dx = x - 25;
        for (int z = 0; z < 51; z++) {
            int dz = z - 25;
            if (abs(dx) > drawRadius || abs(dz) > drawRadius) {
                ps2_visibility_map[x][z] = false;
                continue;
            }

            // Match World3D's yaw transform. "forward" is camera-space depth and "side"
            // is camera-space horizontal distance, both still scaled by the 16.16 trig table.
            int forward = dz * cosEyeYaw - dx * sinEyeYaw;
            int side = dz * sinEyeYaw + dx * cosEyeYaw;

            // The software projection is narrower than this cone; the extra fixed margin keeps
            // large/near tile geometry from popping at the screen edges while still rejecting
            // the large square area that is behind or far to either side of the camera.
            int maxSide = forward / 2 + sideMargin;
            ps2_visibility_map[x][z] =
                forward >= -backMargin && maxSide >= 0 && abs(side) <= maxSide;
        }
    }
}

static bool ps2_ground_is_empty(const Ground *tile) {
    return tile && tile->locCount == 0 && !tile->underlay && !tile->overlay &&
           !tile->wall && !tile->decor && !tile->groundDecor &&
           !tile->groundObj && !tile->bridge;
}

static bool ps2_ground_is_scene_arena_tile(int x, int z) {
    return x >= PS2_TERRAIN_MIN_TILE && x < PS2_TERRAIN_MAX_X_TILE &&
           z >= PS2_TERRAIN_MIN_TILE && z < PS2_TERRAIN_MAX_Z_TILE;
}

static void ps2_submit_local_player(World3D *world3d, int loopCycle) {
#if !PS2_RENDER_LOCAL_PLAYER
    Client *c = ps2_crash_client;
    if (!c || c->scene != world3d) {
        return;
    }

    PlayerEntity *player = c->local_player;
    if (!player || !playerentity_is_visible(player)) {
        return;
    }

    int stx = player->pathing_entity.x >> 7;
    int stz = player->pathing_entity.z >> 7;
    if (stx < 0 || stx >= world3d->maxTileX || stz < 0 || stz >= world3d->maxTileZ) {
        return;
    }

    // The old post-World3D shortcut forced lowmem here, which returned the cached base model before
    // walk/run/action sequence transforms. The real local-player path is never lowmem on desktop;
    // preserve that rule so entity_draw() builds the current animated pose when the tile is drawn.
    player->lowmem = false;
    player->y = getHeightmapY(c, c->currentLevel,
                              player->pathing_entity.x, player->pathing_entity.z);

    int bitset = LOCAL_PLAYER_INDEX << 14;
    if (!player->locModel || loopCycle < player->locStartCycle ||
        loopCycle >= player->locStopCycle) {
        world3d_add_temporary(world3d, c->currentLevel,
                              player->pathing_entity.x, player->y, player->pathing_entity.z,
                              NULL, &player->pathing_entity.entity, bitset,
                              player->pathing_entity.yaw, 60,
                              player->pathing_entity.seqStretches);
    } else {
        world3d_add_temporary2(world3d, c->currentLevel,
                               player->pathing_entity.x, player->y, player->pathing_entity.z,
                               player->minTileX, player->minTileZ,
                               player->maxTileX, player->maxTileZ,
                               NULL, &player->pathing_entity.entity, bitset,
                               player->pathing_entity.yaw);
    }
#else
    (void)world3d;
    (void)loopCycle;
#endif
}

void world3d_init_global(void) {
    _World3D.clickTileX = -1;
    _World3D.clickTileZ = -1;
    _World3D.drawTileQueue = linklist_new();
    for (int x = 0; x < 51; x++) {
        for (int z = 0; z < 51; z++) {
            ps2_visibility_map[x][z] = false;
        }
    }
    _World3D.visibilityMatrix = NULL;
    _World3D.visibilityMap = ps2_visibility_map;
}

void world3d_init(int viewportWidth, int viewportHeight, int frustumStart, int frustumEnd, int *pitchDistance) {
    _World3D.viewportLeft = 0;
    _World3D.viewportTop = 0;
    _World3D.viewportRight = viewportWidth;
    _World3D.viewportBottom = viewportHeight;
    _World3D.viewportCenterX = viewportWidth / 2;
    _World3D.viewportCenterY = viewportHeight / 2;
    (void)frustumStart;
    (void)frustumEnd;
    (void)pitchDistance;
}

// With the bounded PS2 draw radius, the desktop occluder graph costs heap and
// update time for little benefit. Keep geometry/collision intact and simply
// render the already tightly bounded nearby scene without world occluders.
void world3d_add_occluder(int level, int type, int minX, int minY, int minZ, int maxX, int maxY, int maxZ) {
    (void)level;
    (void)type;
    (void)minX;
    (void)minY;
    (void)minZ;
    (void)maxX;
    (void)maxY;
    (void)maxZ;
}

void world3d_update_activeoccluders(void) {
    _World3D.activeOccluderCount = 0;
}

// Dynamic players/NPCs/projectiles are inserted as temporary Locations every
// frame. world3d_add_loc2() creates missing Ground nodes for those locations,
// but the desktop clear path only removes/frees the Location. In a deliberately
// sparse PS2 scene that means actors walking across previously empty tiles leave
// permanent ~Ground-sized breadcrumbs behind. Reclaim any now-empty heap-backed
// scaffolding after removing each temporary location. Ground nodes inside the
// resident terrain window are scene-arena allocations: ground_free() deliberately
// cannot reclaim those individually, so clearing their levelTiles pointer would
// lose the only reusable reference and make the same tile consume another arena
// allocation on the next frame. Keep empty arena-backed nodes linked for reuse;
// their population is strictly bounded by the resident terrain window.
void world3d_clear_temporarylocs(World3D *world3d) {
    for (int i = 0; i < world3d->temporaryLocCount; i++) {
        Location *loc = world3d->temporaryLocs[i];
        if (!loc) {
            continue;
        }

        int level = loc->level;
        int minX = loc->minSceneTileX;
        int maxX = loc->maxSceneTileX;
        int minZ = loc->minSceneTileZ;
        int maxZ = loc->maxSceneTileZ;

        world3d_remove_loc2(world3d, loc);
        world3d->temporaryLocs[i] = NULL;
        free(loc);

        if (minX < 0) minX = 0;
        if (minZ < 0) minZ = 0;
        if (maxX >= world3d->maxTileX) maxX = world3d->maxTileX - 1;
        if (maxZ >= world3d->maxTileZ) maxZ = world3d->maxTileZ - 1;
        if (level >= world3d->maxLevel) level = world3d->maxLevel - 1;

        for (int l = level; l >= 0; l--) {
            for (int x = minX; x <= maxX; x++) {
                for (int z = minZ; z <= maxZ; z++) {
                    Ground *tile = world3d->levelTiles[l][x][z];
                    if (ps2_ground_is_empty(tile)) {
                        if (ps2_ground_is_scene_arena_tile(x, z)) {
                            continue;
                        }
                        ground_free(tile);
                        world3d->levelTiles[l][x][z] = NULL;
                    }
                }
            }
        }
    }

    world3d->temporaryLocCount = 0;
}

// The PS2 draw path is the existing bounded renderer with only the visibility
// map selection changed. Gameplay state/collision remains full-world.
void world3d_draw(World3D *world3d, int eyeX, int eyeY, int eyeZ, int topLevel, int eyeYaw, int eyePitch, int loopCycle) {
    if (eyeX < 0) {
        eyeX = 0;
    } else if (eyeX >= world3d->maxTileX * 128) {
        eyeX = world3d->maxTileX * 128 - 1;
    }
    if (eyeZ < 0) {
        eyeZ = 0;
    } else if (eyeZ >= world3d->maxTileZ * 128) {
        eyeZ = world3d->maxTileZ * 128 - 1;
    }

    // Admit the local player before visibility marking and tile traversal. This makes it participate
    // in the same Location ordering as walls/trees/buildings instead of being painted as a final
    // overlay after the whole scene, while leaving the rest of the PS2 entity caps unchanged.
    ps2_submit_local_player(world3d, loopCycle);

    _World3D.cycle++;
    _World3D.sinEyePitch = _Pix3D.sin_table[eyePitch];
    _World3D.cosEyePitch = _Pix3D.cos_table[eyePitch];
    _World3D.sinEyeYaw = _Pix3D.sin_table[eyeYaw];
    _World3D.cosEyeYaw = _Pix3D.cos_table[eyeYaw];
    const int drawRadius = ps2_runtime_render_radius();
    ps2_update_visibility_map(_World3D.sinEyeYaw, _World3D.cosEyeYaw, drawRadius);
    ps2_inactive_loc_draw_count = 0;
    _World3D.visibilityMap = ps2_visibility_map;
    _World3D.eyeX = eyeX;
    _World3D.eyeY = eyeY;
    _World3D.eyeZ = eyeZ;
    _World3D.eyeTileX = eyeX / 128;
    _World3D.eyeTileZ = eyeZ / 128;
    _World3D.topLevel = topLevel;

    _World3D.minDrawTileX = _World3D.eyeTileX - drawRadius;
    if (_World3D.minDrawTileX < 0) _World3D.minDrawTileX = 0;
    _World3D.minDrawTileZ = _World3D.eyeTileZ - drawRadius;
    if (_World3D.minDrawTileZ < 0) _World3D.minDrawTileZ = 0;
    _World3D.maxDrawTileX = _World3D.eyeTileX + drawRadius;
    if (_World3D.maxDrawTileX > world3d->maxTileX) _World3D.maxDrawTileX = world3d->maxTileX;
    _World3D.maxDrawTileZ = _World3D.eyeTileZ + drawRadius;
    if (_World3D.maxDrawTileZ > world3d->maxTileZ) _World3D.maxDrawTileZ = world3d->maxTileZ;

    world3d_update_activeoccluders();
    _World3D.tilesRemaining = 0;

    for (int level = world3d->minLevel; level < world3d->maxLevel; level++) {
        Ground ***tiles = world3d->levelTiles[level];
        for (int x = _World3D.minDrawTileX; x < _World3D.maxDrawTileX; x++) {
            for (int z = _World3D.minDrawTileZ; z < _World3D.maxDrawTileZ; z++) {
                Ground *tile = tiles[x][z];
                if (!tile) continue;
                if (tile->drawLevel <= topLevel &&
                    ps2_visibility_map[x + 25 - _World3D.eyeTileX][z + 25 - _World3D.eyeTileZ]) {
                    tile->visible = true;
                    tile->update = true;
                    tile->containsLocs = tile->locCount > 0;
                    _World3D.tilesRemaining++;
                } else {
                    tile->visible = false;
                    tile->update = false;
                    tile->checkLocSpans = 0;
                }
            }
        }
    }

    for (int level = world3d->minLevel; level < world3d->maxLevel; level++) {
        Ground ***tiles = world3d->levelTiles[level];
        for (int dx = -drawRadius; dx <= 0; dx++) {
            int rightTileX = _World3D.eyeTileX + dx;
            int leftTileX = _World3D.eyeTileX - dx;
            if (rightTileX < _World3D.minDrawTileX && leftTileX >= _World3D.maxDrawTileX) continue;
            for (int dz = -drawRadius; dz <= 0; dz++) {
                int forwardTileZ = _World3D.eyeTileZ + dz;
                int backwardTileZ = _World3D.eyeTileZ - dz;
                Ground *tile;
                if (rightTileX >= _World3D.minDrawTileX) {
                    if (forwardTileZ >= _World3D.minDrawTileZ) {
                        tile = tiles[rightTileX][forwardTileZ];
                        if (tile && tile->visible) world3d_draw_tile(world3d, tile, true, loopCycle);
                    }
                    if (backwardTileZ < _World3D.maxDrawTileZ) {
                        tile = tiles[rightTileX][backwardTileZ];
                        if (tile && tile->visible) world3d_draw_tile(world3d, tile, true, loopCycle);
                    }
                }
                if (leftTileX < _World3D.maxDrawTileX) {
                    if (forwardTileZ >= _World3D.minDrawTileZ) {
                        tile = tiles[leftTileX][forwardTileZ];
                        if (tile && tile->visible) world3d_draw_tile(world3d, tile, true, loopCycle);
                    }
                    if (backwardTileZ < _World3D.maxDrawTileZ) {
                        tile = tiles[leftTileX][backwardTileZ];
                        if (tile && tile->visible) world3d_draw_tile(world3d, tile, true, loopCycle);
                    }
                }
                if (_World3D.tilesRemaining == 0) {
                    _World3D.takingInput = false;
                    return;
                }
            }
        }
    }

    for (int level = world3d->minLevel; level < world3d->maxLevel; level++) {
        Ground ***tiles = world3d->levelTiles[level];
        for (int dx = -drawRadius; dx <= 0; dx++) {
            int rightTileX = _World3D.eyeTileX + dx;
            int leftTileX = _World3D.eyeTileX - dx;
            if (rightTileX < _World3D.minDrawTileX && leftTileX >= _World3D.maxDrawTileX) continue;
            for (int dz = -drawRadius; dz <= 0; dz++) {
                int forwardTileZ = _World3D.eyeTileZ + dz;
                int backgroundTileZ = _World3D.eyeTileZ - dz;
                Ground *tile;
                if (rightTileX >= _World3D.minDrawTileX) {
                    if (forwardTileZ >= _World3D.minDrawTileZ) {
                        tile = tiles[rightTileX][forwardTileZ];
                        if (tile && tile->visible) world3d_draw_tile(world3d, tile, false, loopCycle);
                    }
                    if (backgroundTileZ < _World3D.maxDrawTileZ) {
                        tile = tiles[rightTileX][backgroundTileZ];
                        if (tile && tile->visible) world3d_draw_tile(world3d, tile, false, loopCycle);
                    }
                }
                if (leftTileX < _World3D.maxDrawTileX) {
                    if (forwardTileZ >= _World3D.minDrawTileZ) {
                        tile = tiles[leftTileX][forwardTileZ];
                        if (tile && tile->visible) world3d_draw_tile(world3d, tile, false, loopCycle);
                    }
                    if (backgroundTileZ < _World3D.maxDrawTileZ) {
                        tile = tiles[leftTileX][backgroundTileZ];
                        if (tile && tile->visible) world3d_draw_tile(world3d, tile, false, loopCycle);
                    }
                }
                if (_World3D.tilesRemaining == 0) {
                    _World3D.takingInput = false;
                    return;
                }
            }
        }
    }
}
#endif