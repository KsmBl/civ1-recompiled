/*
 * Input scripting and screenshots, used for automated testing:
 *
 *   civ --script test.txt
 *
 * Script lines (times are relative to the previous command):
 *   wait MS            pause
 *   key NAME           press and release a key (a-z 0-9 enter esc space
 *                      up down left right kp1..kp9 f1..f10 tab bksp ...)
 *   down NAME / up NAME
 *   type TEXT          type characters
 *   move X Y           move the mouse (game coordinates)
 *   click X Y [r]      click left (or right) mouse button
 *   shot FILE.png      save the current frame
 *   pokew OFF VALUE    write a word into the data segment (hex)
 *   quit               exit
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef NO_PNG
#include <zlib.h>
#endif
#include "cpu.h"
#include "runtime.h"
#include "video.h"
#include "addons.h"

static char **lines;
static int nlines, cur;
static uint64_t next_time;
static int mouse_release_pending;

static const struct { const char *name; uint8_t sc; uint8_t ext; } keys[] = {
    {"esc", 0x01, 0}, {"1", 0x02, 0}, {"2", 0x03, 0}, {"3", 0x04, 0},
    {"4", 0x05, 0}, {"5", 0x06, 0}, {"6", 0x07, 0}, {"7", 0x08, 0},
    {"8", 0x09, 0}, {"9", 0x0A, 0}, {"0", 0x0B, 0}, {"-", 0x0C, 0},
    {"=", 0x0D, 0}, {"bksp", 0x0E, 0}, {"tab", 0x0F, 0}, {"q", 0x10, 0},
    {"w", 0x11, 0}, {"e", 0x12, 0}, {"r", 0x13, 0}, {"t", 0x14, 0},
    {"y", 0x15, 0}, {"u", 0x16, 0}, {"i", 0x17, 0}, {"o", 0x18, 0},
    {"p", 0x19, 0}, {"[", 0x1A, 0}, {"]", 0x1B, 0}, {"enter", 0x1C, 0},
    {"ctrl", 0x1D, 0}, {"a", 0x1E, 0}, {"s", 0x1F, 0}, {"d", 0x20, 0},
    {"f", 0x21, 0}, {"g", 0x22, 0}, {"h", 0x23, 0}, {"j", 0x24, 0},
    {"k", 0x25, 0}, {"l", 0x26, 0}, {";", 0x27, 0}, {"'", 0x28, 0},
    {"`", 0x29, 0}, {"shift", 0x2A, 0}, {"\\", 0x2B, 0}, {"z", 0x2C, 0},
    {"x", 0x2D, 0}, {"c", 0x2E, 0}, {"v", 0x2F, 0}, {"b", 0x30, 0},
    {"n", 0x31, 0}, {"m", 0x32, 0}, {",", 0x33, 0}, {".", 0x34, 0},
    {"/", 0x35, 0}, {"rshift", 0x36, 0}, {"alt", 0x38, 0}, {"space", 0x39, 0},
    {"f1", 0x3B, 0}, {"f2", 0x3C, 0}, {"f3", 0x3D, 0}, {"f4", 0x3E, 0},
    {"f5", 0x3F, 0}, {"f6", 0x40, 0}, {"f7", 0x41, 0}, {"f8", 0x42, 0},
    {"f9", 0x43, 0}, {"f10", 0x44, 0}, {"kp7", 0x47, 0}, {"kp8", 0x48, 0},
    {"kp9", 0x49, 0}, {"kp-", 0x4A, 0}, {"kp4", 0x4B, 0}, {"kp5", 0x4C, 0},
    {"kp6", 0x4D, 0}, {"kp+", 0x4E, 0}, {"kp1", 0x4F, 0}, {"kp2", 0x50, 0},
    {"kp3", 0x51, 0}, {"kp0", 0x52, 0}, {"home", 0x47, 1}, {"up", 0x48, 1},
    {"pgup", 0x49, 1}, {"left", 0x4B, 1}, {"right", 0x4D, 1}, {"end", 0x4F, 1},
    {"down", 0x50, 1}, {"pgdn", 0x51, 1}, {"ins", 0x52, 1}, {"del", 0x53, 1},
};

int key_by_name(const char *name, int *ext)
{
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
        if (!strcasecmp(keys[i].name, name)) {
            *ext = keys[i].ext;
            return keys[i].sc;
        }
    return 0;
}

static void press(const char *name, int down, int up)
{
    int ext;
    int sc = key_by_name(name, &ext);
    if (!sc) {
        warn("script: unknown key '%s'", name);
        return;
    }
    if (down) {
        if (ext)
            kbd_scancode(0xE0);
        kbd_scancode((uint8_t)sc);
    }
    if (up) {
        if (ext)
            kbd_scancode(0xE0);
        kbd_scancode((uint8_t)(sc | 0x80));
    }
}

static void type_char(char c)
{
    char name[2] = {(char)tolower((unsigned char)c), 0};
    static const char shifted[] = "!@#$%^&*()_+{}:\"~|<>?";
    static const char base[] = "1234567890-=[];'`\\,./";
    const char *p = strchr(shifted, c);
    int shift = isupper((unsigned char)c) || (p && c);
    if (p)
        name[0] = base[p - shifted];
    if (c == ' ') {
        press("space", 1, 1);
        return;
    }
    if (shift)
        kbd_scancode(0x2A);
    press(name, 1, 1);
    if (shift)
        kbd_scancode(0xAA);
}

void script_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        warn("cannot open script %s", path);
        return;
    }
    char buf[512];
    while (fgets(buf, sizeof buf, f)) {
        buf[strcspn(buf, "\r\n")] = 0;
        char *p = buf;
        while (isspace((unsigned char)*p))
            p++;
        if (!*p || *p == '#')
            continue;
        lines = realloc(lines, sizeof(char *) * (nlines + 1));
        lines[nlines++] = strdup(p);
    }
    fclose(f);
    next_time = now_us() + 100000;
}

void script_tick(void)
{
    uint64_t now = now_us();
    while (cur < nlines && now >= next_time) {
        char *l = lines[cur++];
        char cmd[16] = "", arg[256] = "";
        sscanf(l, "%15s %255[^\n]", cmd, arg);
        int x, y;
        char b[8] = "";
        if (mouse_release_pending) {
            mouse_event_button(mouse_release_pending - 1, 0);
            mouse_release_pending = 0;
        }
        if (!strcmp(cmd, "waitmap")) {
            /* hold the script until the map screen is up (max 60 s) */
            static uint64_t since;
            if (!since)
                since = now;
            if (!video_is_map() && now - since < 60000000) {
                cur--;
                next_time = now + 100000;
                return;
            }
            since = 0;
            next_time = now + 1500000;
        } else if (!strcmp(cmd, "wait"))
            next_time = now + (uint64_t)atoi(arg) * 1000;
        else if (!strcmp(cmd, "key")) {
            press(arg, 1, 1);
            next_time = now + 150000;
        } else if (!strcmp(cmd, "down"))
            press(arg, 1, 0);
        else if (!strcmp(cmd, "up"))
            press(arg, 0, 1);
        else if (!strcmp(cmd, "type")) {
            for (char *c = arg; *c; c++)
                type_char(*c);
            next_time = now + 150000;
        } else if (!strcmp(cmd, "move") && sscanf(arg, "%d %d", &x, &y) == 2)
            mouse_event_motion(0, 0, video_unmap_mouse_x(x) * 2, y);
        else if (!strcmp(cmd, "click") && sscanf(arg, "%d %d %7s", &x, &y, b) >= 2) {
            int btn = b[0] == 'r' ? 1 : 0;
            mouse_event_motion(0, 0, video_unmap_mouse_x(x) * 2, y);
            mouse_event_button(btn, 1);
            mouse_release_pending = btn + 1;
            next_time = now + 120000;
        } else if (!strcmp(cmd, "wheel")) {
            mapnav_wheel(atoi(arg));
        } else if (!strcmp(cmd, "drag")) {
            int x1, y1, x2, y2;
            if (sscanf(arg, "%d %d %d %d", &x1, &y1, &x2, &y2) == 4) {
                mouse_event_motion(0, 0, video_unmap_mouse_x(x1) * 2, y1);
                mouse_event_button(0, 1);
                for (int k = 1; k <= 8; k++)
                    mouse_event_motion(0, 0, video_unmap_mouse_x(x1 + (x2 - x1) * k / 8) * 2,
                                       y1 + (y2 - y1) * k / 8);
                mouse_event_button(0, 0);
            }
        } else if (!strcmp(cmd, "shot"))
            screenshot_png(arg);
        else if (!strcmp(cmd, "dump")) {
            FILE *d = fopen(arg, "wb");
            if (d) {
                fwrite(mem, 1, 0x110000, d);
                fclose(d);
            }
        } else if (!strcmp(cmd, "pokew")) {
            unsigned o, v;
            if (sscanf(arg, "%x %x", &o, &v) == 2)
                WW((uint16_t)(LOADSEG + 0x2A1B), (uint16_t)o, (uint16_t)v);   /* DGROUP */
        } else if (!strcmp(cmd, "statedump")) {
            native_state_dump_to(arg);
        } else if (!strcmp(cmd, "stack")) {
            fprintf(stderr, "STACK %s:", arg);
            for (int i = shadow_depth - 1, n = 0; i >= 0 && n < 16; i--, n++) {
                struct shadow *sh = &shadow_stack[i & (SHADOW_MAX - 1)];
                fprintf(stderr, " %04x:%04x", (uint16_t)(sh->cs - LOADSEG), sh->ip);
            }
            fprintf(stderr, "\n");
        } else if (!strcmp(cmd, "quit"))
            quit_requested = 1;
        else
            warn("script: bad line '%s'", l);
    }
}

/* ------------------------------------------------------------------ */

#ifdef NO_PNG
void screenshot_png(const char *path)
{
    warn("screenshots are not supported in this build (%s)", path);
}
#else
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t hdr[8];
    put32(hdr, len);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (len)
        fwrite(data, 1, len, f);
    uLong crc = crc32(0, (const Bytef *)type, 4);
    if (len)
        crc = crc32(crc, data, len);
    uint8_t c[4];
    put32(c, (uint32_t)crc);
    fwrite(c, 1, 4, f);
}

void screenshot_png(const char *path)
{
    int w, h;
    const uint8_t *fb = video_framebuffer(&w, &h);
    FILE *f = fopen(path, "wb");
    if (!f) {
        warn("cannot write %s", path);
        return;
    }
    size_t rawlen = (size_t)(w * 3 + 1) * h;
    uint8_t *raw = malloc(rawlen);
    for (int y = 0; y < h; y++) {
        uint8_t *r = raw + (size_t)y * (w * 3 + 1);
        *r++ = 0;
        for (int x = 0; x < w; x++) {
            uint8_t c = fb ? fb[y * w + x] : 0;
            for (int k = 0; k < 3; k++) {
                uint8_t v = vga_dac[c][k];
                *r++ = (uint8_t)(v << 2 | v >> 4);
            }
        }
    }
    uLongf zlen = compressBound(rawlen);
    uint8_t *z = malloc(zlen);
    compress2(z, &zlen, raw, rawlen, 6);
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ihdr[13];
    put32(ihdr, (uint32_t)w);
    put32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;
    ihdr[9] = 2;
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (uint32_t)zlen);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
    logmsg("screenshot %s", path);
}
#endif
