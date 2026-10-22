/*
 * Map view: changes needed for views larger than the original 15x12.
 */
#include "native.h"

/* f_1b05_0004(player, x, y): set the view origin and redraw the map.
   The original draws the squares in a pseudo random order over a 16x16
   grid; squares beyond that grid are drawn afterwards here. */
void f_1b05_0004(void)
{
    uint16_t player = FARG(0), x = FARG(1), y = FARG(2);
    g_map_mode = 1;
    if (VIEW_W <= 16 && VIEW_H <= 16) {
        lifted_f_1b05_0004();           /* same frame: exactly the original */
        return;
    }
    call_lifted(0x1b05, lifted_f_1b05_0004, 3, player, x, y);
    uint16_t mask = (uint16_t)(1u << (player & 15));
    for (int row = 0; row < VIEW_H; row++)
        for (int col = 0; col < VIEW_W; col++) {
            if (col < 16 && row < 16)
                continue;
            int mx = (VIEW_X + col) % MAP_W;
            int my = VIEW_Y + row;
            int visible = GW(0xe726) != 0;
            if (!visible) {
                int8_t v = (int8_t)RB(VISSEG, (uint16_t)(0x7ff8 + mx * 50 + my));
                visible = (mask & (uint16_t)(int16_t)v) != 0;
            }
            if (visible)
                call_lifted(0x1b05, f_1b05_11c6, 2, mx, my);
            else
                call_lifted(0x0000, f_0000_0bec, 6, GW(0xaa), 80 + 16 * col,
                            8 + 16 * row, 16, 16, 0);
        }
    RET_FAR(0);
}
