/*
 * Map navigation with the mouse:
 *   - mouse wheel: zoom (widescreen only) -- changes the size of one game
 *     pixel, so the map view shows more or fewer squares;
 *   - left or right drag on the map: scroll the view.
 * A left click that does not move is passed to the game unchanged.
 *
 * Changes are applied while the game waits for orders (hook in its
 * blinking-unit loop), never in the middle of its drawing.
 */
#include <stdlib.h>
#include "native.h"
#include "runtime.h"
#include "addons.h"
#include "gfx.h"
#include "video.h"

#define DRAG_START 4            /* pixels before a press becomes a drag */
#define MARKER_KEY 0x00FE       /* wakes the order loop after a change */

static int held;                /* left button press held back */
static int dragging, drag_button = -1;
static int last_x, last_y, start_x, start_y;
static int acc_x, acc_y;        /* drag distance not yet applied (pixels) */
static int scroll_x, scroll_y;  /* squares to scroll */
static int zoom_steps;          /* wheel steps not yet applied */

static int on_map(int x, int y)
{
    return x >= 80 && y >= 8 && video_is_map();
}

int mapnav_button(int b, int down, int x, int y)
{
    if (down) {
        if (drag_button >= 0 || !on_map(x, y) || (b != 0 && b != 1))
            return 0;
        drag_button = b;
        dragging = 0;
        start_x = last_x = x;
        start_y = last_y = y;
        acc_x = acc_y = 0;
        if (b == 0) {
            held = 1;           /* decide on release: click or drag */
            return 1;
        }
        return 0;               /* right button: the game gets it too */
    }
    if (b != drag_button)
        return 0;
    drag_button = -1;
    if (b == 0 && held) {
        held = 0;
        if (!dragging)          /* it was a click after all */
            mouse_deliver_click(0);
        dragging = 0;
        return 1;
    }
    int was = dragging;
    dragging = 0;
    return was;
}

void mapnav_motion(int x, int y)
{
    if (drag_button < 0)
        return;
    if (!dragging && (abs(x - start_x) > DRAG_START || abs(y - start_y) > DRAG_START))
        dragging = 1;
    if (dragging) {
        acc_x += x - last_x;
        acc_y += y - last_y;
        /* the map follows the mouse: dragging right shows squares to the left */
        while (acc_x >= 16) { acc_x -= 16; scroll_x--; }
        while (acc_x <= -16) { acc_x += 16; scroll_x++; }
        while (acc_y >= 16) { acc_y -= 16; scroll_y--; }
        while (acc_y <= -16) { acc_y += 16; scroll_y++; }
    }
    last_x = x;
    last_y = y;
}

void mapnav_wheel(int steps)
{
    if (cfg.widescreen && gfx_is_wide())
        zoom_steps += steps;
}

/* game pixel size limits for the window: at most what fits 320x200, at
   least so that the view is not wider than half the world (the game's map
   wrap logic assumes that) */
static int scale_ok(int s)
{
    int w = cfg.win_w / s, h = cfg.win_h / s;
    return w >= 320 && h >= 200 && (w - 80) / 16 <= MAP_W / 2 && (h - 8) / 16 <= 48;
}

static int current_scale(void)
{
    return cfg.win_h / g_scr_h;
}

/* hook before 0860:0caa, inside the loop that blinks the active unit while
   waiting for orders */
void hook_map_idle(void)
{
    realtime_idle();
    if (!scroll_x && !scroll_y && !zoom_steps)
        return;
    cpu_t saved = cpu;
    int vx = VIEW_X, vy = VIEW_Y;
    if (zoom_steps) {
        int cx = vx + VIEW_W / 2, cy = vy + VIEW_H / 2;
        int s = current_scale(), ns = s + (zoom_steps > 0 ? 1 : -1);
        zoom_steps = 0;
        if (scale_ok(ns)) {
            int w = (cfg.win_w / ns) & ~1, h = cfg.win_h / ns;
            gfx_resize(w, h);
            wide_init(w, h);
            mouse_screen_resized();
            vx = cx - VIEW_W / 2;
            vy = cy - VIEW_H / 2;
            ovl_msg("Zoom: %d x %d squares", VIEW_W, VIEW_H);
        }
    }
    vx = ((vx + scroll_x) % MAP_W + MAP_W) % MAP_W;
    vy += scroll_y;
    scroll_x = scroll_y = 0;
    if (vy > 50 - VIEW_H)
        vy = 50 - VIEW_H;
    if (vy < 0)
        vy = 0;
    call_lifted(0x1b05, f_1b05_0004, 3, GW(0xe710), vx, vy);
    cpu = saved;
    /* let the order loop start over so the unit is drawn at its new place */
    bios_key_push(0xFF00 | MARKER_KEY);
}

/* the order key hook turns the marker back into "no key" */
int mapnav_is_marker(uint16_t key)
{
    return (key & 0xFF) == MARKER_KEY;
}
