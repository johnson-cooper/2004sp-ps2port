#ifdef __PS2__

// ps2.yaml redirects the two PS2SDK entry points used by the legacy presenter/input
// path to this file.  Undefine the build macros before including the real SDK headers
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

// Milestone 1 modern PS2 UI: keep the proven 2004 software UI/layout alive off-screen
// and reorganize only the final GS presentation.  This avoids changing World3D,
// projection/picking math, Component coordinates, or allocating another framebuffer.
#define PS2_UI_VIEW_H 418
#define PS2_UI_DOCK_Y PS2_UI_VIEW_H
#define PS2_UI_DOCK_H (SCREEN_FB_HEIGHT - PS2_UI_DOCK_Y)

#define PS2_UI_PANEL_X 438
#define PS2_UI_PANEL_Y 145
#define PS2_UI_PANEL_W 192
#define PS2_UI_PANEL_H 261

#define PS2_UI_DOCK_BUTTON_W 40
#define PS2_UI_DOCK_BUTTON_H 44
#define PS2_UI_DOCK_BUTTON_GAP 7
#define PS2_UI_DOCK_BUTTON_COUNT 13
#define PS2_UI_DOCK_BUTTONS_W \
    (PS2_UI_DOCK_BUTTON_COUNT * PS2_UI_DOCK_BUTTON_W + \
     (PS2_UI_DOCK_BUTTON_COUNT - 1) * PS2_UI_DOCK_BUTTON_GAP)
#define PS2_UI_DOCK_BUTTON_X ((SCREEN_FB_WIDTH - PS2_UI_DOCK_BUTTONS_W) / 2)
#define PS2_UI_DOCK_BUTTON_Y (PS2_UI_DOCK_Y + 9)

// These are the real fixed-mode tab slots.  Tab 7 is intentionally absent in the
// original layout; the lower row begins at tab 8.
static const int ps2_ui_tabs[PS2_UI_DOCK_BUTTON_COUNT] = {
    0, 1, 2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13
};

// Source positions of the 13 already-rendered sideicons in the legacy 765x503
// logical canvas.  Reusing them costs no new sprite/cache memory.
static const int ps2_ui_icon_x[PS2_UI_DOCK_BUTTON_COUNT] = {
    545, 569, 598, 631, 669, 696, 724,
    570, 598, 633, 670, 697, 722
};
static const int ps2_ui_icon_y[PS2_UI_DOCK_BUTTON_COUNT] = {
    173, 171, 171, 172, 173, 171, 173,
    468, 469, 470, 468, 468, 468
};

static bool ps2_ui_panel_open;
static int ps2_ui_cursor_x = SCREEN_FB_WIDTH / 2;
static int ps2_ui_cursor_y = PS2_UI_VIEW_H / 2;
static bool ps2_ui_cross_raw_was_down;
static bool ps2_ui_square_raw_was_down;
static bool ps2_ui_triangle_raw_was_down;
static bool ps2_ui_cross_consumed_until_release;

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

static void ps2_ui_select_tab(Client *c, int tab) {
    if (!c || tab < 0 || tab >= 15 || c->tab_interface_id[tab] == -1) {
        return;
    }

    if (ps2_ui_panel_open && c->selected_tab == tab) {
        ps2_ui_panel_open = false;
        return;
    }

    c->selected_tab = tab;
    c->redraw_sidebar = true;
    c->redraw_sideicons = true;
    ps2_ui_panel_open = true;
}

static void ps2_ui_map_cursor_to_legacy(Client *c) {
    if (!c || !c->shell) {
        return;
    }

    // The floating panel is just the already-rendered legacy sidebar moved by the GS.
    // Map pointer input back to its original 553,205 coordinate space so every existing
    // inventory/interface hit-test and context menu continues to work unchanged.
    if (ps2_ui_panel_open &&
        ps2_ui_cursor_x >= PS2_UI_PANEL_X && ps2_ui_cursor_x < PS2_UI_PANEL_X + PS2_UI_PANEL_W &&
        ps2_ui_cursor_y >= PS2_UI_PANEL_Y && ps2_ui_cursor_y < PS2_UI_PANEL_Y + PS2_UI_PANEL_H) {
        c->shell->mouse_x = 553 + (ps2_ui_cursor_x - PS2_UI_PANEL_X) * 190 / PS2_UI_PANEL_W;
        c->shell->mouse_y = 205 + (ps2_ui_cursor_y - PS2_UI_PANEL_Y) * 261 / PS2_UI_PANEL_H;
        return;
    }

    // Bottom-dock clicks are consumed here, not by the 2004 UI.  Keep the legacy
    // pointer somewhere harmless while it is over the dock.
    if (ps2_ui_cursor_y >= PS2_UI_DOCK_Y) {
        c->shell->mouse_x = 0;
        c->shell->mouse_y = 0;
        return;
    }

    // Final GS presentation scales the original 512x334 game view to 640x418.
    // Convert the visible cursor back to that exact original input space so world
    // picking/click-to-walk remains aligned with what the player sees.
    c->shell->mouse_x = 4 + ps2_ui_cursor_x * 512 / SCREEN_FB_WIDTH;
    c->shell->mouse_y = 4 + ps2_ui_cursor_y * 334 / PS2_UI_VIEW_H;
}

int ps2_modern_padRead(int port, int slot, struct padButtonStatus *buttons) {
    int result = padRead(port, slot, buttons);
    if (result <= 0 || !buttons || !ps2_ui_active()) {
        return result;
    }

    Client *c = ps2_crash_client;

    // Own the visible cursor in physical 640x480 presentation coordinates.  The
    // legacy poller receives neutral left-stick values below so it cannot apply a
    // second movement step after we map this cursor back into old UI coordinates.
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

    int dock_button = ps2_ui_button_at(ps2_ui_cursor_x, ps2_ui_cursor_y);
    if (raw_cross && !ps2_ui_cross_raw_was_down && dock_button >= 0) {
        ps2_ui_select_tab(c, ps2_ui_tabs[dock_button]);
        ps2_ui_cross_consumed_until_release = true;
    }
    if (!raw_cross) {
        ps2_ui_cross_consumed_until_release = false;
    }

    // Square remains the quick Inventory shortcut, but now follows the same toggle
    // contract as clicking the Inventory dock button instead of being open-only.
    if (raw_square && !ps2_ui_square_raw_was_down) {
        ps2_ui_select_tab(c, 3);
    }

    // Triangle closes the floating panel first; if no panel is open it falls through
    // to the legacy Back behavior unchanged.
    bool consume_triangle = false;
    if (raw_triangle && !ps2_ui_triangle_raw_was_down && ps2_ui_panel_open) {
        ps2_ui_panel_open = false;
        consume_triangle = true;
    }

    ps2_ui_cross_raw_was_down = raw_cross;
    ps2_ui_square_raw_was_down = raw_square;
    ps2_ui_triangle_raw_was_down = raw_triangle;

    ps2_ui_map_cursor_to_legacy(c);

    // Prevent platform_poll_events() from moving/clicking the legacy canvas a second
    // time for input that belongs to the modern dock.  Right-click on the dock is also
    // swallowed so it cannot open a world menu at the harmless legacy coordinate.
    buttons->ljoy_h = 128;
    buttons->ljoy_v = 128;
    if (ps2_ui_cross_consumed_until_release) {
        buttons->btns |= PAD_CROSS;
    }
    if (ps2_ui_cursor_y >= PS2_UI_DOCK_Y) {
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

    // The game view now owns the screen.  This is the same proven 512x334 software
    // viewport (logical position 4,4), scaled by the GS only at presentation time.
    ps2_ui_draw_textured_rect(gsGlobal, texture,
                              0.0f, 0.0f, (float)SCREEN_FB_WIDTH, (float)PS2_UI_VIEW_H,
                              4.0f, 4.0f, 516.0f, 338.0f);

    // Compact dark bottom dock.  No animation or heap work: response is immediate and
    // the only per-frame cost is a handful of tiny GS sprites already in the GIF queue.
    gsKit_prim_sprite(gsGlobal,
                      0, PS2_UI_DOCK_Y, SCREEN_FB_WIDTH, SCREEN_FB_HEIGHT, 1,
                      GS_SETREG_RGBAQ(0x16, 0x19, 0x1f, 0x80, 0x00));
    gsKit_prim_sprite(gsGlobal,
                      0, PS2_UI_DOCK_Y, SCREEN_FB_WIDTH, PS2_UI_DOCK_Y + 2, 2,
                      GS_SETREG_RGBAQ(0x45, 0x4b, 0x57, 0x80, 0x00));

    for (int i = 0; i < PS2_UI_DOCK_BUTTON_COUNT; i++) {
        int bx = PS2_UI_DOCK_BUTTON_X + i * (PS2_UI_DOCK_BUTTON_W + PS2_UI_DOCK_BUTTON_GAP);
        int tab = ps2_ui_tabs[i];
        bool selected = ps2_ui_panel_open && c->selected_tab == tab;
        bool available = c->tab_interface_id[tab] != -1;

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

        if (available) {
            ps2_ui_draw_textured_rect(gsGlobal, texture,
                                      (float)(bx + 8), (float)(PS2_UI_DOCK_BUTTON_Y + 7),
                                      (float)(bx + 32), (float)(PS2_UI_DOCK_BUTTON_Y + 31),
                                      (float)ps2_ui_icon_x[i], (float)ps2_ui_icon_y[i],
                                      (float)(ps2_ui_icon_x[i] + 24), (float)(ps2_ui_icon_y[i] + 24));
        }
    }

    if (ps2_ui_panel_open && c->selected_tab >= 0 && c->selected_tab < 15 &&
        c->tab_interface_id[c->selected_tab] != -1) {
        // Floating panel: source is the existing dark PS2_SIMPLE_UI sidebar, including
        // the real inventory/interface contents and any legacy context menu/cursor drawn
        // into it.  A subtle border separates it from gameplay without hiding the view.
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

    // The legacy software cursor is naturally carried through when it is over the
    // scaled viewport or floating sidebar.  The dock has no legacy source surface, so
    // draw a tiny high-contrast cursor there directly through the GS.
    if (ps2_ui_cursor_y >= PS2_UI_DOCK_Y) {
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
