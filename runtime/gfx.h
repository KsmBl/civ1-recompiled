/* Native graphics driver (replacement for MGRAPHIC.EXE). */
#pragma once
#include <stdint.h>
#include "cpu.h"

extern int gfx_native;
extern int gfx_w, gfx_h;
void gfx_init(int native, int w, int h);
void gfx_module_loaded(uint16_t seg);
func_t gfx_lookup(uint16_t off);
const uint8_t *gfx_screen(int *w, int *h);
uint8_t *gfx_page_pixels(uint16_t seg);
void gfx_shadow_report(void);
void gfx_fill_rect(uint16_t page, int x, int y, int w, int h, uint16_t color);
int gfx_is_wide(void);
uint8_t gfx_get_pixel(uint16_t seg, int x, int y);
uint16_t gfx_page_seg(int page);
void gfx_put_pixel(int page, int x, int y, uint8_t color);
int gfx_map_page_w(void);
void gfx_copy_rect(int page, int sx, int sy, int w, int h, int dx, int dy);    /* native driver only */
void gfx_resize(int w, int h);
void gfx_set_map_page_w(int w);
