#ifdef __PS2__

// ps2.yaml redirects the two PS2SDK entry points used by the legacy presenter/input
// path to this file. Undefine the build macros before including the real SDK headers
// so this translation unit can call the underlying implementations without recursion.
#ifdef gsKit_prim_sprite_texture_3d
#undef gsKit_prim_sprite_texture_3d
#endif
#ifdef padRead
#undef padRead
#endif

#include <gsKit.h>
#include <libpad.h>

#include "client.h"
#include "defines.h"
#include "gameshell.h"

// Modern PS2 UI: keep the proven 2004 software UI alive off-screen and reorganize
// only final GS presentation/input mapping. World3D, Component coordinates and the
// underlying chat/keyboard implementations remain untouched.
#define PS2_UI_VIEW_H 418
#define PS2_UI_DOCK_Y PS2_UI_VIEW_H
#define PS2_UI_DOCK_H (SCREEN_FB_HEIGHT - PS2_UI_DOCK_Y)

#define PS2_UI_PANEL_X 438
#define PS2_UI_PANEL_Y 145
#define PS2_UI_PANEL_W 192
#define PS2_UI_PANEL_H 261

#define PS2_UI_CHAT_X 10
#define PS2_UI_CHAT_Y 324
#define PS2_UI_CHAT_W 420
#define PS2_UI_CHAT_H 84
#define PS2_UI_CHAT_SRC_X 17
#define PS2_UI_CHAT_SRC_Y 357
#define PS2_UI_CHAT_SRC_W 479
#define PS2_UI_CHAT_SRC_H 96

// virtual_keyboard_draw() renders a 10x6 grid of 28x20 cells centered on the
// 765x503 legacy canvas. Present that exact grid as a larger controller-friendly
// overlay instead of exposing the old full-width panel coordinates.
#define PS2_UI_KEYBOARD_SRC_W 280
#define PS2_UI_KEYBOARD_SRC_H 120
#define PS2_UI_KEYBOARD_SRC_X ((SCREEN_WIDTH - PS2_UI_KEYBOARD_SRC_W) / 2)
#define PS2_UI_KEYBOARD_SRC_Y (SCREEN_HEIGHT - PS2_UI_KEYBOARD_SRC_H - 10)
#define PS2_UI_KEYBOARD_X 110
#define PS2_UI_KEYBOARD_Y 215
#define PS2_UI_KEYBOARD_W 420
#define PS2_UI_KEYBOARD_H 180

#define PS2_UI_DOCK_BUTTON_W 38
#define PS2_UI_DOCK_BUTTON_H 44
#define PS2_UI_DOCK_BUTTON_GAP 5
#define PS2_UI_TAB_BUTTON_COUNT 13
#define PS2_UI_DOCK_BUTTON_COUNT 14
#define PS2_UI_CHAT_BUTTON 0
#define PS2_UI_DOCK_BUTTONS_W \
    (PS2_UI_DOCK_BUTTON_COUNT * PS2_UI_DOCK_BUTTON_W + \
     (PS2_UI_DOCK_BUTTON_COUNT - 1) * PS2_UI_DOCK_BUTTON_GAP)
#define PS2_UI_DOCK_BUTTON_X ((SCREEN_FB_WIDTH - PS2_UI_DOCK_BUTTONS_W) / 2)
#define PS2_UI_DOCK_BUTTON_Y (PS2_UI_DOCK_Y + 9)

// Fixed-mode tab 7 is absent in the original client.
static const int ps2_ui_tabs[PS2_UI_TAB_BUTTON_COUNT] = {
    0, 1, 2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13
};

// Source positions of the 13 already-rendered sideicons in the 765x503 canvas.
static const int ps2_ui_icon_x[PS2_UI_TAB_BUTTON_COUNT] = {
    545, 569, 598, 631, 669, 696, 724,
    570, 598, 633, 670, 697, 722
};
static const int ps2_ui_icon_y[PS2_UI_TAB_BUTTON_COUNT] = {
    173, 171, 171, 172, 173, 171, 173,
    468, 469, 470, 468, 468, 468
};

static bool ps2_ui_panel_open;
static bool ps2_ui_chat_open;
static int ps2_ui_cursor_x = SCREEN_FB_WIDTH / 2;
static int ps2_ui_cursor_y = PS2_UI_VIEW_H / 2;
static bool ps2_ui_cross_raw_was_down;
static bool ps2_ui_square_raw_was_down;
static bool ps2_ui_triangle_raw_was_down;
static bool ps2_ui_start_raw_was_down;
static bool ps2_ui_cross_consumed_until_release;
static bool ps2_ui_start_consumed_until_release;

static bool ps2_ui_active(void) {
    return ps2_crash_client && ps2_crash_client->ingame && ps2_crash_client->shell;
}

static int ps2_ui_button_at(int x, int y) {
    if (y < PS2_UI_DOCK_BUTTON_Y || y >= PS2_UI_DOCK_BUTTON_Y + PS2_UI_DOCK_BUTTON_H ||
        x < PS2_UI_DOCK_BUTTON_X || x >= PS2_UI_DOCK_BUTTON_X + PS2_UI_DOCK_BUTTONS_W) {
        return -1;
    }

    int relative = x - PS2_UI_DOCK_BUTTON_X;
    int stride = PS2_UI_DOCK_BUTTON_W + PS2_UI_DOCK_BUTTON_GAP;
    int button = relative / stride;
    if (button < 0 || button >= PS2_UI_DOCK_BUTTON_COUNT ||
        relative - button * stride >= PS2_UI_DOCK_BUTTON_W) {
        return -1;
    }
    return button;
}

static void ps2_ui_toggle_chat(void) {
    ps2_ui_chat_open = !ps2_ui_chat_open;
    if (ps2_ui_chat_open) {
        ps2_ui_panel_open = false;
    }
}

static void ps2_ui_select_tab(Client *c, int tab) {
    if (!c || tab < 0 || tab >= 15 || c->tab_interface_id[tab] == -1) {
        return;
    }

    ps2_ui_chat_open = false;
    if (ps2_ui_panel_open && c->selected_tab == tab) {
        ps2_ui_panel_open = false;
        return;
    }

    c->selected_tab = tab;
    c->redraw_sidebar = true;
    c->redraw_sideicons = true;
    ps2_ui_panel_open = true;
}

static bool ps2_ui_cursor_in(int x, int y, int w, int h) {
    return ps2_ui_cursor_x >= x && ps2_ui_cursor_x < x + w &&
           ps2_ui_cursor_y >= y && ps2_ui_cursor_y < y + h;
}

static void ps2_ui_map_cursor_to_legacy(Client *c) {
    if (!c || !c->shell) {
        return;
    }

    // Keyboard has first claim over input. Its visible 420x180 overlay maps directly
    // back to the 280x120 grid virtual_keyboard_handle_input() already understands.
    if (c->virtual_keyboard_visible) {
        if (ps2_ui_cursor_in(PS2_UI_KEYBOARD_X, PS2_UI_KEYBOARD_Y,
                             PS2_UI_KEYBOARD_W, PS2_UI_KEYBOARD_H)) {
            c->shell->mouse_x = PS2_UI_KEYBOARD_SRC_X +
                (ps2_ui_cursor_x - PS2_UI_KEYBOARD_X) * PS2_UI_KEYBOARD_SRC_W / PS2_UI_KEYBOARD_W;
            c->shell->mouse_y = PS2_UI_KEYBOARD_SRC_Y +
                (ps2_ui_cursor_y - PS2_UI_KEYBOARD_Y) * PS2_UI_KEYBOARD_SRC_H / PS2_UI_KEYBOARD_H;
        } else {
            c->shell->mouse_x = 0;
            c->shell->mouse_y = 0;
        }
        return;
    }

    if (ps2_ui_chat_open && ps2_ui_cursor_in(PS2_UI_CHAT_X, PS2_UI_CHAT_Y,
                                              PS2_UI_CHAT_W, PS2_UI_CHAT_H)) {
        c->shell->mouse_x = PS2_UI_CHAT_SRC_X +
            (ps2_ui_cursor_x - PS2_UI_CHAT_X) * PS2_UI_CHAT_SRC_W / PS2_UI_CHAT_W;
        c->shell->mouse_y = PS2_UI_CHAT_SRC_Y +
            (ps2_ui_cursor_y - PS2_UI_CHAT_Y) * PS2_UI_CHAT_SRC_H / PS2_UI_CHAT_H;
        return;
    }

    if (ps2_ui_panel_open && ps2_ui_cursor_in(PS2_UI_PANEL_X, PS2_UI_PANEL_Y,
                                               PS2_UI_PANEL_W, PS2_UI_PANEL_H)) {
        c->shell->mouse_x = 553 + (ps2_ui_cursor_x - PS2_UI_PANEL_X) * 190 / PS2_UI_PANEL_W;
        c->shell->mouse_y = 205 + (ps2_ui_cursor_y - PS2_UI_PANEL_Y) * 261 / PS2_UI_PANEL_H;
        return;
    }

    if (ps2_ui_cursor_y >= PS2_UI_DOCK_Y) {
        c->shell->mouse_x = 0;
        c->shell->mouse_y = 0;
        return;
    }

    c->shell->mouse_x = 4 + ps2_ui_cursor_x * 512 / SCREEN_FB_WIDTH;
    c->shell->mouse_y = 4 + ps2_ui_cursor_y * 334 / PS2_UI_VIEW_H;
}

int ps2_modern_padRead(int port, int slot, struct padButtonStatus *buttons) {
    int result = padRead(port, slot, buttons);
    if (result <= 0 || !buttons || !ps2_ui_active()) {
        return result;
    }

    Client *c = ps2_crash_client;

    int dx = ((int)buttons->ljoy_h - 128) / 24;
    int dy = ((int)buttons->ljoy_v - 128) / 24;
    if (dx || dy) {
        ps2_ui_cursor_x += dx;
        ps2_ui_cursor_y += dy;
        if (ps2_ui_cursor_x < 0) ps2_ui_cursor_x = 0;
        if (ps2_ui_cursor_x >= SCREEN_FB_WIDTH) ps2_ui_cursor_x = SCREEN_FB_WIDTH - 1;
        if (ps2_ui_cursor_y < 0) ps2_ui_cursor_y = 0;
        if (ps2_ui_cursor_y >= SCREEN_FB_HEIGHT) ps2_ui_cursor_y = SCREEN_FB_HEIGHT - 1;
        c->shell->idle_cycles = 0;
    }

    bool raw_cross = !(buttons->btns & PAD_CROSS);
    bool raw_square = !(buttons->btns & PAD_SQUARE);
    bool raw_triangle = !(buttons->btns & PAD_TRIANGLE);
    bool raw_start = !(buttons->btns & PAD_START);

    int dock_button = c->virtual_keyboard_visible ? -1 : ps2_ui_button_at(ps2_ui_cursor_x, ps2_ui_cursor_y);
    if (raw_cross && !ps2_ui_cross_raw_was_down && dock_button >= 0) {
        if (dock_button == PS2_UI_CHAT_BUTTON) {
            ps2_ui_toggle_chat();
        } else {
            ps2_ui_select_tab(c, ps2_ui_tabs[dock_button - 1]);
        }
        ps2_ui_cross_consumed_until_release = true;
    }
    if (!raw_cross) {
        ps2_ui_cross_consumed_until_release = false;
    }

    // Start is the controller equivalent of clicking the permanent Chat dock button.
    // Reuse the exact same modern-UI toggle instead of creating a second chat-focus state.
    // Dialogue and the virtual keyboard retain input ownership; Start is only consumed here
    // when this layer actually handles the press.
    if (!c->virtual_keyboard_visible && c->chat_interface_id == -1 &&
        raw_start && !ps2_ui_start_raw_was_down) {
        ps2_ui_toggle_chat();
        ps2_ui_start_consumed_until_release = true;
    }
    if (!raw_start) {
        ps2_ui_start_consumed_until_release = false;
    }

    // Square remains the fast inventory toggle normally. When the modern Chat
    // panel is open, Chat owns Square contextually and uses it to start typing.
    if (!c->virtual_keyboard_visible && raw_square && !ps2_ui_square_raw_was_down) {
        if (ps2_ui_chat_open && c->chat_interface_id == -1) {
            client_open_public_chat_keyboard(c);
        } else {
            ps2_ui_select_tab(c, 3);
        }
    }

    // Triangle closes modern overlays first. While typing it intentionally falls
    // through untouched so the existing keyboard Back/Backspace behavior still works.
    bool consume_triangle = false;
    if (!c->virtual_keyboard_visible && raw_triangle && !ps2_ui_triangle_raw_was_down) {
        if (ps2_ui_panel_open) {
            ps2_ui_panel_open = false;
            consume_triangle = true;
        } else if (ps2_ui_chat_open) {
            ps2_ui_chat_open = false;
            consume_triangle = true;
        }
    }

    ps2_ui_cross_raw_was_down = raw_cross;
    ps2_ui_square_raw_was_down = raw_square;
    ps2_ui_triangle_raw_was_down = raw_triangle;
    ps2_ui_start_raw_was_down = raw_start;

    ps2_ui_map_cursor_to_legacy(c);

    buttons->ljoy_h = 128;
    buttons->ljoy_v = 128;
    if (ps2_ui_cross_consumed_until_release) {
        buttons->btns |= PAD_CROSS;
    }
    if (ps2_ui_start_consumed_until_release) {
        // Mask the entire physical press, not just its first frame. Otherwise the platform
        // layer would see a delayed Start edge on the next held frame and open the keyboard.
        buttons->btns |= PAD_START;
    }
    // The keyboard already owns Cross through controller_keyboard_confirm_pressed,
    // but right-clicking through it into the world should never happen.
    if (c->virtual_keyboard_visible || ps2_ui_cursor_y >= PS2_UI_DOCK_Y) {
        buttons->btns |= PAD_CIRCLE;
    }
    if (raw_square) {
        buttons->btns |= PAD_SQUARE;
    }
    if (consume_triangle) {
        buttons->btns |= PAD_TRIANGLE;
    }

    return result;
}

static void ps2_ui_draw_textured_rect(GSGLOBAL *gsGlobal, const GSTEXTURE *texture,
                                      float x1, float y1, float x2, float y2,
                                      float u1, float v1, float u2, float v2) {
    gsKit_prim_sprite_texture_3d(gsGlobal, texture,
                                 x1, y1, 0, u1, v1,
                                 x2, y2, 0, u2, v2,
                                 GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0x00));
}

static void ps2_ui_draw_chat_icon(GSGLOBAL *gsGlobal, int bx, int by, bool selected) {
    u64 color = selected
        ? GS_SETREG_RGBAQ(0xf0, 0xd7, 0x91, 0x80, 0x00)
        : GS_SETREG_RGBAQ(0xd4, 0xd8, 0xdf, 0x80, 0x00);
    // Tiny speech bubble: body, hollow center and tail. Pure GS rectangles mean no
    // extra sprite/cache allocation for the one modern-only dock icon.
    gsKit_prim_sprite(gsGlobal, bx + 9, by + 10, bx + 29, by + 27, 4, color);
    gsKit_prim_sprite(gsGlobal, bx + 12, by + 13, bx + 26, by + 24, 5,
                      GS_SETREG_RGBAQ(0x28, 0x2d, 0x36, 0x80, 0x00));
    gsKit_prim_sprite(gsGlobal, bx + 13, by + 27, bx + 18, by + 32, 4, color);
}

void ps2_modern_gsKit_prim_sprite_texture_3d(GSGLOBAL *gsGlobal, const GSTEXTURE *texture,
                                             float x1, float y1, int iz1, float u1, float v1,
                                             float x2, float y2, int iz2, float u2, float v2,
                                             u64 color) {
    if (!ps2_ui_active()) {
        gsKit_prim_sprite_texture_3d(gsGlobal, texture,
                                     x1, y1, iz1, u1, v1,
                                     x2, y2, iz2, u2, v2, color);
        return;
    }

    Client *c = ps2_crash_client;
    bool keyboard_open = c->virtual_keyboard_visible;

    ps2_ui_draw_textured_rect(gsGlobal, texture,
                              0.0f, 0.0f, (float)SCREEN_FB_WIDTH, (float)PS2_UI_VIEW_H,
                              4.0f, 4.0f, 516.0f, 338.0f);

    // Dark permanent dock background. While the keyboard is open the buttons are
    // hidden: virtual_keyboard_draw() overwrites their legacy source pixels and the
    // keyboard should visually own the interaction anyway.
    gsKit_prim_sprite(gsGlobal,
                      0, PS2_UI_DOCK_Y, SCREEN_FB_WIDTH, SCREEN_FB_HEIGHT, 1,
                      GS_SETREG_RGBAQ(0x16, 0x19, 0x1f, 0x80, 0x00));
    gsKit_prim_sprite(gsGlobal,
                      0, PS2_UI_DOCK_Y, SCREEN_FB_WIDTH, PS2_UI_DOCK_Y + 2, 2,
                      GS_SETREG_RGBAQ(0x45, 0x4b, 0x57, 0x80, 0x00));

    if (!keyboard_open) {
        for (int i = 0; i < PS2_UI_DOCK_BUTTON_COUNT; i++) {
            int bx = PS2_UI_DOCK_BUTTON_X + i * (PS2_UI_DOCK_BUTTON_W + PS2_UI_DOCK_BUTTON_GAP);
            bool is_chat = i == PS2_UI_CHAT_BUTTON;
            int tab_index = i - 1;
            int tab = is_chat ? -1 : ps2_ui_tabs[tab_index];
            bool selected = is_chat ? ps2_ui_chat_open : (ps2_ui_panel_open && c->selected_tab == tab);
            bool available = is_chat || c->tab_interface_id[tab] != -1;

            u64 button_color = selected
                ? GS_SETREG_RGBAQ(0x4a, 0x52, 0x61, 0x80, 0x00)
                : (available
                    ? GS_SETREG_RGBAQ(0x28, 0x2d, 0x36, 0x80, 0x00)
                    : GS_SETREG_RGBAQ(0x1c, 0x1f, 0x25, 0x80, 0x00));
            gsKit_prim_sprite(gsGlobal,
                              bx, PS2_UI_DOCK_BUTTON_Y,
                              bx + PS2_UI_DOCK_BUTTON_W, PS2_UI_DOCK_BUTTON_Y + PS2_UI_DOCK_BUTTON_H,
                              2, button_color);
            if (selected) {
                gsKit_prim_sprite(gsGlobal,
                                  bx + 3, PS2_UI_DOCK_BUTTON_Y + PS2_UI_DOCK_BUTTON_H - 3,
                                  bx + PS2_UI_DOCK_BUTTON_W - 3, PS2_UI_DOCK_BUTTON_Y + PS2_UI_DOCK_BUTTON_H - 1,
                                  3, GS_SETREG_RGBAQ(0xc8, 0xad, 0x6c, 0x80, 0x00));
            }

            if (is_chat) {
                ps2_ui_draw_chat_icon(gsGlobal, bx, PS2_UI_DOCK_BUTTON_Y, selected);
            } else if (available) {
                ps2_ui_draw_textured_rect(gsGlobal, texture,
                                          (float)(bx + 7), (float)(PS2_UI_DOCK_BUTTON_Y + 7),
                                          (float)(bx + 31), (float)(PS2_UI_DOCK_BUTTON_Y + 31),
                                          (float)ps2_ui_icon_x[tab_index], (float)ps2_ui_icon_y[tab_index],
                                          (float)(ps2_ui_icon_x[tab_index] + 24),
                                          (float)(ps2_ui_icon_y[tab_index] + 24));
            }
        }

        if (ps2_ui_chat_open) {
            gsKit_prim_sprite(gsGlobal,
                              PS2_UI_CHAT_X - 4, PS2_UI_CHAT_Y - 4,
                              PS2_UI_CHAT_X + PS2_UI_CHAT_W + 4, PS2_UI_CHAT_Y + PS2_UI_CHAT_H + 4,
                              4, GS_SETREG_RGBAQ(0x0d, 0x0f, 0x13, 0x80, 0x00));
            gsKit_prim_sprite(gsGlobal,
                              PS2_UI_CHAT_X - 2, PS2_UI_CHAT_Y - 2,
                              PS2_UI_CHAT_X + PS2_UI_CHAT_W + 2, PS2_UI_CHAT_Y + PS2_UI_CHAT_H + 2,
                              5, GS_SETREG_RGBAQ(0x3b, 0x41, 0x4b, 0x80, 0x00));
            ps2_ui_draw_textured_rect(gsGlobal, texture,
                                      (float)PS2_UI_CHAT_X, (float)PS2_UI_CHAT_Y,
                                      (float)(PS2_UI_CHAT_X + PS2_UI_CHAT_W),
                                      (float)(PS2_UI_CHAT_Y + PS2_UI_CHAT_H),
                                      (float)PS2_UI_CHAT_SRC_X, (float)PS2_UI_CHAT_SRC_Y,
                                      (float)(PS2_UI_CHAT_SRC_X + PS2_UI_CHAT_SRC_W),
                                      (float)(PS2_UI_CHAT_SRC_Y + PS2_UI_CHAT_SRC_H));
        } else if (ps2_ui_panel_open && c->selected_tab >= 0 && c->selected_tab < 15 &&
                   c->tab_interface_id[c->selected_tab] != -1) {
            gsKit_prim_sprite(gsGlobal,
                              PS2_UI_PANEL_X - 4, PS2_UI_PANEL_Y - 4,
                              PS2_UI_PANEL_X + PS2_UI_PANEL_W + 4, PS2_UI_PANEL_Y + PS2_UI_PANEL_H + 4,
                              4, GS_SETREG_RGBAQ(0x0d, 0x0f, 0x13, 0x80, 0x00));
            gsKit_prim_sprite(gsGlobal,
                              PS2_UI_PANEL_X - 2, PS2_UI_PANEL_Y - 2,
                              PS2_UI_PANEL_X + PS2_UI_PANEL_W + 2, PS2_UI_PANEL_Y + PS2_UI_PANEL_H + 2,
                              5, GS_SETREG_RGBAQ(0x3b, 0x41, 0x4b, 0x80, 0x00));
            ps2_ui_draw_textured_rect(gsGlobal, texture,
                                      (float)PS2_UI_PANEL_X, (float)PS2_UI_PANEL_Y,
                                      (float)(PS2_UI_PANEL_X + PS2_UI_PANEL_W),
                                      (float)(PS2_UI_PANEL_Y + PS2_UI_PANEL_H),
                                      553.0f, 205.0f, 743.0f, 466.0f);
        }
    } else {
        // The legacy keyboard is already fully rendered into screenTexture by
        // virtual_keyboard_draw(). Crop only its QWERTY grid and make it large enough
        // for a television/controller without changing a single keyboard code path.
        gsKit_prim_sprite(gsGlobal,
                          PS2_UI_KEYBOARD_X - 5, PS2_UI_KEYBOARD_Y - 5,
                          PS2_UI_KEYBOARD_X + PS2_UI_KEYBOARD_W + 5,
                          PS2_UI_KEYBOARD_Y + PS2_UI_KEYBOARD_H + 5,
                          6, GS_SETREG_RGBAQ(0x0d, 0x0f, 0x13, 0x80, 0x00));
        ps2_ui_draw_textured_rect(gsGlobal, texture,
                                  (float)PS2_UI_KEYBOARD_X, (float)PS2_UI_KEYBOARD_Y,
                                  (float)(PS2_UI_KEYBOARD_X + PS2_UI_KEYBOARD_W),
                                  (float)(PS2_UI_KEYBOARD_Y + PS2_UI_KEYBOARD_H),
                                  (float)PS2_UI_KEYBOARD_SRC_X, (float)PS2_UI_KEYBOARD_SRC_Y,
                                  (float)(PS2_UI_KEYBOARD_SRC_X + PS2_UI_KEYBOARD_SRC_W),
                                  (float)(PS2_UI_KEYBOARD_SRC_Y + PS2_UI_KEYBOARD_SRC_H));
    }

    // Dock has no legacy source surface, so keep a hardware cursor there. Other
    // overlays carry the existing software cursor through their remapped source crop.
    if (!keyboard_open && ps2_ui_cursor_y >= PS2_UI_DOCK_Y) {
        gsKit_prim_sprite(gsGlobal,
                          ps2_ui_cursor_x - 5, ps2_ui_cursor_y - 1,
                          ps2_ui_cursor_x + 6, ps2_ui_cursor_y + 2, 8,
                          GS_SETREG_RGBAQ(0x08, 0x08, 0x08, 0x80, 0x00));
        gsKit_prim_sprite(gsGlobal,
                          ps2_ui_cursor_x - 1, ps2_ui_cursor_y - 5,
                          ps2_ui_cursor_x + 2, ps2_ui_cursor_y + 6, 8,
                          GS_SETREG_RGBAQ(0x08, 0x08, 0x08, 0x80, 0x00));
        gsKit_prim_sprite(gsGlobal,
                          ps2_ui_cursor_x - 4, ps2_ui_cursor_y,
                          ps2_ui_cursor_x + 5, ps2_ui_cursor_y + 1, 9,
                          GS_SETREG_RGBAQ(0xf0, 0xf0, 0xf0, 0x80, 0x00));
        gsKit_prim_sprite(gsGlobal,
                          ps2_ui_cursor_x, ps2_ui_cursor_y - 4,
                          ps2_ui_cursor_x + 1, ps2_ui_cursor_y + 5, 9,
                          GS_SETREG_RGBAQ(0xf0, 0xf0, 0xf0, 0x80, 0x00));
    }
}

#endif
