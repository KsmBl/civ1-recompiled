/*
 * DOS platform layer (DJGPP): real VGA mode 13h, keyboard IRQ, mouse
 * driver, PIT based timing.  Sound port writes go to the real hardware.
 */
#include <crt0.h>
#include <dpmi.h>
#include <go32.h>
#include <pc.h>
#include <sys/farptr.h>
#include <sys/movedata.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cpu.h"
#include "runtime.h"
#include "video.h"

int _crt0_startup_flags = _CRT0_FLAG_LOCK_MEMORY;

static _go32_dpmi_seginfo old_kb, new_kb;
static volatile uint8_t kbuf[256];
static volatile unsigned kb_head, kb_tail;
static int have_mouse, mouse_x, mouse_y, mouse_b;
static uint8_t last_pal[256][3];
static int pal_valid;
static uint8_t frame[320 * 200];

static void kb_handler(void)
{
    uint8_t sc = inportb(0x60);
    kbuf[kb_tail & 255] = sc;
    kb_tail++;
    outportb(0x20, 0x20);
}

int plat_init(void)
{
    __dpmi_regs r;
    memset(&r, 0, sizeof r);
    r.x.ax = 0x0013;
    __dpmi_int(0x10, &r);

    _go32_dpmi_get_protected_mode_interrupt_vector(9, &old_kb);
    new_kb.pm_offset = (unsigned long)kb_handler;
    new_kb.pm_selector = _go32_my_cs();
    _go32_dpmi_allocate_iret_wrapper(&new_kb);
    _go32_dpmi_set_protected_mode_interrupt_vector(9, &new_kb);

    memset(&r, 0, sizeof r);
    r.x.ax = 0;
    __dpmi_int(0x33, &r);
    have_mouse = r.x.ax == 0xFFFF;
    return 0;
}

void plat_shutdown(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    _go32_dpmi_set_protected_mode_interrupt_vector(9, &old_kb);
    _go32_dpmi_free_iret_wrapper(&new_kb);
    outportb(0x61, inportb(0x61) & 0xFC);     /* speaker off */
    __dpmi_regs r;
    memset(&r, 0, sizeof r);
    r.x.ax = 0x0003;
    __dpmi_int(0x10, &r);
}

void plat_audio_lock(int lock)
{
    (void)lock;
}

uint64_t plat_time_us(void)
{
    return (uint64_t)uclock() * 1000000ULL / UCLOCKS_PER_SEC;
}

void plat_sleep_us(int us)
{
    uint64_t end = plat_time_us() + (uint64_t)us;
    while (plat_time_us() < end)
        __dpmi_yield();
}

static void draw_cursor(void)
{
    if (!mouse_cursor_visible())
        return;
    int hx, hy, x, y;
    const uint16_t *shape = mouse_cursor_shape(&hx, &hy);
    mouse_cursor_pos(&x, &y);
    x -= hx;
    y -= hy;
    static const uint16_t arrow_s[16] = {0x3FFF, 0x1FFF, 0x0FFF, 0x07FF, 0x03FF,
        0x01FF, 0x00FF, 0x007F, 0x003F, 0x001F, 0x01FF, 0x10FF, 0x30FF, 0xF87F,
        0xF87F, 0xFC7F};
    static const uint16_t arrow_c[16] = {0x0000, 0x4000, 0x6000, 0x7000, 0x7800,
        0x7C00, 0x7E00, 0x7F00, 0x7F80, 0x7C00, 0x6C00, 0x4600, 0x0600, 0x0300,
        0x0300, 0x0000};
    for (int r = 0; r < 16; r++) {
        int py = y + r;
        if (py < 0 || py >= 200)
            continue;
        uint16_t scr = shape ? shape[r] : arrow_s[r];
        uint16_t cur = shape ? shape[16 + r] : arrow_c[r];
        for (int c = 0; c < 16; c++) {
            int px = x + c;
            if (px < 0 || px >= 320)
                continue;
            uint8_t *p = &frame[py * 320 + px];
            if (!((scr >> (15 - c)) & 1))
                *p = 0;
            if ((cur >> (15 - c)) & 1)
                *p ^= 0x0F;
        }
    }
}

void plat_present(void)
{
    int w, h;
    const uint8_t *fb = video_framebuffer(&w, &h);
    if (!fb || w != 320 || h != 200)
        return;
    memcpy(frame, fb, sizeof frame);
    draw_cursor();
    if (!pal_valid || memcmp(last_pal, vga_dac, sizeof last_pal)) {
        outportb(0x3C8, 0);
        for (int i = 0; i < 256; i++)
            for (int c = 0; c < 3; c++)
                outportb(0x3C9, vga_dac[i][c]);
        memcpy(last_pal, vga_dac, sizeof last_pal);
        pal_valid = 1;
    }
    dosmemput(frame, sizeof frame, 0xA0000);
}

void plat_pump_events(void)
{
    while (kb_head != kb_tail) {
        uint8_t sc = kbuf[kb_head & 255];
        kb_head++;
        kbd_scancode(sc);
    }
    if (have_mouse) {
        __dpmi_regs r;
        memset(&r, 0, sizeof r);
        r.x.ax = 3;
        __dpmi_int(0x33, &r);
        int x = r.x.cx, y = r.x.dx, b = r.x.bx;
        if (x != mouse_x || y != mouse_y)
            mouse_event_motion(0, 0, x, y);
        for (int i = 0; i < 3; i++)
            if ((b ^ mouse_b) & (1 << i))
                mouse_event_button(i, (b >> i) & 1);
        mouse_x = x;
        mouse_y = y;
        mouse_b = b;
    }
}
