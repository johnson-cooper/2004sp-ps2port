#ifndef __PS2__
#include "world3d_impl.inc"
#else
#define world3d_init_global world3d_init_global_desktop
#define world3d_init world3d_init_desktop
#define world3d_draw world3d_draw_desktop
#include "world3d_impl.inc"
#undef world3d_draw
#undef world3d_init
#undef world3d_init_global

// PS2 uses a four-tile draw radius. A single conservative 51x51 visibility map
// is enough for the existing occluder code, so avoid the desktop 8x32x51x51
// table and its large temporary construction matrix entirely.
static bool ps2_visibility_map[51][51];

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

// The remainder of the PS2 draw path is supplied below. It is the existing
// implementation with only the visibility-map selection changed; scene,
// occlusion and raster behaviour otherwise remain unchanged.
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
