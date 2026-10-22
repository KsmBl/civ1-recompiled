/*
 * DOS (INT 21h) emulation: files, memory blocks, overlay loading, time.
 * DOS paths are mapped case-insensitively onto the game directory.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "cpu.h"
#include "runtime.h"
#include "gfx.h"
#include "addons.h"

#define MAX_HANDLES 64
static FILE *handles[MAX_HANDLES];
/* save games (CIVILn.SVE) opened for writing: the features' extra state is
   written to CIVILn.NAT when the file is closed */
static char save_nat[MAX_HANDLES][1024];
static char last_read_nat[1024];       /* sidecar of the last save opened */

/* the game is really loading the last opened CIVILn.SVE (hook in ovl11) */
void dos_load_save_sidecar(void)
{
    FILE *nf = last_read_nat[0] ? fopen(last_read_nat, "rb") : NULL;
    native_load_state(nf);
    if (nf)
        fclose(nf);
}

static int is_save_game(const char *host, char *nat, size_t n)
{
    const char *b = strrchr(host, '/');
    b = b ? b + 1 : host;
    if (strlen(b) != 10 || strncasecmp(b, "CIVIL", 5) || !isdigit((unsigned char)b[5]) ||
        strcasecmp(b + 6, ".SVE"))
        return 0;
    snprintf(nat, n, "%.*sNAT", (int)(strlen(host) - 3), host);
    return 1;
}
static uint16_t dta_seg, dta_off;
static uint8_t cur_drive = 2;          /* C: */
static uint16_t last_error;
static uint8_t pending_ext;     /* second byte of an extended key */
static char auto_answers[8];
static int auto_pos;

static void set_ok(void) { CF = 0; }
static void set_err(uint16_t code)
{
    CF = 1;
    AX = code;
    last_error = code;
}

static void rd_str(uint16_t seg, uint16_t off, char *buf, size_t n)
{
    size_t i = 0;
    for (; i + 1 < n; i++) {
        char c = (char)RB(seg, (uint16_t)(off + i));
        if (!c)
            break;
        buf[i] = c;
    }
    buf[i] = 0;
}

/* --------------------------------------------------------------- */
/* path mapping */

static int ci_lookup(const char *dir, const char *name, char *out, size_t n)
{
    DIR *d = opendir(*dir ? dir : ".");
    if (!d)
        return 0;
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d))) {
        if (!strcasecmp(e->d_name, name)) {
            snprintf(out, n, "%s", e->d_name);
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

/* Convert a DOS path to a host path inside the game directory. */
int dos_resolve_path(const char *dos, char *host, size_t n, int must_exist)
{
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s", dos);
    char *p = tmp;
    if (isalpha((unsigned char)p[0]) && p[1] == ':')
        p += 2;
    for (char *q = p; *q; q++)
        if (*q == '\\')
            *q = '/';
    while (*p == '/')
        p++;
    /* game files all live in one directory: drop directory parts that
       do not exist on the host (e.g. C:\MPS\CIV\) */
    char out[1024] = "";
    char *save, *comp = strtok_r(p, "/", &save);
    while (comp) {
        char *next = strtok_r(NULL, "/", &save);
        char real[256];
        /* strip trailing dots/spaces DOS ignores */
        size_t L = strlen(comp);
        while (L && (comp[L - 1] == ' '))
            comp[--L] = 0;
        if (!strcmp(comp, ".")) {
            comp = next;
            continue;
        }
        if (ci_lookup(out, comp, real, sizeof real)) {
            size_t ol = strlen(out);
            snprintf(out + ol, sizeof out - ol, "%s%s", ol ? "/" : "", real);
        } else if (next) {
            /* unknown directory: ignore it */
        } else {
            size_t ol = strlen(out);
            /* new files are created in upper case like DOS would */
            char up[256];
            size_t k;
            for (k = 0; comp[k] && k < sizeof up - 1; k++)
                up[k] = (char)toupper((unsigned char)comp[k]);
            up[k] = 0;
            snprintf(out + ol, sizeof out - ol, "%s%s", ol ? "/" : "", up);
            if (must_exist)
                return 0;
        }
        comp = next;
    }
    snprintf(host, n, "%s", *out ? out : ".");
    return 1;
}

/* --------------------------------------------------------------- */
/* memory control blocks */

static uint16_t mcb_size(uint16_t s) { return RW(s, 3); }
static uint8_t mcb_type(uint16_t s) { return RB(s, 0); }
static uint16_t mcb_owner(uint16_t s) { return RW(s, 1); }

static void mcb_set(uint16_t s, uint8_t t, uint16_t owner, uint16_t size)
{
    WB(s, 0, t);
    WW(s, 1, owner);
    WW(s, 3, size);
}

static uint16_t first_mcb(void) { return ENVSEG - 1; }

static void mcb_merge_free(void)
{
    uint16_t s = first_mcb();
    while (mcb_type(s) == 'M') {
        uint16_t n = (uint16_t)(s + 1 + mcb_size(s));
        if (mcb_owner(s) == 0 && mcb_owner(n) == 0) {
            mcb_set(s, mcb_type(n), 0, (uint16_t)(mcb_size(s) + 1 + mcb_size(n)));
            continue;
        }
        s = n;
    }
}

static void dos_alloc(void)
{
    uint16_t want = BX, best = 0;
    mcb_merge_free();
    uint16_t s = first_mcb();
    for (;;) {
        uint8_t t = mcb_type(s);
        if (t != 'M' && t != 'Z') {
            set_err(7);
            return;
        }
        if (mcb_owner(s) == 0) {
            uint16_t sz = mcb_size(s);
            if (sz >= want) {
                if (sz > want) {
                    uint16_t n = (uint16_t)(s + 1 + want);
                    mcb_set(n, t, 0, (uint16_t)(sz - want - 1));
                    mcb_set(s, 'M', PSPSEG, want);
                } else
                    mcb_set(s, t, PSPSEG, want);
                AX = (uint16_t)(s + 1);
                set_ok();
                return;
            }
            if (sz > best)
                best = sz;
        }
        if (t == 'Z')
            break;
        s = (uint16_t)(s + 1 + mcb_size(s));
    }
    BX = best;
    set_err(8);
}

static void dos_free(void)
{
    uint16_t s = (uint16_t)(ES - 1);
    if (mcb_type(s) != 'M' && mcb_type(s) != 'Z') {
        set_err(9);
        return;
    }
    WW(s, 1, 0);
    mcb_merge_free();
    set_ok();
}

static void dos_resize(void)
{
    uint16_t s = (uint16_t)(ES - 1), want = BX;
    uint8_t t = mcb_type(s);
    if (t != 'M' && t != 'Z') {
        set_err(9);
        return;
    }
    mcb_merge_free();
    uint16_t sz = mcb_size(s);
    if (want <= sz) {
        if (want < sz) {
            uint16_t n = (uint16_t)(s + 1 + want);
            mcb_set(n, t, 0, (uint16_t)(sz - want - 1));
            mcb_set(s, 'M', mcb_owner(s), want);
            mcb_merge_free();
        }
        set_ok();
        return;
    }
    /* grow into following free block */
    if (t == 'M') {
        uint16_t n = (uint16_t)(s + 1 + sz);
        if (mcb_owner(n) == 0) {
            uint32_t total = (uint32_t)sz + 1 + mcb_size(n);
            uint8_t nt = mcb_type(n);
            if (total >= want) {
                if (total > want) {
                    uint16_t m = (uint16_t)(s + 1 + want);
                    mcb_set(m, nt, 0, (uint16_t)(total - want - 1));
                    mcb_set(s, 'M', mcb_owner(s), want);
                } else
                    mcb_set(s, nt, mcb_owner(s), want);
                set_ok();
                return;
            }
            BX = (uint16_t)total;
            set_err(8);
            return;
        }
    }
    BX = sz;
    set_err(8);
}

/* --------------------------------------------------------------- */
/* EXEC 4B03: load overlay (used for the graphics / sound drivers) */

int load_module_mz(const char *host_path, const char *dos_name,
                   uint16_t load_seg, uint16_t reloc)
{
    FILE *f = fopen(host_path, "rb");
    if (!f)
        return 2;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(len);
    if (fread(d, 1, len, f) != (size_t)len) {
        fclose(f);
        free(d);
        return 5;
    }
    fclose(f);
    if (len < 28 || d[0] != 'M' || d[1] != 'Z') {
        memcpy(&mem[LIN(load_seg, 0)], d, len);
    } else {
        uint16_t cblp = d[2] | d[3] << 8, cp = d[4] | d[5] << 8;
        uint16_t nrel = d[6] | d[7] << 8, hdr = d[8] | d[9] << 8;
        uint16_t relofs = d[0x18] | d[0x19] << 8;
        long size = (cblp ? (cp - 1) * 512L + cblp : cp * 512L) - hdr * 16;
        if (size > len - hdr * 16)
            size = len - hdr * 16;
        memcpy(&mem[LIN(load_seg, 0)], d + hdr * 16, size);
        for (int i = 0; i < nrel; i++) {
            uint16_t o = d[relofs + 4 * i] | d[relofs + 4 * i + 1] << 8;
            uint16_t s = d[relofs + 4 * i + 2] | d[relofs + 4 * i + 3] << 8;
            uint32_t a = LIN((uint16_t)(load_seg + s), o);
            uint16_t v = (uint16_t)(mem[a] | mem[a + 1] << 8);
            v += reloc;
            mem[a] = (uint8_t)v;
            mem[a + 1] = (uint8_t)(v >> 8);
        }
    }
    free(d);
    /* register lifted code for this module */
    static const char *names[] = MOD_NAMES;
    const char *base = strrchr(dos_name, '\\');
    base = base ? base + 1 : dos_name;
    const char *b2 = strrchr(base, ':');
    base = b2 ? b2 + 1 : base;
    for (int m = 0; m < MOD_COUNT; m++)
        if (!strcasecmp(names[m], base)) {
            modbase[m] = load_seg;
            logmsg("loaded module %s at %04x", names[m], load_seg);
            if (m == MOD_MGRAPHIC)
                gfx_module_loaded(load_seg);
        }
    return 0;
}

static void dos_exec(void)
{
    char name[128], host[1024];
    rd_str(DS, DX, name, sizeof name);
    if (AL != 3) {
        warn("EXEC mode %d of %s not supported", AL, name);
        set_err(1);
        return;
    }
    if (!dos_resolve_path(name, host, sizeof host, 1)) {
        set_err(2);
        return;
    }
    uint16_t seg = RW(ES, BX), rel = RW(ES, (uint16_t)(BX + 2));
    int r = load_module_mz(host, name, seg, rel);
    if (r)
        set_err((uint16_t)r);
    else
        set_ok();
}

/* --------------------------------------------------------------- */
/* find first / next */

struct search {
    char dir[1024];
    char pattern[16];
    uint8_t attr;
    long pos;
};
static struct search searches[16];
static int search_next_slot;

static int dos_name_ok(const char *n)
{
    const char *dot = strchr(n, '.');
    size_t base = dot ? (size_t)(dot - n) : strlen(n);
    if (base == 0 || base > 8)
        return 0;
    if (dot && (strlen(dot + 1) > 3 || strchr(dot + 1, '.')))
        return 0;
    return 1;
}

static int match83(const char *pat, const char *name)
{
    /* DOS wildcard semantics on 8.3 names */
    char pb[9], pe[4], nb[9], ne[4];
    const char *src[2] = {pat, name};
    char *bases[2] = {pb, nb}, *exts[2] = {pe, ne};
    for (int k = 0; k < 2; k++) {
        const char *s = src[k];
        const char *dot = strrchr(s, '.');
        size_t bl = dot ? (size_t)(dot - s) : strlen(s);
        memset(bases[k], ' ', 8);
        memset(exts[k], ' ', 3);
        bases[k][8] = exts[k][3] = 0;
        for (size_t i = 0, j = 0; i < bl && j < 8; i++) {
            if (s[i] == '*') {
                while (j < 8)
                    bases[k][j++] = '?';
                break;
            }
            bases[k][j++] = (char)toupper((unsigned char)s[i]);
        }
        if (dot)
            for (size_t i = 1, j = 0; dot[i] && j < 3; i++) {
                if (dot[i] == '*') {
                    while (j < 3)
                        exts[k][j++] = '?';
                    break;
                }
                exts[k][j++] = (char)toupper((unsigned char)dot[i]);
            }
    }
    for (int i = 0; i < 8; i++)
        if (pb[i] != '?' && pb[i] != nb[i])
            return 0;
    for (int i = 0; i < 3; i++)
        if (pe[i] != '?' && pe[i] != ne[i])
            return 0;
    return 1;
}

static void fill_dta(struct search *s, int slot, const char *name, const char *path)
{
    struct stat st;
    stat(path, &st);
    uint32_t a = LIN(dta_seg, dta_off);
    memset(&mem[a], 0, 43);
    mem[a] = (uint8_t)slot;
    mem[a + 0x15] = S_ISDIR(st.st_mode) ? 0x10 : 0x20;
    struct tm *tm = localtime(&st.st_mtime);
    uint16_t t = (uint16_t)(tm->tm_hour << 11 | tm->tm_min << 5 | tm->tm_sec / 2);
    uint16_t d = (uint16_t)((tm->tm_year - 80) << 9 | (tm->tm_mon + 1) << 5 | tm->tm_mday);
    WW(dta_seg, (uint16_t)(dta_off + 0x16), t);
    WW(dta_seg, (uint16_t)(dta_off + 0x18), d);
    WW(dta_seg, (uint16_t)(dta_off + 0x1A), (uint16_t)st.st_size);
    WW(dta_seg, (uint16_t)(dta_off + 0x1C), (uint16_t)(st.st_size >> 16));
    for (int i = 0; i < 12 && name[i]; i++)
        mem[a + 0x1E + i] = (uint8_t)toupper((unsigned char)name[i]);
    (void)s;
}

static void dos_find(int first)
{
    struct search *s;
    int slot;
    if (first) {
        char pat[128], host[1024];
        rd_str(DS, DX, pat, sizeof pat);
        slot = search_next_slot++ % 16;
        s = &searches[slot];
        /* split directory part */
        char *bs = strrchr(pat, '\\');
        char *col = strrchr(pat, ':');
        char *fn = bs ? bs + 1 : col ? col + 1 : pat;
        snprintf(s->pattern, sizeof s->pattern, "%s", fn);
        *fn = 0;
        if (*pat) {
            dos_resolve_path(pat, host, sizeof host, 0);
            snprintf(s->dir, sizeof s->dir, "%s", host);
        } else
            snprintf(s->dir, sizeof s->dir, ".");
        s->attr = CL;
        s->pos = 0;
    } else {
        slot = RB(dta_seg, dta_off) % 16;
        s = &searches[slot];
    }
    DIR *d = opendir(s->dir);
    if (!d) {
        set_err(first ? 3 : 18);
        return;
    }
    struct dirent *e;
    long idx = 0;
    while ((e = readdir(d))) {
        if (idx++ < s->pos)
            continue;
        s->pos = idx;
        if (e->d_name[0] == '.' || !dos_name_ok(e->d_name))
            continue;
        if (!match83(s->pattern, e->d_name))
            continue;
        char path[2048];
        snprintf(path, sizeof path, "%s/%s", s->dir, e->d_name);
        struct stat st;
        if (stat(path, &st))
            continue;
        if (S_ISDIR(st.st_mode) && !(s->attr & 0x10))
            continue;
        fill_dta(s, slot, e->d_name, path);
        closedir(d);
        set_ok();
        return;
    }
    closedir(d);
    set_err(18);
}

/* --------------------------------------------------------------- */
/* FCB functions (29h parse, 11h/12h find first/next) */

static int fcb_sep(char c)
{
    return strchr(":.;,=+ \t/\"[]<>|", c) != NULL || c < 32;
}

static void dos_parse_fcb(void)
{
    uint8_t flags = AL;
    uint16_t si = SI, di = DI;
    char c = (char)RB(DS, si);
    if (flags & 1) {
        while (c == ' ' || c == '\t' || c == ':' || c == ';' || c == ',' ||
               c == '=' || c == '+') {
            si++;
            c = (char)RB(DS, si);
        }
    }
    uint8_t ret = 0;
    if (RB(DS, (uint16_t)(si + 1)) == ':' && isalpha((unsigned char)c)) {
        WB(ES, di, (uint8_t)(toupper((unsigned char)c) - 'A' + 1));
        si += 2;
    } else if (!(flags & 2))
        WB(ES, di, 0);
    /* name */
    char name[8], ext[3];
    int n = 0, wild = 0, have_name = 0, have_ext = 0;
    memset(name, ' ', 8);
    memset(ext, ' ', 3);
    for (;;) {
        c = (char)RB(DS, si);
        if (fcb_sep(c))
            break;
        have_name = 1;
        if (c == '*') {
            while (n < 8)
                name[n++] = '?';
            wild = 1;
        } else if (n < 8) {
            if (c == '?')
                wild = 1;
            name[n++] = (char)toupper((unsigned char)c);
        }
        si++;
    }
    if (RB(DS, si) == '.') {
        si++;
        n = 0;
        for (;;) {
            c = (char)RB(DS, si);
            if (fcb_sep(c))
                break;
            have_ext = 1;
            if (c == '*') {
                while (n < 3)
                    ext[n++] = '?';
                wild = 1;
            } else if (n < 3) {
                if (c == '?')
                    wild = 1;
                ext[n++] = (char)toupper((unsigned char)c);
            }
            si++;
        }
    }
    if (have_name || !(flags & 4))
        for (int i = 0; i < 8; i++)
            WB(ES, (uint16_t)(di + 1 + i), (uint8_t)name[i]);
    if (have_ext || !(flags & 8))
        for (int i = 0; i < 3; i++)
            WB(ES, (uint16_t)(di + 9 + i), (uint8_t)ext[i]);
    if (wild)
        ret = 1;
    SI = si;
    AL = ret;
}

static long fcb_pos;
static char fcb_pat[13];

static void fcb_find(int first)
{
    uint16_t fcb = DX;
    int extended = RB(DS, fcb) == 0xFF;
    uint16_t base = (uint16_t)(fcb + (extended ? 7 : 0));
    if (first) {
        char nm[9], ex[4];
        for (int i = 0; i < 8; i++)
            nm[i] = (char)RB(DS, (uint16_t)(base + 1 + i));
        for (int i = 0; i < 3; i++)
            ex[i] = (char)RB(DS, (uint16_t)(base + 9 + i));
        nm[8] = ex[3] = 0;
        for (int i = 7; i >= 0 && nm[i] == ' '; i--)
            nm[i] = 0;
        for (int i = 2; i >= 0 && ex[i] == ' '; i--)
            ex[i] = 0;
        snprintf(fcb_pat, sizeof fcb_pat, "%s.%s", nm, ex);
        fcb_pos = 0;
    }
    DIR *d = opendir(".");
    if (!d) {
        AL = 0xFF;
        return;
    }
    struct dirent *e;
    long idx = 0;
    while ((e = readdir(d))) {
        if (idx++ < fcb_pos)
            continue;
        fcb_pos = idx;
        if (e->d_name[0] == '.' || !dos_name_ok(e->d_name))
            continue;
        if (!match83(fcb_pat, e->d_name))
            continue;
        struct stat st;
        if (stat(e->d_name, &st) || S_ISDIR(st.st_mode))
            continue;
        uint32_t a = LIN(dta_seg, dta_off);
        int o = 0;
        if (extended) {
            memset(&mem[a], 0, 7);
            mem[a] = 0xFF;
            mem[a + 6] = 0x20;
            o = 7;
        }
        memset(&mem[a + o], ' ', 12);
        mem[a + o] = (uint8_t)(cur_drive + 1);
        const char *dot = strchr(e->d_name, '.');
        size_t bl = dot ? (size_t)(dot - e->d_name) : strlen(e->d_name);
        for (size_t i = 0; i < bl && i < 8; i++)
            mem[a + o + 1 + i] = (uint8_t)toupper((unsigned char)e->d_name[i]);
        if (dot)
            for (int i = 0; i < 3 && dot[1 + i]; i++)
                mem[a + o + 9 + i] = (uint8_t)toupper((unsigned char)dot[1 + i]);
        memset(&mem[a + o + 12], 0, 25);
        uint32_t sz = (uint32_t)st.st_size;
        mem[a + o + 0x1D] = (uint8_t)sz;
        mem[a + o + 0x1E] = (uint8_t)(sz >> 8);
        mem[a + o + 0x1F] = (uint8_t)(sz >> 16);
        mem[a + o + 0x20] = (uint8_t)(sz >> 24);
        closedir(d);
        AL = 0;
        return;
    }
    closedir(d);
    AL = 0xFF;
}

/* Buffer segment override for one DOS transfer (used by game add-ons whose
   data moved out of the data segment): transfers at DS:redir_off use
   redir_seg instead of DS. */
static uint16_t redir_off, redir_seg;

void dos_redirect_buffer(uint16_t off, uint16_t seg)
{
    redir_off = off;
    redir_seg = seg;
}

static uint32_t io_lin(void)
{
    if (redir_seg && DX == redir_off)
        return LIN(redir_seg, DX);
    return LIN(DS, DX);
}

static int new_handle(FILE *f)
{
    for (int i = 5; i < MAX_HANDLES; i++)
        if (!handles[i]) {
            handles[i] = f;
            return i;
        }
    return -1;
}

static void dos_open(int create)
{
    char name[128], host[1024];
    rd_str(DS, DX, name, sizeof name);
    int exists = dos_resolve_path(name, host, sizeof host, 1);
    if (!create && !exists) {
        set_err(2);
        return;
    }
    if (create && !exists)
        dos_resolve_path(name, host, sizeof host, 0);
    const char *mode;
    if (create)
        mode = "w+b";
    else
        switch (AL & 7) {
        case 0: mode = "rb"; break;
        case 1: mode = "r+b"; break;
        default: mode = "r+b"; break;
        }
    FILE *f = fopen(host, mode);
    if (!f && !create && (AL & 7) != 0)
        f = fopen(host, "rb");
    if (!f) {
        set_err(errno == ENOENT ? 2 : 5);
        return;
    }
    int h = new_handle(f);
    if (h < 0) {
        fclose(f);
        set_err(4);
        return;
    }
    if (verbose)
        logmsg("open %s -> %s handle %d", name, host, h);
    char nat[1024];
    save_nat[h][0] = 0;
    if (is_save_game(host, nat, sizeof nat)) {
        if (create || (AL & 7) != 0)
            snprintf(save_nat[h], sizeof save_nat[h], "%s", nat);
        else    /* the save dialogs read every slot: load only on request */
            snprintf(last_read_nat, sizeof last_read_nat, "%s", nat);
    }
    AX = (uint16_t)h;
    set_ok();
}

static void dos_read(void)
{
    uint16_t h = BX;
    if (h == 0) {
        AX = 0;
        set_ok();
        return;
    }
    if (h >= MAX_HANDLES || !handles[h]) {
        set_err(6);
        return;
    }
    uint32_t a = io_lin();
    size_t n = fread(&mem[a], 1, CX, handles[h]);
    AX = (uint16_t)n;
    set_ok();
}

static void dos_write(void)
{
    uint16_t h = BX;
    uint32_t a = io_lin();
    if (h == 1 || h == 2) {
        fwrite(&mem[a], 1, CX, stdout);
        fflush(stdout);
        AX = CX;
        set_ok();
        return;
    }
    if (h >= MAX_HANDLES || !handles[h]) {
        set_err(6);
        return;
    }
    if (CX == 0) {
        /* truncate at current position */
        long pos = ftell(handles[h]);
        fflush(handles[h]);
        if (ftruncate(fileno(handles[h]), pos)) {
        }
        AX = 0;
        set_ok();
        return;
    }
    size_t n = fwrite(&mem[a], 1, CX, handles[h]);
    AX = (uint16_t)n;
    set_ok();
}

static void dos_seek(void)
{
    uint16_t h = BX;
    if (h >= MAX_HANDLES || !handles[h]) {
        set_err(6);
        return;
    }
    long off = (long)(int32_t)((uint32_t)CX << 16 | DX);
    int wh = AL == 0 ? SEEK_SET : AL == 1 ? SEEK_CUR : SEEK_END;
    if (fseek(handles[h], off, wh)) {
        set_err(25);
        return;
    }
    long p = ftell(handles[h]);
    AX = (uint16_t)p;
    DX = (uint16_t)(p >> 16);
    set_ok();
}

static void dos_print_dollar(void)
{
    uint16_t o = DX;
    for (int i = 0; i < 4096; i++) {
        char c = (char)RB(DS, o++);
        if (c == '$')
            break;
        fputc(c, stdout);
    }
    fflush(stdout);
}

void dos_init(void)
{
    /* graphics mode, sound driver, mouse/keyboard */
    if (cfg.autostart)
        snprintf(auto_answers, sizeof auto_answers, "1%c%c",
                 '0' + cfg.sound, cfg.mouse ? '1' : '2');
    dta_seg = PSPSEG;
    dta_off = 0x80;
}

void dos_int21(void)
{
    uint8_t ah = AH;
    if (verbose > 1)
        logmsg("int21 ax=%04x bx=%04x cx=%04x dx=%04x", AX, BX, CX, DX);
    switch (ah) {
    case 0x02:
        fputc(DL, stdout);
        break;
    case 0x06:
        if (DL != 0xFF)
            fputc(DL, stdout);
        else {
            uint16_t w;
            poll_slow();
            if (pending_ext) {
                AL = pending_ext;
                pending_ext = 0;
                ZF = 0;
            } else if (bios_key_peek(&w)) {
                w = bios_key_wait();
                AL = (uint8_t)w;
                if (!AL)
                    pending_ext = (uint8_t)(w >> 8);
                ZF = 0;
            } else {
                ZF = 1;
                AL = 0;
            }
        }
        break;
    case 0x09:
        dos_print_dollar();
        break;
    case 0x01:
    case 0x07:
    case 0x08: {
        uint16_t w;
        /* answer the text mode setup prompts from the configuration */
        if (video_mode == 3 && auto_answers[auto_pos]) {
            AL = (uint8_t)auto_answers[auto_pos++];
            if (ah == 0x01) {
                fputc(AL, stdout);
                fflush(stdout);
            }
            break;
        }
        w = bios_key_wait();
        AL = (uint8_t)w;
        if (AL == 0)
            pending_ext = (uint8_t)(w >> 8);
        if (ah == 0x01 && AL >= 32) {
            fputc(AL, stdout);
            fflush(stdout);
        }
        break;
    }
    case 0x0B: {
        uint16_t w;
        poll_slow();
        AL = bios_key_peek(&w) ? 0xFF : 0x00;
        break;
    }
    case 0x0C: {
        uint16_t w;
        while (bios_key_peek(&w))
            bios_key_wait();
        if (AL == 0x01 || AL == 0x06 || AL == 0x07 || AL == 0x08) {
            AH = AL;
            dos_int21();
            AH = 0x0C;
        }
        break;
    }
    case 0x0E:
        cur_drive = DL;
        AL = 26;
        break;
    case 0x11:
        fcb_find(1);
        break;
    case 0x12:
        fcb_find(0);
        break;
    case 0x19:
        AL = cur_drive;
        break;
    case 0x29:
        dos_parse_fcb();
        break;
    case 0x1A:
        dta_seg = DS;
        dta_off = DX;
        break;
    case 0x25:
        WW(0, AL * 4, DX);
        WW(0, AL * 4 + 2, DS);
        break;
    case 0x2A: {
        time_t t = deterministic ? 692323200 : time(NULL);   /* 1991-12-09 */
        struct tm *tm = deterministic ? gmtime(&t) : localtime(&t);
        CX = (uint16_t)(tm->tm_year + 1900);
        DH = (uint8_t)(tm->tm_mon + 1);
        DL = (uint8_t)tm->tm_mday;
        AL = (uint8_t)tm->tm_wday;
        break;
    }
    case 0x2C: {
        uint64_t us = now_us();
        time_t t = deterministic ? 692323200 + (time_t)(us / 1000000) : time(NULL);
        struct tm *tm = deterministic ? gmtime(&t) : localtime(&t);
        CH = (uint8_t)tm->tm_hour;
        CL = (uint8_t)tm->tm_min;
        DH = (uint8_t)tm->tm_sec;
        DL = (uint8_t)((us / 10000) % 100);
        break;
    }
    case 0x2F:
        ES = dta_seg;
        BX = dta_off;
        break;
    case 0x30:
        AX = 0x0005;      /* DOS 5.0 */
        BX = 0;
        CX = 0;
        break;
    case 0x33:
        if (AL == 0)
            DL = 0;
        break;
    case 0x35:
        BX = RW(0, AL * 4);
        ES = RW(0, AL * 4 + 2);
        break;
    case 0x36:
        AX = 64;          /* sectors per cluster */
        BX = 0x7FFF;      /* free clusters */
        CX = 512;
        DX = 0xFFFF;
        break;
    case 0x3B:
        set_ok();
        break;
    case 0x3C:
        dos_open(1);
        break;
    case 0x3D:
        dos_open(0);
        break;
    case 0x3E:
        if (BX < MAX_HANDLES && handles[BX]) {
            fclose(handles[BX]);
            handles[BX] = NULL;
            if (save_nat[BX][0]) {
                FILE *nf = fopen(save_nat[BX], "wb");
                if (nf) {
                    native_save_state(nf);
                    fclose(nf);
                }
                save_nat[BX][0] = 0;
            }
            set_ok();
        } else if (BX < 5)
            set_ok();
        else
            set_err(6);
        break;
    case 0x3F:
        dos_read();
        break;
    case 0x40:
        dos_write();
        break;
    case 0x41: {
        char name[128], host[1024];
        rd_str(DS, DX, name, sizeof name);
        if (dos_resolve_path(name, host, sizeof host, 1) && !remove(host))
            set_ok();
        else
            set_err(2);
        break;
    }
    case 0x42:
        dos_seek();
        break;
    case 0x43: {
        char name[128], host[1024];
        rd_str(DS, DX, name, sizeof name);
        if (!dos_resolve_path(name, host, sizeof host, 1)) {
            set_err(2);
            break;
        }
        CX = 0x20;
        set_ok();
        break;
    }
    case 0x44:
        if (AL == 0) {
            if (BX < 5)
                DX = BX == 0 ? 0x81 : BX == 1 || BX == 2 ? 0x82 : 0x80;
            else if (BX < MAX_HANDLES && handles[BX])
                DX = 0x0002;
            else {
                set_err(6);
                break;
            }
            set_ok();
        } else if (AL == 1) {
            set_ok();
        } else if (AL == 8) {
            AX = 1;       /* fixed disk */
            set_ok();
        } else {
            set_err(1);
        }
        break;
    case 0x47:
        WB(DS, SI, 0);
        set_ok();
        break;
    case 0x48:
        dos_alloc();
        break;
    case 0x49:
        dos_free();
        break;
    case 0x4A:
        dos_resize();
        break;
    case 0x4B:
        dos_exec();
        break;
    case 0x4C:
        logmsg("program exit code %d", AL);
        native_state_dump();
        gfx_shadow_report();
        plat_shutdown();
        exit(AL);
    case 0x4E:
        dos_find(1);
        break;
    case 0x4F:
        dos_find(0);
        break;
    case 0x54:
        AL = 0;
        break;
    case 0x56: {
        char a[128], b[128], ha[1024], hb[1024];
        rd_str(DS, DX, a, sizeof a);
        rd_str(ES, DI, b, sizeof b);
        if (dos_resolve_path(a, ha, sizeof ha, 1)) {
            dos_resolve_path(b, hb, sizeof hb, 0);
            if (!rename(ha, hb)) {
                set_ok();
                break;
            }
        }
        set_err(2);
        break;
    }
    case 0x57:
        if (AL == 0) {
            CX = 0;
            DX = (11 << 9) | (12 << 5) | 3;
        }
        set_ok();
        break;
    case 0x59:
        AX = last_error;
        BX = 0;
        CH = 0;
        break;
    case 0x62:
        BX = PSPSEG;
        break;
    default:
        warn("unhandled int 21h AH=%02x AX=%04x", ah, AX);
        set_err(1);
        break;
    }
}
