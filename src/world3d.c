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

// PS2 uses a four-tile draw radius. A single conservative 51x51 visibility map
// is enough for the bounded PS2 scene, avoiding the desktop 8x32x51x51 table
// and its large temporary construction matrix.
static bool ps2_visibility_map[51][51];

static bool ps2_ground_is_empty(const Ground *tile) {
    return tile && tile->locCount == 0 && !tile->underlay && !tile->overlay &&
           !tile->wall && !tile->decor && !tile->groundDecor &&
           !tile->groundObj && !tile->bridge;
}

static bool ps2_ground_is_scene_arena_tile(int x, int z) {
    return x >= PS2_TERRAIN_MIN_TILE && x < PS2_TERRAIN_MAX_X_TILE &&
           z >= PS2_TERRAIN_MIN_TILE && z < PS2_TERRAIN_MAX_Z_TILE;
}

void world3d_init_global(void) {
    _World3D.clickTileX = -1;
    _World3D.clickTileZ = -1;
    _World3D.drawTileQueue = linklist_new();
    for (int x = 0; x < 51; x++) {
        for (int z = 0; z < 51; z++) {
            ps2_visibility_map[x][z] =
                abs(x - 25) <= PS2_RENDER_RADIUS && abs(z - 25) <= PS2_RENDER_RADIUS;
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

// With a four-tile PS2 draw radius, the desktop occluder graph costs heap and
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
// their population is strictly bounded by the tiny resident terrain window.
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

    _World3D.cycle++;
    _World3D.sinEyePitch = _Pix3D.sin_table[eyePitch];
    _World3D.cosEyePitch = _Pix3D.cos_table[eyePitch];
    _World3D.sinEyeYaw = _Pix3D.sin_table[eyeYaw];
    _World3D.cosEyeYaw = _Pix3D.cos_table[eyeYaw];
    _World3D.visibilityMap = ps2_visibility_map;
    _World3D.eyeX = eyeX;
    _World3D.eyeY = eyeY;
    _World3D.eyeZ = eyeZ;
    _World3D.eyeTileX = eyeX / 128;
    _World3D.eyeTileZ = eyeZ / 128;
    _World3D.topLevel = topLevel;

    const int drawRadius = PS2_RENDER_RADIUS;
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
                    (ps2_visibility_map[x + 25 - _World3D.eyeTileX][z + 25 - _World3D.eyeTileZ] ||
                     world3d->levelHeightmaps[level][x][z] - eyeY >= 2000)) {
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
