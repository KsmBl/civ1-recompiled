/*
 * Loads CIV.EXE into emulated memory the way DOS + the EXEPACK stub would:
 * the resident image is unpacked and relocated at LOADSEG, the overlays that
 * follow it in the file are kept for ovl_load().
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"
#include "runtime.h"

struct overlay overlays[32];
uint16_t entry_cs, entry_ip, entry_ss, entry_sp;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static uint8_t *read_file(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        fatal("cannot open %s", path);
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(*len);
    if (fread(d, 1, *len, f) != (size_t)*len)
        fatal("cannot read %s", path);
    fclose(f);
    return d;
}

/* Unpack the EXEPACK'd resident image directly into memory at `seg`. */
static void unexepack(const uint8_t *d, long len, uint16_t seg)
{
    uint16_t cblp = rd16(d + 2), cp = rd16(d + 4), parhdr = rd16(d + 8);
    uint16_t ip = rd16(d + 0x14), cs = rd16(d + 0x16);
    long img_end = cblp ? (cp - 1) * 512L + cblp : cp * 512L;
    const uint8_t *img = d + parhdr * 16;
    long img_len = img_end - parhdr * 16;
    long ep = cs * 16L;
    if (img_end > len || ep + 18 > img_len)
        fatal("CIV.EXE: bad header");
    const uint8_t *h = img + ep;
    uint16_t skip = 1;
    if (ip == 0x12)
        skip = rd16(h + 14);
    else if (ip != 0x10)
        fatal("CIV.EXE: not EXEPACK compressed (unexpected version?)");
    if (rd16(h + (ip == 0x12 ? 16 : 14)) != 0x4252)
        fatal("CIV.EXE: EXEPACK signature missing");
    entry_ip = rd16(h + 0);
    entry_cs = rd16(h + 2);
    entry_sp = rd16(h + 8);
    entry_ss = rd16(h + 10);
    uint16_t dest_len = rd16(h + 12);

    uint8_t *buf = calloc(1, (size_t)dest_len * 16 + ep + 16);
    memcpy(buf, img, ep);
    long src = ep - (skip - 1) * 16L - 1;
    long dst = dest_len * 16L - 1;
    while (buf[src] == 0xFF)
        src--;
    for (;;) {
        uint8_t c = buf[src--];
        uint16_t n = (uint16_t)(buf[src] << 8 | buf[src - 1]);
        src -= 2;
        if ((c & 0xFE) == 0xB0) {
            uint8_t v = buf[src--];
            while (n--)
                buf[dst--] = v;
        } else if ((c & 0xFE) == 0xB2) {
            while (n--)
                buf[dst--] = buf[src--];
        } else
            fatal("CIV.EXE: corrupt EXEPACK data");
        if (c & 1)
            break;
    }
    memcpy(&mem[LIN(seg, 0)], buf, (size_t)dest_len * 16);
    free(buf);

    /* relocation table follows the "Packed file is corrupt" message */
    const char *msg = "Packed file is corrupt";
    const uint8_t *p = NULL;
    for (long i = ep; i + 22 < img_len; i++)
        if (!memcmp(img + i, msg, 22)) {
            p = img + i + 22;
            break;
        }
    if (!p)
        fatal("CIV.EXE: EXEPACK relocations not found");
    for (int s = 0; s < 16; s++) {
        uint16_t n = rd16(p);
        p += 2;
        while (n--) {
            uint32_t a = LIN(seg, 0) + s * 0x10000u + rd16(p);
            p += 2;
            uint16_t v = (uint16_t)(mem[a] | mem[a + 1] << 8);
            v += seg;
            mem[a] = (uint8_t)v;
            mem[a + 1] = (uint8_t)(v >> 8);
        }
    }
}

static void load_overlays(const uint8_t *d, long len)
{
    long p = rd16(d + 4) * 512L;
    while (p + 28 < len && rd16(d + p) == 0x5A4D) {
        const uint8_t *h = d + p;
        uint16_t cblp = rd16(h + 2), cp = rd16(h + 4), nrel = rd16(h + 6);
        uint16_t parhdr = rd16(h + 8), relofs = rd16(h + 0x18);
        uint16_t num = rd16(h + 0x1A);
        long size = (cblp ? (cp - 1) * 512L + cblp : cp * 512L) - parhdr * 16;
        if (num >= 32)
            fatal("overlay number %d out of range", num);
        struct overlay *o = &overlays[num];
        o->size = (uint32_t)size;
        o->image = malloc(size);
        memcpy(o->image, h + parhdr * 16, size);
        o->nrel = nrel;
        o->rel = malloc(sizeof(uint16_t) * (nrel + 1));
        for (int i = 0; i < nrel; i++) {
            uint16_t off = rd16(h + relofs + 4 * i);
            uint16_t sg = rd16(h + relofs + 4 * i + 2);
            o->rel[i] = (uint16_t)((sg - OVL_LINKSEG) * 16 + off);
        }
        p += cp * 512L;
    }
}

static void setup_mcb(uint16_t seg, char type, uint16_t owner, uint16_t size)
{
    uint32_t a = LIN(seg, 0);
    mem[a] = (uint8_t)type;
    mem[a + 1] = (uint8_t)owner;
    mem[a + 2] = (uint8_t)(owner >> 8);
    mem[a + 3] = (uint8_t)size;
    mem[a + 4] = (uint8_t)(size >> 8);
}

void load_program(const char *exe_path, const char *cmdline)
{
    long len;
    uint8_t *d = read_file(exe_path, &len);
    unexepack(d, len, LOADSEG);
    load_overlays(d, len);
    free(d);

    /* environment block */
    uint16_t envsize = PSPSEG - 1 - ENVSEG;
    setup_mcb(ENVSEG - 1, 'M', PSPSEG, envsize);
    static const char env[] = "COMSPEC=C:\\COMMAND.COM\0PATH=C:\\\0\0\1\0C:\\CIV.EXE";
    memcpy(&mem[LIN(ENVSEG, 0)], env, sizeof env);

    /* PSP */
    setup_mcb(PSPSEG - 1, 'Z', PSPSEG, 0xA000 - PSPSEG);
    uint8_t *psp = &mem[LIN(PSPSEG, 0)];
    psp[0] = 0xCD;
    psp[1] = 0x20;
    psp[2] = 0x00;
    psp[3] = 0xA0;              /* top of memory: A000 */
    psp[0x2C] = (uint8_t)ENVSEG;
    psp[0x2D] = ENVSEG >> 8;
    psp[0x50] = 0xCD;           /* int 21h; retf */
    psp[0x51] = 0x21;
    psp[0x52] = 0xCB;
    for (int i = 0; i < 20; i++)
        psp[0x18 + i] = i < 5 ? (uint8_t)(i == 3 ? 2 : i == 4 ? 3 : i < 3 ? 1 : 0) : 0xFF;
    psp[0x18] = 1; psp[0x19] = 1; psp[0x1A] = 1; psp[0x1B] = 0; psp[0x1C] = 2;
    psp[0x32] = 20;
    psp[0x34] = 0x18;
    psp[0x36] = (uint8_t)PSPSEG;
    psp[0x37] = PSPSEG >> 8;
    size_t cl = cmdline ? strlen(cmdline) : 0;
    if (cl > 125)
        cl = 125;
    psp[0x80] = (uint8_t)(cl ? cl + 1 : 0);
    if (cl) {
        psp[0x81] = ' ';
        memcpy(psp + 0x82, cmdline, cl);
        psp[0x82 + cl] = 0x0D;
    } else
        psp[0x81] = 0x0D;

    memset(&cpu, 0, sizeof cpu);
    CS = (uint16_t)(entry_cs + LOADSEG);
    SS = (uint16_t)(entry_ss + LOADSEG);
    SP = entry_sp;
    DS = ES = PSPSEG;
    cpu.ifl = 1;
}
