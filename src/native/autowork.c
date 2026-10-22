/*
 * Settler automation: press A on an active Settlers unit to have it improve
 * an area on its own.  The area is the 21 square radius of the nearest own
 * city within 2 squares, otherwise the 5x5 squares around the unit.  For
 * every land square it builds a road, then a railroad, then irrigation
 * (only on terrain where irrigating does not change the terrain type).
 *
 * The unit is driven with the game's own orders: when it waits for orders
 * the next key (a move or R / I) is supplied, so the game applies all its
 * rules (technology, terrain, water).  An order the game refuses is
 * skipped.  Pressing any key while the unit is active cancels the job.
 */
#include <stdio.h>
#include <string.h>
#include "native.h"
#include "runtime.h"
#include "addons.h"

#define UNIT(p, u) (0x90f4 + 0x600 * (p) + 12 * (u))
#define CITY(c) (0x706c + 0x1c * (c))

/* map square improvement bits (map layer in graphics page 2) */
#define IMP_CITY 0x01
#define IMP_IRRIGATION 0x02
#define IMP_MINE 0x04
#define IMP_ROAD 0x08
#define IMP_RAILROAD 0x10

enum { T_ROAD, T_RAIL, T_IRRIGATE, T_COUNT };

struct square { uint8_t x, y, failed; };   /* failed: bit per task */
struct job {
    uint8_t active, player, type;
    uint8_t n, stuck, tries, progress;
    uint8_t last_x, last_y, last_task, last_sq;   /* last_task 0xff = move */
    struct square sq[25];
};
static struct job jobs[128];

/* call a lifted helper without disturbing the interrupted code */
static uint16_t map_call(func_t fn, int x, int y)
{
    cpu_t saved = cpu;
    call_lifted(0x1b05, fn, 2, x, y);
    uint16_t r = AX;
    cpu = saved;
    return r;
}

static int terrain(int x, int y) { return map_call(f_1b05_133c, x, y); }
static int improvements(int x, int y) { return map_call(f_1b05_1577, x, y); }

static int wrapx(int x) { return (x % MAP_W + MAP_W) % MAP_W; }

static int dist(int ax, int ay, int bx, int by)
{
    int dx = wrapx(ax - bx);
    if (dx > MAP_W / 2)
        dx = MAP_W - dx;
    int dy = ay > by ? ay - by : by - ay;
    return dx > dy ? dx : dy;
}

static int irrigable(int t)
{
    /* desert, plains, grassland, hills, tundra, river */
    return t == 0 || t == 1 || t == 2 || t == 4 || t == 6 || t == 11;
}

static int needs(struct square *s, int task)
{
    if (s->failed & (1 << task))
        return 0;
    int t = terrain(s->x, s->y);
    if (t == 10)                    /* ocean */
        return 0;
    int imp = improvements(s->x, s->y);
    if (imp & IMP_CITY)
        return 0;
    switch (task) {
    case T_ROAD: return !(imp & IMP_ROAD);
    case T_RAIL: return (imp & IMP_ROAD) && !(imp & IMP_RAILROAD);
    default: return irrigable(t) && !(imp & (IMP_IRRIGATION | IMP_MINE));
    }
}

static void start(int player, int unit, int ux, int uy)
{
    struct job *j = &jobs[unit];
    memset(j, 0, sizeof *j);
    j->active = 1;
    j->player = (uint8_t)player;
    j->last_task = 0xfe;
    /* nearest own city within two squares */
    int best = -1, bd = 3;
    for (int c = 0; c < 128; c++) {
        uint16_t r = CITY(c);
        if (GB(r + 6) == 0xff || GB(r + 11) != player)
            continue;
        int d = dist(GB(r + 4), GB(r + 5), ux, uy);
        if (d < bd) {
            bd = d;
            best = c;
        }
    }
    int cx = ux, cy = uy, radius = 2;
    if (best >= 0) {
        cx = GB(CITY(best) + 4);
        cy = GB(CITY(best) + 5);
    }
    /* squares sorted by distance from the unit */
    for (int d = 0; d <= 4 && j->n < 25; d++)
        for (int dy = -radius; dy <= radius; dy++)
            for (int dx = -radius; dx <= radius; dx++) {
                if (best >= 0 && (dx == 2 || dx == -2) && (dy == 2 || dy == -2))
                    continue;       /* city radius has no corners */
                int x = wrapx(cx + dx), y = cy + dy;
                if (y < 0 || y > 49 || dist(x, y, ux, uy) != d || j->n >= 25)
                    continue;
                j->sq[j->n].x = (uint8_t)x;
                j->sq[j->n].y = (uint8_t)y;
                j->n++;
            }
    ovl_msg(best >= 0 ? "Settlers: improving the city area (%d squares)"
                      : "Settlers: improving the area (%d squares)", j->n);
}

/* direction keys clockwise from north */
static const uint16_t dir_key[8] = {0x4800, 0x4900, 0x4d00, 0x5100,
                                    0x5000, 0x4f00, 0x4b00, 0x4700};
static const int dir_dx[8] = {0, 1, 1, 1, 0, -1, -1, -1};
static const int dir_dy[8] = {-1, -1, 0, 1, 1, 1, 0, -1};

/* step towards the target; after failed attempts try the neighbouring
   directions (0, +1, -1, +2, -2) to get around water */
static uint16_t move_key(int ux, int uy, int tx, int ty, int attempt)
{
    int dx = wrapx(tx - ux);
    if (dx > MAP_W / 2)
        dx -= MAP_W;
    int dy = ty - uy;
    int sx = dx > 0 ? 1 : dx < 0 ? -1 : 0, sy = dy > 0 ? 1 : dy < 0 ? -1 : 0;
    int d = 0;
    for (int k = 0; k < 8; k++)
        if (dir_dx[k] == sx && dir_dy[k] == sy)
            d = k;
    static const int turn[5] = {0, 1, -1, 2, -2};
    return dir_key[(d + turn[attempt % 5] + 8) % 8];
}

static uint16_t next_key(struct job *j, int ux, int uy)
{
    /* did the last order have an effect? */
    if (j->last_task < T_COUNT) {
        struct square *s = &j->sq[j->last_sq];
        if (s->x == ux && s->y == uy && needs(s, j->last_task))
            s->failed |= (uint8_t)(1 << j->last_task);   /* refused */
        else
            j->progress = 1;
        j->tries = 0;
    } else if (j->last_task == 0xff) {
        if (j->last_x == ux && j->last_y == uy) {
            if (++j->tries >= 5) {                        /* cannot get there */
                j->sq[j->last_sq].failed = 0xff;
                j->tries = 0;
            }
        } else
            j->tries = 0;
    }
    /* first square (nearest first) that still needs work */
    for (int i = 0; i < j->n; i++) {
        struct square *s = &j->sq[i];
        for (int t = 0; t < T_COUNT; t++) {
            if (!needs(s, t))
                continue;
            j->last_sq = (uint8_t)i;
            j->last_x = (uint8_t)ux;
            j->last_y = (uint8_t)uy;
            if (s->x != ux || s->y != uy) {
                j->last_task = 0xff;
                logmsg("autowork: unit at %d,%d moves towards %d,%d (task %d, imp %02x)",
                       ux, uy, s->x, s->y, t, improvements(s->x, s->y));
                return move_key(ux, uy, s->x, s->y, j->tries);
            }
            j->last_task = (uint8_t)t;
            j->tries = 0;
            logmsg("autowork: unit at %d,%d: %s (imp %02x, terrain %d)", ux, uy,
                   t == T_ROAD ? "road" : t == T_RAIL ? "railroad" : "irrigation",
                   improvements(ux, uy), terrain(ux, uy));
            return t == T_IRRIGATE ? 'i' : 'r';
        }
    }
    /* everything done or refused: work done in this pass may have made
       refused orders possible (irrigation next to new irrigation), so go
       round again; unreachable squares stay skipped */
    if (j->progress) {
        j->progress = 0;
        for (int i = 0; i < j->n; i++)
            if (j->sq[i].failed != 0xff)
                j->sq[i].failed = 0;
        j->last_task = 0xfe;
        return next_key(j, ux, uy);
    }
    j->active = 0;
    ovl_msg("Settlers: area finished");
    return 0;
}

static uint16_t pending_key;      /* order put into the keyboard buffer */
static int pending_unit = -1;

/* BIOS keyboard word for an order key */
static uint16_t bios_word(uint16_t key)
{
    if (key == 'r') return 0x1372;
    if (key == 'i') return 0x1769;
    return key;                     /* direction keys are scan << 8 */
}

/* hook before 0402:0645: the active unit is about to wait for orders */
void hook_unit_await(void)
{
    int player = (int16_t)RW(SS, (uint16_t)(BP + 6));
    if (player == (int16_t)GW(0xe710))
        realtime_order_wait();
    int unit = (int16_t)RW(SS, (uint16_t)(BP - 0x32));
    if (player != (int16_t)GW(0xe710) || unit < 0 || unit >= 128)
        return;
    if (pending_key)
        return;
    struct job *j = &jobs[unit];
    if (!j->active)
        return;
    uint16_t u = UNIT(player, unit);
    if (j->player != player || j->type != GB(u + 3)) {
        j->active = 0;          /* the slot now holds another unit */
        return;
    }
    uint16_t k = next_key(j, GB(u + 1), GB(u + 2));
    if (k) {
        pending_key = k;
        pending_unit = unit;
        bios_key_push(bios_word(k));
    }
}

/* hook before 0402:0677 "mov [bp-0x44], ax": AX = key read for the
   active unit (0 = none) */
void hook_unit_order_key(void)
{
    int player = (int16_t)RW(SS, (uint16_t)(BP + 6));
    int unit = (int16_t)RW(SS, (uint16_t)(BP - 0x32));
    if (player != (int16_t)GW(0xe710) || unit < 0 || unit >= 128)
        return;
    if (mapnav_is_marker(AX)) {
        AX = 0;                 /* view changed: just start over */
        realtime_apply_select();
        return;
    }
    uint16_t u = UNIT(player, unit);
    int type = GB(u + 3);
    struct job *j = &jobs[unit];
    if (pending_key) {
        int ours = AX == pending_key && unit == pending_unit;
        pending_key = 0;
        if (ours)
            return;             /* our order: let the game execute it */
    }
    if (AX == 'a' || AX == 'A') {
        if (type == 0) {        /* Settlers */
            start(player, unit, GB(u + 1), GB(u + 2));
            j->type = (uint8_t)type;
            AX = 0;
        }
        return;
    }
    if (j->active && AX != 0) {
        j->active = 0;
        ovl_msg("Settlers: automation cancelled");
    }
}

void autowork_save(void *fp)
{
    FILE *f = fp;
    fwrite(jobs, sizeof jobs, 1, f);
}

void autowork_load(void *fp)
{
    FILE *f = fp;
    if (fread(jobs, sizeof jobs, 1, f) != 1)
        memset(jobs, 0, sizeof jobs);
}
