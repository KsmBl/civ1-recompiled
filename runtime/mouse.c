/* INT 33h mouse driver emulation (Microsoft compatible subset). */
#include <string.h>
#include "cpu.h"
#include "runtime.h"
#include "wide.h"
#include "addons.h"

static int mx = 320, my = 100;          /* virtual 640x200 coordinates */
static int buttons;
static int show_count = -1;
static int xmin = 0, xmax = 639, ymin = 0, ymax = 199;
static uint16_t cb_mask, cb_seg, cb_off;
static uint16_t pending_events;
static int press_count[3], release_count[3];
static int press_x[3], press_y[3], release_x[3], release_y[3];
static int mickey_x, mickey_y;
static uint16_t cursor_shape[32];
static int hot_x, hot_y;
static int have_shape;

static void clamp(void)
{
    /* widescreen: the game limits the mouse to 320x200, allow the whole
       wider screen */
    if (g_scr_w > 320 && xmax == 639)
        xmax = 2 * g_scr_w - 1;
    if (g_scr_h > 200 && ymax == 199)
        ymax = g_scr_h - 1;
    if (mx < xmin) mx = xmin;
    if (mx > xmax) mx = xmax;
    if (my < ymin) my = ymin;
    if (my > ymax) my = ymax;
}

void mouse_event_motion(int dx, int dy, int abs_x, int abs_y)
{
    (void)dx; (void)dy;
    mapnav_motion(abs_x / 2, abs_y);
    int ox = mx, oy = my;
    mx = abs_x;
    my = abs_y;
    clamp();
    mickey_x += (mx - ox) * 8 / 2;
    mickey_y += (my - oy) * 16 / 2;
    if (mx != ox || my != oy)
        pending_events |= 1;
}

static void raw_button(int b, int down);

void mouse_event_button(int b, int down)
{
    if (b < 0 || b > 2)
        return;
    if (mapnav_button(b, down, mx / 2, my))
        return;
    raw_button(b, down);
}

/* a click the map navigation held back is delivered to the game */
void mouse_deliver_button(int b, int down)
{
    raw_button(b, down);
}

/* The game polls the button state, so a held back click is delivered as a
   press that lasts a moment (the release follows ~100 ms later). */
static int click_button = -1;
static uint64_t click_release_at;

void mouse_deliver_click(int b)
{
    raw_button(b, 1);
    click_button = b;
    click_release_at = now_us() + 100000;
}

static void click_tick(void)
{
    if (click_button >= 0 && now_us() >= click_release_at) {
        raw_button(click_button, 0);
        click_button = -1;
    }
}

void mouse_screen_resized(void)
{
    xmax = 2 * g_scr_w - 1;
    ymax = g_scr_h - 1;
    clamp();
}

static void raw_button(int b, int down)
{
    static const uint16_t ev_down[3] = {2, 8, 32}, ev_up[3] = {4, 16, 64};
    static int swallowed;
    if (down && native_mouse_filter(b, mx / 2, my)) {
        swallowed |= 1 << b;
        return;
    }
    if (!down && (swallowed & (1 << b))) {
        swallowed &= ~(1 << b);
        return;
    }
    if (down) {
        buttons |= 1 << b;
        press_count[b]++;
        press_x[b] = mx;
        press_y[b] = my;
        pending_events |= ev_down[b];
    } else {
        buttons &= ~(1 << b);
        release_count[b]++;
        release_x[b] = mx;
        release_y[b] = my;
        pending_events |= ev_up[b];
    }
}

/* Called from the poll loop with interrupts enabled. */
void mouse_poll_callback(void)
{
    click_tick();
    static int busy;
    uint16_t ev = pending_events & cb_mask;
    pending_events = 0;
    if (!ev || !cb_seg || busy)
        return;
    busy = 1;
    cpu_t saved = cpu;
    AX = ev;
    BX = (uint16_t)buttons;
    CX = (uint16_t)mx;
    DX = (uint16_t)my;
    SI = (uint16_t)mickey_x;
    DI = (uint16_t)mickey_y;
    cpu.ifl = 0;
    call_far(cb_seg, cb_off, 0xFFFF);
    uint16_t ss = SS, sp = SP;
    cpu = saved;
    (void)ss; (void)sp;
    busy = 0;
}

int mouse_cursor_visible(void) { return show_count >= 0; }

void mouse_cursor_pos(int *x, int *y)
{
    *x = mx / 2;
    *y = my;
}

const uint16_t *mouse_cursor_shape(int *hx, int *hy)
{
    *hx = hot_x;
    *hy = hot_y;
    return have_shape ? cursor_shape : NULL;
}

void mouse_int33(void)
{
    click_tick();
    switch (AX) {
    case 0x00:
    case 0x21:
        AX = 0xFFFF;
        BX = 2;
        show_count = -1;
        xmin = 0; xmax = 639; ymin = 0; ymax = 199;
        cb_mask = 0;
        mx = 320; my = 100;
        break;
    case 0x01:
        if (show_count < 0)
            show_count++;
        break;
    case 0x02:
        show_count--;
        break;
    case 0x03:
        BX = (uint16_t)buttons;
        CX = (uint16_t)mx;
        DX = (uint16_t)my;
        break;
    case 0x04:
        mx = (int16_t)CX;
        my = (int16_t)DX;
        clamp();
        break;
    case 0x05: {
        int b = BX & 3;
        AX = (uint16_t)buttons;
        BX = (uint16_t)press_count[b];
        CX = (uint16_t)press_x[b];
        DX = (uint16_t)press_y[b];
        press_count[b] = 0;
        break;
    }
    case 0x06: {
        int b = BX & 3;
        AX = (uint16_t)buttons;
        BX = (uint16_t)release_count[b];
        CX = (uint16_t)release_x[b];
        DX = (uint16_t)release_y[b];
        release_count[b] = 0;
        break;
    }
    case 0x07:
        xmin = (int16_t)CX < (int16_t)DX ? (int16_t)CX : (int16_t)DX;
        xmax = (int16_t)CX < (int16_t)DX ? (int16_t)DX : (int16_t)CX;
        clamp();
        break;
    case 0x08:
        ymin = (int16_t)CX < (int16_t)DX ? (int16_t)CX : (int16_t)DX;
        ymax = (int16_t)CX < (int16_t)DX ? (int16_t)DX : (int16_t)CX;
        clamp();
        break;
    case 0x09:
        hot_x = (int16_t)BX;
        hot_y = (int16_t)CX;
        for (int i = 0; i < 32; i++)
            cursor_shape[i] = RW(ES, (uint16_t)(DX + i * 2));
        have_shape = 1;
        break;
    case 0x0A:
        break;
    case 0x0B:
        CX = (uint16_t)mickey_x;
        DX = (uint16_t)mickey_y;
        mickey_x = mickey_y = 0;
        break;
    case 0x0C:
        cb_mask = CX;
        cb_seg = ES;
        cb_off = DX;
        break;
    case 0x0F:
    case 0x13:
    case 0x1A:
    case 0x1D:
        break;
    case 0x14: {
        uint16_t m = cb_mask, s = cb_seg, o = cb_off;
        cb_mask = CX; cb_seg = ES; cb_off = DX;
        CX = m; ES = s; DX = o;
        break;
    }
    case 0x15:
        BX = 64;
        break;
    case 0x24:
        BX = 0x0626;
        CH = 4;
        CL = 0;
        break;
    default:
        if (verbose)
            warn("unhandled int 33h AX=%04x", AX);
        break;
    }
}
