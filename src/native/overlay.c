/*
 * On-screen overlay for features added to the game: short messages and
 * the city screen's "GOLD" button.  Drawn onto the presented frame only,
 * never into game memory, so the game's own drawing is not disturbed.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "native.h"
#include "runtime.h"
#include "addons.h"
#include "video.h"
#include "mp.h"

/* 5x7 font, one byte per row (bit 4 = leftmost pixel) */
static const struct { char c; uint8_t row[7]; } font[] = {
    {'A', {14, 17, 17, 31, 17, 17, 17}}, {'B', {30, 17, 17, 30, 17, 17, 30}},
    {'C', {14, 17, 16, 16, 16, 17, 14}}, {'D', {30, 17, 17, 17, 17, 17, 30}},
    {'E', {31, 16, 16, 30, 16, 16, 31}}, {'F', {31, 16, 16, 30, 16, 16, 16}},
    {'G', {14, 17, 16, 23, 17, 17, 15}}, {'H', {17, 17, 17, 31, 17, 17, 17}},
    {'I', {14, 4, 4, 4, 4, 4, 14}},      {'J', {7, 2, 2, 2, 2, 18, 12}},
    {'K', {17, 18, 20, 24, 20, 18, 17}}, {'L', {16, 16, 16, 16, 16, 16, 31}},
    {'M', {17, 27, 21, 21, 17, 17, 17}}, {'N', {17, 17, 25, 21, 19, 17, 17}},
    {'O', {14, 17, 17, 17, 17, 17, 14}}, {'P', {30, 17, 17, 30, 16, 16, 16}},
    {'Q', {14, 17, 17, 17, 21, 18, 13}}, {'R', {30, 17, 17, 30, 20, 18, 17}},
    {'S', {15, 16, 16, 14, 1, 1, 30}},   {'T', {31, 4, 4, 4, 4, 4, 4}},
    {'U', {17, 17, 17, 17, 17, 17, 14}}, {'V', {17, 17, 17, 17, 17, 10, 4}},
    {'W', {17, 17, 17, 21, 21, 21, 10}}, {'X', {17, 17, 10, 4, 10, 17, 17}},
    {'Y', {17, 17, 10, 4, 4, 4, 4}},     {'Z', {31, 1, 2, 4, 8, 16, 31}},
    {'0', {14, 17, 19, 21, 25, 17, 14}}, {'1', {4, 12, 4, 4, 4, 4, 14}},
    {'2', {14, 17, 1, 2, 4, 8, 31}},     {'3', {31, 2, 4, 2, 1, 17, 14}},
    {'4', {2, 6, 10, 18, 31, 2, 2}},     {'5', {31, 16, 30, 1, 1, 17, 14}},
    {'6', {6, 8, 16, 30, 17, 17, 14}},   {'7', {31, 1, 2, 4, 8, 8, 8}},
    {'8', {14, 17, 17, 14, 17, 17, 14}}, {'9', {14, 17, 17, 15, 1, 2, 12}},
    {'-', {0, 0, 0, 31, 0, 0, 0}},       {'+', {0, 4, 4, 31, 4, 4, 0}},
    {':', {0, 12, 12, 0, 12, 12, 0}},    {'.', {0, 0, 0, 0, 0, 12, 12}},
    {'!', {4, 4, 4, 4, 4, 0, 4}},        {'(', {2, 4, 8, 8, 8, 4, 2}},
    {')', {8, 4, 2, 2, 2, 4, 8}},        {'/', {1, 1, 2, 4, 8, 16, 16}},
    {'?', {14, 17, 1, 2, 4, 0, 4}},      {',', {0, 0, 0, 0, 12, 4, 8}},
};

static const uint8_t *glyph(char c)
{
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 32);
    for (size_t i = 0; i < sizeof font / sizeof font[0]; i++)
        if (font[i].c == c)
            return font[i].row;
    return NULL;
}

/* palette index closest to an RGB colour (6 bit components) */
static uint8_t nearest(int r, int g, int b)
{
    int best = 0, bd = 1 << 30;
    for (int i = 0; i < 256; i++) {
        int dr = vga_dac[i][0] - r, dg = vga_dac[i][1] - g, db = vga_dac[i][2] - b;
        int d = dr * dr + dg * dg + db * db;
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return (uint8_t)best;
}

static void fill(uint8_t *buf, int W, int H, int x, int y, int w, int h, uint8_t c)
{
    for (int r = y; r < y + h; r++)
        for (int k = x; k < x + w; k++)
            if (r >= 0 && r < H && k >= 0 && k < W)
                buf[r * W + k] = c;
}

static void text(uint8_t *buf, int W, int H, int x, int y, const char *s, uint8_t c)
{
    for (; *s; s++, x += 6) {
        const uint8_t *g = glyph(*s);
        if (!g)
            continue;
        for (int r = 0; r < 7; r++)
            for (int k = 0; k < 5; k++)
                if ((g[r] >> (4 - k)) & 1) {
                    int px = x + k, py = y + r;
                    if (px >= 0 && px < W && py >= 0 && py < H)
                        buf[py * W + px] = c;
                }
    }
}

int text_width(const char *s)
{
    return (int)strlen(s) * 6 - 1;
}

/* ---------------------------------------------------------------- */

static char msg[96];
static uint64_t msg_until;

void ovl_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    msg_until = now_us() + 3500000;
    logmsg("%s", msg);
}

int native_overlay_active(void)
{
    return city_screen_city() >= 0 || (msg[0] && now_us() < msg_until) ||
           (realtime_on() && video_is_map()) || mp_overlay_active();
}

void native_overlay_draw(uint8_t *buf, int W, int H, int ox, int oy)
{
    uint8_t white = nearest(63, 63, 63), black = nearest(0, 0, 0);
    uint8_t yellow = nearest(63, 55, 0), grey = nearest(22, 22, 22);
    int city = city_screen_city();
    if (city >= 0) {
        int on = city_produces_gold(city);
        int x = ox + GOLD_BTN_X, y = oy + GOLD_BTN_Y;
        fill(buf, W, H, x, y, GOLD_BTN_W, GOLD_BTN_H, on ? yellow : grey);
        fill(buf, W, H, x, y, GOLD_BTN_W, 1, white);
        const char *label = on ? "GOLD!" : "GOLD";
        text(buf, W, H, x + (GOLD_BTN_W - text_width(label)) / 2, y + 1, label,
             on ? black : white);
    }
    if (realtime_on() && video_is_map() && city < 0) {
        /* turn clock at the bottom of the status panel */
        int x = ox + 2, y = oy + H - 9, w = 76;
        fill(buf, W, H, x, y, w, 8, black);
        int p = realtime_progress() * (w - 26) / 1000;
        fill(buf, W, H, x + 1, y + 1, p, 6, realtime_paused() ? grey : yellow);
        char t[16];
        int s10 = realtime_turn_secs10();
        if (realtime_paused())
            snprintf(t, sizeof t, "PAUSE");
        else
            snprintf(t, sizeof t, "%d.%ds", s10 / 10, s10 % 10);
        text(buf, W, H, x + w - 24, y + 1, t, white);
    }
    mp_overlay_draw(buf, W, H, ox, oy, fill, text, white, black, yellow, grey);
    if (msg[0] && now_us() < msg_until) {
        int w = text_width(msg) + 8;
        int x = (W - w) / 2, y = 10;
        fill(buf, W, H, x, y, w, 11, black);
        text(buf, W, H, x + 4, y + 2, msg, yellow);
    }
}
