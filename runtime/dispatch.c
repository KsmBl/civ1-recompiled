/*
 * Run-time control transfer: indirect calls/jumps, overlays, interrupts.
 */
#include <stdio.h>
#include <stdlib.h>
#include "cpu.h"
#include "runtime.h"
#include "gfx.h"

uint16_t modbase[MOD_COUNT];
struct shadow shadow_stack[SHADOW_MAX];
int shadow_depth;
int unwind_to = -1;

/* ---------------------------------------------------------------- */
/* function lookup: open addressing hash on (module, seg, off) */

#define HASH_BITS 14
#define HASH_SIZE (1 << HASH_BITS)
static const struct func_entry *htab[HASH_SIZE];

static unsigned hkey(int mod, uint16_t seg, uint16_t off)
{
    uint32_t k = ((uint32_t)mod * 2654435761u) ^ ((uint32_t)seg << 16 | off);
    k *= 2246822519u;
    return (k >> (32 - HASH_BITS)) & (HASH_SIZE - 1);
}

void dispatch_init(void)
{
    for (int i = 0; i < func_table_len; i++) {
        const struct func_entry *e = &func_table[i];
        unsigned h = hkey(e->mod, e->seg, e->off);
        while (htab[h])
            h = (h + 1) & (HASH_SIZE - 1);
        htab[h] = e;
    }
}

static func_t find(int mod, uint16_t seg, uint16_t off)
{
    unsigned h = hkey(mod, seg, off);
    while (htab[h]) {
        const struct func_entry *e = htab[h];
        if (e->mod == mod && e->seg == seg && e->off == off)
            return e->fn;
        h = (h + 1) & (HASH_SIZE - 1);
    }
    return NULL;
}

static int ovl_cur;          /* overlay currently resident, 0 = none */
static void unknown_target(const char *what, uint16_t seg, uint16_t off);
static int jump_is_return(uint16_t cs, uint16_t ip);

/* Map a run-time seg:off to a lifted function.  Returns NULL if the
   address is not known code. */
func_t lookup_code(uint16_t seg, uint16_t off)
{
    if (interp_ref_mode)
        return NULL;
    if (seg == OVLSEG) {
        if (ovl_cur > 0)
            return find(MOD_OVL01 + ovl_cur - 1, OVL_LINKSEG, off);
        return NULL;
    }
    if (seg >= LOADSEG && seg < LOADSEG + 0x2A1B)
        return find(MOD_ROOT, (uint16_t)(seg - LOADSEG), off);
    for (int m = 0; m < MOD_COUNT; m++) {
        if (modbase[m] && seg >= modbase[m] && seg < modbase[m] + 0x1000) {
            if (m == MOD_MGRAPHIC && gfx_native && seg == modbase[m]) {
                func_t g = gfx_lookup(off);
                if (g)
                    return g;
                static uint16_t warned[64];
                static int nw;
                int seen = 0;
                for (int k = 0; k < nw; k++)
                    seen |= warned[k] == off;
                if (!seen && nw < 64) {
                    warned[nw++] = off;
                    warn("graphics driver entry %04x has no native version", off);
                }
            }
            func_t f = find(m, (uint16_t)(seg - modbase[m]), off);
            if (f)
                return f;
        }
    }
    /* normalised pointer into root code? */
    uint32_t lin = LIN(seg, off);
    if (lin >= (uint32_t)LOADSEG * 16 && lin < (uint32_t)(LOADSEG + 0x2A1B) * 16) {
        for (const struct func_entry *e = func_table;
             e < func_table + func_table_len; e++)
            if (e->mod == MOD_ROOT &&
                LIN((uint16_t)(e->seg + LOADSEG), e->off) == lin)
                return e->fn;
    }
    return NULL;
}

/* Tiny interpreter for code generated at run time that is not part of
   the lifted program: MSC's int86()/intdos() build "int nn; retf" on the
   stack.  Returns 1 if the code was executed (including its return). */
static int interp_stub(uint16_t seg, uint16_t off)
{
    for (int n = 0; n < 16; n++) {
        uint8_t op = RB(seg, off);
        switch (op) {
        case 0xCD: {
            uint8_t v = RB(seg, (uint16_t)(off + 1));
            uint16_t saved = CS;
            CS = seg;
            do_int(v, (uint16_t)(off + 2));
            CS = saved;
            off += 2;
            break;
        }
        case 0x44:
            SP++;
            off++;
            break;
        case 0x90:
            off++;
            break;
        case 0xCB:
            RET_FAR(0);
            return 1;
        case 0xCA:
            RET_FAR(RW(seg, (uint16_t)(off + 1)));
            return 1;
        case 0xC3:
            RET_NEAR(0);
            return 1;
        default:
            if (n == 0)
                return 0;
            fatal("unsupported opcode %02x in generated code at %04x:%04x",
                  op, seg, off);
        }
    }
    return 0;
}

static void unknown_target(const char *what, uint16_t seg, uint16_t off)
{
    const char *where = "?";
    char buf[64];
    if (seg == OVLSEG) {
        snprintf(buf, sizeof buf, "ovl%02d %04x:%04x", ovl_cur, OVL_LINKSEG, off);
        where = buf;
    } else if (seg >= LOADSEG && seg < LOADSEG + 0x2A1B) {
        snprintf(buf, sizeof buf, "root %04x:%04x", seg - LOADSEG, off);
        where = buf;
    } else {
        for (int m = 0; m < MOD_COUNT; m++)
            if (modbase[m] && seg >= modbase[m] && seg < modbase[m] + 0x1000) {
                static const char *names[] = MOD_NAMES;
                snprintf(buf, sizeof buf, "%s %04x:%04x", names[m],
                         seg - modbase[m], off);
                where = buf;
            }
    }
    fatal("%s to unknown code %04x:%04x (%s) -- add it to tools/seeds.txt",
          what, seg, off, where);
}

/* ---------------------------------------------------------------- */

void call_near(uint16_t target, uint16_t ret)
{
    func_t f = lookup_code(CS, target);
    PUSH(ret);
    shadow_push(ret);
    if (f)
        f();
    else
        interp_exec(CS, target);
    shadow_pop();
}

static int bios_far(uint16_t seg, uint16_t off);

void call_far(uint16_t seg, uint16_t off, uint16_t ret)
{
    PUSH(CS);
    PUSH(ret);
    shadow_push(ret);
    if (seg == BIOS_SEG && bios_far(seg, off)) {
        /* e.g. pushf; call far [old_vector] */
        shadow_pop();
        return;
    }
    func_t f = lookup_code(seg, off);
    if (!f && !interp_ref_mode && interp_stub(seg, off)) {
        shadow_pop();
        return;
    }
    CS = seg;
    if (f)
        f();
    else
        interp_exec(seg, off);
    shadow_pop();
}

void jump_near(uint16_t target)
{
    /* "pop reg; jmp reg" style return to a caller? */
    if (jump_is_return(CS, target))
        return;     /* the lifted code returns right after this */
    func_t f = lookup_code(CS, target);
    if (f)
        f();
    else
        interp_exec(CS, target);
}

void jump_near_ovl(int ovl, uint16_t target)
{
    (void)ovl;
    jump_near(target);
}

void jump_far(uint16_t seg, uint16_t off)
{
    if (seg == BIOS_SEG) {
        if (bios_far(seg, off))
            return;
    }
    if (jump_is_return(seg, off)) {
        CS = seg;
        return;
    }
    func_t f = lookup_code(seg, off);
    CS = seg;
    if (f)
        f();
    else
        interp_exec(seg, off);
}

/* A return popped an address that is not the innermost call's. */
void ret_to(uint16_t cs, uint16_t ip, uint16_t sp)
{
    for (int k = shadow_depth - 2; k >= 0 && k >= shadow_depth - 256; k--) {
        struct shadow *s = &shadow_stack[k & (SHADOW_MAX - 1)];
        if (s->ip == ip && s->cs == cs && s->sp == sp) {
            unwind_to = k;     /* frames above k return immediately */
            return;
        }
    }
    /* not a return address at all: "push target; ret" */
    func_t f = lookup_code(cs, ip);
    if (f)
        f();
    else
        interp_exec(cs, ip);
}

/* Is (cs, ip) the return address of an active frame?  Used for
   "pop reg; jmp reg" returns and far jumps through saved return
   addresses. */
static int jump_is_return(uint16_t cs, uint16_t ip)
{
    for (int k = shadow_depth - 1; k >= 0 && k >= shadow_depth - 256; k--) {
        struct shadow *s = &shadow_stack[k & (SHADOW_MAX - 1)];
        /* the innermost frame may return with a different SP (_setargv
           leaves argv on the stack) */
        if (s->ip == ip && s->cs == cs &&
            (k == shadow_depth - 1 || (uint16_t)(SP - s->sp) <= 8)) {
            if (k != shadow_depth - 1)
                unwind_to = k;
            return 1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* overlays */

void ovl_load(int n)
{
    const struct overlay *o = &overlays[n];
    if (!o->image)
        fatal("overlay %d not present", n);
    uint32_t base = LIN(OVLSEG, 0);
    memcpy(&mem[base], o->image, o->size);
    for (int i = 0; i < o->nrel; i++) {
        uint32_t a = base + o->rel[i];
        uint16_t v = (uint16_t)(mem[a] | mem[a + 1] << 8);
        v += LOADSEG;
        mem[a] = (uint8_t)v;
        mem[a + 1] = (uint8_t)(v >> 8);
    }
    ovl_cur = n;
}

int ovl_enter(int n)
{
    int prev = ovl_cur;
    if (n != ovl_cur)
        ovl_load(n);
    return prev;
}

void ovl_leave(int prev)
{
    if (prev && prev != ovl_cur)
        ovl_load(prev);
}

void ovl_call_id(int n, uint16_t off, uint16_t ret)
{
    PUSH(CS);
    PUSH(ret);
    shadow_push(ret);
    int p = ovl_enter(n);
    func_t f = interp_ref_mode ? NULL : find(MOD_OVL01 + n - 1, OVL_LINKSEG, off);
    CS = OVLSEG;
    if (f)
        f();
    else
        interp_exec(OVLSEG, off);
    ovl_leave(p);
    shadow_pop();
}

/* ---------------------------------------------------------------- */
/* interrupts */

/* The IVT initially points to F000:<n*4>; such vectors are serviced by
   native code.  If the program installs its own handler the lifted handler
   is called with a real interrupt frame. */

void do_int(uint8_t n, uint16_t ret)
{
    uint16_t off = RW(0, n * 4), seg = RW(0, n * 4 + 2);
    if (seg == BIOS_SEG && off == n * 4) {
        trace_event("bios", n);
        bios_int(n);
        return;
    }
    func_t f = lookup_code(seg, off);
    trace_event("int", n);
    PUSH(GET_FLAGS());
    PUSH(CS);
    PUSH(ret);
    shadow_push(ret);
    cpu.ifl = 0;
    cpu.tf = 0;
    CS = seg;
    if (f)
        f();
    else
        interp_exec(seg, off);
    shadow_pop();
}

/* Deliver a hardware interrupt between two instructions. */
void raise_irq_vector(uint8_t n)
{
    uint16_t off = RW(0, n * 4), seg = RW(0, n * 4 + 2);
    if (seg == BIOS_SEG && off == n * 4) {
        bios_int(n);
        return;
    }
    func_t f = lookup_code(seg, off);
    uint16_t saved_cs = CS;
    /* Lifted code only keeps live flags up to date, so the interrupted
       code's arithmetic flags are saved here and restored after the handler
       instead of trusting the copy in the interrupt frame. */
    uint8_t sf[6] = {CF, PF, AF, ZF, SF, OF};
    PUSH((uint16_t)(GET_FLAGS() & ~F_ALL));
    PUSH(CS);
    PUSH(0xFFFF);
    shadow_push(0xFFFF);
    cpu.ifl = 0;
    cpu.tf = 0;
    CS = seg;
    if (f)
        f();
    else
        interp_exec(seg, off);
    shadow_pop();
    CS = saved_cs;
    CF = sf[0]; PF = sf[1]; AF = sf[2]; ZF = sf[3]; SF = sf[4]; OF = sf[5];
}

/* far call/jump into the BIOS segment: run the native handler and
   perform the IRET the real ROM code would execute */
static int bios_far(uint16_t seg, uint16_t off)
{
    (void)seg;
    if (off >= 0x400 || (off & 3))
        return 0;
    bios_int((uint8_t)(off / 4));
    IRET();
    return 1;
}
