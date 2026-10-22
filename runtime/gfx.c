/*
 * Native implementation of the MCGA graphics driver (MGRAPHIC.EXE).
 *
 * The game draws exclusively through this driver's entry table.  Replacing
 * it with C lets the pages (screen and off-screen buffers) have any size,
 * which is what the widescreen mode needs.  In classic mode the pages live
 * in emulated memory exactly where the original driver keeps them.
 *
 * Each entry is called like the original far procedure: arguments on the
 * emulated stack (C calling convention) or in registers, result in AX.
 * The behaviour follows the disassembly of the 09-19-91 driver; offsets in
 * comments refer to it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"
#include "runtime.h"
#include "gfx.h"
#include "wide.h"

int gfx_native;                 /* native driver active */
int gfx_w = 320, gfx_h = 200;   /* page size */
static int wide;                /* pages live in host memory */

/* driver state (cs:1970.. in the original); page_seg is declared above */
static int16_t org_x, org_y, clip_r;    /* cs:1982, 1984, 1986 */
static uint8_t draw_color, draw_mode;   /* cs:198a, 198b */

static uint16_t drv_cs, drv_ds;         /* driver code / data segment */

/* ---------------------------------------------------------------- */
/* page storage */

/* Pages in host memory (wide mode).  Screen pages have the screen size;
   the page holding the map layers (driver page 2) is wider when the map
   is wider than 80 squares. */
struct hostpage { uint16_t seg; uint8_t *pix; int w, h; };
static struct hostpage hp[16];
static int nhp;
static uint8_t sink;
static int map_page_w = 320;    /* width needed for the map layers */
static uint16_t page_seg[9];    /* [8] = current drawing page */

static struct hostpage *host_page(uint16_t seg)
{
    static struct hostpage *last;
    if (last && last->seg == seg)
        return last;
    for (int i = 0; i < nhp; i++)
        if (hp[i].seg == seg)
            return last = &hp[i];
    if (nhp == 16)
        fatal("gfx: too many pages");
    struct hostpage *p = &hp[nhp++];
    p->seg = seg;
    p->w = gfx_w;
    p->h = gfx_h;
    /* page 2 holds the map layers, page 1 is where the world generator
       builds them */
    if ((seg == page_seg[1] || seg == page_seg[2]) && seg != page_seg[0] &&
        map_page_w > p->w)
        p->w = map_page_w;
    p->pix = calloc(1, (size_t)p->w * p->h);
    if (getenv("CIV_GFXDEBUG"))
        fprintf(stderr, "gfx: host page %04x %dx%d\n", seg, p->w, p->h);
    return last = p;
}

uint8_t *gfx_page_pixels(uint16_t seg)
{
    if (!wide)
        return &mem[(uint32_t)seg << 4];
    return host_page(seg)->pix;
}

/* pixel address: classic mode keeps the 16 bit offset wrap of the
   original, wide mode clips */
static inline uint8_t *PX(uint16_t seg, int x, int y)
{
    if (!wide)
        return &mem[((uint32_t)seg << 4) + (uint16_t)(y * 320 + x)];
    struct hostpage *p = host_page(seg);
    if (x < 0 || y < 0 || x >= p->w || y >= p->h)
        return &sink;
    return p->pix + (size_t)y * p->w + x;
}

static inline uint16_t pseg(int page)
{
    return page_seg[page & 7];
}

const uint8_t *gfx_screen(int *w, int *h)
{
    *w = gfx_w;
    *h = gfx_h;
    if (!wide)
        return &mem[0xA0000];
    return gfx_page_pixels(0xA000);
}

/* ---------------------------------------------------------------- */
/* calling convention helpers */

#define ARG(n) RW(SS, (uint16_t)(SP + 4 + 2 * (n)))
#define SARG(n) ((int16_t)ARG(n))

static inline uint8_t mode_op(uint8_t dst, uint8_t c, int mode)
{
    switch (mode & 3) {
    case 1: return dst & c;
    case 2: return dst | c;
    case 3: return dst ^ c;
    default: return c;
    }
}

/* context structure used by most entries (in DGROUP, SS relative):
   +0 page, +2 x origin, +4 y origin, +6 right clip, +8 bottom clip,
   +a flags, +c colour (+d mode), +e background, +10 font */
#define CTX(p, o) RW(SS, (uint16_t)((p) + (o)))
#define SCTX(p, o) ((int16_t)RW(SS, (uint16_t)((p) + (o))))

/* ---------------------------------------------------------------- */
/* DAC helpers */

static void dac_write(int first, int count, uint16_t seg, uint16_t off)
{
    for (int i = 0; i < count; i++)
        for (int c = 0; c < 3; c++)
            vga_dac[(first + i) & 0xFF][c] =
                RB(seg, (uint16_t)(off + i * 3 + c)) & 0x3F;
}

/* ---------------------------------------------------------------- */
/* sprite heap and RLE sprites (0a1e .. 0b85) */

static uint16_t heap_off, heap_seg, heap_start, heap_end, heap_limit;
static uint16_t cap_w, cap_h;

static void e1_mem_avail(void)
{
    AH = 0x48;
    BX = 0xFFFF;
    dos_int21();
    AX = BX;
}

static void e18_heap_init(void)
{
    e1_mem_avail();
    BX = AX;
    if (BX) {
        AH = 0x48;
        dos_int21();
        if (CF) {
            AX = 0;
            BX = 0;
        }
    } else
        AX = 0;
    BX += AX;
    heap_off = 0;
    heap_seg = AX;
    heap_start = AX;
    heap_end = BX;
}

static void e7_heap_release(void)
{
    uint16_t a = heap_start;
    int32_t used = (int32_t)heap_seg - a;
    uint16_t es = 0;
    if (used >= 0) {
        ES = a;
        AH = used == 0 ? 0x49 : 0x4A;
        BX = (uint16_t)used;
        dos_int21();
        es = CF ? 0 : a;
        if (CF)
            ES = 0;
    }
    heap_seg = heap_end = heap_start = 0;
    AX = es;
}

static uint16_t e35_alloc(uint16_t w, uint16_t h)
{
    int32_t avail = (int32_t)heap_end - heap_seg;
    if (avail <= 0) {
        heap_off = 0;
        heap_seg = 0;
        return 0;
    }
    if (avail >= 0x1000)
        avail = 0xFFF;
    heap_limit = (uint16_t)(avail << 4);
    WW(heap_seg, heap_off, w);
    cap_w = w;
    WW(heap_seg, (uint16_t)(heap_off + 2), h);
    cap_h = h;
    heap_off += 4;
    return heap_seg;
}

static uint16_t e33_finish(void)
{
    uint16_t off = heap_off;
    heap_off = 0;
    uint16_t old = heap_seg;
    heap_seg = (uint16_t)(((off + 15) >> 4) + old);
    return old;
}

/* compress one row of `cap_w` pixels; the source is a page position or,
   with page == 0xFFFF, game memory at seg:x */
static void e34_row(uint16_t seg, int x, int y);
static int row_from_mem;

static void e34_row(uint16_t seg, int x, int y)
{
    if (!heap_seg)
        return;
    if ((uint32_t)cap_w + 4 + heap_off > heap_limit) {
        WW(heap_seg, 2, (uint16_t)(RW(heap_seg, 2) - 1));
        heap_end = 0;
        return;
    }
    uint16_t lead = 0, trail = 0, cnt = 0;
    uint16_t rec = heap_off;
    uint16_t di = (uint16_t)(heap_off + 4);
    for (int i = 0; i < cap_w; i++) {
        uint8_t c = row_from_mem ? RB(seg, (uint16_t)(x + i)) : *PX(seg, x + i, y);
        if (c) {
            trail = 0;
        } else if (!cnt) {
            lead++;
            continue;
        } else
            trail++;
        cnt++;
        WB(heap_seg, di++, c);
    }
    if (trail) {
        cnt -= trail;
        di -= trail;
    }
    WW(heap_seg, rec, cnt);
    WW(heap_seg, (uint16_t)(rec + 2), lead);
    heap_off = di;
}

/* 0a78 alloc(w, h), 0ae3 row(si) from DS:SI, 0ac6 finish */
static void e35_alloc_entry(void)
{
    AX = e35_alloc(ARG(0), ARG(1));
}

static void e34_row_entry(void)
{
    row_from_mem = 1;
    e34_row(DS, ARG(0), 0);
    row_from_mem = 0;
    AX = heap_seg;
}

static void e33_finish_entry(void)
{
    AX = e33_finish();
}

static void e2_capture(void)
{
    int page = SARG(0);
    int x = SARG(1), y = SARG(2);
    uint16_t w = ARG(3), h = ARG(4);
    if (page < 0)
        page = -page;
    uint16_t r = e35_alloc(w, h);
    if (r && cap_h && cap_w) {
        uint16_t seg = pseg(page);
        for (uint16_t row = 0; row < h; row++) {
            /* the original advances its source by 320 per row */
            if (!wide) {
                uint32_t off = (uint16_t)(y * 320 + x + row * 320);
                e34_row(seg, (int)(off % 320), (int)(off / 320));
            } else
                e34_row(seg, x, y + row);
        }
    }
    AX = e33_finish();
}

/* clip state of the RLE renderer (cs:8c2..8d0) */
static int16_t clip_top, clip_left, clip_bottom, clip_right;

static int rle_clip(uint16_t ctx, int16_t x, int16_t y, uint16_t sw, uint16_t sh)
{
    /* 0c77: clip against the context's right/bottom limits */
    if (y < 0) {
        if ((uint16_t)-y >= sh)
            return 0;
        clip_top = (int16_t)-y;
    }
    int16_t maxy = SCTX(ctx, 8);
    if (maxy < 0 || y > maxy)
        return 0;
    int16_t a = (int16_t)(y + sh - 1 - maxy);
    if (a > 0)
        clip_bottom = a;
    if (x < 0) {
        if ((uint16_t)-x >= sw)
            return 0;
        clip_left = (int16_t)-x;
    }
    int16_t maxx = SCTX(ctx, 6);
    if (maxx < 0 || x > maxx)
        return 0;
    a = (int16_t)(x + sw - 1 - maxx);
    if (a > 0)
        clip_right = a;
    return 1;
}

/* 0d68: draw an RLE sprite at (x, y) relative to the context origin */
static void rle_draw(uint16_t ctx, int16_t x, int16_t y, uint16_t sseg)
{
    uint16_t seg = pseg(CTX(ctx, 0));
    int dx = x + SCTX(ctx, 2);
    int dy = y + SCTX(ctx, 4) + clip_top;
    uint16_t si = 0;
    int16_t w = (int16_t)(RW(sseg, si) - clip_right);
    si += 2;
    if (w <= 0)
        return;
    int16_t rows = (int16_t)(RW(sseg, si) - clip_top - clip_bottom);
    si += 2;
    if (rows <= 0)
        return;
    for (int k = 0; k < clip_top; k++)
        si = (uint16_t)(si + RW(sseg, si) + 4);
    while (rows-- > 0) {
        uint16_t len = RW(sseg, si);
        uint16_t skip = RW(sseg, (uint16_t)(si + 2));
        si += 4;
        int16_t remain = (int16_t)len;
        if (len) {
            int16_t start = (int16_t)skip, end = (int16_t)(skip + len);
            int16_t a = (int16_t)(skip - clip_left);
            if (a <= 0) {
                /* first pixels are left of the clip edge */
                si = (uint16_t)(si - a);
                start = (int16_t)(start - a);
                remain = (int16_t)(remain + a);
            }
            if (end > w)
                end = w;
            int16_t n = (int16_t)(end - start);
            if (n > 0) {
                remain = (int16_t)(remain - n);
                int px = dx + start;
                for (int i = 0; i < n; i++) {
                    uint8_t c = RB(sseg, si++);
                    if (c)
                        *PX(seg, px + i, dy) = c;
                }
            }
        }
        si = (uint16_t)(si + remain);
        dy++;
    }
}

static void rle_reset(void)
{
    clip_top = clip_left = clip_bottom = clip_right = 0;
}

static void e32_sprite(void)
{
    uint16_t ctx = ARG(0);
    int16_t x = SARG(1), y = SARG(2);
    uint16_t sseg = ARG(3);
    rle_reset();
    if (sseg && RW(sseg, 0) && RW(sseg, 2))
        rle_draw(ctx, x, y, sseg);
}

static void e6_sprite_clipped(void)
{
    uint16_t ctx = ARG(0);
    int16_t x = SARG(1), y = SARG(2);
    uint16_t sseg = ARG(3);
    rle_reset();
    if (sseg && RW(sseg, 0) && RW(sseg, 2)) {
        uint16_t sw = RW(sseg, 0), sh = RW(sseg, 2);
        if (rle_clip(ctx, x, y, sw, sh))
            rle_draw(ctx, x, y, sseg);
    }
    AX = 0x8C2;
    DX = drv_cs;
}

/* ---------------------------------------------------------------- */
/* blits and pixel access */

static void e20_blit(void)
{
    uint16_t src = ARG(0), dst = ARG(5);
    int sx = SCTX(src, 2) + SARG(1), sy = SCTX(src, 4) + SARG(2);
    int dx = SCTX(dst, 2) + SARG(6), dy = SCTX(dst, 4) + SARG(7);
    uint16_t w = ARG(3), h = ARG(4);
    uint16_t ss = pseg(CTX(src, 0)), ds = pseg(CTX(dst, 0));
    AX = w;
    BX = h;
    if (!w || !h)
        return;
    if (ds == page_seg[0] && w >= 320 && h >= 200)
        g_map_mode = 0;
    if (!wide) {
        /* exact copy of "rep movsw; adc cx,cx; rep movsb" with 16 bit
           offsets in two 64K segments */
        uint32_t sb = (uint32_t)ss << 4, db = (uint32_t)ds << 4;
        uint16_t so = (uint16_t)(sy * 320 + sx), dof = (uint16_t)(dy * 320 + dx);
        uint16_t gap = (uint16_t)(320 - w);
        for (uint16_t r = 0; r < h; r++) {
            for (uint16_t i = 0; i < w / 2; i++) {
                uint8_t a = mem[sb + so], b = mem[sb + (uint16_t)(so + 1)];
                mem[db + dof] = a;
                mem[db + (uint16_t)(dof + 1)] = b;
                so += 2;
                dof += 2;
            }
            if (w & 1)
                mem[db + dof++] = mem[sb + so++];
            so += gap;
            dof += gap;
        }
    } else {
        for (int r = 0; r < h; r++)
            for (int i = 0; i < w; i += 2) {
                uint8_t a = *PX(ss, sx + i, sy + r);
                uint8_t b = i + 1 < w ? *PX(ss, sx + i + 1, sy + r) : 0;
                *PX(ds, dx + i, dy + r) = a;
                if (i + 1 < w)
                    *PX(ds, dx + i + 1, dy + r) = b;
            }
    }
    CX = 0;
    BX = 0;
    DX = (uint16_t)(gfx_w - w);
    ES = ds;
}

static void e3_recolor(void)
{
    uint16_t ctx = ARG(0);
    int x = SARG(1) + SCTX(ctx, 2), y = SARG(2) + SCTX(ctx, 4);
    uint16_t w = ARG(3), h = ARG(4);
    uint8_t from = (uint8_t)ARG(5), to = (uint8_t)ARG(6);
    uint16_t seg = pseg(CTX(ctx, 0));
    for (uint16_t r = 0; r < h; r++)
        for (uint16_t i = 0; i < w; i++) {
            uint8_t *p = PX(seg, x + i, y + r);
            if (*p == from)
                *p = to;
        }
    AH = from;
    AL = to;
}

static void e14_getpixel(void)
{
    uint16_t seg = pseg(ARG(0));
    AX = *PX(seg, SARG(1), SARG(2));
}

static void e13_read_row(void)
{
    uint16_t dst = ARG(0), seg = pseg(ARG(1));
    int x = SARG(2), y = SARG(3);
    uint16_t n = ARG(4);
    for (uint16_t i = 0; i < n; i++)
        WB(DS, (uint16_t)(dst + i), *PX(seg, x + i, y));
    CX = 0;
}

static void e19_write_row(void)
{
    uint16_t src = ARG(0), seg = pseg(ARG(1));
    int x = SARG(2), y = SARG(3);
    uint16_t n = (uint16_t)((ARG(4) + 1) >> 1) * 2;
    for (uint16_t i = 0; i < n; i++)
        *PX(seg, x + i, y) = RB(DS, (uint16_t)(src + i));
    CX = 0;
    ES = seg;
}

/* ---------------------------------------------------------------- */
/* pages */

static void e0_alloc_page(void)
{
    if (ARG(0) == 0) {
        AX = page_seg[0];
        return;
    }
    AH = 0x48;
    BX = 0x0FA0;
    dos_int21();
    if (CF)
        AX = 0;
}

static void e25_set_page(void)
{
    page_seg[ARG(0) & 7] = ARG(1);
    if (getenv("CIV_GFXDEBUG"))
        fprintf(stderr, "gfx: page %d = %04x\n", ARG(0) & 7, ARG(1));
}

static void e8_free_page(void)
{
    uint16_t p = ARG(0);
    if (p) {
        ES = pseg(p);
        AH = 0x49;
        dos_int21();
    }
}

static void copy_page(uint16_t from, uint16_t to)
{
    if (!wide) {
        memmove(&mem[(uint32_t)to << 4], &mem[(uint32_t)from << 4], 64000);
        return;
    }
    struct hostpage *a = host_page(from), *b = host_page(to);
    int w = a->w < b->w ? a->w : b->w, h = a->h < b->h ? a->h : b->h;
    for (int y = 0; y < h; y++)
        memmove(b->pix + (size_t)y * b->w, a->pix + (size_t)y * a->w, w);
}

/* ---------------------------------------------------------------- */
/* mouse cursor (0270, 030e, 022f) */

static uint16_t cur_seg;
static int16_t cur_x, cur_y, cur_cx, cur_cy, cur_w, cur_h;
static int cur_on;
static uint8_t cur_save[512 * 8];

static void cursor_restore(void)
{
    if (!cur_on)
        return;
    uint16_t seg = page_seg[0];
    int k = 0;
    for (int r = 0; r < cur_h; r++)
        for (int i = 0; i < cur_w; i++)
            *PX(seg, cur_cx + i, cur_cy + r) = cur_save[k++];
}

static void draw_sprite_ctx(int page, int16_t x, int16_t y, uint16_t sseg)
{
    /* build the context the original builds on the stack */
    uint16_t sp = SP;
    SP -= 10;
    WW(SS, SP, (uint16_t)page);
    WW(SS, (uint16_t)(SP + 2), 0);
    WW(SS, (uint16_t)(SP + 4), 0);
    WW(SS, (uint16_t)(SP + 6), (uint16_t)(gfx_w - 1));
    WW(SS, (uint16_t)(SP + 8), (uint16_t)(gfx_h - 1));
    uint16_t ctx = SP;
    rle_reset();
    if (sseg && RW(sseg, 0) && RW(sseg, 2) &&
        rle_clip(ctx, x, y, RW(sseg, 0), RW(sseg, 2)))
        rle_draw(ctx, x, y, sseg);
    SP = sp;
}

static void cursor_draw(int page)
{
    if (!cur_on)
        return;
    uint16_t seg = pseg(page);
    int k = 0;
    for (int r = 0; r < cur_h; r++)
        for (int i = 0; i < cur_w; i++)
            cur_save[k++] = *PX(seg, cur_cx + i, cur_cy + r);
    draw_sprite_ctx(page, cur_x, cur_y, cur_seg);
}

static void e30_cursor_set(void)
{
    int16_t x = SARG(0), y = SARG(1);
    uint16_t seg = ARG(2);
    cur_seg = seg;
    cur_x = x;
    cur_y = y;
    cur_on = 0;
    int16_t w = (int16_t)RW(seg, 0);
    if (!w)
        return;
    int16_t ax = x;
    if (ax < 0) {
        w = (int16_t)(w + ax);
        if (w <= 0)
            return;
        ax = 0;
    }
    int16_t room = (int16_t)(gfx_w - ax);
    if (room <= 0)
        return;
    if (w > room)
        w = room;
    cur_w = w;
    cur_cx = ax;
    int16_t maxrows = (int16_t)(sizeof cur_save / (uint16_t)w);
    int16_t h = (int16_t)RW(seg, 2);
    if (!h)
        return;
    int16_t ay = y;
    if (ay < 0) {
        h = (int16_t)(h + ay);
        if (h <= 0)
            return;
        ay = 0;
    }
    room = (int16_t)(gfx_h - ay);
    if (room <= 0)
        return;
    if (h > room)
        h = room;
    if (!wide && h > (int16_t)(0x200 / (uint16_t)w))
        h = (int16_t)(0x200 / (uint16_t)w);
    if (h > maxrows)
        h = maxrows;
    cur_h = h;
    cur_cy = ay;
    cur_on = 1;
    cursor_draw(0);
}

/* ---------------------------------------------------------------- */
/* text (11d7 -> 1328, 125c, 1208, 12be, 12fe, 0fac) */

static uint16_t font_base(int font)
{
    uint16_t n = RW(drv_ds, 0x630);
    if (font == 0 || (uint16_t)font > n)
        return 0;
    return (uint16_t)(0x630 + RW(drv_ds, (uint16_t)(0x630 + 2 * font)));
}

#define FB(fb, o) RB(drv_ds, (uint16_t)((fb) + (o)))

static void e4_char_width(void)
{
    int font = (int16_t)ARG(0);
    uint8_t ch = (uint8_t)ARG(1);
    AX = 0;
    if (ch & 0x80)
        return;
    uint16_t fb = font_base(font);
    if (!fb || ch < FB(fb, -8) || ch > FB(fb, -7))
        return;
    uint8_t sp = FB(fb, -3), w = FB(fb, -5);
    if (!w)
        w = FB(fb, -9 - (FB(fb, -7) - ch));
    AX = (uint16_t)(int16_t)(int8_t)(uint8_t)(w + sp);
}

static void e12_font_height(void)
{
    uint16_t fb = font_base((int16_t)ARG(0));
    AX = fb ? (uint8_t)(FB(fb, -4) + FB(fb, -2)) : 0;
}

static void e11_font_ptr(void)
{
    uint16_t n = RW(drv_ds, 0x20), f = ARG(0);
    AX = 0;
    if (f > n || !f)
        return;
    AX = (uint16_t)(0x630 + RW(drv_ds, (uint16_t)(0x20 + 2 * f)));
    DX = drv_ds;
}

static int text_draw(uint16_t ctx, int16_t x, int16_t y, uint16_t str)
{
    int16_t tx = x, ty = y;
    uint16_t trunc = 0;
    uint8_t trunc_ch = 0;
    uint8_t lskip = 0, rcut = 0;
    int16_t result_x;
    uint16_t fb;
    int rows, stride, shift;
    uint8_t first, last, spacing, fixed;
    uint16_t widths, glyphs;

    if (RB(SS, str) == 0)
        goto done;
    fb = font_base((int16_t)CTX(ctx, 0x10));
    if (!fb)
        goto done;
    glyphs = fb;
    rows = FB(fb, -4);
    if (CTX(ctx, 0xA) & 1)
        rows = (int8_t)(uint8_t)(rows + FB(fb, -2));
    else
        rows = (int8_t)(uint8_t)rows;
    spacing = FB(fb, -3);
    fixed = FB(fb, -5);
    if (fixed)
        fixed = (uint8_t)(fixed + spacing);
    first = FB(fb, -8);
    last = FB(fb, -7);
    shift = FB(fb, -6) - 1;
    widths = (uint16_t)(fb - 9 - (last - first));
    stride = (last - first + 1) << shift;

#define CHW(c) (fixed ? fixed : (uint8_t)(RB(drv_ds, (uint16_t)(widths + (c) - first)) + spacing))
    /* right clip (125c) */
    {
        int16_t room = SCTX(ctx, 6);
        if (room < 0)
            goto done;
        room = (int16_t)(room - tx);
        if (room < 0)
            goto done;
        room++;
        for (uint16_t s = str;; s++) {
            uint8_t c = RB(SS, s);
            if (!c)
                break;
            if ((c & 0x80) || c > last || c < first)
                continue;
            room = (int16_t)(room - CHW(c));
            if (room > 0)
                continue;
            rcut = (uint8_t)-room;
            trunc = (uint16_t)(s + 1);
            trunc_ch = RB(SS, trunc);
            WB(SS, trunc, 0);
            break;
        }
    }
    /* left clip (1208): drawing starts at the first character that is
       at least partly visible */
    if (SCTX(ctx, 6) < 0)
        goto done;
    if (tx < 0) {
        int16_t acc = tx;
        tx = 0;
        for (;; str++) {
            uint8_t c = RB(SS, str);
            if (!c)
                goto done;
            if ((c & 0x80) || c > last || c < first)
                continue;
            uint8_t w = CHW(c);
            acc = (int16_t)(acc + w);
            if (acc <= 0)
                continue;
            lskip = (uint8_t)(w - acc);
            break;
        }
    }
    /* vertical clip (12be) */
    {
        int16_t maxy = SCTX(ctx, 8);
        if (maxy < 0)
            goto done;
        if (ty < 0) {
            int16_t n = (int16_t)-ty;
            rows -= n;
            if (rows <= 0)
                goto done;
            ty = 0;
            glyphs = (uint16_t)(glyphs + n * stride);
        }
        if (ty > maxy)
            goto done;
        int16_t over = (int16_t)(ty + rows - 1 - maxy);
        if (over > 0)
            rows -= over;
    }
    /* draw (12fe / 0fac) */
    tx = (int16_t)(tx + SCTX(ctx, 2));
    ty = (int16_t)(ty + SCTX(ctx, 4));
    {
        uint8_t fg = (uint8_t)CTX(ctx, 0xC), mode = (uint8_t)(CTX(ctx, 0xC) >> 8) & 3;
        uint8_t bg = (uint8_t)CTX(ctx, 0xE);
        int opaque = (uint8_t)CTX(ctx, 0xA) == 1;
        uint16_t seg = pseg(CTX(ctx, 0));
        draw_color = fg;
        draw_mode = mode;
        for (uint16_t s = str;; s++) {
            uint8_t c = RB(SS, s);
            uint8_t next = RB(SS, (uint16_t)(s + 1));
            if (!c)
                break;
            if (c & 0x80) {
                fg = c & 0x7F;
                continue;
            }
            if (c > last || c < first)
                continue;
            int idx = c - first;
            uint8_t adv = CHW(c);
            if (!next)
                adv = (uint8_t)(adv - rcut);
            adv = (uint8_t)(adv - lskip);
            uint16_t g = (uint16_t)((idx << shift) + glyphs);
            for (int r = 0; r < (uint8_t)rows; r++) {
                uint16_t bits = (uint16_t)(RB(drv_ds, g) << 8 | RB(drv_ds, (uint16_t)(g + 1)));
                bits = (uint16_t)(bits << lskip);
                int y0 = ty + r;
                if (!opaque) {
                    uint8_t n = adv;
                    if (next)
                        n = (uint8_t)(n - spacing);
                    int px = tx;
                    do {
                        int bit = bits >> 15;
                        bits = (uint16_t)(bits << 1);
                        if (bit) {
                            uint8_t *p = PX(seg, px, y0);
                            *p = mode_op(*p, fg, mode);
                        }
                        px++;
                    } while (--n);
                } else {
                    uint8_t n = adv;
                    int px = tx;
                    do {
                        int bit = bits >> 15;
                        bits = (uint16_t)(bits << 1);
                        *PX(seg, px, y0) = bit ? fg : bg;
                        px++;
                    } while (--n);
                }
                g = (uint16_t)(g + stride);
            }
            lskip = 0;
            tx = (int16_t)(tx + adv);
        }
    }
done:
    if (trunc)
        WB(SS, trunc, trunc_ch);
    result_x = (int16_t)(tx - SCTX(ctx, 2));
    return result_x;
#undef CHW
}

static void e21_text(void)
{
    uint16_t ctx = ARG(0);
    int16_t x = SARG(1), y = SARG(2);
    uint16_t str = ARG(3);
    AX = (uint16_t)text_draw(ctx, x, y, str);
}

/* ---------------------------------------------------------------- */
/* primitives used by the game's own drawing library (register calls) */

static void e48_set_page(void)
{
    page_seg[8] = pseg(AX);
}

static void e49_set_ctx(void)
{
    org_x = (int16_t)RW(DS, (uint16_t)(BX + 2));
    org_y = (int16_t)RW(DS, (uint16_t)(BX + 4));
    clip_r = (int16_t)RW(DS, (uint16_t)(BX + 6));
    page_seg[8] = pseg(RW(DS, BX));
}

static void e41_set_color(void)
{
    draw_color = AL;
    draw_mode = AH & 3;
}

static void plot(int x, int y)
{
    uint8_t *p = PX(page_seg[8], x, y);
    *p = mode_op(*p, draw_color, draw_mode);
}

static void e38_putpixel(void)
{
    plot((int16_t)AX + org_x, (int16_t)BX + org_y);
}

static void e42_line(void)
{
    int x1 = (int16_t)AX + org_x, x2 = (int16_t)CX + org_x;
    int y1 = (int16_t)BX + org_y, y2 = (int16_t)DX + org_y;
    if ((uint16_t)y1 > (uint16_t)y2) {
        int t = x1; x1 = x2; x2 = t;
        t = y1; y1 = y2; y2 = t;
    }
    if (y1 == y2 && x1 == x2) {
        plot(x1, y1);
        return;
    }
    int dy = y2 - y1, dx = x2 - x1, sx = 1;
    if (dx < 0) {
        dx = -dx;
        sx = -1;
    }
    /* the original walks with offsets: minor/major steps */
    int major_x = dx >= dy;
    int n = major_x ? dx : dy, m = major_x ? dy : dx;
    if (n == 0 || m == 0) {
        /* straight line */
        int x = x1, y = y1;
        for (int i = 0; i <= n; i++) {
            plot(x, y);
            if (major_x)
                x += sx;
            else
                y++;
        }
        return;
    }
    int err = -((n + 1) >> 1);
    int x = x1, y = y1;
    for (int i = n; i >= 0; i--) {
        plot(x, y);
        if (major_x)
            x += sx;
        else
            y++;
        err += m;
        if (err >= 0) {
            err -= n;
            if (major_x)
                y++;
            else
                x += sx;
        }
    }
}

/* 040a: fill horizontal spans (rows AX..CX, tables at BX / BX+0x190) */
static void e39_spans(void)
{
    int16_t first = (int16_t)AX, lastr = (int16_t)CX;
    uint16_t tab = BX;
    if (first < 0 || first > lastr)
        return;
    uint16_t seg = page_seg[8];
    for (int y = first; y <= lastr; y++) {
        uint16_t x0 = RW(DS, (uint16_t)(tab + 2 * y));
        uint16_t x1 = RW(DS, (uint16_t)(tab + 2 * y + 0x190));
        if (x1 < x0)
            continue;
        if (x1 == x0 && (x0 == 0 || x0 == (uint16_t)clip_r))
            continue;
        for (int x = x0; x <= (int)x1; x++)
            *PX(seg, x, y) = draw_color;
    }
}

/* ---------------------------------------------------------------- */
/* palette and video mode */

static void e28_palette(void)
{
    uint16_t p = ARG(0);
    if (RB(DS, p) == 'M' && RB(DS, (uint16_t)(p + 1)) == '0') {
        int first = RB(DS, (uint16_t)(p + 4)), lastc = RB(DS, (uint16_t)(p + 5));
        dac_write(first, lastc - first + 1, DS, (uint16_t)(p + 6));
    }
}

static void e27_palette16(void)
{
    uint16_t si = ARG(0);
    for (int i = 0; i < 16; i++) {
        uint8_t idx = RB(SS, (uint16_t)(si + i));
        for (int c = 0; c < 3; c++)
            vga_dac[i][c] = RB(drv_ds, (uint16_t)(0x50 + idx * 3 + c)) & 0x3F;
    }
}

static void e37_set_mode(void)
{
    uint8_t mode = (uint8_t)((ARG(0) & 0x80) | 0x13);
    g_map_mode = 0;
    AX = mode;
    bios_int(0x10);
    /* default palette from the driver data */
    dac_write(0, 0x80, drv_ds, 0x50);
    if (wide && !(mode & 0x80))
        memset(gfx_page_pixels(0xA000), 0, (size_t)gfx_w * gfx_h);
}

/* ---------------------------------------------------------------- */
/* page to screen copies */

static void e31_show_page(void)
{
    uint16_t p = ARG(0);
    if (!p)
        return;
    g_map_mode = 0;
    cursor_draw(p);
    copy_page(pseg(p), page_seg[0]);
}

static void e10_dissolve(void)
{
    /* wait for the next timer tick like the original */
    WB(0, 0x440, 1);
    while (RB(0, 0x440) == 1) {
        hlt_wait();
        if (quit_requested)
            break;
    }
    uint16_t p = ARG(0);
    uint16_t speed = ARG(1);
    if (!p)
        return;
    g_map_mode = 0;
    uint16_t from = pseg(p), to = page_seg[0];
    if (!speed) {
        copy_page(from, to);
        return;
    }
    /* pseudo random dissolve over ~speed ticks */
    int total = wide ? gfx_w * gfx_h : 64000;
    uint8_t *s = wide ? gfx_page_pixels(from) : &mem[(uint32_t)from << 4];
    uint8_t *d = wide ? gfx_page_pixels(to) : &mem[(uint32_t)to << 4];
    uint32_t lfsr = 1, done = 0;
    uint64_t start = now_us(), dur = (uint64_t)speed * 1000000ULL / 182 * 10 / 10;
    if (dur > 3000000)
        dur = 3000000;
    while (done < (uint32_t)total) {
        uint64_t t = now_us() - start;
        uint32_t target = dur ? (uint32_t)((uint64_t)total * t / dur) : (uint32_t)total;
        if (target > (uint32_t)total)
            target = (uint32_t)total;
        while (done < target) {
            /* 20 bit maximal LFSR, skip values outside the page */
            do {
                lfsr = (lfsr >> 1) ^ (-(lfsr & 1u) & 0x90000u);
            } while (lfsr > (uint32_t)total);
            d[lfsr - 1] = s[lfsr - 1];
            done++;
        }
        if (done < (uint32_t)total)
            hlt_wait();
        if (quit_requested)
            break;
    }
    memcpy(d, s, (size_t)total);
}

/* 10bb: screen shake, called from the timer interrupt.  The state lives
   in the driver image (cs:10ac random seed, cs:10ae remaining frames) so
   that the shake setter (10af) can stay the original code. */
static void e51_shake(void)
{
    uint16_t r = (uint16_t)(RW(drv_cs, 0x10ac) * 5 + 1);
    WW(drv_cs, 0x10ac, r);
    AX = r;
    uint8_t n = RB(drv_cs, 0x10ae);
    if (!n)
        return;
    AH &= 3;
    WB(drv_cs, 0x10ae, (uint8_t)(n - 1));
    if (n == 1)
        AH = 0;
    AL = 0x0d;
    DX = 0x3d4;
    port_out16(DX, AX);
}

static void e17_info(void)
{
    AX = RW(drv_cs, (uint16_t)(ARG(0) + 0x1966));
}

/* ---------------------------------------------------------------- */
/* dispatch */

struct gfx_entry { uint16_t off; void (*fn)(void); };
static const struct gfx_entry entries[] = {
    {0x04ae, e0_alloc_page}, {0x0492, e1_mem_avail}, {0x0b85, e2_capture},
    {0x009a, e3_recolor}, {0x115d, e4_char_width}, {0x0c3e, e6_sprite_clipped},
    {0x0a4a, e7_heap_release}, {0x04d1, e8_free_page}, {0x06b7, e10_dissolve},
    {0x10e8, e11_font_ptr}, {0x11ae, e12_font_height}, {0x03b1, e13_read_row},
    {0x038c, e14_getpixel}, {0x0460, e17_info}, {0x0a1e, e18_heap_init},
    {0x03df, e19_write_row}, {0x07d8, e20_blit}, {0x11d7, e21_text},
    {0x049c, e25_set_page}, {0x010c, e27_palette16}, {0x0162, e28_palette},
    {0x0270, e30_cursor_set}, {0x063c, e31_show_page}, {0x0d47, e32_sprite},
    {0x04e8, e37_set_mode}, {0x0550, e38_putpixel}, {0x040a, e39_spans},
    {0x020c, e41_set_color}, {0x0599, e42_line}, {0x0484, e48_set_page},
    {0x046d, e49_set_ctx}, {0x10bb, e51_shake},
    {0x0a78, e35_alloc_entry}, {0x0ae3, e34_row_entry}, {0x0ac6, e33_finish_entry},
};

/* entries that only adjust driver state or do nothing */
static void e_nop(void) {}

/* ---------------------------------------------------------------- */
/* shadow mode: run the original driver and the native one on the same
   state and report differences (CIV_GFX=shadow) */

int gfx_shadow;
static uint8_t *snap, *res_l;
static long shadow_calls, shadow_fails;

void gfx_shadow_report(void)
{
    if (gfx_shadow)
        fprintf(stderr, "gfx shadow: %ld driver calls compared, %ld mismatches\n",
                shadow_calls, shadow_fails);
}

static const char *entry_name(uint16_t off)
{
    static char b[16];
    snprintf(b, sizeof b, "drv_%04x", off);
    return b;
}

static void shadow_run(void (*native)(void), uint16_t off)
{
    if (!snap) {
        snap = malloc(MEM_SIZE);
        res_l = malloc(MEM_SIZE);
    }
    gfx_native = 0;
    func_t lifted = lookup_code(CS, off);
    gfx_native = 1;
    cpu_t c0 = cpu;
    int sd0 = shadow_depth, uw0 = unwind_to;
    uint8_t dac0[256][3];
    memcpy(dac0, vga_dac, sizeof dac0);
    memcpy(snap, mem, MEM_SIZE);
    uint16_t args[8];
    for (int k = 0; k < 8; k++)
        args[k] = ARG(k);

    irq_hold = 1;
    lifted();
    cpu_t cl = cpu;
    int sdl = shadow_depth, uwl = unwind_to;
    uint8_t dacl[256][3];
    memcpy(dacl, vga_dac, sizeof dacl);
    memcpy(res_l, mem, MEM_SIZE);

    memcpy(mem, snap, MEM_SIZE);
    cpu = c0;
    shadow_depth = sd0;
    unwind_to = uw0;
    memcpy(vga_dac, dac0, sizeof dac0);
    native();
    RET_FAR(0);
    irq_hold = 0;

    shadow_calls++;
    uint32_t drv_lo = (uint32_t)drv_cs << 4, drv_hi = drv_lo + 0x6530;
    uint32_t stk_hi = ((uint32_t)c0.ss << 4) + c0.sp, stk_lo = stk_hi - 0x800;
    uint32_t first = 0, ndiff = 0;
    for (uint32_t a = 0; a < 0x110000; a++) {
        if (mem[a] == res_l[a] || (a >= drv_lo && a < drv_hi) ||
            (a >= stk_lo && a < stk_hi))
            continue;
        if (!ndiff++)
            first = a;
    }
    /* only entries with a result are compared on AX */
    static const uint16_t ret_ax[] = {0x04ae, 0x0492, 0x0b85, 0x115d, 0x0a4a,
        0x10e8, 0x11ae, 0x038c, 0x0460, 0x0a1e, 0x11d7, 0x0c3e, 0x10bb, 0};
    int cmp_ax = 0;
    for (int k = 0; ret_ax[k]; k++)
        if (ret_ax[k] == off)
            cmp_ax = 1;
    int bad = ndiff || (cmp_ax && cpu.a.x != cl.a.x) ||
              memcmp(dacl, vga_dac, sizeof dacl) || cpu.sp != cl.sp || cpu.cs != cl.cs;
    if (off == 0x0c3e || off == 0x10e8)
        bad |= cpu.d.x != cl.d.x;
    if (bad && shadow_fails++ < 200) {
        fprintf(stderr, "GFX MISMATCH %s args %04x %04x %04x %04x %04x %04x %04x %04x"
                " regs ax=%04x bx=%04x cx=%04x dx=%04x: ax %04x/%04x dx %04x/%04x sp %04x/%04x",
                entry_name(off), args[0], args[1], args[2], args[3], args[4], args[5],
                args[6], args[7], c0.a.x, c0.b.x, c0.c.x, c0.d.x, cl.a.x, cpu.a.x,
                cl.d.x, cpu.d.x, cl.sp, cpu.sp);
        if (ndiff)
            fprintf(stderr, " mem %u bytes, first %05x lifted %02x native %02x",
                    ndiff, first, res_l[first], mem[first]);
        if (memcmp(dacl, vga_dac, sizeof dacl))
            fprintf(stderr, " palette");
        fputc('\n', stderr);
    }
    /* continue with the original driver's result */
    memcpy(mem, res_l, MEM_SIZE);
    cpu = cl;
    shadow_depth = sdl;
    unwind_to = uwl;
    memcpy(vga_dac, dacl, sizeof dacl);
}

static void gfx_call(int i)
{
    entries[i].fn();
}

#define WRAP(i) static void w##i(void) { if (gfx_shadow) { shadow_run(entries[i].fn, entries[i].off); return; } gfx_call(i); RET_FAR(0); }
WRAP(0) WRAP(1) WRAP(2) WRAP(3) WRAP(4) WRAP(5) WRAP(6) WRAP(7) WRAP(8)
WRAP(9) WRAP(10) WRAP(11) WRAP(12) WRAP(13) WRAP(14) WRAP(15) WRAP(16)
WRAP(17) WRAP(18) WRAP(19) WRAP(20) WRAP(21) WRAP(22) WRAP(23) WRAP(24)
WRAP(25) WRAP(26) WRAP(27) WRAP(28) WRAP(29) WRAP(30) WRAP(31) WRAP(32)
WRAP(33) WRAP(34)
static void (*const wrappers[])(void) = {w0, w1, w2, w3, w4, w5, w6, w7, w8,
    w9, w10, w11, w12, w13, w14, w15, w16, w17, w18, w19, w20, w21, w22, w23,
    w24, w25, w26, w27, w28, w29, w30, w31, w32, w33, w34};
_Static_assert(sizeof wrappers / sizeof wrappers[0] == sizeof entries / sizeof entries[0],
               "one wrapper per entry");

static void n_cursor_restore(void) { cursor_restore(); }
static void n_cursor_draw(void) { cursor_draw(AX); }
static void w_cursor_restore(void)
{
    if (gfx_shadow) { shadow_run(n_cursor_restore, 0x0224); return; }
    cursor_restore();
    RET_FAR(0);
}
static void w_cursor_draw(void)
{
    if (gfx_shadow) { shadow_run(n_cursor_draw, 0x030e); return; }
    cursor_draw(AX);
    RET_FAR(0);
}
static void w_nop(void) { e_nop(); RET_FAR(0); }

/* Native function for a call into the driver at offset `off`, or NULL. */
func_t gfx_lookup(uint16_t off)
{
    if (!gfx_native)
        return NULL;
    for (size_t i = 0; i < sizeof entries / sizeof entries[0]; i++)
        if (entries[i].off == off)
            return wrappers[i];
    switch (off) {
    case 0x0224: return w_cursor_restore;
    case 0x030e: return w_cursor_draw;
    case 0x0223: return w_nop;
    default: return NULL;
    }
}

/* Filled rectangle as the game's library does it (set page, colour, then
   one span per row), without the 200 row span tables. */
void gfx_fill_rect(uint16_t page, int x, int y, int w, int h, uint16_t color)
{
    page_seg[8] = pseg(page);
    draw_color = (uint8_t)color;
    draw_mode = (color >> 8) & 3;
    for (int r = 0; r < h; r++) {
        if (w == 1 && (x == 0 || x == clip_r))
            continue;
        for (int i = 0; i < w; i++)
            *PX(page_seg[8], x + i, y + r) = draw_color;
    }
}

/* Change the page size (widescreen zoom).  Every page keeps the part of
   its picture that fits; the caller redraws the map afterwards. */
void gfx_resize(int w, int h)
{
    if (!wide || (w == gfx_w && h == gfx_h))
        return;
    for (int i = 0; i < nhp; i++) {
        if (hp[i].w != gfx_w || hp[i].h != gfx_h)
            continue;               /* the map page keeps its size */
        uint8_t *n = calloc(1, (size_t)w * h);
        int cw = w < gfx_w ? w : gfx_w, ch = h < gfx_h ? h : gfx_h;
        for (int y = 0; y < ch; y++)
            memcpy(n + (size_t)y * w, hp[i].pix + (size_t)y * gfx_w, cw);
        free(hp[i].pix);
        hp[i].pix = n;
        hp[i].w = w;
        hp[i].h = h;
    }
    gfx_w = w;
    gfx_h = h;
}

/* one pixel on a page given by number, clipped to the page */
void gfx_put_pixel(int page, int x, int y, uint8_t color)
{
    *PX(pseg(page), x, y) = color;
}

/* copy a rectangle within one page (overlap safe) */
void gfx_copy_rect(int page, int sx, int sy, int w, int h, int dx, int dy)
{
    uint16_t seg = pseg(page);
    uint8_t *tmp = malloc((size_t)w * h);
    for (int r = 0; r < h; r++)
        for (int i = 0; i < w; i++)
            tmp[r * w + i] = *PX(seg, sx + i, sy + r);
    for (int r = 0; r < h; r++)
        for (int i = 0; i < w; i++)
            *PX(seg, dx + i, dy + r) = tmp[r * w + i];
    free(tmp);
}

int gfx_map_page_w(void)
{
    return map_page_w;
}

uint16_t gfx_page_seg(int page)
{
    return page_seg[page & 7];
}

uint8_t gfx_get_pixel(uint16_t seg, int x, int y)
{
    return *PX(seg, x, y);
}

int gfx_is_wide(void)
{
    return wide;
}

void gfx_module_loaded(uint16_t seg)
{
    drv_cs = seg;
    drv_ds = RW(seg, 0x1988);
    /* initial page table from the loaded driver image */
    for (int i = 0; i < 9; i++)
        page_seg[i] = RW(seg, (uint16_t)(0x1970 + 2 * i));
    if (!drv_ds)
        drv_ds = (uint16_t)(seg + RW(seg, 0x2a));
}

/* wider maps: width of the pages that hold the map layers */
void gfx_set_map_page_w(int w)
{
    map_page_w = w;
    if (map_page_w > 320 && gfx_native)
        wide = 1;
}

void gfx_init(int native, int w, int h)
{
    gfx_native = native;
    gfx_shadow = native == 2;
    gfx_w = w;
    gfx_h = h;
    wide = native && (w != 320 || h != 200 || map_page_w > 320);
}
