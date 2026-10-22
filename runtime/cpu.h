/*
 * Emulated 8086/80286 CPU state and instruction helpers used by the lifted
 * code in gen/.  Everything is global: lifted functions take no arguments and
 * communicate through the register file and the emulated stack, exactly like
 * the original machine code did.
 */
#pragma once
#include <stdint.h>
#include <string.h>
#include "modules.h"

#if defined(__GNUC__)
#define INLINE static inline __attribute__((always_inline))
#define NORETURN __attribute__((noreturn))
#else
#define INLINE static inline
#define NORETURN
#endif

typedef union {
    uint16_t x;
    struct { uint8_t l, h; } b;
} reg16_t;

typedef struct {
    reg16_t a, b, c, d;
    uint16_t si, di, bp, sp;
    uint16_t cs, ds, es, ss;
    uint8_t cf, pf, af, zf, sf, of, df, ifl, tf;
} cpu_t;

extern cpu_t cpu;
extern uint8_t mem[];
#define MEM_SIZE 0x120000

/* Segment the root image is loaded at (PSP is LOADSEG-0x10) */
#ifndef LOADSEG
#define LOADSEG 0x0820
#endif
#define OVL_LINKSEG 0x2388
#define OVLSEG ((uint16_t)(LOADSEG + OVL_LINKSEG))
extern uint16_t modbase[MOD_COUNT];

#define AX cpu.a.x
#define BX cpu.b.x
#define CX cpu.c.x
#define DX cpu.d.x
#define AL cpu.a.b.l
#define AH cpu.a.b.h
#define BL cpu.b.b.l
#define BH cpu.b.b.h
#define CL cpu.c.b.l
#define CH cpu.c.b.h
#define DL cpu.d.b.l
#define DH cpu.d.b.h
#define SI cpu.si
#define DI cpu.di
#define BP cpu.bp
#define SP cpu.sp
#define CS cpu.cs
#define DS cpu.ds
#define ES cpu.es
#define SS cpu.ss
#define CF cpu.cf
#define PF cpu.pf
#define AF cpu.af
#define ZF cpu.zf
#define SF cpu.sf
#define OF cpu.of
#define DF cpu.df

#define F_CF 0x001
#define F_PF 0x004
#define F_AF 0x010
#define F_ZF 0x040
#define F_SF 0x080
#define F_OF 0x800
#define F_ALL (F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF)

/* ------------------------------------------------------------------ */
/* memory */

INLINE uint32_t LIN(uint16_t s, uint16_t o) { return ((uint32_t)s << 4) + o; }
INLINE uint8_t RB(uint16_t s, uint16_t o) { return mem[LIN(s, o)]; }
INLINE uint16_t RW(uint16_t s, uint16_t o)
{
    uint32_t a = LIN(s, o);
    return (uint16_t)(mem[a] | (mem[a + 1] << 8));
}
#ifdef MEMWATCH
void memwatch_hit(uint32_t lin);
extern uint32_t memwatch_lo[4], memwatch_hi[4];
#define MW(a) do { for (int w_ = 0; w_ < 4; w_++) \
    if ((a) >= memwatch_lo[w_] && (a) < memwatch_hi[w_]) memwatch_hit(a); } while (0)
#else
#define MW(a) ((void)0)
#endif
INLINE void WB(uint16_t s, uint16_t o, uint8_t v) { MW(LIN(s, o)); mem[LIN(s, o)] = v; }
INLINE void WW(uint16_t s, uint16_t o, uint16_t v)
{
    uint32_t a = LIN(s, o);
    MW(a);
    mem[a] = (uint8_t)v;
    mem[a + 1] = (uint8_t)(v >> 8);
}

INLINE void PUSH(uint16_t v) { SP -= 2; WW(SS, SP, v); }
INLINE uint16_t POP(void) { uint16_t v = RW(SS, SP); SP += 2; return v; }

/* ------------------------------------------------------------------ */
/* flags */

extern const uint8_t parity_tab[256];

uint16_t GET_FLAGS(void);
void SET_FLAGS(uint16_t f);
void SET_FLAGS_LO(uint8_t f);

#define SZP8(r, m) do { \
    if ((m) & F_ZF) ZF = (uint8_t)(r) == 0; \
    if ((m) & F_SF) SF = ((uint8_t)(r) >> 7) & 1; \
    if ((m) & F_PF) PF = parity_tab[(uint8_t)(r)]; } while (0)
#define SZP16(r, m) do { \
    if ((m) & F_ZF) ZF = (uint16_t)(r) == 0; \
    if ((m) & F_SF) SF = ((uint16_t)(r) >> 15) & 1; \
    if ((m) & F_PF) PF = parity_tab[(uint8_t)(r)]; } while (0)

#define DEF_ADD(N, T, TT, SH, ST) \
INLINE T ADD##N(T a, T b, unsigned m) { \
    TT r = (TT)a + b; T res = (T)r; \
    if (m & F_CF) CF = (r >> N) & 1; \
    if (m & F_OF) OF = (((a ^ res) & (b ^ res)) >> SH) & 1; \
    if (m & F_AF) AF = ((a ^ b ^ res) >> 4) & 1; \
    SZP##N(res, m); return res; } \
INLINE T ADC##N(T a, T b, unsigned m) { \
    TT r = (TT)a + b + CF; T res = (T)r; \
    if (m & F_CF) CF = (r >> N) & 1; \
    if (m & F_OF) OF = (((a ^ res) & (b ^ res)) >> SH) & 1; \
    if (m & F_AF) AF = ((a ^ b ^ res) >> 4) & 1; \
    SZP##N(res, m); return res; } \
INLINE T SUB##N(T a, T b, unsigned m) { \
    T res = (T)(a - b); \
    if (m & F_CF) CF = a < b; \
    if (m & F_OF) OF = (((a ^ b) & (a ^ res)) >> SH) & 1; \
    if (m & F_AF) AF = ((a ^ b ^ res) >> 4) & 1; \
    SZP##N(res, m); return res; } \
INLINE T SBB##N(T a, T b, unsigned m) { \
    uint8_t c = CF; T res = (T)(a - b - c); \
    if (m & F_CF) CF = (TT)a < (TT)b + c; \
    if (m & F_OF) OF = (((a ^ b) & (a ^ res)) >> SH) & 1; \
    if (m & F_AF) AF = ((a ^ b ^ res) >> 4) & 1; \
    SZP##N(res, m); return res; } \
INLINE void CMP##N(T a, T b, unsigned m) { (void)SUB##N(a, b, m); } \
INLINE T NEG##N(T a, unsigned m) { \
    T res = (T)(0 - a); \
    if (m & F_CF) CF = a != 0; \
    if (m & F_OF) OF = ((a & res) >> SH) & 1; \
    if (m & F_AF) AF = ((a ^ res) >> 4) & 1; \
    SZP##N(res, m); return res; } \
INLINE T INC##N(T a, unsigned m) { \
    T res = (T)(a + 1); \
    if (m & F_OF) OF = res == (T)((T)1 << SH); \
    if (m & F_AF) AF = (res & 0xf) == 0; \
    SZP##N(res, m); return res; } \
INLINE T DEC##N(T a, unsigned m) { \
    T res = (T)(a - 1); \
    if (m & F_OF) OF = a == (T)((T)1 << SH); \
    if (m & F_AF) AF = (a & 0xf) == 0; \
    SZP##N(res, m); return res; } \
INLINE T LOGIC##N(T res, unsigned m) { \
    if (m & F_CF) CF = 0; \
    if (m & F_OF) OF = 0; \
    if (m & F_AF) AF = 0; \
    SZP##N(res, m); return res; } \
INLINE T AND##N(T a, T b, unsigned m) { return LOGIC##N((T)(a & b), m); } \
INLINE T OR##N(T a, T b, unsigned m) { return LOGIC##N((T)(a | b), m); } \
INLINE T XOR##N(T a, T b, unsigned m) { return LOGIC##N((T)(a ^ b), m); } \
INLINE T SHL##N(T a, uint8_t c, unsigned m) { \
    c &= 0x1f; if (!c) return a; \
    T res = c > N ? 0 : (T)((TT)a << c); \
    if (m & F_CF) CF = c > N ? 0 : (((TT)a << c) >> N) & 1; \
    if (m & F_OF) OF = ((res >> SH) & 1) ^ CF; \
    if (m & F_AF) AF = 0; \
    SZP##N(res, m); return res; } \
INLINE T SHR##N(T a, uint8_t c, unsigned m) { \
    c &= 0x1f; if (!c) return a; \
    T res = c >= N ? 0 : (T)(a >> c); \
    if (m & F_CF) CF = c > N ? 0 : (a >> (c - 1)) & 1; \
    if (m & F_OF) OF = (a >> SH) & 1; \
    if (m & F_AF) AF = 0; \
    SZP##N(res, m); return res; } \
INLINE T SAR##N(T a, uint8_t c, unsigned m) { \
    c &= 0x1f; if (!c) return a; \
    if (c > N) c = N; \
    int32_t sv = (ST)a; T res = (T)(sv >> c); \
    if (m & F_CF) CF = (sv >> (c - 1)) & 1; \
    if (m & F_OF) OF = 0; \
    if (m & F_AF) AF = 0; \
    SZP##N(res, m); return res; } \
INLINE T ROL##N(T a, uint8_t c, unsigned m) { \
    c &= 0x1f; if (!c) return a; \
    uint8_t k = c % N; T res = k ? (T)((a << k) | (a >> (N - k))) : a; \
    if (m & (F_CF | F_OF)) CF = res & 1; \
    if (m & F_OF) OF = ((res >> SH) & 1) ^ (res & 1); \
    return res; } \
INLINE T ROR##N(T a, uint8_t c, unsigned m) { \
    c &= 0x1f; if (!c) return a; \
    uint8_t k = c % N; T res = k ? (T)((a >> k) | (a << (N - k))) : a; \
    if (m & (F_CF | F_OF)) CF = (res >> SH) & 1; \
    if (m & F_OF) OF = ((res >> SH) ^ (res >> (SH - 1))) & 1; \
    return res; } \
INLINE T RCL##N(T a, uint8_t c, unsigned m) { \
    c = (c & 0x1f) % (N + 1); (void)m; \
    while (c--) { uint8_t nc = (a >> SH) & 1; a = (T)((a << 1) | CF); CF = nc; } \
    OF = ((a >> SH) & 1) ^ CF; return a; } \
INLINE T RCR##N(T a, uint8_t c, unsigned m) { \
    c = (c & 0x1f) % (N + 1); (void)m; \
    if (c) OF = ((a >> SH) & 1) ^ CF; \
    while (c--) { uint8_t nc = a & 1; a = (T)((a >> 1) | ((T)CF << SH)); CF = nc; } \
    return a; }

DEF_ADD(8, uint8_t, uint16_t, 7, int8_t)
DEF_ADD(16, uint16_t, uint32_t, 15, int16_t)

#define SET_FLAGS_LOGICB(v, m) ((void)LOGIC8((uint8_t)(v), (m)))
#define SET_FLAGS_LOGICW(v, m) ((void)LOGIC16((uint16_t)(v), (m)))

INLINE void MUL8(uint8_t v, unsigned m)
{
    AX = (uint16_t)(AL * v);
    CF = OF = AH != 0;
    SZP8(AL, m);
}
INLINE void MUL16(uint16_t v, unsigned m)
{
    uint32_t r = (uint32_t)AX * v;
    AX = (uint16_t)r;
    DX = (uint16_t)(r >> 16);
    CF = OF = DX != 0;
    SZP16(AX, m);
}
INLINE void IMUL8(uint8_t v, unsigned m)
{
    int16_t r = (int16_t)((int8_t)AL * (int8_t)v);
    AX = (uint16_t)r;
    CF = OF = r != (int8_t)r;
    SZP8(AL, m);
}
INLINE void IMUL16(uint16_t v, unsigned m)
{
    int32_t r = (int32_t)(int16_t)AX * (int16_t)v;
    AX = (uint16_t)r;
    DX = (uint16_t)((uint32_t)r >> 16);
    CF = OF = r != (int16_t)r;
    SZP16(AX, m);
}
INLINE uint16_t IMUL16X(uint16_t a, uint16_t b, unsigned m)
{
    int32_t r = (int32_t)(int16_t)a * (int16_t)b;
    CF = OF = r != (int16_t)r;
    SZP16(r, m);
    return (uint16_t)r;
}

NORETURN void div_error(void);
void raise_int0(void);

INLINE void DIV8(uint8_t v)
{
    if (!v || AX / v > 0xff) { raise_int0(); return; }
    uint16_t q = AX / v, r = AX % v;
    AL = (uint8_t)q; AH = (uint8_t)r;
}
INLINE void DIV16(uint16_t v)
{
    uint32_t n = ((uint32_t)DX << 16) | AX;
    if (!v || n / v > 0xffff) { raise_int0(); return; }
    AX = (uint16_t)(n / v); DX = (uint16_t)(n % v);
}
INLINE void IDIV8(uint8_t v)
{
    int16_t n = (int16_t)AX; int8_t d = (int8_t)v;
    if (!d) { raise_int0(); return; }
    int16_t q = n / d, r = n % d;
    if (q > 127 || q < -128) { raise_int0(); return; }
    AL = (uint8_t)q; AH = (uint8_t)r;
}
INLINE void IDIV16(uint16_t v)
{
    int32_t n = (int32_t)(((uint32_t)DX << 16) | AX); int16_t d = (int16_t)v;
    if (!d) { raise_int0(); return; }
    int32_t q = n / d, r = n % d;
    if (q > 32767 || q < -32768) { raise_int0(); return; }
    AX = (uint16_t)q; DX = (uint16_t)r;
}

void DAA(void);
void DAS(void);
void AAA(void);
void AAS(void);
void AAM(uint8_t b);
void AAD(uint8_t b);

INLINE void PUSHA(void)
{
    uint16_t sp = SP;
    PUSH(AX); PUSH(CX); PUSH(DX); PUSH(BX);
    PUSH(sp); PUSH(BP); PUSH(SI); PUSH(DI);
}
INLINE void POPA(void)
{
    DI = POP(); SI = POP(); BP = POP(); (void)POP();
    BX = POP(); DX = POP(); CX = POP(); AX = POP();
}
void ENTER(uint16_t size, uint8_t level);

/* ------------------------------------------------------------------ */
/* string instructions */

#define STEP(n) (DF ? -(n) : (n))
INLINE void MOVSB(uint16_t s) { WB(ES, DI, RB(s, SI)); SI += STEP(1); DI += STEP(1); }
INLINE void MOVSW(uint16_t s) { WW(ES, DI, RW(s, SI)); SI += STEP(2); DI += STEP(2); }
INLINE void STOSB(uint16_t s) { (void)s; WB(ES, DI, AL); DI += STEP(1); }
INLINE void STOSW(uint16_t s) { (void)s; WW(ES, DI, AX); DI += STEP(2); }
INLINE void LODSB(uint16_t s) { AL = RB(s, SI); SI += STEP(1); }
INLINE void LODSW(uint16_t s) { AX = RW(s, SI); SI += STEP(2); }
INLINE void CMPSB(uint16_t s) { CMP8(RB(s, SI), RB(ES, DI), F_ALL); SI += STEP(1); DI += STEP(1); }
INLINE void CMPSW(uint16_t s) { CMP16(RW(s, SI), RW(ES, DI), F_ALL); SI += STEP(2); DI += STEP(2); }
INLINE void SCASB(uint16_t s) { (void)s; CMP8(AL, RB(ES, DI), F_ALL); DI += STEP(1); }
INLINE void SCASW(uint16_t s) { (void)s; CMP16(AX, RW(ES, DI), F_ALL); DI += STEP(2); }

void rep_movsb(uint16_t s);
void rep_movsw(uint16_t s);
void rep_stosb(void);
void rep_stosw(void);
#define REP_MOVSB(s) rep_movsb(s)
#define REP_MOVSW(s) rep_movsw(s)
#define REP_STOSB(s) rep_stosb()
#define REP_STOSW(s) rep_stosw()
#define REP_LODSB(s) do { while (CX) { LODSB(s); CX--; } } while (0)
#define REP_LODSW(s) do { while (CX) { LODSW(s); CX--; } } while (0)
#define REPE_CMPSB(s) do { while (CX) { CMPSB(s); CX--; if (!ZF) break; } } while (0)
#define REPE_CMPSW(s) do { while (CX) { CMPSW(s); CX--; if (!ZF) break; } } while (0)
#define REPNE_CMPSB(s) do { while (CX) { CMPSB(s); CX--; if (ZF) break; } } while (0)
#define REPNE_CMPSW(s) do { while (CX) { CMPSW(s); CX--; if (ZF) break; } } while (0)
#define REPE_SCASB(s) do { while (CX) { SCASB(s); CX--; if (!ZF) break; } } while (0)
#define REPE_SCASW(s) do { while (CX) { SCASW(s); CX--; if (!ZF) break; } } while (0)
#define REPNE_SCASB(s) do { while (CX) { SCASB(s); CX--; if (ZF) break; } } while (0)
#define REPNE_SCASW(s) do { while (CX) { SCASW(s); CX--; if (ZF) break; } } while (0)

/* ------------------------------------------------------------------ */
/* control flow */

typedef void (*func_t)(void);
struct func_entry { int mod; uint16_t seg, off; func_t fn; };
extern const struct func_entry func_table[];
extern const int func_table_len;

/* Shadow call stack.  Every call made by lifted code records the return
   address it pushed and the stack pointer at which it lives.  A return
   instruction checks the address it pops against this stack:
     - matches the top frame: ordinary return;
     - matches a deeper frame: the code returned past some callers (thunks
       that pop their own return address, longjmp-style code); the C frames
       in between are unwound by returning until that frame is reached;
     - matches nothing: "push addr; ret" style jump, dispatched as a jump. */
struct shadow { uint16_t cs, ip, sp; };
extern struct shadow shadow_stack[];
extern int shadow_depth;
extern int unwind_to;
#define SHADOW_MAX 4096
#define SHADOW_TOP() (&shadow_stack[(shadow_depth - 1) & (SHADOW_MAX - 1)])

INLINE void shadow_push(uint16_t ip)
{
    struct shadow *s = &shadow_stack[shadow_depth++ & (SHADOW_MAX - 1)];
    s->cs = CS; s->ip = ip; s->sp = SP;
}
#define shadow_pop() (shadow_depth--)

#if defined(__GNUC__)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define UNLIKELY(x) (x)
#endif
/* after a call returns: keep unwinding if a deeper frame is the target */
#define UNWIND_CHECK() do { if (UNLIKELY(unwind_to >= 0)) { \
    if (shadow_depth > unwind_to) return; unwind_to = -1; } } while (0)

#define CALLN(r, fn) do { PUSH(r); shadow_push(r); fn(); shadow_pop(); \
                          UNWIND_CHECK(); } while (0)
#define CALLF(seg, r, fn) do { PUSH(CS); PUSH(r); shadow_push(r); CS = (seg); \
                               fn(); shadow_pop(); UNWIND_CHECK(); } while (0)
#define JMPF(seg, fn) do { CS = (seg); fn(); } while (0)

void call_near(uint16_t target, uint16_t ret);
void call_far(uint16_t seg, uint16_t off, uint16_t ret);
void jump_near(uint16_t target);
void jump_near_ovl(int ovl, uint16_t target);
void jump_far(uint16_t seg, uint16_t off);
#define CALL_NEAR(t, r) do { call_near((t), (r)); UNWIND_CHECK(); } while (0)
#define CALL_FAR(s, o, r) do { call_far((s), (o), (r)); UNWIND_CHECK(); } while (0)
#define JUMP_NEAR(t) jump_near(t)
#define JUMP_FAR(s, o) jump_far((s), (o))

int ovl_enter(int n);
void ovl_leave(int prev);
#define OVL_CALL(n, fn, r) do { PUSH(CS); PUSH(r); shadow_push(r); \
    int p_ = ovl_enter(n); CS = OVLSEG; fn(); ovl_leave(p_); shadow_pop(); \
    UNWIND_CHECK(); } while (0)
void ovl_call_id(int n, uint16_t off, uint16_t ret);
#define OVL_CALL_ID(n, o, r) do { ovl_call_id((n), (o), (r)); UNWIND_CHECK(); } while (0)

void ret_to(uint16_t cs, uint16_t ip, uint16_t sp);
INLINE void ret_near(uint16_t n)
{
    uint16_t sp = SP, ip = POP();
    SP += n;
    struct shadow *s = SHADOW_TOP();
    if (UNLIKELY(shadow_depth <= 0 || s->ip != ip || s->cs != CS))
        ret_to(CS, ip, sp);
}
INLINE void ret_far(uint16_t n)
{
    uint16_t sp = SP, ip = POP();
    CS = POP();
    SP += n;
    struct shadow *s = SHADOW_TOP();
    if (UNLIKELY(shadow_depth <= 0 || s->ip != ip || s->cs != CS))
        ret_to(CS, ip, sp);
}
INLINE void iret(void)
{
    uint16_t sp = SP, ip = POP();
    CS = POP();
    SET_FLAGS(POP());
    struct shadow *s = SHADOW_TOP();
    if (UNLIKELY(shadow_depth <= 0 || s->ip != ip || s->cs != CS))
        ret_to(CS, ip, sp);
}
#define RET_NEAR(n) ret_near(n)
#define RET_FAR(n) ret_far(n)
#define IRET() iret()

void do_int(uint8_t n, uint16_t ret);
#define INT(n, r) do { do_int((n), (r)); UNWIND_CHECK(); } while (0)

void bad_code(uint16_t off, const char *fn);
void interp_step(uint16_t off);
#define INTERP_STEP(o) interp_step(o)
#define BAD_CODE(o) bad_code((o), __func__)
#define UNIMPL(o) bad_code((o), __func__)
void BOUND(uint16_t v, uint16_t seg, uint16_t ea, uint16_t ip);
void HLT(uint16_t ip);

/* interrupts and I/O */
extern int poll_counter;
void poll_slow(void);
#define IRQ_POLL() do { if (--poll_counter <= 0) poll_slow(); } while (0)
void CLI(void);
void STI(void);
uint8_t port_in8(uint16_t port);
uint16_t port_in16(uint16_t port);
void port_out8(uint16_t port, uint8_t v);
void port_out16(uint16_t port, uint16_t v);

/* string I/O */
INLINE void OUTSB(uint16_t s) { port_out8(DX, RB(s, SI)); SI += STEP(1); }
INLINE void OUTSW(uint16_t s) { port_out16(DX, RW(s, SI)); SI += STEP(2); }
INLINE void INSB(uint16_t s) { (void)s; WB(ES, DI, port_in8(DX)); DI += STEP(1); }
INLINE void INSW(uint16_t s) { (void)s; WW(ES, DI, port_in16(DX)); DI += STEP(2); }
#define REP_OUTSB(s) do { while (CX) { OUTSB(s); CX--; } } while (0)
#define REP_OUTSW(s) do { while (CX) { OUTSW(s); CX--; } } while (0)
#define REP_INSB(s) do { while (CX) { INSB(s); CX--; } } while (0)
#define REP_INSW(s) do { while (CX) { INSW(s); CX--; } } while (0)
