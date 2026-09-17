#pragma once

#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 260
#endif

// used for avoiding overdraw, scaling touch, and width is sometimes used for aligned access in vram too
#if defined(__WII__) || defined(_arch_dreamcast) || defined(NXDK) || defined(__PS2__)
#define SCREEN_FB_WIDTH 640
#define SCREEN_FB_HEIGHT 480
#elif defined(__vita__)
#define SCREEN_FB_WIDTH 960
#define SCREEN_FB_HEIGHT 544
#elif defined(__3DS__)
#define SCREEN_FB_WIDTH 320
#define SCREEN_FB_HEIGHT 240
#elif defined(__PSP__)
#define SCREEN_FB_WIDTH 480
#define SCREEN_FB_HEIGHT 272
#elif defined(__NDS__)
#define SCREEN_FB_WIDTH 256
#define SCREEN_FB_HEIGHT 192
#else
#define SCREEN_FB_WIDTH SCREEN_WIDTH
#define SCREEN_FB_HEIGHT SCREEN_HEIGHT
#endif

// rev254 "fixed" mode (config.ini's resizable=0) uses a 765x503 frame with a single backtop1 top
// panel - Client3 was hardcoded to the "resizable" mode's larger 789x532 frame (which needs a
// second backtop2 top panel to cover the extra width), confirmed against the reference client.
#define SCREEN_WIDTH 765
#define SCREEN_HEIGHT 503

#ifdef __vita__
#define SCREEN_CENTER_XOFF ((SCREEN_FB_WIDTH - SCREEN_WIDTH) / 2)
#endif

// (2048 / (2 * M_PI))
#define RADIANS_TO_RS 325.949

// arbitrary to fix -Wall possible overflow warnings
// NOTE maybe change the ones using half_str to use strncpy or double sizes but yolo
#define HALF_STR (CHAT_LENGTH / 2)
#define SIXTY_STR 60
#define DOUBLE_STR (CHAT_LENGTH * 2)
// arbitrary + null terminator
#define MAX_STR (CHAT_LENGTH + 1)

#define MAX_CHATS 50

// avoid doubles on consoles with only single precision floats or no fpu at all!
// TODO: this doesn't change all occurences of doubles into floats yet
// 2026-09-14: __PS2__ added per the original C client author - PS2's EE FPU is single-precision only
// (per the PS2 wiki), same constraint as PSP/NDS here. Previously this flag only gated two genuinely
// cold-path functions (pix3d_set_gamma, jrand); this session extended it to the real hot paths -
// model.c's per-face/per-vertex sqrt() calls and world.c's per-tile lighting sqrt() (up to 104*104
// times per level, 4 levels, every single scene build) - which is the far more likely explanation for
// this whole session's real-hardware-only "hangs" that never once reproduced on PCSX2/desktop (native
// double FPU there vs. software-emulated double math here): not a crash, just severe, compounding
// slowness from tens of thousands of software-emulated double-precision sqrt calls.
#if defined(__PSP__) || defined(__NDS__) || defined(__PS2__)
#define USE_FLOATS
#endif

#if defined(__PS2__)
// Gameplay-first 32 MiB profile. Prefer bounded geometry and entities over cosmetic fidelity.
#define MODEL_MAX_DEPTH 600
#define MODEL_DEPTH_FACE_COUNT 80
// Terrain textures are disabled below, so a single texel slot is enough for the remaining UI/model
// users and avoids the extra permanent pool that made the dense Lumbridge rebuild regress.
#define PIX3D_POOL_COUNT 1
#define DISABLE_FLAMES
// Radius 14 / 29x29 is hardware-good, but the former 32x32 terrain block left only ~1-2 tiles of
// margin at an edge. That is too little to walk far enough for the normal 8-tile REBUILD_NORMAL
// zone shift to arrive before the camera/picker reaches non-materialized terrain. Keep the proven
// draw radius and widen only bounded terrain to 48x48 (27..74). This gives roughly ten tiles of
// residency beyond the camera radius while staying far below the full 104x104 desktop scene.
#define PS2_RENDER_RADIUS 14
#define PS2_TERRAIN_MIN_TILE 27
#define PS2_TERRAIN_MAX_X_TILE 75
#define PS2_TERRAIN_MAX_Z_TILE 75
#define PS2_TERRAIN_MAX_TILE 75
// Keep native projection until the fixed <<9 projection is made resolution-aware. Rendering at a
// smaller surface without scaling projection was proven to clip almost the entire terrain scene.
#define PS2_3D_RENDER_WIDTH 512
#define PS2_3D_RENDER_HEIGHT 334
// Simulation/networking stay at 50 Hz. Present the expensive software 3D view at 10 Hz for now;
// gameplay remains responsive while we establish a stable memory/performance floor.
#define PS2_RENDER_DIVISOR 5
// Bounded static-world restoration is now hardware-good. client_build_scene() and
// world_load_locations() gate PS2 loc I/O and placement to the local 32x32 tile window (32..63),
// keeping the full desktop 104x104 loc scene out of the hardware profile. This terrain-only test
// deliberately leaves that loc window unchanged so any hardware regression is attributable to
// Ground/underlay/overlay residency rather than extra location models.
#define PS2_DEFER_STATIC_LOCATIONS 0
// The 512x512 minimap and map-function sprites cost too much for the current gameplay baseline.
#define PS2_DISABLE_MINIMAP 1
#define PS2_SIMPLE_UI 1
// Terrain remains untextured on PS2, with normal underlay/overlay rasterization enabled.
#define PS2_UNTEXTURED_TERRAIN 1
#define PS2_FLAT_TERRAIN 0
// Keep synchronous scene/land construction enabled. The minimap remains disabled.
#define PS2_DEFER_SCENE_REBUILD 0
#define PS2_NULL_SCENE_REBUILD 0
// Normal dynamic chat/sidebar/tab composition is hardware-good. Lift the final safe-interface gate
// for this test so the controller virtual cursor is visible and type-2 inventory slots can render
// their normal 32x32 object icons. This also admits normal type-6 interface model components, so
// treat the whole interactive/model-backed UI layer as one hardware acceptance test and watch heap,
// model/icon cache pressure and frame time. The minimap and heavy decorative chrome remain disabled.
#define PS2_NULL_UI 0
#define PS2_SAFE_INTERFACE 0
#define PS2_UI_PROFILE 0
// Hardware-tested restoration step after radius-14 terrain became stable. The generic PS2
// pushPlayers() path intentionally skips LOCAL_PLAYER_INDEX, so keep the dedicated direct local
// avatar draw enabled while restoring the interactive/model-backed gameplay UI.
#define PS2_RENDER_LOCAL_PLAYER 1
#elif defined(_arch_dreamcast) || defined(__NDS__)
// NOTE: more extreme lowmem mode, making the game fully explorable on 32 MB
// -2 MB RAM, may cause some models to be invisible
#define MODEL_MAX_DEPTH 600
// -1 MB RAM, may cause buffer overflows
#define MODEL_DEPTH_FACE_COUNT 80
// -1 MB RAM, may cause more lag
#define PIX3D_POOL_COUNT 5
// -1 MB RAM, disables login screen flames
#define DISABLE_FLAMES
#else
#define MODEL_MAX_DEPTH 1500
#define MODEL_DEPTH_FACE_COUNT 512
#define PIX3D_POOL_COUNT 20
#endif
#define LOCBUFFER_COUNT 100
#define MAX_NPC_COUNT 16384 // rev254's npc index field is 14 bits (max real index 16382, 16383 is the loop sentinel) - see getNpcPosNewVis
#define MAX_PLAYER_COUNT 2048
#define LOCAL_PLAYER_INDEX 2047
#define VARPS_COUNT 2000
#define BFS_STEP_SIZE 4000
#define FLAME_BUFFER_SIZE 32768
#define MENU_OPTION_LENGTH 500
#define CHATBACK_LENGTH 10
#define REPORT_ABUSE_LENGTH 12
#define CHAT_LENGTH 80
#define USERNAME_LENGTH 12
#define PASSWORD_LENGTH 20

// key codes
#define K_BACKSPACE 8
#define K_TAB 9

#define K_ENTER 13

#define K_CONTROL 17

#define K_ESCAPE 27

#define K_PAGE_UP 33
#define K_PAGE_DOWN 34
#define K_END 35
#define K_HOME 36
#define K_LEFT 37
#define K_UP 38
#define K_RIGHT 39
#define K_DOWN 40

#define K_ASTERISK 42
#define K_PLUS 43

#define K_MINUS 45
#define K_PERIOD 46
#define K_FWD_SLASH 47
#define K_0 48
#define K_1 49
#define K_2 50
#define K_3 51
#define K_4 52
#define K_5 53
#define K_6 54
#define K_7 55
#define K_8 56
#define K_9 57

#define K_F1 112
#define K_F2 113
#define K_F3 114
#define K_F4 115
#define K_F5 116
#define K_F6 117
#define K_F7 118
#define K_F8 119
#define K_F9 120
#define K_F10 121
#define K_F11 122
#define K_F12 123

// ---- these are in rgb 24 bits
#define RED 0xff0000      // 16711680
#define GREEN 0xff00      // 65280
#define BLUE 0xff         // 255
#define YELLOW 0xffff00   // 16776960
#define CYAN 0xffff       // 65535
#define MAGENTA 0xff00ff  // 16711935
#define WHITE 0xffffff    // 16777215
#define BLACK 0x0         // 0
#define LIGHTRED 0xff9040 // 16748608
#define DARKRED 0x800000  // 8388608
#define DARKBLUE 0x80     // 128
#define ORANGE1 0xffb000      // 16756736
#define ORANGE2 0xff7000      // 16740352
#define ORANGE3 0xff3000      // 16724736
#define GREEN1 0xc0ff00       // 12648192
#define GREEN2 0x80ff00       // 8453888
#define GREEN3 0x40ff00       // 4259584

// other
#define PROGRESS_RED 0x8c1111              // 9179409
#define OPTIONS_MENU 0x5d5447              // 6116423
#define SCROLLBAR_TRACK 0x23201b            // 2301979
#define SCROLLBAR_GRIP_FOREGROUND 0x4d4233 // 5063219
#define SCROLLBAR_GRIP_HIGHLIGHT 0x766654  // 7759444
#define SCROLLBAR_GRIP_LOWLIGHT 0x332d25   // 3353893
#define TRADE_MESSAGE 0x800080             // 8388736
#define DUEL_MESSAGE 0xcbb789              // 13347721

// ---- these are in hsl 16 bits
// hair
#define HAIR_DARK_BROWN 6798
#define HAIR_WHITE 107
#define HAIR_LIGHT_GREY 10283
#define HAIR_DARK_GREY 16
#define HAIR_APRICOT 4797
#define HAIR_STRAW 7744
#define HAIR_LIGHT_BROWN 5799
#define HAIR_BROWN 4634
#define HAIR_TURQUOISE 33697
#define HAIR_GREEN 22433
#define HAIR_GINGER 2983
#define HAIR_MAGENTA 54193

// body
#define BODY_KHAKI 8741
#define BODY_CHARCOAL 12
#define BODY_CRIMSON 64030
#define BODY_NAVY 43162
#define BODY_STRAW 7735
#define BODY_WHITE 8404
#define BODY_RED 1701
#define BODY_BLUE 38430
#define BODY_GREEN 24094
#define BODY_YELLOW 10153
#define BODY_PURPLE 56621
#define BODY_ORANGE 4783
#define BODY_ROSE 1341
#define BODY_LIME 16578
#define BODY_CYAN 35003
#define BODY_EMERALD 25239

#define BODY_RECOLOR_KHAKI 9104
#define BODY_RECOLOR_CHARCOAL 10275
#define BODY_RECOLOR_CRIMSON 7595
#define BODY_RECOLOR_NAVY 3610
#define BODY_RECOLOR_STRAW 7975
#define BODY_RECOLOR_WHITE 8526
#define BODY_RECOLOR_RED 918
#define BODY_RECOLOR_BLUE 38802
#define BODY_RECOLOR_GREEN 24466
#define BODY_RECOLOR_YELLOW 10145
#define BODY_RECOLOR_PURPLE 58654
#define BODY_RECOLOR_ORANGE 5027
#define BODY_RECOLOR_ROSE 1457
#define BODY_RECOLOR_LIME 16565
#define BODY_RECOLOR_CYAN 34991
#define BODY_RECOLOR_EMERALD 25486

// feet
#define FEET_BROWN 4626
#define FEET_KHAKI 11146
#define FEET_ASHEN 6439
#define FEET_DARK 12
#define FEET_TERRACOTTA 4758
#define FEET_GREY 10270

// skin
#define SKIN 4574
#define SKIN_DARKER 4550
#define SKIN_DARKER_DARKER 4537
#define SKIN_DARKER_DARKER_DARKER 5681
#define SKIN_DARKER_DARKER_DARKER_DARKER 5673
#define SKIN_DARKER_DARKER_DARKER_DARKER_DARKER 5790
#define SKIN_DARKER_DARKER_DARKER_DARKER_DARKER_DARKER 6806
#define SKIN_DARKER_DARKER_DARKER_DARKER_DARKER_DARKER_DARKER 8076
