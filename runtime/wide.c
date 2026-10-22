/* Screen and map view geometry (classic 320x200 or widescreen). */
#include "wide.h"

int16_t g_scr_w = 320, g_scr_h = 200;
int16_t g_view_w = 15, g_view_h = 12;
int g_map_mode;
int16_t g_map_w = 80;
int g_map_wide;
int g_barbarians = 1;

void wide_init(int scr_w, int scr_h)
{
    g_scr_w = (int16_t)scr_w;
    g_scr_h = (int16_t)scr_h;
    g_view_w = (int16_t)((scr_w - 80) / 16);
    g_view_h = (int16_t)((scr_h - 8) / 16);
}
