#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "allocator.h"
#include "client.h"
#include "custom.h"
#include "datastruct/jstring.h"
#include "gameshell.h"
#include "inputtracking.h"
#include "thirdparty/ini.h"

extern ClientData _Client;
extern InputTracking _InputTracking;

#ifdef __PS2__
// See platform/ps2.c - "mass:/" once a USB mass-storage device is detected, else "" (today's
// plain relative path, which PCSX2's host: dev shortcut transparently redirects).
const char *ps2_cache_prefix(void);
#endif

#if defined(__PS2__)
// Keep the normal hardware presentation clean. The existing ::perf command can opt back into
// the FPS/LRU diagnostics at runtime whenever hardware profiling is needed.
Custom _Custom = {.chat_era = 2, .http_port = 80, .show_performance = false};
#elif defined(__WII__) || defined(__3DS__) || defined(__WIIU__) || defined(__SWITCH__) || defined(__PSP__) || defined(__vita__) || defined(_arch_dreamcast) || defined(NXDK) || defined(__NDS__) || defined(ANDROID)
Custom _Custom = {.chat_era = 2, .http_port = 80, .show_performance = true};
#else
Custom _Custom = {.chat_era = 2, .http_port = 80};
#endif

// NOTE: having this file separate from client easily creates conflicts if other non-client entrypoints compile all c files

// macro assumes ini setting has the same name as the member variable
// works with pointers and non-pointers, no bounds checks for ints
#define INI_STR(dst, member)                                    \
    {                                                           \
        const char *tmp = ini_get(config, NULL, #member);       \
        if (tmp) {                                              \
            strncpy(dst->member, tmp, sizeof(dst->member) - 1); \
        }                                                       \
    }

#define INI_STR_LOG(dst, member) \
    INI_STR(dst, member);        \
    rs2_log("  %s: %s\n", #member, dst->member)

#define INI_STR_LOG_HIDDEN(dst, member)                \
    INI_STR(dst, member);                              \
    char *censored = jstring_to_asterisk(dst->member); \
    rs2_log("  %s: %s\n", #member, censored);          \
    free(censored)

#define INI_INT_LOG(dst, member, calc)                     \
    {                                                      \
        int tmp = 0;                                       \
        if (ini_sget(config, NULL, #member, "%d", &tmp)) { \
            *dst->member = tmp;                            \
            calc;                                          \
        }                                                  \
        rs2_log("  %s: %d\n", #member, *dst->member);      \
    }

bool load_ini_args(void) {
#ifdef NXDK
    ini_t *config = ini_load("D:\\config.ini");
#elif defined(__PS2__)
    char config_path[256];
    snprintf(config_path, sizeof(config_path), "%sconfig.ini", ps2_cache_prefix());
    ini_t *config = ini_load(config_path);
#else
    ini_t *config = ini_load("config.ini");
#endif
    if (!config) {
        return false;
    }

    rs2_log("World config:\n");
    INI_STR_LOG((&_Client), socketip);
    INI_INT_LOG(&(&_Custom), http_port, );
    // world nodeid 1 = 10 (default)
    INI_INT_LOG(&(&_Client), nodeid, _Client.nodeid = 10 + _Client.nodeid - 1);
    INI_INT_LOG(&(&_Client), portoff, );
    INI_INT_LOG(&(&_Client), lowmem, );
#if defined(__PSP__) || defined(_arch_dreamcast) || defined(__NDS__) || defined(NXDK)
    // implicitly ignore highmem, avoids confusion as there's no way it'll load, except if xbox has mem expansion
    // 2026-09-14: __PS2__ was added here then REVERTED same day - real-hardware test regressed the
    // hang to BEFORE client_build_scene() even starts (previously reached deep into
    // loctype_get_model()/model_calculate_normals()). _Client.lowmem also flips
    // `p1(c->login, _Client.lowmem ? 1 : 0)` in the login packet (see client.c) - telling the server
    // this is a lowmem client, untested territory that could change server-side packet behavior before
    // any of this scene-build code even runs. The two other same-day fixes (model.c OOM/NULL-deref
    // guards, pushLocs() disabled on PS2) only touch code deep in scene/render paths that were never
    // reached in the regressed run, so they're not implicated - this flag is the one variable that
    // plausibly explains a hang moving THIS much earlier. Don't re-add without isolating exactly what
    // about lowmem=true breaks the login/build-scene handshake first.
    _Client.lowmem = true;
#endif
    INI_INT_LOG(&(&_Client), members, _Client.members = !_Client.members);

    rs2_log("\n");
    ini_free(config);
    return true;
}

void load_ini_config(Client *c) {
#ifdef NXDK
    ini_t *config = ini_load("D:\\config.ini");
#elif defined(__PS2__)
    char config_path[256];
    snprintf(config_path, sizeof(config_path), "%sconfig.ini", ps2_cache_prefix());
    ini_t *config = ini_load(config_path);
#else
    ini_t *config = ini_load("config.ini");
#endif
    if (!config) {
        return;
    }

    rs2_log("Options config:\n");
    INI_STR_LOG((&_Client), rsa_exponent);
    INI_STR_LOG((&_Client), rsa_modulus);

    INI_STR_LOG(c, username);
    INI_STR_LOG_HIDDEN(c, password);

    INI_INT_LOG(&(&_Custom), hide_dns, );
    INI_INT_LOG(&(&_Custom), hide_debug_sprite, );
    INI_INT_LOG(&(&_Custom), allow_commands, );
    INI_INT_LOG(&(&_Custom), allow_debugprocs, );
    INI_INT_LOG(&(&_Custom), chat_era, );
    INI_INT_LOG(&(&_Custom), remember_username, );
    INI_INT_LOG(&(&_Custom), remember_password, );
    INI_INT_LOG(&(&_Custom), resizable, );
#ifdef ANDROID
    _Custom.resizable = true;
#endif
    INI_INT_LOG(&(&_Custom), item_outlines, );
#ifndef __PS2__
    INI_INT_LOG(&(&_Custom), show_performance, );
#else
    // The embedded PS2 config historically enabled the bring-up HUD. Keep diagnostics available via
    // ::perf, but do not let that legacy config entry make them visible on every normal boot.
    _Custom.show_performance = false;
    rs2_log("  show_performance: 0 (PS2 default; toggle with ::perf)\n");
#endif
#ifdef GL11
    _Custom.use_opengl11 = true;
#endif

    rs2_log("\n");
    ini_free(config);
}

void draw_info_overlay(Client *c) {
    int x = 507;
    int y = 13;

    char buf[MAX_STR];
    if (_Custom.show_performance) {
        // skip corner of interfaces, makes text harder to read
        y += 13;
        // skip 2 possible spots of "Close Window"
        y += 13;
        y += 13;
        sprintf(buf, "LRU: %dK / %dK", bump_allocator_used() >> 10, bump_allocator_capacity() >> 10);
        drawStringRight(c->font_plain11, x, y, buf, YELLOW, true);
        y += 13;
        sprintf(buf, "FPS: %d", c->shell->fps);
        drawStringRight(c->font_plain11, x, y, buf, YELLOW, true);
        y += 13;
#ifdef __PS2__
        sprintf(buf, "LocLOD: %d", ps2_world3d_decorative_radius());
        drawStringRight(c->font_plain11, x, y, buf, YELLOW, true);
        y += 13;
        sprintf(buf, "LocEM:%d Arm:%d Step:%d/2",
                ps2_world3d_decorative_emergency_active(),
                ps2_world3d_decorative_trigger_armed(),
                ps2_world3d_decorative_tiles_travelled());
        drawStringRight(c->font_plain11, x, y, buf, YELLOW, true);
        y += 13;
        sprintf(buf, "Grace:%d/30", ps2_world3d_decorative_scene_frames());
        drawStringRight(c->font_plain11, x, y, buf, YELLOW, true);
        y += 13;
#endif
#if defined(__vita__)
#include <psp2/power.h>
        sprintf(buf, "BAT: %d%%", scePowerGetBatteryLifePercent());
        drawStringRight(c->font_plain11, x, y, buf, YELLOW, true);
        y += 13;
        sprintf(buf, "CPU: %d MHz", scePowerGetArmClockFrequency());
        drawStringRight(c->font_plain11, x, y, buf, YELLOW, true);
        y += 13;
        sprintf(buf, "GPU: %d MHz", scePowerGetGpuClockFrequency());
        drawStringRight(c->font_plain11, x, y, buf, YELLOW, true);
        y += 13;
#endif
    }

    if (_Custom.camera_editor || _Custom.show_debug) {
        sprintf(buf, "Local Pos: %d, %d, %d", c->local_player->pathing_entity.x, c->local_player->pathing_entity.z, c->local_player->y);
        drawStringRight(c->font_plain11, x, y, buf, YELLOW, true);
        y += 13;

        sprintf(buf, "Camera Pos: %d, %d, %d", c->cameraX, c->cameraZ, c->cameraY);
        drawStringRight(c->font_plain11, x, y, buf, YELLOW, true);
        y += 13;

        sprintf(buf, "Camera Angle: %d, %d", c->cameraYaw, c->cameraPitch);
        drawStringRight(c->font_plain11, x, y, buf, YELLOW, true);
        y += 13;

        sprintf(buf, "Cutscene Source: %d, %d %d; %d, %d", c->cutsceneSrcLocalTileX, c->cutsceneSrcLocalTileZ, c->cutsceneSrcHeight, c->cutsceneMoveSpeed, c->cutsceneMoveAcceleration);
        drawStringRight(c->font_plain11, x, y, buf, YELLOW, true);
        y += 13;

        sprintf(buf, "Cutscene Destination: %d, %d %d; %d, %d", c->cutsceneDstLocalTileX, c->cutsceneDstLocalTileZ, c->cutsceneDstHeight, +c->cutsceneRotateSpeed, c->cutsceneRotateAcceleration);
        drawStringRight(c->font_plain11, x, y, buf, YELLOW, true);
        y += 13;
    }

    if (_Custom.camera_editor) {
        y += 13;
        drawStringRight(c->font_plain11, x, y, "Instructions:", YELLOW, true);
        y += 13;

        drawStringRight(c->font_plain11, x, y, "- Arrows to move Camera", YELLOW, true);
        y += 13;

        drawStringRight(c->font_plain11, x, y, "- Shift to control Source or Dest", YELLOW, true);
        y += 13;

        drawStringRight(c->font_plain11, x, y, "- Alt to control Height", YELLOW, true);
        y += 13;

        drawStringRight(c->font_plain11, x, y, "- Ctrl to control Modifier", YELLOW, true);
        y += 13;
    }
}

bool insideChatInputArea(Client *c) {
    // 495 x 33
    const int chatInputAreaX1 = 11;
    const int chatInputAreaY1 = 449;
    const int chatInputAreaX2 = chatInputAreaX1 + 495;
    const int chatInputAreaY2 = chatInputAreaY1 + 33;
    return c->ingame &&
           c->chat_interface_id == -1 &&
           !c->chatback_input_open &&
           !c->show_social_input &&
           c->shell->mouse_x >= chatInputAreaX1 &&
           c->shell->mouse_x <= chatInputAreaX2 &&
           c->shell->mouse_y >= chatInputAreaY1 &&
           c->shell->mouse_y <= chatInputAreaY2;
}

bool insideChatPopupArea(Client *c) {
    // 495 x 99
    const int chatInputAreaX1 = 11;
    const int chatInputAreaY1 = 383;
    const int chatInputAreaX2 = chatInputAreaX1 + 495;
    const int chatInputAreaY2 = chatInputAreaY1 + 99;
    return c->ingame && (c->chatback_input_open || c->show_social_input) && c->shell->mouse_x >= chatInputAreaX1 && c->shell->mouse_x <= chatInputAreaX2 && c->shell->mouse_y >= chatInputAreaY1 && c->shell->mouse_y <= chatInputAreaY2;
}

bool insideReportInterfaceTextArea(Client *c) {
    // custom: for report abuse input on mobile
    // actual component size is [233, 137, 58 14]
    // extended it a little bit for easier interaction, since the area to
    // touch is not obvious (it's a bit narrow)
    if (!c->ingame) {
        return false;
    }
    // either viewport or report-abuse interface Ids are bad
    if (c->viewport_interface_id == -1 || c->reportAbuseInterfaceID == -1) {
        return false;
    }
    // active viewport interface Id does not match
    if (c->viewport_interface_id != c->reportAbuseInterfaceID) {
        return false;
    }
    const int reportInputAreaX1 = 82;
    const int reportInputAreaY1 = 137;
    const int reportInputAreaX2 = reportInputAreaX1 + 366;
    const int reportInputAreaY2 = reportInputAreaY1 + 26;
    return c->shell->mouse_x >= reportInputAreaX1 && c->shell->mouse_x <= reportInputAreaX2 && c->shell->mouse_y >= reportInputAreaY1 && c->shell->mouse_y <= reportInputAreaY2;
}

bool insideUsernameArea(Client *c) {
    // 261 x 17
    const int usernameAreaX1 = 301;
    const int usernameAreaY1 = 262;
    const int usernameAreaX2 = usernameAreaX1 + 261;
    const int usernameAreaY2 = usernameAreaY1 + 17;
    return !c->ingame && c->title_screen_state == 2 && c->shell->mouse_x >= usernameAreaX1 && c->shell->mouse_x <= usernameAreaX2 && c->shell->mouse_y >= usernameAreaY1 && c->shell->mouse_y <= usernameAreaY2;
}

bool inPasswordArea(Client *c) {
    // 261 x 17
    const int passwordAreaX1 = 301;
    const int passwordAreaY1 = 279;
    const int passwordAreaX2 = passwordAreaX1 + 261;
    const int passwordAreaY2 = passwordAreaY1 + 17;
    return !c->ingame && c->title_screen_state == 2 && c->shell->mouse_x >= passwordAreaX1 && c->shell->mouse_x <= passwordAreaX2 && c->shell->mouse_y >= passwordAreaY1 && c->shell->mouse_y <= passwordAreaY2;
}

bool insideMobileInputArea(Client *c) {
    // custom: for mobile keyboard input
    return insideChatInputArea(c) || insideChatPopupArea(c) || insideUsernameArea(c) || inPasswordArea(c) || insideReportInterfaceTextArea(c);
}

bool insideTabArea(Client *c) {
    // 190 x 261
    const int tabAreaX1 = 562;
    const int tabAreaY1 = 231;
    const int tabAreaX2 = tabAreaX1 + 190;
    const int tabAreaY2 = tabAreaY1 + 261;
    return c->ingame && c->shell->mouse_x >= tabAreaX1 && c->shell->mouse_x <= tabAreaX2 && c->shell->mouse_y >= tabAreaY1 && c->shell->mouse_y <= tabAreaY2;
}

bool insideViewportArea(Client *c) {
    // 512 x 334
    const int viewportAreaX1 = 8;
    const int viewportAreaY1 = 11;
    const int viewportAreaX2 = viewportAreaX1 + 512;
    const int viewportAreaY2 = viewportAreaY1 + 334;
    return c->ingame && c->shell->mouse_x >= viewportAreaX1 && c->shell->mouse_x <= viewportAreaX2 && c->shell->mouse_y >= viewportAreaY1 && c->shell->mouse_y <= viewportAreaY2;
}
