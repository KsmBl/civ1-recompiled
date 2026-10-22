/*
 * Reference 8086/80286 interpreter.
 *
 * Executes code straight from emulated memory.  It is used
 *   - as a fallback for code the static lifter did not find (so a missed
 *     function pointer target costs speed instead of crashing), and
 *   - in reference mode (CIV_INTERP=1) to run the whole program without
 *     the lifted code, for differential testing against it.
 *
 * Calls are executed by C recursion exactly like the lifted code does, and
 * the shared shadow stack decides where a return goes, so interpreted and
 * lifted code can call each other freely.
 */
#include <stdio.h>
#include <stdlib.h>
#include "cpu.h"
#include "runtime.h"

int interp_ref_mode;
long interp_insns;

static uint16_t *const r16[8] = {&cpu.a.x, &cpu.c.x, &cpu.d.x, &cpu.b.x,
                                 &cpu.sp, &cpu.bp, &cpu.si, &cpu.di};
static uint8_t *const r8[8] = {&cpu.a.b.l, &cpu.c.b.l, &cpu.d.b.l, &cpu.b.b.l,
                               &cpu.a.b.h, &cpu.c.b.h, &cpu.d.b.h, &cpu.b.b.h};
static uint16_t *const sr[4] = {&cpu.es, &cpu.cs, &cpu.ss, &cpu.ds};

/* decoded ModRM operand */
struct modrm {
    int mod, reg, rm;
    uint16_t seg, ea;
};

struct ctx {
    uint16_t ip;          /* next byte to fetch */
    uint16_t start;       /* address of current instruction */
    int segov;            /* -1 or segment register value index */
    int rep;              /* 0, 0xF3, 0xF2 */
};

static uint8_t fetch8(struct ctx *c) { return RB(CS, c->ip++); }
static uint16_t fetch16(struct ctx *c)
{
    uint16_t v = RW(CS, c->ip);
    c->ip += 2;
    return v;
}

static uint16_t seg_default(struct ctx *c, uint16_t def)
{
    return c->segov >= 0 ? *sr[c->segov] : def;
}

static void decode_modrm(struct ctx *c, struct modrm *m)
{
    uint8_t b = fetch8(c);
    m->mod = b >> 6;
    m->reg = (b >> 3) & 7;
    m->rm = b & 7;
    if (m->mod == 3)
        return;
    uint16_t ea = 0, def = DS;
    switch (m->rm) {
    case 0: ea = BX + SI; break;
    case 1: ea = BX + DI; break;
    case 2: ea = BP + SI; def = SS; break;
    case 3: ea = BP + DI; def = SS; break;
    case 4: ea = SI; break;
    case 5: ea = DI; break;
    case 6:
        if (m->mod == 0) {
            ea = fetch16(c);
        } else {
            ea = BP;
            def = SS;
        }
        break;
    case 7: ea = BX; break;
    }
    if (m->mod == 1)
        ea += (uint16_t)(int8_t)fetch8(c);
    else if (m->mod == 2)
        ea += fetch16(c);
    m->ea = ea;
    m->seg = seg_default(c, def);
}

static uint8_t get8(struct modrm *m)
{
    return m->mod == 3 ? *r8[m->rm] : RB(m->seg, m->ea);
}
static uint16_t get16(struct modrm *m)
{
    return m->mod == 3 ? *r16[m->rm] : RW(m->seg, m->ea);
}
static void put8(struct modrm *m, uint8_t v)
{
    if (m->mod == 3)
        *r8[m->rm] = v;
    else
        WB(m->seg, m->ea, v);
}
static void put16(struct modrm *m, uint16_t v)
{
    if (m->mod == 3)
        *r16[m->rm] = v;
    else
        WW(m->seg, m->ea, v);
}

static int cond(int cc)
{
    switch (cc) {
    case 0: return OF;
    case 1: return !OF;
    case 2: return CF;
    case 3: return !CF;
    case 4: return ZF;
    case 5: return !ZF;
    case 6: return CF || ZF;
    case 7: return !CF && !ZF;
    case 8: return SF;
    case 9: return !SF;
    case 10: return PF;
    case 11: return !PF;
    case 12: return SF != OF;
    case 13: return SF == OF;
    case 14: return ZF || SF != OF;
    default: return !ZF && SF == OF;
    }
}

static uint8_t alu8(int op, uint8_t a, uint8_t b)
{
    switch (op) {
    case 0: return ADD8(a, b, F_ALL);
    case 1: return OR8(a, b, F_ALL);
    case 2: return ADC8(a, b, F_ALL);
    case 3: return SBB8(a, b, F_ALL);
    case 4: return AND8(a, b, F_ALL);
    case 5: return SUB8(a, b, F_ALL);
    case 6: return XOR8(a, b, F_ALL);
    default: CMP8(a, b, F_ALL); return a;
    }
}
static uint16_t alu16(int op, uint16_t a, uint16_t b)
{
    switch (op) {
    case 0: return ADD16(a, b, F_ALL);
    case 1: return OR16(a, b, F_ALL);
    case 2: return ADC16(a, b, F_ALL);
    case 3: return SBB16(a, b, F_ALL);
    case 4: return AND16(a, b, F_ALL);
    case 5: return SUB16(a, b, F_ALL);
    case 6: return XOR16(a, b, F_ALL);
    default: CMP16(a, b, F_ALL); return a;
    }
}
static uint8_t shift8(int op, uint8_t a, uint8_t n)
{
    switch (op) {
    case 0: return ROL8(a, n, F_ALL);
    case 1: return ROR8(a, n, F_ALL);
    case 2: return RCL8(a, n, F_ALL);
    case 3: return RCR8(a, n, F_ALL);
    case 4: case 6: return SHL8(a, n, F_ALL);
    case 5: return SHR8(a, n, F_ALL);
    default: return SAR8(a, n, F_ALL);
    }
}
static uint16_t shift16(int op, uint16_t a, uint8_t n)
{
    switch (op) {
    case 0: return ROL16(a, n, F_ALL);
    case 1: return ROR16(a, n, F_ALL);
    case 2: return RCL16(a, n, F_ALL);
    case 3: return RCR16(a, n, F_ALL);
    case 4: case 6: return SHL16(a, n, F_ALL);
    case 5: return SHR16(a, n, F_ALL);
    default: return SAR16(a, n, F_ALL);
    }
}

/* string instruction with optional REP prefix */
static void string_op(struct ctx *c, uint8_t op)
{
    uint16_t s = seg_default(c, DS);
    int is_cmp = op == 0xA6 || op == 0xA7 || op == 0xAE || op == 0xAF;
    if (!c->rep) {
        switch (op) {
        case 0xA4: MOVSB(s); break;
        case 0xA5: MOVSW(s); break;
        case 0xA6: CMPSB(s); break;
        case 0xA7: CMPSW(s); break;
        case 0xAA: STOSB(s); break;
        case 0xAB: STOSW(s); break;
        case 0xAC: LODSB(s); break;
        case 0xAD: LODSW(s); break;
        case 0xAE: SCASB(s); break;
        case 0xAF: SCASW(s); break;
        case 0x6C: INSB(s); break;
        case 0x6D: INSW(s); break;
        case 0x6E: OUTSB(s); break;
        case 0x6F: OUTSW(s); break;
        }
        return;
    }
    while (CX) {
        switch (op) {
        case 0xA4: MOVSB(s); break;
        case 0xA5: MOVSW(s); break;
        case 0xA6: CMPSB(s); break;
        case 0xA7: CMPSW(s); break;
        case 0xAA: STOSB(s); break;
        case 0xAB: STOSW(s); break;
        case 0xAC: LODSB(s); break;
        case 0xAD: LODSW(s); break;
        case 0xAE: SCASB(s); break;
        case 0xAF: SCASW(s); break;
        case 0x6C: INSB(s); break;
        case 0x6D: INSW(s); break;
        case 0x6E: OUTSB(s); break;
        case 0x6F: OUTSW(s); break;
        }
        CX--;
        if (is_cmp) {
            if (c->rep == 0xF3 && !ZF)
                break;
            if (c->rep == 0xF2 && ZF)
                break;
        }
    }
}

/* perform a call made by interpreted code; returns 1 if the interpreter
   loop must return (unwinding past its frame) */
static int do_call(uint16_t seg, uint16_t off, uint16_t ret, int far)
{
    if (far)
        PUSH(CS);
    PUSH(ret);
    shadow_push(ret);
    if (far)
        CS = seg;
    func_t f = interp_ref_mode ? NULL : lookup_code(CS, off);
    if (f)
        f();
    else if (CS == BIOS_SEG && off < 0x400 && !(off & 3)) {
        bios_int((uint8_t)(off / 4));
        IRET();
    } else
        interp_exec(CS, off);
    shadow_pop();
    if (unwind_to >= 0) {
        if (shadow_depth > unwind_to)
            return 1;
        unwind_to = -1;
    }
    return 0;
}

/* Does a return/jump to (cs, ip) leave the interpreter frame?
   Mirrors ret_near/ret_far + ret_to of the lifted code. */
static int returns_here(uint16_t cs, uint16_t ip, uint16_t sp, int strict_sp)
{
    if (shadow_depth > 0) {
        struct shadow *s = SHADOW_TOP();
        if (s->ip == ip && s->cs == cs)
            return 1;
    }
    for (int k = shadow_depth - 2; k >= 0 && k >= shadow_depth - 256; k--) {
        struct shadow *s = &shadow_stack[k & (SHADOW_MAX - 1)];
        if (s->ip == ip && s->cs == cs &&
            (strict_sp ? s->sp == sp : (uint16_t)(SP - s->sp) <= 8)) {
            unwind_to = k;
            return 1;
        }
    }
    return 0;
}

static NORETURN void bad_op(struct ctx *c, uint8_t op)
{
    fatal("interpreter: unsupported opcode %02x at %04x:%04x", op, CS, c->start);
}

#define POLL() do { IRQ_POLL(); } while (0)

static void run(uint16_t seg, uint16_t off, int single);

void interp_exec(uint16_t seg, uint16_t off)
{
    if (!interp_ref_mode && verbose)
        logmsg("interpreting code at %04x:%04x (not lifted)", seg, off);
    run(seg, off, 0);
}

/* Execute the single (non control-flow) instruction at CS:off.  Used by
   lifted code for instructions that the program modifies at run time. */
void interp_step(uint16_t off)
{
    run(CS, off, 1);
}

static void run(uint16_t seg, uint16_t off, int single)
{
    struct ctx c;
    CS = seg;
    c.ip = off;
    static long itrace = -1, itrace_from;
    if (itrace < 0) {
        const char *e = getenv("CIV_ITRACE");
        itrace = e ? atol(e) : 0;
        e = getenv("CIV_ITRACE_FROM");
        itrace_from = e ? atol(e) : 0;
        if (itrace_from)
            itrace = 1L << 40;
    }
    if (itrace_from && trace_count() > itrace_from + 3)
        itrace = 0;
    for (;;) {
        interp_insns++;
        c.start = c.ip;
        if (itrace && interp_insns <= itrace && trace_count() >= itrace_from)
            fprintf(stderr, "I %04x:%04x %02x %02x %02x ax=%04x bx=%04x cx=%04x dx=%04x si=%04x di=%04x sp=%04x\n",
                    CS, c.ip, RB(CS, c.ip), RB(CS, c.ip + 1), RB(CS, c.ip + 2),
                    AX, BX, CX, DX, SI, DI, SP);
        c.segov = -1;
        c.rep = 0;
        uint8_t op;
        for (;;) {
            op = fetch8(&c);
            if (op == 0x26) c.segov = 0;
            else if (op == 0x2E) c.segov = 1;
            else if (op == 0x36) c.segov = 2;
            else if (op == 0x3E) c.segov = 3;
            else if (op == 0xF2 || op == 0xF3) c.rep = op;
            else if (op == 0xF0) ;
            else break;
        }
        struct modrm m;
        switch (op) {
        /* ALU r/m, r and immediate forms */
        case 0x00: case 0x08: case 0x10: case 0x18:
        case 0x20: case 0x28: case 0x30: case 0x38:
            decode_modrm(&c, &m);
            {
                uint8_t r = alu8(op >> 3, get8(&m), *r8[m.reg]);
                if ((op >> 3) != 7)
                    put8(&m, r);
            }
            break;
        case 0x01: case 0x09: case 0x11: case 0x19:
        case 0x21: case 0x29: case 0x31: case 0x39:
            decode_modrm(&c, &m);
            {
                uint16_t r = alu16(op >> 3, get16(&m), *r16[m.reg]);
                if ((op >> 3) != 7)
                    put16(&m, r);
            }
            break;
        case 0x02: case 0x0A: case 0x12: case 0x1A:
        case 0x22: case 0x2A: case 0x32: case 0x3A:
            decode_modrm(&c, &m);
            {
                uint8_t r = alu8(op >> 3, *r8[m.reg], get8(&m));
                if ((op >> 3) != 7)
                    *r8[m.reg] = r;
            }
            break;
        case 0x03: case 0x0B: case 0x13: case 0x1B:
        case 0x23: case 0x2B: case 0x33: case 0x3B:
            decode_modrm(&c, &m);
            {
                uint16_t r = alu16(op >> 3, *r16[m.reg], get16(&m));
                if ((op >> 3) != 7)
                    *r16[m.reg] = r;
            }
            break;
        case 0x04: case 0x0C: case 0x14: case 0x1C:
        case 0x24: case 0x2C: case 0x34: case 0x3C: {
            uint8_t r = alu8(op >> 3, AL, fetch8(&c));
            if ((op >> 3) != 7)
                AL = r;
            break;
        }
        case 0x05: case 0x0D: case 0x15: case 0x1D:
        case 0x25: case 0x2D: case 0x35: case 0x3D: {
            uint16_t r = alu16(op >> 3, AX, fetch16(&c));
            if ((op >> 3) != 7)
                AX = r;
            break;
        }
        case 0x06: PUSH(ES); break;
        case 0x07: ES = POP(); break;
        case 0x0E: PUSH(CS); break;
        case 0x16: PUSH(SS); break;
        case 0x17: SS = POP(); break;
        case 0x1E: PUSH(DS); break;
        case 0x1F: DS = POP(); break;
        case 0x27: DAA(); break;
        case 0x2F: DAS(); break;
        case 0x37: AAA(); break;
        case 0x3F: AAS(); break;
        case 0x40: case 0x41: case 0x42: case 0x43:
        case 0x44: case 0x45: case 0x46: case 0x47:
            *r16[op & 7] = INC16(*r16[op & 7], F_ALL);
            break;
        case 0x48: case 0x49: case 0x4A: case 0x4B:
        case 0x4C: case 0x4D: case 0x4E: case 0x4F:
            *r16[op & 7] = DEC16(*r16[op & 7], F_ALL);
            break;
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
            PUSH(*r16[op & 7]);
            break;
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            *r16[op & 7] = POP();
            break;
        case 0x60: PUSHA(); break;
        case 0x61: POPA(); break;
        case 0x62:
            decode_modrm(&c, &m);
            BOUND(*r16[m.reg], m.seg, m.ea, c.start);
            break;
        case 0x68: PUSH(fetch16(&c)); break;
        case 0x6A: PUSH((uint16_t)(int8_t)fetch8(&c)); break;
        case 0x69:
            decode_modrm(&c, &m);
            *r16[m.reg] = IMUL16X(get16(&m), fetch16(&c), F_ALL);
            break;
        case 0x6B:
            decode_modrm(&c, &m);
            *r16[m.reg] = IMUL16X(get16(&m), (uint16_t)(int8_t)fetch8(&c), F_ALL);
            break;
        case 0x6C: case 0x6D: case 0x6E: case 0x6F:
            string_op(&c, op);
            break;
        case 0x70: case 0x71: case 0x72: case 0x73:
        case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B:
        case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
            int8_t d = (int8_t)fetch8(&c);
            if (cond(op & 15)) {
                uint16_t t = (uint16_t)(c.ip + d);
                if (t <= c.start)
                    POLL();
                c.ip = t;
            }
            break;
        }
        case 0x80: case 0x82:
            decode_modrm(&c, &m);
            {
                uint8_t r = alu8(m.reg, get8(&m), fetch8(&c));
                if (m.reg != 7)
                    put8(&m, r);
            }
            break;
        case 0x81:
            decode_modrm(&c, &m);
            {
                uint16_t r = alu16(m.reg, get16(&m), fetch16(&c));
                if (m.reg != 7)
                    put16(&m, r);
            }
            break;
        case 0x83:
            decode_modrm(&c, &m);
            {
                uint16_t r = alu16(m.reg, get16(&m), (uint16_t)(int8_t)fetch8(&c));
                if (m.reg != 7)
                    put16(&m, r);
            }
            break;
        case 0x84:
            decode_modrm(&c, &m);
            SET_FLAGS_LOGICB(get8(&m) & *r8[m.reg], F_ALL);
            break;
        case 0x85:
            decode_modrm(&c, &m);
            SET_FLAGS_LOGICW(get16(&m) & *r16[m.reg], F_ALL);
            break;
        case 0x86:
            decode_modrm(&c, &m);
            {
                uint8_t t = get8(&m);
                put8(&m, *r8[m.reg]);
                *r8[m.reg] = t;
            }
            break;
        case 0x87:
            decode_modrm(&c, &m);
            {
                uint16_t t = get16(&m);
                put16(&m, *r16[m.reg]);
                *r16[m.reg] = t;
            }
            break;
        case 0x88: decode_modrm(&c, &m); put8(&m, *r8[m.reg]); break;
        case 0x89: decode_modrm(&c, &m); put16(&m, *r16[m.reg]); break;
        case 0x8A: decode_modrm(&c, &m); *r8[m.reg] = get8(&m); break;
        case 0x8B: decode_modrm(&c, &m); *r16[m.reg] = get16(&m); break;
        case 0x8C: decode_modrm(&c, &m); put16(&m, *sr[m.reg & 3]); break;
        case 0x8D:
            decode_modrm(&c, &m);
            *r16[m.reg] = m.ea;
            break;
        case 0x8E:
            decode_modrm(&c, &m);
            *sr[m.reg & 3] = get16(&m);
            break;
        case 0x8F:
            decode_modrm(&c, &m);
            {
                uint16_t v = POP();
                put16(&m, v);
            }
            break;
        case 0x90: break;
        case 0x91: case 0x92: case 0x93: case 0x94:
        case 0x95: case 0x96: case 0x97: {
            uint16_t t = AX;
            AX = *r16[op & 7];
            *r16[op & 7] = t;
            break;
        }
        case 0x98: AX = (uint16_t)(int16_t)(int8_t)AL; break;
        case 0x99: DX = (AX & 0x8000) ? 0xFFFF : 0; break;
        case 0x9A: {
            uint16_t o = fetch16(&c), s = fetch16(&c);
            if (do_call(s, o, c.ip, 1))
                return;
            break;
        }
        case 0x9B: break;
        case 0x9C: PUSH(GET_FLAGS()); break;
        case 0x9D: SET_FLAGS(POP()); break;
        case 0x9E: SET_FLAGS_LO(AH); break;
        case 0x9F: AH = (uint8_t)GET_FLAGS(); break;
        case 0xA0: AL = RB(seg_default(&c, DS), fetch16(&c)); break;
        case 0xA1: AX = RW(seg_default(&c, DS), fetch16(&c)); break;
        case 0xA2: WB(seg_default(&c, DS), fetch16(&c), AL); break;
        case 0xA3: WW(seg_default(&c, DS), fetch16(&c), AX); break;
        case 0xA4: case 0xA5: case 0xA6: case 0xA7:
        case 0xAA: case 0xAB: case 0xAC: case 0xAD:
        case 0xAE: case 0xAF:
            string_op(&c, op);
            break;
        case 0xA8: SET_FLAGS_LOGICB(AL & fetch8(&c), F_ALL); break;
        case 0xA9: SET_FLAGS_LOGICW(AX & fetch16(&c), F_ALL); break;
        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7:
            *r8[op & 7] = fetch8(&c);
            break;
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            *r16[op & 7] = fetch16(&c);
            break;
        case 0xC0:
            decode_modrm(&c, &m);
            put8(&m, shift8(m.reg, get8(&m), fetch8(&c)));
            break;
        case 0xC1:
            decode_modrm(&c, &m);
            put16(&m, shift16(m.reg, get16(&m), fetch8(&c)));
            break;
        case 0xC2: case 0xC3: case 0xCA: case 0xCB: case 0xCF: {
            uint16_t n = (op == 0xC2 || op == 0xCA) ? fetch16(&c) : 0;
            uint16_t sp = SP, ip = POP();
            if (op >= 0xCA)
                CS = POP();
            if (op == 0xCF)
                SET_FLAGS(POP());
            SP += n;
            if (returns_here(CS, ip, sp, 1))
                return;
            c.ip = ip;       /* "push addr; ret" jump */
            break;
        }
        case 0xC4: case 0xC5:
            decode_modrm(&c, &m);
            {
                uint16_t o = RW(m.seg, m.ea), s = RW(m.seg, (uint16_t)(m.ea + 2));
                *r16[m.reg] = o;
                if (op == 0xC4)
                    ES = s;
                else
                    DS = s;
            }
            break;
        case 0xC6: decode_modrm(&c, &m); put8(&m, fetch8(&c)); break;
        case 0xC7: decode_modrm(&c, &m); put16(&m, fetch16(&c)); break;
        case 0xC8: {
            uint16_t sz = fetch16(&c);
            ENTER(sz, fetch8(&c));
            break;
        }
        case 0xC9: SP = BP; BP = POP(); break;
        case 0xCC:
            do_int(3, c.ip);
            goto after_int;
        case 0xCD: {
            uint8_t n = fetch8(&c);
            if (n == 0x3F) {
                /* MS overlay manager call: CD 3F nn oooo */
                uint8_t ovl = fetch8(&c);
                uint16_t o = fetch16(&c);
                PUSH(CS);
                PUSH(c.ip);
                shadow_push(c.ip);
                int p = ovl_enter(ovl);
                CS = OVLSEG;
                func_t f = interp_ref_mode ? NULL : lookup_code(CS, o);
                if (f)
                    f();
                else
                    interp_exec(CS, o);
                ovl_leave(p);
                shadow_pop();
                if (unwind_to >= 0) {
                    if (shadow_depth > unwind_to)
                        return;
                    unwind_to = -1;
                }
                break;
            }
            do_int(n, c.ip);
        after_int:
            if (unwind_to >= 0) {
                if (shadow_depth > unwind_to)
                    return;
                unwind_to = -1;
            }
            break;
        }
        case 0xCE:
            if (OF) {
                do_int(4, c.ip);
                goto after_int;
            }
            break;
        case 0xD0: decode_modrm(&c, &m); put8(&m, shift8(m.reg, get8(&m), 1)); break;
        case 0xD1: decode_modrm(&c, &m); put16(&m, shift16(m.reg, get16(&m), 1)); break;
        case 0xD2: decode_modrm(&c, &m); put8(&m, shift8(m.reg, get8(&m), CL)); break;
        case 0xD3: decode_modrm(&c, &m); put16(&m, shift16(m.reg, get16(&m), CL)); break;
        case 0xD4: AAM(fetch8(&c)); break;
        case 0xD5: AAD(fetch8(&c)); break;
        case 0xD6: AL = CF ? 0xFF : 0; break;
        case 0xD7: AL = RB(seg_default(&c, DS), (uint16_t)(BX + AL)); break;
        case 0xE0: case 0xE1: case 0xE2: {
            int8_t d = (int8_t)fetch8(&c);
            CX--;
            int take = CX != 0;
            if (op == 0xE0)
                take = take && !ZF;
            else if (op == 0xE1)
                take = take && ZF;
            if (take) {
                uint16_t t = (uint16_t)(c.ip + d);
                if (t <= c.start)
                    POLL();
                c.ip = t;
            }
            break;
        }
        case 0xE3: {
            int8_t d = (int8_t)fetch8(&c);
            if (CX == 0)
                c.ip = (uint16_t)(c.ip + d);
            break;
        }
        case 0xE4: AL = port_in8(fetch8(&c)); POLL(); break;
        case 0xE5: AX = port_in16(fetch8(&c)); POLL(); break;
        case 0xE6: port_out8(fetch8(&c), AL); break;
        case 0xE7: port_out16(fetch8(&c), AX); break;
        case 0xEC: AL = port_in8(DX); POLL(); break;
        case 0xED: AX = port_in16(DX); POLL(); break;
        case 0xEE: port_out8(DX, AL); break;
        case 0xEF: port_out16(DX, AX); break;
        case 0xE8: {
            uint16_t d = fetch16(&c);
            if (do_call(CS, (uint16_t)(c.ip + d), c.ip, 0))
                return;
            break;
        }
        case 0xE9: case 0xEB: {
            /* fetch the displacement before reading the updated IP */
            uint16_t d = op == 0xE9 ? fetch16(&c) : (uint16_t)(int8_t)fetch8(&c);
            uint16_t t = (uint16_t)(c.ip + d);
            if (t <= c.start)
                POLL();
            c.ip = t;
            break;
        }
        case 0xEA: {
            uint16_t o = fetch16(&c), s = fetch16(&c);
            if (s == BIOS_SEG && o < 0x400 && !(o & 3)) {
                bios_int((uint8_t)(o / 4));
                uint16_t sp = SP, ip = POP();
                CS = POP();
                SET_FLAGS(POP());
                if (returns_here(CS, ip, sp, 1))
                    return;
                c.ip = ip;
                break;
            }
            if (returns_here(s, o, SP, 0)) {
                CS = s;
                return;
            }
            CS = s;
            c.ip = o;
            break;
        }
        case 0xF4: HLT(c.start); break;
        case 0xF5: CF = !CF; break;
        case 0xF6:
            decode_modrm(&c, &m);
            switch (m.reg) {
            case 0: case 1: SET_FLAGS_LOGICB(get8(&m) & fetch8(&c), F_ALL); break;
            case 2: put8(&m, (uint8_t)~get8(&m)); break;
            case 3: put8(&m, NEG8(get8(&m), F_ALL)); break;
            case 4: MUL8(get8(&m), F_ALL); break;
            case 5: IMUL8(get8(&m), F_ALL); break;
            case 6: DIV8(get8(&m)); break;
            case 7: IDIV8(get8(&m)); break;
            }
            break;
        case 0xF7:
            decode_modrm(&c, &m);
            switch (m.reg) {
            case 0: case 1: SET_FLAGS_LOGICW(get16(&m) & fetch16(&c), F_ALL); break;
            case 2: put16(&m, (uint16_t)~get16(&m)); break;
            case 3: put16(&m, NEG16(get16(&m), F_ALL)); break;
            case 4: MUL16(get16(&m), F_ALL); break;
            case 5: IMUL16(get16(&m), F_ALL); break;
            case 6: DIV16(get16(&m)); break;
            case 7: IDIV16(get16(&m)); break;
            }
            break;
        case 0xF8: CF = 0; break;
        case 0xF9: CF = 1; break;
        case 0xFA: CLI(); break;
        case 0xFB: STI(); break;
        case 0xFC: DF = 0; break;
        case 0xFD: DF = 1; break;
        case 0xFE:
            decode_modrm(&c, &m);
            if (m.reg == 0)
                put8(&m, INC8(get8(&m), F_ALL));
            else if (m.reg == 1)
                put8(&m, DEC8(get8(&m), F_ALL));
            else
                bad_op(&c, op);
            break;
        case 0xFF:
            decode_modrm(&c, &m);
            switch (m.reg) {
            case 0: put16(&m, INC16(get16(&m), F_ALL)); break;
            case 1: put16(&m, DEC16(get16(&m), F_ALL)); break;
            case 2: {
                uint16_t t = get16(&m);
                if (do_call(CS, t, c.ip, 0))
                    return;
                break;
            }
            case 3: {
                uint16_t o = RW(m.seg, m.ea), s = RW(m.seg, (uint16_t)(m.ea + 2));
                if (do_call(s, o, c.ip, 1))
                    return;
                break;
            }
            case 4: {
                uint16_t t = get16(&m);
                if (returns_here(CS, t, SP, 0))
                    return;
                /* no IRQ poll: the lifted code does not poll on
                   indirect jumps either (keeps timing identical) */
                c.ip = t;
                break;
            }
            case 5: {
                uint16_t o = RW(m.seg, m.ea), s = RW(m.seg, (uint16_t)(m.ea + 2));
                if (s == BIOS_SEG && o < 0x400 && !(o & 3)) {
                    bios_int((uint8_t)(o / 4));
                    uint16_t sp = SP, ip = POP();
                    CS = POP();
                    SET_FLAGS(POP());
                    if (returns_here(CS, ip, sp, 1))
                        return;
                    c.ip = ip;
                    break;
                }
                if (returns_here(s, o, SP, 0)) {
                    CS = s;
                    return;
                }
                CS = s;
                c.ip = o;
                break;
            }
            case 6: PUSH(get16(&m)); break;
            default: bad_op(&c, op);
            }
            break;
        default:
            bad_op(&c, op);
        }
        if (single)
            return;
    }
}
