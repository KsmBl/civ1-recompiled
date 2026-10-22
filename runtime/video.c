/* Frame presentation.  Classic mode shows VGA memory (or the native
   driver's screen page).  Widescreen composes the wider screen: the map
   screen uses all of it, other screens are drawn by the game in the top
   left 320x200 and shown centred. */
#include <stdlib.h>
#include <string.h>
#include "cpu.h"
#include "runtime.h"
#include "video.h"
#include "gfx.h"
#include "wide.h"
#include "addons.h"

static uint8_t *comp;
static int off_x, off_y;

/* The map screen has the side panel's border: one colour along the whole
   column x=79 from y=8 to 199, different from the panel at x=0.  Popups may
   cover part of it. */
static int is_map_screen(const uint8_t *scr, int W)
{
    uint8_t c = scr[100 * W + 79];
    if (!c || scr[100 * W] == c || scr[100 * W + 40] == c)
        return 0;
    /* the minimap's bottom border is a line of the same colour */
    for (int x = 0; x < 80; x++)
        if (scr[58 * W + x] != c)
            return 0;
    int n = 0;
    for (int y = 8; y < 200; y++)
        n += scr[y * W + 79] == c;
    return n >= 140;
}

static const uint8_t *compose(int *w, int *h);

/* frame to present: the game's screen plus the features' overlay */
const uint8_t *video_framebuffer(int *w, int *h)
{
    const uint8_t *f = compose(w, h);
    if (!native_overlay_active())
        return f;
    static uint8_t *ov;
    static size_t ovsize;
    size_t n = (size_t)*w * *h;
    if (ovsize < n) {
        free(ov);
        ov = malloc(n);
        ovsize = n;
    }
    memcpy(ov, f, n);
    native_overlay_draw(ov, *w, *h, off_x, off_y);
    return ov;
}

static const uint8_t *compose(int *w, int *h)
{
    if (!gfx_native) {
        *w = 320;
        *h = 200;
        off_x = off_y = 0;
        return &mem[0xA0000];
    }
    const uint8_t *scr = gfx_screen(w, h);
    if (*w == 320 && *h == 200) {
        off_x = off_y = 0;
        return scr;
    }
    int W = *w, H = *h;
    static size_t comp_size;
    if (comp_size < (size_t)W * H) {
        free(comp);
        comp = malloc((size_t)W * H);
        comp_size = (size_t)W * H;
    }
    if (is_map_screen(scr, W)) {
        off_x = off_y = 0;
        memcpy(comp, scr, (size_t)W * H);
        /* continue the side panel below the original screen height */
        /* the panel's bottom border (rows 197-199) moves to the new
           bottom, the rows above it repeat the plain panel background */
        for (int y = 197; y < H; y++) {
            int src = y >= H - 3 ? 199 - (H - 1 - y) : 189 + (y - 197) % 8;
            memcpy(comp + (size_t)y * W, scr + (size_t)src * W, 80);
        }
    } else {
        off_x = (W - 320) / 2;
        off_y = (H - 200) / 2;
        memset(comp, 0, (size_t)W * H);
        for (int y = 0; y < 200; y++)
            memcpy(comp + (size_t)(y + off_y) * W + off_x, scr + (size_t)y * W, 320);
    }
    return comp;
}

/* is the main map screen shown (used by test scripts) */
int video_is_map(void)
{
    int w, h;
    const uint8_t *scr = gfx_native ? gfx_screen(&w, &h) : &mem[0xA0000];
    if (!gfx_native)
        w = 320;
    return is_map_screen(scr, w);
}

double video_pixel_aspect(void)
{
    if (cfg.widescreen)
        return 1.0;     /* square pixels in the widened view */
    return 1.2;         /* 320x200 shown on a 4:3 monitor */
}

int video_map_mouse_x(int game_x) { return game_x + off_x; }
int video_unmap_mouse_x(int fb_x) { return fb_x - off_x; }
int video_map_mouse_y(int game_y) { return game_y + off_y; }
int video_unmap_mouse_y(int fb_y) { return fb_y - off_y; }
