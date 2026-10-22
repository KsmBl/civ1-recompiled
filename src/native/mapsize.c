/*
 * Wider maps: support code for the patches in tools/patches.txt.
 *
 * The per-square "explored" table (DS:7FF8, 50 bytes per column) moves to
 * segment VISSEG when the map is wider than 80 squares.  The save game code
 * reads / writes it with an ordinary DOS call on DS:7FF8; these hooks
 * point that one transfer at the relocated table.
 */
#include "native.h"
#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "gfx.h"
#include "addons.h"

/* f_1b05_17c1(x, y, value, page): set a map square in a layer.  The
   original goes through the drawing context at DS:[0xaa], which clips at
   the screen's right edge; the wide map pages are wider than that. */
void f_1b05_17c1(void)
{
    int page = FARG(3);
    static FILE *trace;
    static int trace_init;
    if (!trace_init) {
        trace_init = 1;
        const char *e = getenv("CIV_MAPTRACE");
        trace = e ? fopen(e, "w") : NULL;
    }
    if (trace) {
        /* layer independent form: layer column, x, y */
        int16_t tx = (int16_t)FARG(0);
        int col = 0;
        for (int k = 3; k > 0; k--)
            if (tx >= MAP_LX(k)) {
                col = k;
                break;
            }
        fprintf(trace, "set p%d L%d x%d y%d v%d", page, col, tx - MAP_LX(col),
                (int16_t)FARG(1), FARG(2));
        for (int i = shadow_depth - 1, n = 0; i >= 0 && n < 4; i--, n++) {
            struct shadow *sh = &shadow_stack[i & (SHADOW_MAX - 1)];
            fprintf(trace, " %04x:%04x", (uint16_t)(sh->cs - LOADSEG), sh->ip);
        }
        fputc('\n', trace);
    }
    if (!MAP_WIDE || (page != 1 && page != 2)) {
        lifted_f_1b05_17c1();
        return;
    }
    int16_t x = (int16_t)FARG(0), y = (int16_t)FARG(1);
    uint16_t ctx = GW(0xaa);
    if (x >= 0 && x < gfx_map_page_w() && y >= 0 && y <= (int16_t)GW((uint16_t)(ctx + 8)))
        gfx_put_pixel(page, x + (int16_t)GW((uint16_t)(ctx + 2)),
                      y + (int16_t)GW((uint16_t)(ctx + 4)), (uint8_t)FARG(2));
    RET_FAR(0);
}

/* World generator (ovl07) in the wide layout: it backs up layer columns
   0-1 into 2-3 (and back) with one blit, which needs two there (the blits
   themselves get width 0 in tools/patches.txt). */
static int gen_page(void)
{
    return GW(GW(0xaa));
}

void hook_gen_backup(void)              /* ovl07 0686 */
{
    if (!MAP_WIDE)
        return;
    gfx_copy_rect(gen_page(), MAP_LX(0), 0, 160, 100, MAP_LX(2), 0);
    gfx_copy_rect(gen_page(), MAP_LX(1), 0, 160, 100, MAP_LX(3), 0);
}

void hook_gen_restore(void)             /* ovl07 07f4 */
{
    if (!MAP_WIDE)
        return;
    gfx_copy_rect(gen_page(), MAP_LX(2), 0, 160, 100, MAP_LX(0), 0);
    gfx_copy_rect(gen_page(), MAP_LX(3), 0, 160, 100, MAP_LX(1), 0);
}

/* ovl07 0051: the EARTH map (map.pic, original 80 column layout) was
   loaded into page 1; move it to the wide layout, stretched to MAP_W */
void hook_earth_loaded(void)
{
    if (!MAP_WIDE)
        return;
    static uint8_t old[200][320];
    int page = gen_page();
    for (int y = 0; y < 200; y++)
        for (int x = 0; x < 320; x++)
            old[y][x] = gfx_get_pixel(gfx_page_seg(page), x, y);
    for (int k = 0; k < 4; k++)
        for (int y = 0; y < 200; y++)
            for (int c = 0; c < MAP_W; c++)
                gfx_put_pixel(page, MAP_LX(k) + c, y, old[y][80 * k + c * 80 / MAP_W]);
}

/* ovl05 0c3f: EARTH start position from the 80 column table */
void hook_earth_start(void)
{
    if (MAP_WIDE) {
        int16_t x = (int16_t)RW(SS, (uint16_t)(BP - 8));
        WW(SS, (uint16_t)(BP - 8), (uint16_t)(x * MAP_W / 80));
    }
}

/* ovl07 0596: the original clears x 160-319 of page 1 here (layers 2-3),
   which includes the picture area x 160-319, y 50-199; the patched fill
   only clears the moved layers */
void hook_gen_clear(void)
{
    if (MAP_WIDE)
        for (int y = 0; y < 200; y++)
            for (int x = 160; x < 320; x++)
                gfx_put_pixel(gen_page(), x, y, 0);
}

/* minimap: an 80 column window of the map (f_1b05_0004) */
void hook_minimap_width(void)           /* 1b05:01e1: columns before the wrap */
{
    if ((int16_t)RW(SS, (uint16_t)(BP - 6)) > 80)
        WW(SS, (uint16_t)(BP - 6), 80);
}

void hook_minimap_city(void)            /* 1b05:032b, AX = y on the minimap */
{
    if ((int16_t)RW(SS, (uint16_t)(BP - 0x18)) >= 80)
        AX = 0xFFFF;
}

void hook_minimap_unit(void)            /* 0860:0b77 */
{
    if ((int16_t)RW(SS, (uint16_t)(BP - 2)) >= 80)
        WW(SS, (uint16_t)(BP - 4), 0xFFFF);
}

/* Save games: the game stores page 2 as a 320x200 picture (CIVILn.MAP),
   which cannot hold the wide layout.  The whole page goes into CIVILn.WMP
   next to it, written after the picture and restored after loading it. */
static int wmp_path(char *host, size_t n)
{
    char dos[128];
    uint16_t name = RW(SS, (uint16_t)(BP + 6));     /* "a:CIVILn.MAP" */
    int i = 0;
    for (; i < (int)sizeof dos - 1 && RB(DS, (uint16_t)(name + i)); i++)
        dos[i] = (char)RB(DS, (uint16_t)(name + i));
    dos[i] = 0;
    if (i < 4 || !dos_resolve_path(dos, host, n, 0))
        return -1;
    size_t l = strlen(host);
    if (l < 3)
        return -1;
    memcpy(host + l - 3, "WMP", 3);
    return 0;
}

void hook_map_saved(void)               /* ovl11 0916: picture written */
{
    char path[1024];
    if (!MAP_WIDE || wmp_path(path, sizeof path))
        return;
    int w = gfx_map_page_w(), h = 200;
    uLong n = (uLong)w * h, zn = compressBound(n);
    uint8_t *raw = malloc(n), *z = malloc(zn);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            raw[y * w + x] = gfx_get_pixel(gfx_page_seg(2), x, y);
    FILE *f = fopen(path, "wb");
    if (f && compress2(z, &zn, raw, n, 9) == Z_OK) {
        uint16_t hdr[3] = {(uint16_t)MAP_W, (uint16_t)w, (uint16_t)h};
        fwrite("CIVW1", 1, 5, f);
        fwrite(hdr, sizeof hdr, 1, f);
        fwrite(z, 1, zn, f);
    }
    if (f)
        fclose(f);
    free(raw);
    free(z);
}

void hook_game_loaded(void)             /* ovl11 08b3: save opened for loading */
{
    dos_load_save_sidecar();
}

void hook_map_loaded(void)              /* ovl11 08ea: picture loaded */
{
    char path[1024];
    if (!MAP_WIDE || wmp_path(path, sizeof path))
        return;
    FILE *f = fopen(path, "rb");
    char tag[5];
    uint16_t hdr[3];
    if (!f || fread(tag, 1, 5, f) != 5 || memcmp(tag, "CIVW1", 5) ||
        fread(hdr, sizeof hdr, 1, f) != 1) {
        if (f)
            fclose(f);
        ovl_msg("No wide map data for this game");
        return;
    }
    if (hdr[0] != (uint16_t)MAP_W)
        ovl_msg("Saved with map width %d, now %d", hdr[0], MAP_W);
    fseek(f, 0, SEEK_END);
    long zn = ftell(f) - 11;
    fseek(f, 11, SEEK_SET);
    uLong n = (uLong)hdr[1] * hdr[2];
    uint8_t *z = malloc(zn > 0 ? zn : 1), *raw = malloc(n);
    if (zn > 0 && fread(z, 1, zn, f) == (size_t)zn &&
        uncompress(raw, &n, z, zn) == Z_OK)
        for (int y = 0; y < hdr[2]; y++)
            for (int x = 0; x < hdr[1]; x++)
                gfx_put_pixel(2, x, y, raw[y * hdr[1] + x]);
    fclose(f);
    free(z);
    free(raw);
}

/* continent tables in CONTSEG: the original clears them with memset() on a
   DS pointer (patched to size 0) and saves one with a DS buffer */
static void blk_clear(uint16_t off)
{
    if (MAP_WIDE)
        for (int i = 0; i < BLK_SIZE; i++)
            WB(CONTSEG, (uint16_t)(off + i), 0);
}

void hook_blk_clear_path(void) { blk_clear(0xe736); }
void hook_blk_clear_land(void) { blk_clear(0xea64); }
void hook_blk_clear_sea(void) { blk_clear(0x7eb8); }

void hook_blk_io_begin(void)
{
    if (MAP_WIDE)
        dos_redirect_buffer(0xea64, CONTSEG);
}

void hook_blk_io_end(void)
{
    dos_redirect_buffer(0, 0);
}

void hook_vis_io_begin(void)
{
    if (MAP_WIDE)
        dos_redirect_buffer(0x7ff8, 0xFFFF);
}

void hook_vis_io_end(void)
{
    dos_redirect_buffer(0, 0);
}

/* Test aid (CIV_STATEDUMP=file): write the game state in a layout
   independent form -- the data segment without the stack, the explored
   table from wherever it lives, and the map layers of page 2 read square
   by square.  Runs with different map storage can then be compared. */
#include <stdio.h>
#include <stdlib.h>

void native_state_dump_to(const char *path);

void native_state_dump(void)
{
    const char *path = getenv("CIV_STATEDUMP");
    if (path)
        native_state_dump_to(path);
}

void native_state_dump_to(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    uint8_t ds[0xF000];
    for (uint32_t a = 0; a < sizeof ds; a++)
        ds[a] = RB(DS, (uint16_t)a);
    for (int i = 0; i < VIS_SIZE && i < 4000; i++)
        ds[0x7ff8 + i] = RB(VISSEG, (uint16_t)(0x7ff8 + i));
    static const uint16_t blk[] = {0xea64, 0x7eb8, 0xe736};
    for (int t = 0; t < 3; t++)
        for (int i = 0; i < BLK_SIZE && i < 260; i++)
            ds[blk[t] + i] = RB(CONTSEG, (uint16_t)(blk[t] + i));
    fwrite(ds, 1, sizeof ds, f);
    /* page 2: 4 layer columns x 4 rows of 80 x 50 squares */
    uint16_t seg = gfx_native ? gfx_page_seg(2) : RW(modbase[MOD_MGRAPHIC], 0x1970 + 4);
    for (int ly = 0; ly < 4; ly++)
        for (int lx = 0; lx < 4; lx++)
            for (int y = 0; y < 50; y++)
                for (int x = 0; x < MAP_W; x++) {
                    int px = MAP_LX(lx) + x, py = ly * 50 + y;
                    uint8_t v;
                    if (gfx_native && gfx_is_wide())
                        v = gfx_get_pixel(seg, px, py);
                    else
                        v = mem[((uint32_t)seg << 4) + (uint16_t)(py * 320 + px)];
                    fputc(v, f);
                }
    fclose(f);
}
