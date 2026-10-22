/* Presentation of the emulated display (the hook point for widescreen). */
#pragma once
#include <stdint.h>

/* Returns the current frame (8-bit palette indices) and its size. */
const uint8_t *video_framebuffer(int *w, int *h);
/* Height/width ratio of one game pixel on the original display. */
double video_pixel_aspect(void);
/* Mouse x in game coordinates <-> framebuffer coordinates */
int video_map_mouse_x(int game_x);
int video_unmap_mouse_x(int fb_x);
int video_is_map(void);
int video_map_mouse_y(int game_y);
int video_unmap_mouse_y(int fb_y);
