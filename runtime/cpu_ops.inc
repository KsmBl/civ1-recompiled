/* Instruction helpers shared by the runtime and the test harness
   (included from cpu.c and tests/harness.c). */

void rep_movsb(uint16_t s)
{
    while (CX) {
        MOVSB(s);
        CX--;
    }
}

void rep_movsw(uint16_t s)
{
    while (CX) {
        MOVSW(s);
        CX--;
    }
}

void rep_stosb(void)
{
    if (!DF && (uint32_t)DI + CX <= 0x10000) {
        memset(&mem[LIN(ES, DI)], AL, CX);
        DI += CX;
        CX = 0;
        return;
    }
    while (CX) {
        STOSB(0);
        CX--;
    }
}

void rep_stosw(void)
{
    while (CX) {
        STOSW(0);
        CX--;
    }
}

void DAA(void)
{
    uint8_t al = AL, cf = CF;
    if ((al & 0xf) > 9 || AF) {
        AL += 6;
        AF = 1;
    } else
        AF = 0;
    if (al > 0x99 || cf) {
        AL += 0x60;
        CF = 1;
    } else
        CF = 0;
    SZP8(AL, F_ALL);
}

void DAS(void)
{
    uint8_t al = AL, cf = CF;
    CF = 0;
    if ((al & 0xf) > 9 || AF) {
        CF = cf || al < 6;      /* borrow from AL - 6 */
        AL -= 6;
        AF = 1;
    } else
        AF = 0;
    if (al > 0x99 || cf) {
        AL -= 0x60;
        CF = 1;
    }
    SZP8(AL, F_ALL);
}

void AAA(void)
{
    if ((AL & 0xf) > 9 || AF) {
        AX += 0x106;
        AF = CF = 1;
    } else
        AF = CF = 0;
    AL &= 0xf;
}

void AAS(void)
{
    if ((AL & 0xf) > 9 || AF) {
        AX -= 6;
        AH -= 1;
        AF = CF = 1;
    } else
        AF = CF = 0;
    AL &= 0xf;
}

void AAM(uint8_t b)
{
    if (!b) {
        raise_int0();
        return;
    }
    AH = AL / b;
    AL = AL % b;
    SZP8(AL, F_ALL);
}

void AAD(uint8_t b)
{
    AL = (uint8_t)(AH * b + AL);
    AH = 0;
    SZP8(AL, F_ALL);
}

void ENTER(uint16_t size, uint8_t level)
{
    PUSH(BP);
    uint16_t fp = SP;
    level &= 31;
    for (int i = 1; i < level; i++) {
        BP -= 2;
        PUSH(RW(SS, BP));
    }
    if (level)
        PUSH(fp);
    BP = fp;
    SP -= size;
}

