/*
 * BIOS emulation: interrupt vector table, BIOS data area, INT 10h video,
 * INT 16h keyboard, INT 1Ah clock, IRQ0 / IRQ1 default handlers.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"
#include "runtime.h"
#include "addons.h"

int video_mode = 3;
uint8_t vga_dac[256][3];

#define BDA 0x40

/* scan code -> ASCII (unshifted, shifted) for scan codes 0x00-0x39 */
static const char kb_lo[] =
    "\0\x1b" "1234567890-=\b\tqwertyuiop[]\r\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 ";
static const char kb_hi[] =
    "\0\x1b" "!@#$%^&*()_+\b\tQWERTYUIOP{}\r\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 ";

static void kbuf_put(uint16_t w)
{
    uint16_t head = RW(BDA, 0x1A), tail = RW(BDA, 0x1C);
    uint16_t next = (uint16_t)(tail + 2);
    if (next >= 0x3E)
        next = 0x1E;
    if (next == head)
        return;     /* buffer full */
    WW(BDA, tail, w);
    WW(BDA, 0x1C, next);
}

static int kbuf_get(uint16_t *w, int remove)
{
    uint16_t head = RW(BDA, 0x1A), tail = RW(BDA, 0x1C);
    if (head == tail)
        return 0;
    *w = RW(BDA, head);
    if (remove) {
        head = (uint16_t)(head + 2);
        if (head >= 0x3E)
            head = 0x1E;
        WW(BDA, 0x1A, head);
    }
    return 1;
}

extern uint8_t kbd_port60;
static int kbd_e0;

/* INT 9: translate scan code from port 60h into the BIOS buffer */
static void bios_int9(void)
{
    uint8_t sc = kbd_port60;
    uint8_t flags = RB(BDA, 0x17);
    if (sc == 0xE0) {
        kbd_e0 = 1;
        port_out8(0x20, 0x20);
        return;
    }
    int e0 = kbd_e0;
    kbd_e0 = 0;
    int brk = sc & 0x80;
    uint8_t k = sc & 0x7F;
    switch (k) {
    case 0x2A: case 0x36:     /* shift */
        if (e0)
            break;
        if (brk)
            flags &= (uint8_t)~(k == 0x2A ? 2 : 1);
        else
            flags |= k == 0x2A ? 2 : 1;
        break;
    case 0x1D:
        flags = brk ? flags & ~4 : flags | 4;
        break;
    case 0x38:
        flags = brk ? flags & ~8 : flags | 8;
        break;
    case 0x3A:
        if (!brk)
            flags ^= 0x40;
        break;
    case 0x45:
        if (!brk)
            flags ^= 0x20;
        break;
    default:
        if (brk)
            break;
        {
            int shift = (flags & 3) != 0;
            int ctrl = (flags & 4) != 0;
            int alt = (flags & 8) != 0;
            uint8_t ascii = 0;
            uint8_t scan = k;
            if (k < sizeof kb_lo - 1 && !e0) {
                ascii = (uint8_t)(shift ? kb_hi[k] : kb_lo[k]);
                if ((flags & 0x40) && ascii >= 'a' && ascii <= 'z' && !shift)
                    ascii -= 32;
                else if ((flags & 0x40) && ascii >= 'A' && ascii <= 'Z' && shift)
                    ascii += 32;
                if (ctrl && ascii >= 'a' && ascii <= 'z')
                    ascii = (uint8_t)(ascii - 'a' + 1);
                else if (ctrl && ascii >= 'A' && ascii <= 'Z')
                    ascii = (uint8_t)(ascii - 'A' + 1);
                if (alt)
                    ascii = 0;
            } else if (k >= 0x47 && k <= 0x53) {
                /* keypad */
                static const char pad[] = "789-456+1230.";
                int num = (flags & 0x20) != 0;
                if (shift)
                    num = !num;
                if (num && !e0)
                    ascii = (uint8_t)pad[k - 0x47];
                else
                    ascii = (k == 0x4A || k == 0x4E) ? (uint8_t)pad[k - 0x47] : 0;
                if (e0)
                    ascii = 0xE0 == 0 ? 0 : 0;   /* grey keys: ascii 0 */
            } else if (k >= 0x3B && k <= 0x44) {
                if (shift)
                    scan = (uint8_t)(k + 0x19);
                else if (ctrl)
                    scan = (uint8_t)(k + 0x23);
                else if (alt)
                    scan = (uint8_t)(k + 0x2D);
            } else if (k == 0x1C && e0) {
                ascii = '\r';
            } else if (k == 0x35 && e0) {
                ascii = '/';
            }
            if (alt && k >= 0x02 && k <= 0x0D)
                scan = (uint8_t)(k + 0x76);
            if (!native_key_filter((uint16_t)(scan << 8 | ascii)))
                kbuf_put((uint16_t)(scan << 8 | ascii));
        }
        break;
    }
    WB(BDA, 0x17, flags);
    port_out8(0x20, 0x20);
}

static void bios_int8(void)
{
    uint32_t t = (uint32_t)RW(BDA, 0x6C) | (uint32_t)RW(BDA, 0x6E) << 16;
    t++;
    if (t >= 0x1800B0) {
        t = 0;
        WB(BDA, 0x70, 1);
    }
    WW(BDA, 0x6C, (uint16_t)t);
    WW(BDA, 0x6E, (uint16_t)(t >> 16));
    /* diskette motor-off countdown (programs use it as a tick timer) */
    uint8_t motor = RB(BDA, 0x40);
    if (motor) {
        WB(BDA, 0x40, (uint8_t)(motor - 1));
        if (motor == 1)
            WB(BDA, 0x3F, RB(BDA, 0x3F) & 0xF0);
    }
    do_int(0x1C, 0xFFFF);
    port_out8(0x20, 0x20);
}

/* put a key into the BIOS keyboard buffer (used by game add-ons) */
void bios_key_push(uint16_t w)
{
    kbuf_put(w);
}

uint16_t bios_key_wait(void)
{
    uint16_t w;
    while (!kbuf_get(&w, 1)) {
        hlt_wait();
        if (quit_requested) {
            plat_shutdown();
            exit(0);
        }
    }
    return w;
}

int bios_key_peek(uint16_t *w)
{
    return kbuf_get(w, 0);
}

static void bios_int16(void)
{
    uint16_t w;
    switch (AH) {
    case 0x00:
    case 0x10:
        while (!kbuf_get(&w, 1)) {
            hlt_wait();
            if (quit_requested) {
                plat_shutdown();
                exit(0);
            }
        }
        AX = w;
        break;
    case 0x01:
    case 0x11:
        poll_slow();
        if (kbuf_get(&w, 0)) {
            AX = w;
            ZF = 0;
        } else
            ZF = 1;
        break;
    case 0x02:
    case 0x12:
        AL = RB(BDA, 0x17);
        break;
    case 0x05:
        kbuf_put(CX);
        AL = 0;
        break;
    default:
        break;
    }
}

static void set_default_palette(void)
{
    /* standard VGA mode 13h palette: EGA 16, grey ramp, colour cube */
    static const uint8_t ega[16][3] = {
        {0, 0, 0}, {0, 0, 42}, {0, 42, 0}, {0, 42, 42}, {42, 0, 0},
        {42, 0, 42}, {42, 21, 0}, {42, 42, 42}, {21, 21, 21},
        {21, 21, 63}, {21, 63, 21}, {21, 63, 63}, {63, 21, 21},
        {63, 21, 63}, {63, 63, 21}, {63, 63, 63}};
    memcpy(vga_dac, ega, sizeof ega);
    for (int i = 16; i < 256; i++) {
        uint8_t v = (uint8_t)((i * 63) / 255);
        vga_dac[i][0] = vga_dac[i][1] = vga_dac[i][2] = v;
    }
}

static void bios_int10(void)
{
    switch (AH) {
    case 0x00:
        video_mode = AL & 0x7F;
        WB(BDA, 0x49, (uint8_t)video_mode);
        if (!(AL & 0x80)) {
            if (video_mode == 0x13)
                memset(&mem[0xA0000], 0, 0x10000);
            else
                memset(&mem[0xB8000], 0, 0x8000);
        }
        set_default_palette();
        logmsg("video mode %02x", video_mode);
        break;
    case 0x01:
    case 0x02:
    case 0x05:
    case 0x06:
    case 0x07:
    case 0x09:
    case 0x0A:
    case 0x0B:
        break;
    case 0x03:
        DX = 0;
        CX = 0x0607;
        break;
    case 0x08:
        AX = 0x0720;
        break;
    case 0x0E:
        if (video_mode < 4) {
            fputc(AL, stdout);
            fflush(stdout);
        }
        break;
    case 0x0F:
        AL = (uint8_t)video_mode;
        AH = video_mode == 0x13 ? 40 : 80;
        BH = 0;
        break;
    case 0x10:
        switch (AL) {
        case 0x10:
            vga_dac[BX & 0xFF][0] = DH & 0x3F;
            vga_dac[BX & 0xFF][1] = CH & 0x3F;
            vga_dac[BX & 0xFF][2] = CL & 0x3F;
            break;
        case 0x12:
            for (int i = 0; i < CX; i++) {
                int idx = (BX + i) & 0xFF;
                for (int c = 0; c < 3; c++)
                    vga_dac[idx][c] = RB(ES, (uint16_t)(DX + i * 3 + c)) & 0x3F;
            }
            break;
        case 0x15:
            DH = vga_dac[BX & 0xFF][0];
            CH = vga_dac[BX & 0xFF][1];
            CL = vga_dac[BX & 0xFF][2];
            break;
        case 0x17:
            for (int i = 0; i < CX; i++) {
                int idx = (BX + i) & 0xFF;
                for (int c = 0; c < 3; c++)
                    WB(ES, (uint16_t)(DX + i * 3 + c), vga_dac[idx][c]);
            }
            break;
        default:
            break;
        }
        break;
    case 0x11:
        if (AL == 0x30) {
            CX = 8;
            DL = 24;
            ES = 0xF000;
            BP = 0xFA6E;
        }
        break;
    case 0x12:
        if (BL == 0x10) {
            BH = 0;
            BL = 3;
            CX = 0;
        }
        break;
    case 0x1A:
        if (AL == 0) {
            AL = 0x1A;
            BX = 0x0008;     /* VGA colour */
        }
        break;
    default:
        if (verbose)
            warn("unhandled int 10h AX=%04x", AX);
        break;
    }
}

static void bios_int1a(void)
{
    switch (AH) {
    case 0x00:
        CX = RW(BDA, 0x6E);
        DX = RW(BDA, 0x6C);
        AL = RB(BDA, 0x70);
        WB(BDA, 0x70, 0);
        break;
    case 0x02:
    case 0x04:
        CF = 1;
        break;
    default:
        break;
    }
}

void bios_int(uint8_t n)
{
    switch (n) {
    case 0x08: bios_int8(); break;
    case 0x09: bios_int9(); break;
    case 0x10: bios_int10(); break;
    case 0x11: AX = 0x0026; break;          /* equipment: no FPU */
    case 0x12: AX = 640; break;
    case 0x15:
        if (AH == 0x88)
            AX = 0;
        CF = 1;
        AH = 0x86;
        break;
    case 0x16: bios_int16(); break;
    case 0x1A: bios_int1a(); break;
    case 0x1C: break;
    case 0x20:
        plat_shutdown();
        exit(0);
    case 0x21: dos_int21(); break;
    case 0x23: break;
    case 0x24: AL = 3; break;
    case 0x28: break;
    case 0x2F: AL = 0; break;
    case 0x33: mouse_int33(); break;
    case 0x67: AH = 0x84; break;
    default:
        if (verbose)
            warn("unhandled interrupt %02x AX=%04x", n, AX);
        break;
    }
}

void bios_init(void)
{
    for (int i = 0; i < 256; i++) {
        WW(0, i * 4, (uint16_t)(i * 4));
        WW(0, i * 4 + 2, BIOS_SEG);
        mem[LIN(BIOS_SEG, (uint16_t)(i * 4))] = 0xCF;   /* IRET */
    }
    WW(BDA, 0x10, 0x0026);
    WW(BDA, 0x13, 640);
    WW(BDA, 0x1A, 0x1E);
    WW(BDA, 0x1C, 0x1E);
    WB(BDA, 0x17, 0x00);
    WB(BDA, 0x49, 3);
    WW(BDA, 0x4A, 80);
    WW(BDA, 0x63, 0x3D4);
    WB(BDA, 0x84, 24);
    /* BIOS date / machine id */
    memcpy(&mem[0xFFFF5], "11/20/91", 8);
    mem[0xFFFFE] = 0xFC;
    set_default_palette();
}
