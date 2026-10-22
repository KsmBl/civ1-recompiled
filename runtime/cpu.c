/* CPU helpers that are too large to inline. */
#include <stdio.h>
#include <stdlib.h>
#include "cpu.h"
#include "runtime.h"

cpu_t cpu;
uint8_t mem[MEM_SIZE];

const uint8_t parity_tab[256] = {
#define P2(n) n, n ^ 1, n ^ 1, n
#define P4(n) P2(n), P2(n ^ 1), P2(n ^ 1), P2(n)
#define P6(n) P4(n), P4(n ^ 1), P4(n ^ 1), P4(n)
    P6(1), P6(0), P6(0), P6(1)
};

uint16_t GET_FLAGS(void)
{
    return (uint16_t)(0x0002 | (CF ? 0x001 : 0) | (PF ? 0x004 : 0) |
                      (AF ? 0x010 : 0) | (ZF ? 0x040 : 0) |
                      (SF ? 0x080 : 0) | (cpu.tf ? 0x100 : 0) |
                      (cpu.ifl ? 0x200 : 0) | (DF ? 0x400 : 0) |
                      (OF ? 0x800 : 0) | 0xF000);
}

void SET_FLAGS(uint16_t f)
{
    CF = f & 1;
    PF = (f >> 2) & 1;
    AF = (f >> 4) & 1;
    ZF = (f >> 6) & 1;
    SF = (f >> 7) & 1;
    cpu.tf = (f >> 8) & 1;
    cpu.ifl = (f >> 9) & 1;
    DF = (f >> 10) & 1;
    OF = (f >> 11) & 1;
}

void SET_FLAGS_LO(uint8_t f)
{
    CF = f & 1;
    PF = (f >> 2) & 1;
    AF = (f >> 4) & 1;
    ZF = (f >> 6) & 1;
    SF = (f >> 7) & 1;
}

#include "cpu_ops.inc"

void raise_int0(void)
{
    fatal("divide error (INT 0) at CS=%04x", CS);
}

void BOUND(uint16_t v, uint16_t seg, uint16_t ea, uint16_t ip)
{
    int16_t lo = (int16_t)RW(seg, ea), hi = (int16_t)RW(seg, (uint16_t)(ea + 2));
    if ((int16_t)v < lo || (int16_t)v > hi)
        fatal("BOUND exception at %04x:%04x", CS, ip);
}

void bad_code(uint16_t off, const char *fn)
{
    fatal("reached undecodable/unimplemented code at %04x:%04x in %s",
          CS, off, fn);
}

void CLI(void)
{
    cpu.ifl = 0;
}

void STI(void)
{
    cpu.ifl = 1;
}

void HLT(uint16_t ip)
{
    (void)ip;
    hlt_wait();
}
