/*
 * The game's own drawing library (root segment 0) keeps polygon spans in
 * tables of 200 rows.  In widescreen the screen is taller, so the
 * rectangle fill is done natively there.
 */
#include "native.h"
#include "gfx.h"

/* f_0000_0bec(ctx, x, y, w, h, color): filled rectangle */
void f_0000_0bec(void)
{
    if (!gfx_is_wide()) {
        lifted_f_0000_0bec();
        return;
    }
    uint16_t ctx = FARG(0);
    int16_t x = (int16_t)FARG(1), y = (int16_t)FARG(2);
    int16_t w = (int16_t)FARG(3), h = (int16_t)FARG(4);
    uint16_t color = FARG(5);
    if (w > 0 && h > 0) {
        /* clear the span range of the previous fill (0c80) */
        int16_t lo = (int16_t)GW(0x57fa), hi = (int16_t)GW(0x57fc);
        if (lo >= 0) {
            for (int r = lo; r <= hi && r < 200; r++) {
                WW(DS, (uint16_t)(0x54d6 + 2 * r), 0xffff);
                WW(DS, (uint16_t)(0x5666 + 2 * r), 0);
            }
            WW(DS, 0x57fa, 0xffff);
            WW(DS, 0x57fc, 0);
        }
        gfx_fill_rect(GW(ctx), x + (int16_t)GW((uint16_t)(ctx + 2)),
                      y + (int16_t)GW((uint16_t)(ctx + 4)), w, h, color);
    }
    RET_FAR(0);
}
