/*
 * Multiplayer: two PCs play one game in real time.
 *
 * The host's game is the real one.  The joining player gets one of its
 * civilizations; on the host that civilization is moved by the orders the
 * player gives on the other PC instead of by the computer.
 *
 * Every turn:
 *   host  turn start: sends the game state (the game's own save data from
 *                     ovl11_09e4 plus the map page) -- SNAP
 *   both  play their own civilization for the turn time; a turn that is
 *         due ends as soon as the player is back on the map (dialogs and
 *         city screens are never closed for the player)
 *   host  turn time over: TURN_END; the host's own units are done
 *   peer  on TURN_END: ends its turn, sends the orders it gave (unit keys
 *         in order, goto targets, city builds and gold settings) -- DONE
 *   host  when the joining player's civilization moves: applies DONE and
 *         plays the recorded keys back through the computer player's
 *         unit order hook (f_15da_0c76)
 *   peer  skips all other civilizations and waits for the next SNAP
 *
 * Lobby: "Multiplayer" in the start menu, then H = host, F = find games.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "native.h"
#include "runtime.h"
#include "addons.h"
#include "gfx.h"
#include "mp.h"
#include "video.h"

#define MSG_HELLO 1
#define MSG_START 2
#define MSG_SNAP 3
#define MSG_TURN_END 4
#define MSG_DONE 5
#define MP_VERSION 1

#define UNIT(p, u) (0x90f4 + 0x600 * (p) + 12 * (u))
#define CITY(c) (0x706c + 0x1c * (c))
#define HUMAN GW(0xe710)
#define MENU_TEXT 0xc926
#define SNAP_FILE "MPSNAP.SVE"
#define KEY_ENTER 0x1C0D

enum { MP_OFF, MP_LOBBY, MP_HOSTING, MP_SCANNING, MP_JOINED, MP_GAME_HOST, MP_GAME_PEER };
static int state = MP_OFF;
static char status_line[96];
static struct mp_host hosts[MP_MAX_HOSTS];
static int nhosts, sel;
static uint64_t last_scan_us;
static char peer_name[32];
static int peer_ok;                 /* host: the joined player said hello */

/* game */
static int remote_civ = -1;         /* host: the joining player's civ */
static int my_civ = -1;             /* peer */
static int turn_no = -1;
static uint64_t turn_start_us;
static int turn_ms_mp = 3000;
static int turn_end;                /* host: sent / peer: received */
static int in_phase;                /* our own civilization is moving */
static uint64_t last_idle_us, last_enter_us, last_orders_us;
static int auto_start;              /* peer: answer the new game questions */

/* peer: orders given this turn */
struct cmd { uint8_t unit; uint16_t key; };
static struct cmd cmds[512];
static int ncmds;
static int auto_key;                /* the key was made by the game, not the player */

static int final_turn = -1;          /* host: last turn with all orders in */
static int remote_goal = -1;        /* host: the joining player's research */
static uint8_t base_build[128];     /* peer: builds as the host sent them */
static int16_t intent_build[128];   /* peer: builds chosen here, -1 none */

/* host: orders of the joining player, per unit */
static uint16_t queue[128][16];
static int qlen[128], qpos[128];

#define DBG(...) do { if (getenv("CIV_MPDEBUG")) { fprintf(stderr, "mp: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

static int myname(char *b, size_t n)
{
    const char *u = getenv("USER");
    return snprintf(b, n, "%s", u && *u ? u : "Player");
}

/* ------------------------------------------------------------------ */
/* game state <-> bytes */

static void put_name(void)
{
    const char *s = SNAP_FILE;
    int i = 0;
    for (; s[i]; i++)
        WB(DS, (uint16_t)(MENU_TEXT + i), (uint8_t)s[i]);
    WB(DS, (uint16_t)(MENU_TEXT + i), 0);
}

/* run the game's save game data routine (ovl11_09e4) on MPSNAP.SVE */
static int game_data_io(int load)
{
    put_name();
    if (load)
        call_lifted(0x2058, f_2058_17e6, 2, MENU_TEXT, 0x8000);
    else
        call_lifted(0x2058, f_2058_17e6, 3, MENU_TEXT, 0x8301, 0x80);
    int16_t h = (int16_t)AX;
    if (h < 0)
        return -1;
    WW(DS, 0x67a0, (uint16_t)h);
    WW(DS, 0x679c, (uint16_t)(load ? 1 : 0));
    ovl_call_id(11, 0x09e4, 0xFFF0);
    call_lifted(0x2058, f_2058_174c, 1, (uint16_t)h);
    return 0;
}

static uint16_t page2_seg(void)
{
    return gfx_native ? gfx_page_seg(2) : RW(modbase[MOD_MGRAPHIC], 0x1970 + 4);
}

static int page2_w(void)
{
    return gfx_native && gfx_is_wide() ? gfx_map_page_w() : 320;
}

static uint8_t page2_get(int x, int y)
{
    if (gfx_native)
        return gfx_get_pixel(page2_seg(), x, y);
    return mem[((uint32_t)page2_seg() << 4) + (uint16_t)(y * 320 + x)];
}

static void page2_put(int x, int y, uint8_t v)
{
    if (gfx_native)
        gfx_put_pixel(2, x, y, v);
    else
        mem[((uint32_t)page2_seg() << 4) + (uint16_t)(y * 320 + x)] = v;
}

struct snap_hdr {
    uint16_t turn, civ;
    uint32_t turn_ms;
    uint16_t mapw, pagew, pageh, pad;
    uint32_t sve_len;
};

static char *game_path(const char *name)
{
    static char host[1024];
    if (!dos_resolve_path(name, host, sizeof host, 0))
        snprintf(host, sizeof host, "%s", name);
    return host;
}

static void send_snap(void)
{
    if (game_data_io(0) < 0)
        return;
    FILE *f = fopen(game_path(SNAP_FILE), "rb");
    if (!f)
        return;
    fseek(f, 0, SEEK_END);
    long sl = ftell(f);
    fseek(f, 0, SEEK_SET);
    int pw = page2_w(), ph = 200;
    size_t raw_len = sizeof(struct snap_hdr) + (size_t)sl + (size_t)pw * ph;
    uint8_t *raw = malloc(raw_len);
    struct snap_hdr h = {(uint16_t)GW(0x90f2), (uint16_t)remote_civ, (uint32_t)turn_ms_mp,
                         (uint16_t)MAP_W, (uint16_t)pw, (uint16_t)ph, 0, (uint32_t)sl};
    memcpy(raw, &h, sizeof h);
    if (fread(raw + sizeof h, 1, (size_t)sl, f) != (size_t)sl)
        sl = 0;
    fclose(f);
    uint8_t *p = raw + sizeof h + sl;
    for (int y = 0; y < ph; y++)
        for (int x = 0; x < pw; x++)
            *p++ = page2_get(x, y);
    uLong zl = compressBound(raw_len);
    uint8_t *z = malloc(zl + 4);
    z[0] = (uint8_t)raw_len;
    z[1] = (uint8_t)(raw_len >> 8);
    z[2] = (uint8_t)(raw_len >> 16);
    z[3] = (uint8_t)(raw_len >> 24);
    if (compress2(z + 4, &zl, raw, raw_len, 6) == Z_OK)
        net_send(MSG_SNAP, z, (uint32_t)zl + 4);
    free(z);
    free(raw);
}

static uint8_t *snap_buf;           /* peer: latest SNAP, uncompressed */
static uLong snap_len;

static void keep_snap(const uint8_t *d, uint32_t l)
{
    if (l < 4)
        return;
    uLong raw = d[0] | d[1] << 8 | d[2] << 16 | (uLong)d[3] << 24;
    uint8_t *b = malloc(raw);
    if (uncompress(b, &raw, d + 4, l - 4) != Z_OK || raw < sizeof(struct snap_hdr)) {
        free(b);
        return;
    }
    free(snap_buf);
    snap_buf = b;
    snap_len = raw;
}

/* peer: replace the game state by the latest SNAP */
static int load_snap(void)
{
    if (!snap_buf)
        return 0;
    struct snap_hdr h;
    memcpy(&h, snap_buf, sizeof h);
    if (h.mapw != MAP_W) {
        ovl_msg("Host map width %d, ours %d: start with --mapwidth %d", h.mapw, MAP_W, h.mapw);
        return 0;
    }
    FILE *f = fopen(game_path(SNAP_FILE), "wb");
    if (!f)
        return 0;
    fwrite(snap_buf + sizeof h, 1, h.sve_len, f);
    fclose(f);
    int16_t vx = (int16_t)GW(0xe3ec), vy = (int16_t)GW(0xe67e);
    uint16_t goal = GW(0x19be);         /* our research goal (one global) */
    if (my_civ >= 0)
        for (int c = 0; c < 128; c++)   /* builds changed here since the last state */
            if (GB(CITY(c) + 6) != 0xff && GB(CITY(c) + 11) == my_civ &&
                GB(CITY(c) + 9) != base_build[c])
                intent_build[c] = GB(CITY(c) + 9);
    game_data_io(1);
    const uint8_t *p = snap_buf + sizeof h + h.sve_len;
    int pw = page2_w();
    for (int y = 0; y < h.pageh; y++)
        for (int x = 0; x < h.pagew; x++, p++)
            if (x < pw)
                page2_put(x, y, *p);
    int first = my_civ < 0;
    my_civ = h.civ;
    turn_ms_mp = (int)h.turn_ms;
    if (!first)
        WW(DS, 0x19be, goal);
    for (int c = 0; c < 128; c++) {
        uint16_t r = CITY(c);
        int ours = GB(r + 6) != 0xff && GB(r + 11) == my_civ;
        if (!ours || first)
            intent_build[c] = -1;
        else if (intent_build[c] >= 0) {
            if (GB(r + 9) == (uint8_t)intent_build[c])
                intent_build[c] = -1;    /* the host has it */
        }
        base_build[c] = GB(r + 9);
        if (ours && intent_build[c] >= 0)
            WB(DS, (uint16_t)(r + 9), (uint8_t)intent_build[c]);
    }
    WW(DS, 0xe710, (uint16_t)my_civ);
    WW(DS, 0x7eb6, (uint16_t)(1u << my_civ));
    if (turn_no < 0) {
        /* first state: look at one of our units */
        for (int u = 0; u < 128; u++)
            if (GB(UNIT(my_civ, u) + 3) != 0xff) {
                vx = (int16_t)(GB(UNIT(my_civ, u) + 1) - VIEW_W / 2);
                vy = (int16_t)(GB(UNIT(my_civ, u) + 2) - VIEW_H / 2);
                break;
            }
    }
    WW(DS, 0xe3ec, (uint16_t)vx);
    WW(DS, 0xe67e, (uint16_t)vy);
    turn_no = h.turn;
    free(snap_buf);
    snap_buf = NULL;
    return 1;
}

/* ------------------------------------------------------------------ */
/* messages */

static void send_hello(void)
{
    char b[96];
    int n = snprintf(b, sizeof b, "%d|%d|%d|", MP_VERSION, MAP_W, g_barbarians);
    n += myname(b + n, sizeof b - (size_t)n);
    net_send(MSG_HELLO, b, (uint32_t)n + 1);
}

static void send_done(int final)
{
    uint8_t b[4096];
    int n = 0;
    b[n++] = (uint8_t)turn_no;
    b[n++] = (uint8_t)(turn_no >> 8);
    b[n++] = (uint8_t)ncmds;
    b[n++] = (uint8_t)(ncmds >> 8);
    for (int i = 0; i < ncmds; i++) {
        b[n++] = cmds[i].unit;
        b[n++] = (uint8_t)cmds[i].key;
        b[n++] = (uint8_t)(cmds[i].key >> 8);
    }
    int nc = n++;
    b[nc] = 0;
    for (int c = 0; c < 128; c++) {
        uint16_t r = CITY(c);
        if (GB(r + 6) == 0xff || GB(r + 11) != my_civ)
            continue;
        b[n++] = (uint8_t)c;
        b[n++] = GB(r + 9);
        b[n++] = (uint8_t)city_produces_gold(c);
        b[nc]++;
    }
    int nu = n++;
    b[nu] = 0;
    for (int u = 0; u < 128; u++) {
        uint16_t r = UNIT(my_civ, u);
        if (GB(r + 3) == 0xff || GB(r + 6) == 0xff)
            continue;
        b[n++] = (uint8_t)u;
        b[n++] = GB(r + 6);
        b[n++] = GB(r + 7);
        b[nu]++;
    }
    b[n++] = (uint8_t)final;            /* the units of this turn are done */
    b[n++] = GB(0x19be);                /* research goal */
    b[n++] = GB(0x19bf);
    net_send(MSG_DONE, b, (uint32_t)n);
    ncmds = 0;
}

/* host: the joining player's orders */
static void apply_done(const uint8_t *b, uint32_t l)
{
    if (l < 4)
        return;
    int turn = b[0] | b[1] << 8, nc = b[2] | b[3] << 8;
    uint32_t n = 4;
    for (int i = 0; i < nc && n + 3 <= l; i++, n += 3) {
        int u = b[n] & 127;
        if (qlen[u] < 16)
            queue[u][qlen[u]++] = (uint16_t)(b[n + 1] | b[n + 2] << 8);
    }
    if (n < l) {
        int ncity = b[n++];
        for (int i = 0; i < ncity && n + 3 <= l; i++, n += 3) {
            int c = b[n] & 127;
            uint16_t r = CITY(c);
            if (GB(r + 6) == 0xff || GB(r + 11) != remote_civ)
                continue;
            WB(DS, (uint16_t)(r + 9), b[n + 1]);
            city_set_gold(c, b[n + 2]);
        }
    }
    if (n < l) {
        int nu = b[n++];
        for (int i = 0; i < nu && n + 3 <= l; i++, n += 3) {
            uint16_t r = UNIT(remote_civ, b[n] & 127);
            if (GB(r + 3) == 0xff)
                continue;
            WB(DS, (uint16_t)(r + 6), b[n + 1]);
            WB(DS, (uint16_t)(r + 7), b[n + 2]);
        }
    }
    if (n + 3 <= l) {
        if (b[n])
            final_turn = turn;
        remote_goal = (int16_t)(b[n + 1] | b[n + 2] << 8);
    }
    if (nc)
        DBG("orders from turn %d: %d keys", turn, nc);
}

static void handle(int type, uint8_t *d, uint32_t l)
{
    switch (type) {
    case MSG_HELLO: {
        int ver = 0, mw = 0, barb = 1;
        char name[40] = "";
        sscanf((char *)d, "%d|%d|%d|%39[^\n]", &ver, &mw, &barb, name);
        if (state == MP_HOSTING) {
            if (!peer_name[0])
                send_hello();           /* answer */
            snprintf(peer_name, sizeof peer_name, "%s", name);
            peer_ok = ver == MP_VERSION && mw == MAP_W;
            snprintf(status_line, sizeof status_line, peer_ok ?
                     "%s joined. Enter = start the game" :
                     "%s uses another version or map width", peer_name);
        } else if (state == MP_JOINED) {
            if (ver != MP_VERSION || mw != MAP_W)
                snprintf(status_line, sizeof status_line,
                         "Host map width %d (ours %d): use --mapwidth %d", mw, MAP_W, mw);
            else {
                g_barbarians = barb;
                snprintf(status_line, sizeof status_line, "Connected to %s. Waiting for the host...", name);
            }
        }
        break;
    }
    case MSG_START:
        if (state == MP_JOINED) {
            state = MP_GAME_PEER;
            auto_start = 1;
            bios_key_push(KEY_ENTER);   /* "Start a New Game" */
        }
        break;
    case MSG_SNAP:
        if (state == MP_GAME_PEER)
            keep_snap(d, l);
        break;
    case MSG_TURN_END:
        DBG("turn end %d (ours %d)", l >= 2 ? d[0] | d[1] << 8 : -1, turn_no);
        if (state == MP_GAME_PEER && l >= 2 && (d[0] | d[1] << 8) == turn_no)
            turn_end = 1;
        break;
    case MSG_DONE:
        if (state == MP_GAME_HOST)
            apply_done(d, l);       /* played back when that civ moves */
        break;
    }
}

/* ------------------------------------------------------------------ */
/* per frame (from the runtime's frame tick) */

static void frame(void);

/* every frame, from the runtime's frame tick: DS is that of the code that
   happens to run, game data is reached through DGROUP */
void native_frame(void)
{
    if (state == MP_OFF)
        return;
    uint16_t ds = DS;
    DS = DGROUP;
    frame();
    DS = ds;
}

static void frame(void)
{
    net_poll();
    int type;
    uint8_t *d;
    uint32_t l;
    while (net_recv(&type, &d, &l))
        handle(type, d, l);
    uint64_t now = now_us();
    if (state == MP_HOSTING && net_connected() && !peer_name[0] && now - last_scan_us > 500000) {
        last_scan_us = now;
        send_hello();
    }
    if (state == MP_SCANNING && now - last_scan_us > 1000000) {
        last_scan_us = now;
        net_scan();
        nhosts = net_hosts(hosts, MP_MAX_HOSTS);
    }
    if (state == MP_GAME_PEER && auto_start && now - last_enter_us > 500000) {
        uint16_t k;
        last_enter_us = now;
        if (!bios_key_peek(&k))
            bios_key_push(KEY_ENTER);   /* new game questions: defaults */
    }
    if (state == MP_GAME_HOST && turn_no >= 0 && !turn_end &&
        now - turn_start_us >= (uint64_t)turn_ms_mp * 1000) {
        turn_end = 1;
        uint8_t b[2] = {(uint8_t)turn_no, (uint8_t)(turn_no >> 8)};
        net_send(MSG_TURN_END, b, 2);
        DBG("host turn %d over", turn_no);
    }
    /* the joining player's orders go out as they are given */
    if (state == MP_GAME_PEER && in_phase && now - last_orders_us > 500000) {
        last_orders_us = now;
        send_done(0);
    }
    if ((state == MP_GAME_HOST || state == MP_GAME_PEER) && !net_connected() && turn_no >= 0) {
        ovl_msg("Multiplayer: the other player left");
        state = MP_OFF;
        net_close();
    }
}

/* wait inside a hook, keeping the window alive */
static void wait_frame(void)
{
    plat_pump_events();
    script_tick();
    native_frame();
    plat_present();
    plat_sleep_us(10000);
    if (quit_requested) {
        plat_shutdown();
        exit(0);
    }
}

/* ------------------------------------------------------------------ */
/* clock (realtime.c) */

int mp_in_game(void)
{
    return state == MP_GAME_HOST || state == MP_GAME_PEER;
}

int mp_turn_due(int unit)
{
    /* the host's clock ends the units' moves on both PCs; the joining
       player then stays on the map ("End of Turn") until the host's next
       turn has begun (its state arrived) */
    if (state == MP_GAME_PEER && unit >= 128)
        return snap_buf != NULL;
    return turn_end;
}

/* hook_realtime_end_of_turn: the human's units are all done */
void mp_units_done(void)
{
    if (state == MP_GAME_PEER && in_phase) {
        send_done(1);               /* the host uses them this turn */
        DBG("units done, turn %d", turn_no);
    }
}

int mp_progress(void)
{
    uint64_t t = (uint64_t)turn_ms_mp * 1000, e = now_us() - turn_start_us;
    return e >= t ? 1000 : (int)(e * 1000 / t);
}

int mp_turn_secs10(void)
{
    return turn_ms_mp / 100;
}

void mp_note_idle(void)
{
    last_idle_us = now_us();
}

/* ------------------------------------------------------------------ */
/* hooks in the game */

/* 01a7:04a6, start menu text copied to DS:C926 */
void hook_mp_menu_text(void)
{
    if (getenv("CIV_ORIGINAL"))
        return;                         /* comparison runs: original menus */
    int n = 0;
    while (n < 900 && GB(MENU_TEXT + n))
        n++;
    const char *add = " Multiplayer\n";
    for (int i = 0; add[i]; i++)
        WB(DS, (uint16_t)(MENU_TEXT + n + i), (uint8_t)add[i]);
    WB(DS, (uint16_t)(MENU_TEXT + n + strlen(add)), 0);
}

/* 01a7:04bd, [6AB2] = start menu choice */
void hook_mp_menu_choice(void)
{
    if ((int16_t)GW(0x6ab2) == 5) {
        state = MP_LOBBY;
        snprintf(status_line, sizeof status_line, "H = host a game   F = find games   Esc = back");
        WW(DS, 0x6ab2, 0xffff);         /* show the menu again under the lobby */
    }
}

/* 0237:009c, start of a turn (f_0237_0094) */
void hook_mp_turn_start(void)
{
    if (state == MP_GAME_HOST) {
        if (remote_civ < 0) {
            int alive = GW(0x6b0a);
            for (int c = 1; c < 8; c++)
                if (c != (int)HUMAN && (alive & (1 << c))) {
                    remote_civ = c;
                    break;
                }
            ovl_msg("%s plays civilization %d", peer_name, remote_civ);
            /* the computer's plans for its units are void now */
            for (int u = 0; remote_civ > 0 && u < 128; u++) {
                uint16_t r = UNIT(remote_civ, u);
                if (GB(r + 3) == 0xff)
                    continue;
                WB(DS, (uint16_t)(r + 6), 0xff);    /* goto */
                WB(DS, r, GB(r) & ~0xc2);           /* settler work */
            }
        }
        turn_no = (int16_t)GW(0x90f2);
        WB(DS, 0x19c0, GB(0x19c0) & ~1);   /* no Instant Advice pop-ups */
        DBG("host turn %d starts", turn_no);
        turn_start_us = now_us();
        turn_end = 0;
        in_phase = 1;
        send_snap();
    } else if (state == MP_GAME_PEER) {
        auto_start = 0;
        while (!snap_buf && state == MP_GAME_PEER) {     /* only when joining */
            snprintf(status_line, sizeof status_line, "Joining the game...");
            wait_frame();
        }
        status_line[0] = 0;
        if (state != MP_GAME_PEER)
            return;
        load_snap();
        WB(DS, 0x19c0, GB(0x19c0) & ~1);
        DBG("peer turn %d starts, civ %d", turn_no, my_civ);
        turn_start_us = now_us();
        turn_end = 0;
        in_phase = 1;
        ncmds = 0;
        call_lifted(0x1b05, f_1b05_0004, 3, HUMAN, GW(0xe3ec), GW(0xe67e));
    }
}

/* 0237:0151, a civilization's part of the turn starts, [BP-6] = civ */
void hook_mp_civ_start(void)
{
    int civ = (int16_t)RW(SS, (uint16_t)(BP - 6));
    if (state != MP_GAME_HOST || civ != remote_civ)
        return;
    /* the other player's last orders of this turn are under way (the turn
       ended on both PCs at the same time): give them a moment, without
       showing anything; a player in a city screen is not waited for */
    uint64_t until = now_us() + 1500000;
    while (state == MP_GAME_HOST && final_turn != turn_no && now_us() < until)
        wait_frame();
}

/* 0ad5:165f in f_0ad5_1569(civ): a computer player is given advance
   [BP-0x14].  For the joining player's civilization: the advance chosen on
   the other PC, if it can be researched now. */
static int has_advance(int civ, int t)
{
    if (t < 0 || t >= 0x48)
        return t < 0;               /* "no prerequisite" */
    call_lifted(0x0ad5, f_0ad5_2293, 2, civ, t);
    return AX != 0;
}

void hook_mp_research(void)
{
    int civ = (int16_t)RW(SS, (uint16_t)(BP + 6)), t = remote_goal;
    if (state != MP_GAME_HOST || civ != remote_civ || t < 0 || t >= 0x48)
        return;
    int8_t p1 = (int8_t)GB((uint16_t)(0x4ee + 0x16 * t));
    int8_t p2 = (int8_t)GB((uint16_t)(0x4ef + 0x16 * t));
    if (has_advance(civ, t) || !has_advance(civ, p1) || !has_advance(civ, p2))
        return;
    WW(SS, (uint16_t)(BP - 0x14), (uint16_t)t);
    if ((int16_t)RW(SS, (uint16_t)(BP - 0x3c)) == -1)
        WW(SS, (uint16_t)(BP - 0x3c), 0);
    DBG("civ %d researches %d (chosen on the other PC)", civ, t);
}

/* 0237:0285, AX = 1 << civ before "test [6B0A], ax": the joining player's
   game moves only its own civilization */
void hook_mp_civ_filter(void)
{
    if (state == MP_GAME_PEER && AX != (uint16_t)(1u << HUMAN))
        AX = 0;
}

/* 0237:0239, after a civilization's units moved, [BP-6] = civ */
void hook_mp_civ_done(void)
{
    int civ = (int16_t)RW(SS, (uint16_t)(BP - 6));
    if (state == MP_GAME_HOST && civ == remote_civ) {
        memset(qlen, 0, sizeof qlen);   /* orders not used this turn are void */
        memset(qpos, 0, sizeof qpos);
        return;
    }
    if (!mp_in_game() || civ != (int)HUMAN)
        return;
    in_phase = 0;
    DBG("civ %d done", civ);
    if (state == MP_GAME_PEER)
        send_done(1);
}

/* 0402:05c5: the game made the order (settler work, goto) */
void hook_mp_auto_order(void)
{
    auto_key = 1;
}

/* 0402:0ced: an order for unit [BP-0x32] is carried out, key [BP-0x44] */
void hook_mp_record(void)
{
    int was_auto = auto_key;
    auto_key = 0;
    if (state != MP_GAME_PEER || (int16_t)RW(SS, (uint16_t)(BP + 6)) != (int)HUMAN || was_auto)
        return;
    int unit = (int16_t)RW(SS, (uint16_t)(BP - 0x32));
    uint16_t key = RW(SS, (uint16_t)(BP - 0x44));
    if (unit < 0 || unit >= 128 || key == 0 || key == 0xffff || ncmds >= 512)
        return;
    uint8_t a = (uint8_t)key;
    if (key < 0x100 && strchr("gGwWcC\r\x1b", a))
        return;                     /* view and selection keys */
    cmds[ncmds].unit = (uint8_t)unit;
    cmds[ncmds].key = key;
    ncmds++;
    DBG("record unit %d key %04x", unit, key);
}

/* f_15da_0c76(civ, unit): the computer player's order for a unit.  For the
   joining player's civilization: the orders recorded on the other PC. */
void f_15da_0c76(void)
{
    int civ = (int16_t)FARG(0), unit = (int16_t)FARG(1);
    if (state != MP_GAME_HOST || civ != remote_civ || unit < 0 || unit >= 128) {
        lifted_f_15da_0c76();
        return;
    }
    uint16_t r = UNIT(civ, unit), key;
    uint8_t st = GB(r), type = GB(r + 3);
    if (qpos[unit] < qlen[unit]) {
        key = queue[unit][qpos[unit]++];
        /* keypad moves of the human order code -> the direction numbers
           the computer player's orders use (0402:328a / 1b7a) */
        static const uint16_t pad[8][2] = {{0x4800, 1}, {0x4900, 2}, {0x4d00, 3},
            {0x5100, 4}, {0x5000, 5}, {0x4f00, 6}, {0x4b00, 7}, {0x4700, 8}};
        for (int i = 0; i < 8; i++)
            if (key == pad[i][0])
                key = pad[i][1];
    }
    else if (GB(r + 6) != 0xff)
        key = 0;                    /* goto: the game steps it (0402:0ced) */
    else if (st & 0xc2) {           /* settler work continues (as 0402:04f3) */
        key = 0x72;
        if (st & 0x40)
            key = type ? 0x6d : 0x69;
        if (st & 0x80)
            key = (st & 0x40) ? 0x66 : 0x6d;
        if (st & 2)
            key = 0x70;
    } else
        key = 0x20;                 /* nothing to do this turn */
    DBG("replay unit %d key %04x at %d,%d", unit, key, GB(r + 1), GB(r + 2));
    AX = key;
    RET_FAR(0);
}

/* ------------------------------------------------------------------ */
/* lobby */

int mp_key(uint16_t key)
{
    if (state < MP_LOBBY || state > MP_JOINED)
        return 0;
    uint8_t a = (uint8_t)key, scan = (uint8_t)(key >> 8);
    if (a == 0x1b) {                    /* back */
        net_close();
        state = state == MP_LOBBY ? MP_OFF : MP_LOBBY;
        peer_name[0] = 0;
        snprintf(status_line, sizeof status_line, "H = host a game   F = find games   Esc = back");
        if (state == MP_OFF)
            status_line[0] = 0;
        return 1;
    }
    if (state == MP_LOBBY && (a == 'h' || a == 'H')) {
        char info[96];
        int n = myname(info, sizeof info);
        snprintf(info + n, sizeof info - (size_t)n, "|%d", MAP_W);
        net_set_info(info);
        if (net_host_start() < 0)
            snprintf(status_line, sizeof status_line, "Cannot open port 28400");
        else {
            state = MP_HOSTING;
            snprintf(status_line, sizeof status_line, "Waiting for a player...  (Esc = cancel)");
        }
        return 1;
    }
    if (state == MP_LOBBY && (a == 'f' || a == 'F')) {
        state = MP_SCANNING;
        nhosts = sel = 0;
        last_scan_us = 0;
        snprintf(status_line, sizeof status_line, "Searching...  Enter = join   Esc = back");
        return 1;
    }
    if (state == MP_SCANNING) {
        if (scan == 0x48 && sel > 0)
            sel--;
        if (scan == 0x50 && sel + 1 < nhosts)
            sel++;
        if (a == '\r' && sel < nhosts) {
            if (net_connect(hosts[sel].ip) < 0)
                snprintf(status_line, sizeof status_line, "Cannot connect to %s", hosts[sel].ip);
            else {
                state = MP_JOINED;
                send_hello();
                snprintf(status_line, sizeof status_line, "Connecting to %s...", hosts[sel].ip);
            }
        }
        return 1;
    }
    if (state == MP_HOSTING && a == '\r' && peer_ok) {
        net_send(MSG_START, "", 0);
        state = MP_GAME_HOST;
        status_line[0] = 0;
        cfg.realtime = 1;
        turn_ms_mp = cfg.turn_ms;
        bios_key_push(KEY_ENTER);       /* "Start a New Game" */
        return 1;
    }
    return 1;                           /* the lobby takes all keys */
}

int mp_overlay_active(void)
{
    return (state >= MP_LOBBY && state <= MP_JOINED) || status_line[0];
}

void mp_overlay_draw(uint8_t *buf, int W, int H, int ox, int oy,
                     void (*fill)(uint8_t *, int, int, int, int, int, int, uint8_t),
                     void (*text)(uint8_t *, int, int, int, int, const char *, uint8_t),
                     uint8_t white, uint8_t black, uint8_t yellow, uint8_t grey)
{
    (void)ox;
    (void)oy;
    if (state >= MP_LOBBY && state <= MP_JOINED) {
        int w = 220, h = 40 + 9 * (state == MP_SCANNING ? (nhosts ? nhosts : 1) : 1);
        int x = (W - w) / 2, y = (H - h) / 2;
        fill(buf, W, H, x - 1, y - 1, w + 2, h + 2, yellow);
        fill(buf, W, H, x, y, w, h, black);
        text(buf, W, H, x + 6, y + 5, "MULTIPLAYER", yellow);
        int ly = y + 18;
        if (state == MP_SCANNING) {
            if (!nhosts)
                text(buf, W, H, x + 6, ly, "no games found yet", grey);
            for (int i = 0; i < nhosts; i++) {
                char line[96], name[64] = "?";
                int mw = 0;
                sscanf(hosts[i].info, "%63[^|]|%d", name, &mw);
                snprintf(line, sizeof line, "%s %s  (%s, width %d)", i == sel ? ">" : " ",
                         name, hosts[i].ip, mw);
                text(buf, W, H, x + 6, ly + 9 * i, line, i == sel ? white : grey);
            }
            ly += 9 * (nhosts ? nhosts : 1);
        } else {
            const char *s = state == MP_LOBBY ? "Play together over the network" :
                            state == MP_HOSTING ? "Hosting on port 28400" : "";
            text(buf, W, H, x + 6, ly, s, grey);
            ly += 9;
        }
        text(buf, W, H, x + 6, ly + 4, status_line, white);
        return;
    }
    if (status_line[0]) {               /* waiting during the game */
        int w = 200, x = (W - w) / 2, y = 24;
        fill(buf, W, H, x, y, w, 11, black);
        text(buf, W, H, x + 4, y + 2, status_line, yellow);
    }
}
