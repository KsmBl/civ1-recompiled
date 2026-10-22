/*
 * PC hardware emulation: 8259 PIC, 8253 PIT, keyboard controller, VGA
 * DAC/status ports, PC speaker gate, and the poll loop that delivers
 * hardware interrupts to the lifted code between instructions.
 */
#include <stdio.h>
#include <stdlib.h>
#include "cpu.h"
#include "runtime.h"
#include "audio.h"
#include "gfx.h"

int poll_counter = 1;
int irq_hold;            /* suppress IRQ delivery (driver shadow tests) */
int quit_requested;

/* PIC */
static uint8_t pic_mask = 0xB8, pic_isr;
static uint8_t pic_irr;

/* PIT */
static uint16_t pit_div[3] = {0, 0, 0};
static uint8_t pit_mode[3], pit_flip[3], pit_latch_flip[3];
static uint16_t pit_latch[3];
static int pit_latched[3];
static uint64_t pit0_period_us = 54925;
static uint64_t pit0_next;
uint8_t port61;

/* keyboard */
uint8_t kbd_port60;
static uint8_t kq[64];
static int kq_head, kq_tail;
static int kbd_full;

/* VGA */
static uint8_t dac_widx, dac_ridx, dac_wcomp, dac_rcomp;
static uint8_t crtc_idx, crtc[32], seq_idx, seq[8], gc_idx, gc[16];
static int attr_flip;

static uint64_t t0;

int deterministic;
static uint64_t virt_us;
static FILE *trace;
static long trace_n;

uint64_t now_us(void)
{
    if (deterministic)
        return virt_us;
    return plat_time_us() - t0;
}

static uint32_t mem_hash(uint32_t lo, uint32_t hi)
{
    uint32_t h = 2166136261u;
    for (uint32_t a = lo; a < hi; a++)
        h = (h ^ mem[a]) * 16777619u;
    return h;
}

long trace_count(void) { return trace_n; }

/* Event trace for comparing interpreted and lifted runs. */
void trace_event(const char *what, int n)
{
    if (!trace)
        return;
    trace_n++;
    fprintf(trace, "%ld %s %02x t=%llu ax=%04x bx=%04x cx=%04x dx=%04x si=%04x "
            "di=%04x bp=%04x sp=%04x ds=%04x es=%04x ss=%04x",
            trace_n, what, n, (unsigned long long)virt_us, AX, BX, CX, DX, SI, DI,
            BP, SP, DS, ES, SS);
    static int with_stack = -1;
    if (with_stack < 0)
        with_stack = getenv("CIV_TRACE_STACK") != NULL;
    if (with_stack)     /* lifted runs only: the interpreter has no shadow stack */
        for (int i = shadow_depth - 1, k = 0; i >= 0 && k < 6; i--, k++) {
            struct shadow *sh = &shadow_stack[i & (SHADOW_MAX - 1)];
            fprintf(trace, " %04x:%04x", (uint16_t)(sh->cs - LOADSEG), sh->ip);
        }
    if (trace_n % 16 == 0) {
        /* the 4K below SS:SP is dead stack, which native overrides of
           game functions may leave different */
        uint32_t sp = ((uint32_t)SS << 4) + SP, lo = sp > 0x1000 ? sp - 0x1000 : 0;
        uint32_t h = mem_hash(0x500, lo < 0x500 ? 0x500 : lo) ^
                     (mem_hash(sp, 0xB0000) * 31);
        fprintf(trace, " mem=%08x", h);
    }
    static long dump_at = -1;
    if (dump_at < 0) {
        const char *e = getenv("CIV_MEMDUMP_AT");
        dump_at = e ? atol(e) : 0;
    }
    if (dump_at && trace_n == dump_at) {
        const char *e = getenv("CIV_MEMDUMP_FILE");
        FILE *d = fopen(e ? e : "memdump.bin", "wb");
        if (d) {
            fwrite(mem, 1, 0x110000, d);
            fclose(d);
        }
    }
    fputc('\n', trace);
}

void timer_set_divisor(uint16_t div)
{
    uint32_t d = div ? div : 65536;
    pit0_period_us = (uint64_t)d * 1000000ULL / 1193182ULL;
    if (pit0_period_us < 100)
        pit0_period_us = 100;
}

void kbd_scancode(uint8_t sc)
{
    int n = (kq_tail + 1) & 63;
    if (n == kq_head)
        return;
    kq[kq_tail] = sc;
    kq_tail = n;
}

void hw_init(void)
{
    const char *e = getenv("CIV_DETERMINISTIC");
    deterministic = e && atoi(e);
    e = getenv("CIV_TRACE");
    if (e)
        trace = fopen(e, "w");
    t0 = plat_time_us();
    pit0_next = pit0_period_us;
}

/* ---------------------------------------------------------------- */

static int irq_deliverable(int irq)
{
    if (pic_mask & (1 << irq))
        return 0;
    /* an IRQ of higher or equal priority is still being serviced */
    if (pic_isr & ((2 << irq) - 1))
        return 0;
    return 1;
}

static long irq_count[8];
static void deliver(int irq)
{
    irq_count[irq]++;
    trace_event("irq", irq);
    pic_irr &= (uint8_t)~(1 << irq);
    pic_isr |= (uint8_t)(1 << irq);
    raise_irq_vector((uint8_t)(8 + irq));
}

static uint64_t next_frame;
static int in_poll;

static uint64_t dump_at;

static void debug_dump(void)
{
    fprintf(stderr, "--- state dump at %.1fs: CS=%04x SS:SP=%04x:%04x BP=%04x "
            "AX=%04x BX=%04x CX=%04x DX=%04x IF=%d irq0=%ld irq1=%ld mask=%02x isr=%02x\n",
            now_us() / 1e6, CS, SS, SP, BP, AX, BX, CX, DX, cpu.ifl,
            irq_count[0], irq_count[1], pic_mask, pic_isr);
    for (int i = shadow_depth - 1, n = 0; i >= 0 && n < 16; i--, n++) {
        struct shadow *s = &shadow_stack[i & (SHADOW_MAX - 1)];
        fprintf(stderr, "    return to %04x:%04x (root seg %04x)\n", s->cs, s->ip,
                (uint16_t)(s->cs - LOADSEG));
    }
}

#ifdef MEMWATCH
static void memwatch_arm(void);
#endif
void poll_slow(void)
{
    poll_counter = 4000;
#ifdef MEMWATCH
    memwatch_arm();
#endif
    if (deterministic)
        virt_us += 4000;
    uint64_t now = now_us();
    if (!dump_at) {
        const char *e = getenv("CIV_DUMP");
        dump_at = e ? (uint64_t)(atof(e) * 1e6) : ~0ULL;
    }
    if (now >= dump_at) {
        debug_dump();
        const char *e = getenv("CIV_DUMP_EVERY");
        dump_at = e ? now + (uint64_t)(atof(e) * 1e6) : ~0ULL;
    }

    if (now >= next_frame && !in_poll) {
        in_poll = 1;
        next_frame = now + 1000000 / 60;
        plat_pump_events();
        script_tick();
        native_frame();
        if (!deterministic)
            plat_present();
        in_poll = 0;
        if (quit_requested) {
            gfx_shadow_report();
            plat_shutdown();
            exit(0);
        }
    }

    /* timer */
    if (now >= pit0_next) {
        pic_irr |= 1;
        pit0_next += pit0_period_us;
        if (now > pit0_next + 4 * pit0_period_us)
            pit0_next = now + pit0_period_us;   /* drop lost ticks */
    }
    /* keyboard */
    if (!kbd_full && kq_head != kq_tail) {
        kbd_port60 = kq[kq_head];
        kq_head = (kq_head + 1) & 63;
        kbd_full = 1;
        pic_irr |= 2;
    }
    if (!cpu.ifl || irq_hold)
        return;
    if ((pic_irr & 1) && irq_deliverable(0))
        deliver(0);
    if ((pic_irr & 2) && irq_deliverable(1) && cpu.ifl) {
        deliver(1);
        kbd_full = 0;
    }
    if (cpu.ifl)
        mouse_poll_callback();
}

void hlt_wait(void)
{
    uint64_t start = now_us();
    int ifl = cpu.ifl;
    cpu.ifl = 1;
    while (now_us() - start < 2000) {
        uint8_t irr = pic_irr;
        poll_slow();
        if (irr != pic_irr || quit_requested)
            break;
        if (!deterministic)
            plat_sleep_us(500);
    }
    cpu.ifl = (uint8_t)ifl;
}

/* ---------------------------------------------------------------- */
/* ports */

static uint8_t vga_status(void)
{
    /* 70 Hz vertical refresh, retrace lasts ~1.1 ms */
    uint64_t t = now_us() % 14286;
    uint8_t s = 0;
    if (t < 1100)
        s |= 0x08 | 0x01;
    else if ((t / 32) & 1)
        s |= 0x01;      /* horizontal retrace toggling */
    return s;
}

uint8_t port_in8(uint16_t port)
{
    switch (port) {
    case 0x20:
        return pic_irr;
    case 0x21:
        return pic_mask;
    case 0x40:
    case 0x41:
    case 0x42: {
        int ch = port - 0x40;
        uint16_t v;
        if (pit_latched[ch])
            v = pit_latch[ch];
        else {
            uint32_t d = pit_div[ch] ? pit_div[ch] : 65536;
            uint64_t ticks = now_us() * 1193182ULL / 1000000ULL;
            v = (uint16_t)(d - (ticks % d));
        }
        uint8_t r = pit_latch_flip[ch] ? (uint8_t)(v >> 8) : (uint8_t)v;
        pit_latch_flip[ch] ^= 1;
        if (!pit_latch_flip[ch])
            pit_latched[ch] = 0;
        return r;
    }
    case 0x60:
        return kbd_port60;
    case 0x61:
        return (uint8_t)(port61 | ((now_us() / 15) & 1 ? 0x10 : 0) |
                         ((now_us() / 500) & 1 ? 0x20 : 0));
    case 0x64:
        return (uint8_t)(0x14 | (kbd_full ? 1 : 0));
    case 0x201:
        return 0xF0;     /* no joystick */
    case 0x388:
    case 0x389:
        return adlib_status();
    case 0x3C7:
        return 3;
    case 0x3C8:
        return dac_widx;
    case 0x3C9: {
        uint8_t v = vga_dac[dac_ridx][dac_rcomp];
        if (++dac_rcomp == 3) {
            dac_rcomp = 0;
            dac_ridx++;
        }
        return v;
    }
    case 0x3C5:
        return seq[seq_idx & 7];
    case 0x3CF:
        return gc[gc_idx & 15];
    case 0x3D5:
        return crtc[crtc_idx & 31];
    case 0x3DA:
    case 0x3BA:
        attr_flip = 0;
        return vga_status();
    default:
        if (verbose > 1)
            warn("in from port %04x", port);
        return 0xFF;
    }
}

uint16_t port_in16(uint16_t port)
{
    return (uint16_t)(port_in8(port) | port_in8((uint16_t)(port + 1)) << 8);
}

void port_out8(uint16_t port, uint8_t v)
{
    switch (port) {
    case 0x20:
        if ((v & 0xE0) == 0x20) {      /* EOI: clear highest priority */
            for (int i = 0; i < 8; i++)
                if (pic_isr & (1 << i)) {
                    pic_isr &= (uint8_t)~(1 << i);
                    break;
                }
        }
        break;
    case 0x21:
        pic_mask = v;
        break;
    case 0x40:
    case 0x41:
    case 0x42: {
        int ch = port - 0x40;
        int access = (pit_mode[ch] >> 4) & 3;
        if (access == 1)
            pit_div[ch] = (pit_div[ch] & 0xFF00) | v;
        else if (access == 2)
            pit_div[ch] = (uint16_t)((pit_div[ch] & 0xFF) | v << 8);
        else {
            if (!pit_flip[ch])
                pit_div[ch] = (pit_div[ch] & 0xFF00) | v;
            else
                pit_div[ch] = (uint16_t)((pit_div[ch] & 0xFF) | v << 8);
            pit_flip[ch] ^= 1;
        }
        if (access != 3 || !pit_flip[ch]) {
            if (ch == 0) {
                timer_set_divisor(pit_div[0]);
                pit0_next = now_us() + pit0_period_us;
            } else if (ch == 2)
                speaker_set(port61, pit_div[2]);
        }
        break;
    }
    case 0x43: {
        int ch = v >> 6;
        if (ch == 3)
            break;
        if (((v >> 4) & 3) == 0) {     /* latch */
            uint32_t d = pit_div[ch] ? pit_div[ch] : 65536;
            uint64_t ticks = now_us() * 1193182ULL / 1000000ULL;
            pit_latch[ch] = (uint16_t)(d - (ticks % d));
            pit_latched[ch] = 1;
            pit_latch_flip[ch] = 0;
            break;
        }
        pit_mode[ch] = v;
        pit_flip[ch] = 0;
        break;
    }
    case 0x61:
        port61 = v;
        speaker_set(port61, pit_div[2]);
        break;
    case 0x388:
        adlib_write_index(v);
        break;
    case 0x389:
        adlib_write_data(v);
        break;
    case 0x3C7:
        dac_ridx = v;
        dac_rcomp = 0;
        break;
    case 0x3C8:
        dac_widx = v;
        dac_wcomp = 0;
        break;
    case 0x3C9:
        vga_dac[dac_widx][dac_wcomp] = v & 0x3F;
        if (++dac_wcomp == 3) {
            dac_wcomp = 0;
            dac_widx++;
        }
        break;
    case 0x3C0:
        attr_flip ^= 1;
        break;
    case 0x3C4:
        seq_idx = v;
        break;
    case 0x3C5:
        seq[seq_idx & 7] = v;
        break;
    case 0x3CE:
        gc_idx = v;
        break;
    case 0x3CF:
        gc[gc_idx & 15] = v;
        break;
    case 0x3D4:
        crtc_idx = v;
        break;
    case 0x3D5:
        crtc[crtc_idx & 31] = v;
        break;
    default:
        if (verbose > 1)
            warn("out %02x to port %04x", v, port);
        break;
    }
}

void port_out16(uint16_t port, uint16_t v)
{
    port_out8(port, (uint8_t)v);
    port_out8((uint16_t)(port + 1), (uint8_t)(v >> 8));
}

#ifdef MEMWATCH
/* Debug aid: report code outside the graphics driver that writes to the
   driver's page buffers. */
uint32_t memwatch_lo[4], memwatch_hi[4];
void memwatch_hit(uint32_t lin)
{
    static struct { uint16_t cs, ip; } seen[256];
    static int nseen;
    if (CS == modbase[MOD_MGRAPHIC] && lin < memwatch_lo[3])
        return;
    uint16_t ip = shadow_depth > 0 ? SHADOW_TOP()->ip : 0;
    uint16_t rcs = shadow_depth > 0 ? SHADOW_TOP()->cs : 0;
    for (int i = 0; i < nseen; i++)
        if (seen[i].cs == CS && seen[i].ip == ip)
            return;
    if (nseen < 256) {
        seen[nseen].cs = CS;
        seen[nseen].ip = ip;
        nseen++;
    }
    fprintf(stderr, "MEMWATCH write %05x by CS=%04x (root seg %04x), caller ret %04x:%04x\n",
            lin, CS, (uint16_t)(CS - LOADSEG), rcs, ip);
}

static void memwatch_arm(void)
{
    uint16_t drv = modbase[MOD_MGRAPHIC];
    if (!drv)
        return;
    static int armed;
    const char *e = getenv("CIV_WATCH");    /* "lo,hi" linear range */
    if (e) {
        unsigned lo, hi;
        if (sscanf(e, "%x,%x", &lo, &hi) == 2) {
            memwatch_lo[3] = lo;
            memwatch_hi[3] = hi;
        }
    }
    if (getenv("CIV_WATCH_ONLY"))
        return;
    for (int i = 0; i < 3; i++) {
        uint16_t seg = RW(drv, (uint16_t)(0x1970 + 2 * i));
        memwatch_lo[i] = (uint32_t)seg << 4;
        memwatch_hi[i] = memwatch_lo[i] + 64000;
    }
    if (!armed && memwatch_lo[1] && memwatch_lo[1] != 0xA0000) {
        armed = 1;
        fprintf(stderr, "MEMWATCH armed: %05x %05x %05x\n", memwatch_lo[0],
                memwatch_lo[1], memwatch_lo[2]);
    }
}
#endif
