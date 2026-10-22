/*
 * Screen / map view geometry used by the patched game code (see
 * tools/patches.txt).  In classic mode these are the original constants:
 * a 320x200 screen with a 15x12 tile map view at (80, 8).
 */
#pragma once
#include <stdint.h>

extern int16_t g_scr_w, g_scr_h;      /* screen (page) size in pixels */
extern int16_t g_view_w, g_view_h;    /* map view size in tiles */

#define SCR_W g_scr_w
#define SCR_H g_scr_h
#define VIEW_W g_view_w
#define VIEW_H g_view_h
/* x of the first pixel right of the map view / y of the last tile row */
#define MAP_RIGHT (80 + 16 * g_view_w)
#define MAP_LAST_Y (8 + 16 * (g_view_h - 1))

/* set while the main map screen is shown; other screens are drawn in the
   top left 320x200 and presented centred */
extern int g_map_mode;

/* World map size.  The original is 80 x 50; wider maps keep the height.
   The map's layers in graphics page 2 are 4 columns at x = MAP_LX(k): 80
   pixels apart in the original layout.  For wider maps column 0 stays at
   x 0 and columns 1-3 move to x 320, 480, 640, because the game keeps
   pictures in page 2 at x 160-319, y 50-199 (unused by the original
   layout).  That limits the width to 160 squares.  And the per-square "explored"
   table (DS:7FF8, x*50+y) moves to segment VISSEG when it no longer fits
   into the data segment. */
extern int16_t g_map_w;
extern int g_map_wide;         /* wide storage layout in use */
#define MAP_W g_map_w
#define MAP_H 50
#define MAP_WIDE g_map_wide
#define MAP_W_MAX 160
#define MAP_LX(k) (MAP_WIDE ? ((k) ? 160 + 160 * (k) : 0) : 80 * (k))
#define VISSEG (MAP_WIDE ? 0xFFFF : DS)
#define VIS_SIZE (g_map_w * 50)
/* The AI's continent tables: one byte per block of 4x4 squares, 13 per
   column (DS:EA64 connections, DS:7EB8 the same for sea, DS:E736 path
   search).  20 columns fill them; wider maps move them to CONTSEG. */
#define BLK_W (g_map_w / 4)
#define BLK_SIZE (BLK_W * 13)
#define CONTSEG (MAP_WIDE ? 0xD000 : DS)

/* game options used by tools/patches.txt */
extern int g_barbarians;       /* 0: no barbarian raids, uprisings or huts */

void wide_init(int scr_w, int scr_h);
