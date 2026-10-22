/*
 * Entry point of the native build: parses options, loads CIV.EXE from the
 * game directory into emulated memory and runs the lifted program.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <signal.h>
#include "cpu.h"
#include "runtime.h"
#include "gfx.h"
#include "wide.h"

struct config cfg = {
    .win_w = 1280, .win_h = 800, .fullscreen = 0, .widescreen = 0,
    .scale_filter = 1, .aspect = 1, .integer_scale = 0, .gamedir = "",
    .autostart = 1, .sound = 2, .mouse = 1, .turn_ms = 3000, .rankings = 1,
};
int verbose;
static char script_path[1024];

void fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "civ: fatal: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n  CS=%04x DS=%04x ES=%04x SS=%04x SP=%04x BP=%04x "
            "AX=%04x BX=%04x CX=%04x DX=%04x SI=%04x DI=%04x\n",
            CS, DS, ES, SS, SP, BP, AX, BX, CX, DX, SI, DI);
    va_end(ap);
    fprintf(stderr, "  call stack (most recent first):\n");
    for (int i = shadow_depth - 1, n = 0; i >= 0 && n < 24; i--, n++) {
        struct shadow *s = &shadow_stack[i & (SHADOW_MAX - 1)];
        fprintf(stderr, "    return to %04x:%04x (root seg %04x)  sp=%04x\n",
                s->cs, s->ip, (uint16_t)(s->cs - LOADSEG), s->sp);
    }
    plat_shutdown();
    exit(1);
}

void warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "civ: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

void logmsg(const char *fmt, ...)
{
    if (!verbose)
        return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "civ: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* debug aid: CIV_WATCHDOG=N prints where the program is every N seconds */
static void watchdog(int sig)
{
    (void)sig;
    fprintf(stderr, "watchdog: CS=%04x (root seg %04x) SP=%04x BP=%04x AX=%04x\n",
            CS, (uint16_t)(CS - LOADSEG), SP, BP, AX);
    for (int i = shadow_depth - 1, n = 0; i >= 0 && n < 12; i--, n++) {
        struct shadow *s = &shadow_stack[i & (SHADOW_MAX - 1)];
        fprintf(stderr, "    return to %04x:%04x (root seg %04x)\n", s->cs, s->ip,
                (uint16_t)(s->cs - LOADSEG));
    }
    const char *e = getenv("CIV_WATCHDOG");
    alarm(e ? (unsigned)atoi(e) : 0);
}

static void usage(void)
{
    printf("usage: civ [options] [-- game arguments]\n"
           "  --gamedir DIR     directory with the original game files (default: .)\n"
           "  --res WxH         window / screen resolution, e.g. --res 1920x1080\n"
           "  --fullscreen      start in fullscreen (Alt+Enter toggles)\n"
           "  --widescreen      widen the game view to fill the screen aspect\n"
           "  --classic         original 320x200 view (letterboxed, default)\n"
           "  --zoom N          widescreen: size of one game pixel (default: height/200)\n"
           "  --mapwidth N      world width in squares, 80 (original) to 160\n"
           "  --realtime        turns pass on a clock (see --turnsecs)\n"
           "  --turnsecs S      realtime: seconds per turn (default 3)\n"
           "  --barbarians 0|1  barbarians appear (default 1)\n"
           "  --rankings 0|1    historians' reports of the greatest civs (default 1)\n"
           "  --square-pixels   do not correct the 320x200 pixel aspect\n"
           "  --nearest         nearest-neighbour scaling (no smoothing)\n"
           "  --integer         integer scale factors only\n"
           "  --config FILE     read options from FILE (default: civ.cfg)\n"
           "  --sound NAME      none, speaker, adlib (default speaker)\n"
           "  --no-mouse        keyboard only\n"
           "  --setup           ask the original setup questions in text mode\n"
           "  --script FILE     run an input script (testing)\n"
           "  --log FILE        write messages to FILE\n"
           "  -v                verbose log\n");
}

static void apply_option(const char *k, const char *v)
{
    if (!strcmp(k, "gamedir") && v)
        snprintf(cfg.gamedir, sizeof cfg.gamedir, "%s", v);
    else if (!strcmp(k, "res") && v) {
        int w, h;
        if (sscanf(v, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 200) {
            cfg.win_w = w;
            cfg.win_h = h;
        } else
            warn("bad resolution '%s'", v);
    } else if (!strcmp(k, "fullscreen"))
        cfg.fullscreen = v ? atoi(v) : 1;
    else if (!strcmp(k, "widescreen"))
        cfg.widescreen = v ? atoi(v) : 1;
    else if (!strcmp(k, "log") && v) {
        if (!freopen(v, "w", stderr))
            warn("cannot open log %s", v);
        setvbuf(stderr, NULL, _IONBF, 0);
    } else if (!strcmp(k, "mapwidth") && v) {
        int mw = atoi(v);
        if (mw < 80 || mw > MAP_W_MAX || mw % 2)
            warn("map width must be an even number from 80 to %d", MAP_W_MAX);
        else
            g_map_w = (int16_t)mw;
    } else if (!strcmp(k, "realtime"))
        cfg.realtime = v ? atoi(v) : 1;
    else if (!strcmp(k, "turnsecs") && v) {
        double t = atof(v);
        if (t < 0.25 || t > 600)
            warn("turnsecs must be between 0.25 and 600");
        else
            cfg.turn_ms = (int)(t * 1000);
    } else if (!strcmp(k, "rankings"))
        cfg.rankings = v ? atoi(v) != 0 : 1;
    else if (!strcmp(k, "barbarians"))
        g_barbarians = v ? atoi(v) != 0 : 1;
    else if (!strcmp(k, "zoom") && v)
        cfg.zoom = atoi(v);
    else if (!strcmp(k, "classic"))
        cfg.widescreen = 0;
    else if (!strcmp(k, "square-pixels"))
        cfg.aspect = 0;
    else if (!strcmp(k, "aspect"))
        cfg.aspect = v ? atoi(v) : 1;
    else if (!strcmp(k, "nearest"))
        cfg.scale_filter = 0;
    else if (!strcmp(k, "sound") && v) {
        static const char *names[] = {"", "none", "speaker", "tandy", "adlib", "mt32"};
        cfg.sound = atoi(v);
        for (int i = 1; i <= 5; i++)
            if (!strcasecmp(v, names[i]))
                cfg.sound = i;
        if (cfg.sound < 1 || cfg.sound > 5)
            cfg.sound = 1;
    } else if (!strcmp(k, "mouse"))
        cfg.mouse = v ? atoi(v) : 1;
    else if (!strcmp(k, "no-mouse"))
        cfg.mouse = 0;
    else if (!strcmp(k, "setup"))
        cfg.autostart = 0;
    else if (!strcmp(k, "autostart"))
        cfg.autostart = v ? atoi(v) : 1;
    else if (!strcmp(k, "script") && v)
        snprintf(script_path, sizeof script_path, "%s", v);
    else if (!strcmp(k, "integer"))
        cfg.integer_scale = v ? atoi(v) : 1;
    else
        warn("unknown option '%s'", k);
}

static void read_config(const char *path, int must)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        if (must)
            warn("cannot read config %s", path);
        return;
    }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || *p == ';' || *p == '\n' || !*p)
            continue;
        p[strcspn(p, "\r\n#")] = 0;
        for (char *e = p + strlen(p); e > p && (e[-1] == ' ' || e[-1] == '\t');)
            *--e = 0;
        if (!*p)
            continue;
        char *eq = strchr(p, '=');
        if (eq) {
            *eq = 0;
            char *k = p, *v = eq + 1;
            while (*v == ' ')
                v++;
            char *e = k + strlen(k);
            while (e > k && (e[-1] == ' ' || e[-1] == '\t'))
                *--e = 0;
            apply_option(k, v);
        } else
            apply_option(p, NULL);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *cfgfile = NULL;
    char game_args[128] = "";
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--config") && i + 1 < argc)
            cfgfile = argv[i + 1];
    read_config(cfgfile ? cfgfile : "civ.cfg", cfgfile != NULL);

    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (!strcmp(a, "--")) {
            for (i++; i < argc; i++) {
                strncat(game_args, argv[i], sizeof game_args - strlen(game_args) - 2);
                if (i + 1 < argc)
                    strcat(game_args, " ");
            }
            break;
        }
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage();
            return 0;
        }
        if (!strcmp(a, "-v")) {
            verbose++;
            continue;
        }
        if (!strcmp(a, "--config")) {
            i++;
            continue;
        }
        if (!strncmp(a, "--", 2)) {
            char key[64];
            const char *eq = strchr(a, '=');
            const char *val = NULL;
            if (eq) {
                snprintf(key, sizeof key, "%.*s", (int)(eq - a - 2), a + 2);
                val = eq + 1;
            } else {
                snprintf(key, sizeof key, "%s", a + 2);
                static const char *with_value[] = {"res", "gamedir", "script",
                                                   "sound", "zoom", "log", "mapwidth",
                                                   "turnsecs", "barbarians", "rankings", NULL};
                int takes = 0;
                for (int k = 0; with_value[k]; k++)
                    if (!strcmp(key, with_value[k]))
                        takes = 1;
                if (takes && i + 1 < argc)
                    val = argv[++i];
            }
            apply_option(key, val);
            continue;
        }
        warn("unknown argument '%s'", a);
    }

    if (*script_path && script_path[0] != '/') {
        /* make relative to the starting directory */
        char cwd[1024], tmp[2048];
        if (getcwd(cwd, sizeof cwd)) {
            snprintf(tmp, sizeof tmp, "%s/%s", cwd, script_path);
            snprintf(script_path, sizeof script_path, "%s", tmp);
        }
    }
    if (*cfg.gamedir && chdir(cfg.gamedir)) {
        fprintf(stderr, "civ: cannot enter game directory %s\n", cfg.gamedir);
        return 1;
    }
    char exe[1024];
    if (!dos_resolve_path("CIV.EXE", exe, sizeof exe, 1)) {
        fprintf(stderr, "civ: CIV.EXE not found. Run from the game directory "
                "or use --gamedir.\n");
        return 1;
    }

    bios_init();
    load_program(exe, game_args);
    dos_init();
    dispatch_init();
    if (plat_init())
        return 1;
    hw_init();
    if (*script_path)
        script_load(script_path);

    {
        const char *g = getenv("CIV_GFX");      /* lifted | native | shadow */
        int native = cfg.widescreen || g_map_w != 80 || (g && !strcmp(g, "native"));
        if (g && !strcmp(g, "shadow"))
            native = 2;
        int w = 320, h = 200;
        if (cfg.widescreen) {
            int scale = cfg.zoom ? cfg.zoom : cfg.win_h / 200;
            if (scale < 1)
                scale = 1;
            w = cfg.win_w / scale;
            h = cfg.win_h / scale;
            if (w < 320) w = 320;
            if (h < 200) h = 200;
            w &= ~1;
            logmsg("widescreen: %dx%d game pixels (x%d)", w, h, scale);
        }
        /* CIV_WIDESTORE=1: wide map storage with the original width (test) */
        const char *ws = getenv("CIV_WIDESTORE");
        g_map_wide = g_map_w != 80 || (ws && atoi(ws));
        if (g_map_wide) {
            native = native ? native : 1;
            gfx_set_map_page_w(MAP_LX(4));
        }
        gfx_init(native, w, h);
        wide_init(w, h);
    }
    const char *wd = getenv("CIV_WATCHDOG");
    if (wd) {
        signal(SIGALRM, watchdog);
        alarm((unsigned)atoi(wd));
    }
    const char *e = getenv("CIV_INTERP");
    interp_ref_mode = e && atoi(e);
    func_t entry = lookup_code(CS, entry_ip);
    if (entry)
        entry();
    else
        interp_exec(CS, entry_ip);
    fatal("program returned from entry point");
}
