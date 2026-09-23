#pragma once

#include <stdbool.h>

#include "platform.h"

struct GameShell {
    int state;
    int fps;
    int screen_width;
    int screen_height;
    PixMap *draw_area;
    int idle_cycles;
    int mouse_button;
    int mouse_x;
    int mouse_y;
    int mouse_click_button;
    int mouse_click_x;
    int mouse_click_y;
    int deltime;
    int mindel;
    int64_t *otim;
    bool refresh;
    int *action_key;
    int *key_queue;
    int key_queue_read_pos;
    int key_queue_write_pos;
    bool has_focus;
    // True on every platform except a keyboard-less gamepad-only one (currently just PS2, set
    // false in ps2.c's platform_new()) - gates whether entry/client.c's on-screen virtual keyboard
    // auto-opens at text-entry focus points. Platform-agnostic by design so any other
    // keyboard-less port can opt in later just by also setting this false.
    bool has_keyboard;
#ifdef __PS2__
    // Last completed rendered frame's client_draw + GS queue/sync/present time. Heap-owned here
    // rather than a new global so normal PS2 BSS placement remains unchanged.
    int ps2_last_render_ms;
#endif
};

extern bool update_touch;
extern int last_touch_x;
extern int last_touch_y;
extern int last_touch_button;

GameShell *gameshell_new(void);
void gameshell_free(GameShell *shell);
void gameshell_init_application(Client *c, int width, int height);
void gameshell_run(Client *c);
void gameshell_destroy(Client *c);
void gameshell_shutdown(Client *c);
void gameshell_set_framerate(GameShell *shell, int fps);
void gameshell_draw_progress(GameShell *shell, const char *message, int progress);
int poll_key(GameShell *shell);
void key_pressed(GameShell *shell, int code, int ch);
void key_released(GameShell *shell, int code, int ch);
