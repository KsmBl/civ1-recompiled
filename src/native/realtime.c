/*
 * Real-time mode (realtime = 1): turns pass on a clock.
 *
 * The game's turn structure stays: every turn each civilization moves its
 * units, cities produce, the year advances.  What changes is who ends the
 * human player's turn: a clock.  While the game waits for orders (the
 * blinking unit, or "End of Turn") the turn time runs; when it is used up
 * the remaining moves of all your units are dropped and the turn ends by
 * itself.  Units with goto orders keep walking every turn, so with short
 * turns fast units visibly move faster than slow ones.
 *
 * Time only runs while the game waits on the map: dialogs, city screens
 * and menus pause it.
 *
 * Keys on the map: [ or -  slower,  ] or =  faster,  F12  pause,
 * Tab  next unit (the game's "wait").
 *
 * Multiplayer later: the turn clock is the one place that decides when a
 * turn ends (realtime_turn_due); a network session would drive it from a
 * host instead of the local timer.
 */
#include "native.h"
#include "runtime.h"
#include "addons.h"
#include "video.h"
#include "mp.h"

#define UNIT(p, u) (0x90f4 + 0x600 * (p) + 12 * (u))
#define UNIT_TYPE 3
#define UNIT_MOVES 4
#define END_OF_TURN 0x80            /* unit number of the End of Turn wait */
#define KEY_ENTER 0x1C0D
#define KEY_WAIT 0x1177             /* 'w': the game's "wait" (next unit) */
#define MARKER_KEY 0xFFFE           /* wakes the order loop (see mapnav.c) */

static const int speeds[] = {12000, 8000, 5000, 3000, 2000, 1500, 1000, 600};
#define NSPEEDS ((int)(sizeof speeds / sizeof speeds[0]))
static int speed = -1;              /* index into speeds */
static int paused;
static uint64_t elapsed_us, last_us;
static int last_turn = -1;
static uint64_t last_push_us;

static int turn_ms(void)
{
    if (speed < 0) {                /* the configured length picks the start */
        speed = 0;
        for (int i = 0; i < NSPEEDS; i++)
            if (speeds[i] >= cfg.turn_ms)
                speed = i;
        if (cfg.turn_ms != speeds[speed])
            return cfg.turn_ms;
    }
    return speeds[speed];
}

int realtime_on(void)
{
    return cfg.realtime || mp_in_game();
}

/* fraction of the turn used, 0..1000, for the clock display */
int realtime_progress(void)
{
    if (mp_in_game())
        return mp_progress();
    uint64_t t = (uint64_t)turn_ms() * 1000;
    return elapsed_us >= t ? 1000 : (int)(elapsed_us * 1000 / t);
}

int realtime_paused(void)
{
    return paused && !mp_in_game();
}

int realtime_turn_secs10(void)
{
    if (mp_in_game())
        return mp_turn_secs10();
    return turn_ms() / 100;
}

static int turn_due(void)
{
    if (mp_in_game())
        return mp_turn_due();       /* the host's clock, never paused */
    return !paused && elapsed_us >= (uint64_t)turn_ms() * 1000;
}

/* called while the game waits for orders on the map (hook_map_idle) */
void realtime_idle(void)
{
    if (!cfg.realtime && !mp_in_game())
        return;
    mp_note_idle();
    uint64_t now = now_us();
    int turn = (int16_t)GW(0x90f2);
    if (turn != last_turn) {
        last_turn = turn;
        elapsed_us = 0;
    } else if (!paused && now - last_us < 2000000) {
        /* only time spent waiting here counts (the blink loop comes by
           every 0.3-0.8 s): a longer gap means a dialog, a city screen
           or the other civilizations' moves */
        elapsed_us += now - last_us;
    }
    last_us = now;
    if (!turn_due())
        return;
    uint16_t pending;
    if (bios_key_peek(&pending)) {
        if (pending == 0x011B)
            bios_key_wait();        /* a multiplayer "close the dialog" Esc */
        else if (now - last_push_us < 1000000)
            return;                 /* the last push is not read yet */
    }
    last_push_us = now;
    int player = (int16_t)GW(0xe710);
    int unit = (int16_t)RW(SS, (uint16_t)(BP + 8));   /* f_0860_0aa8(player, unit) */
    if (unit == END_OF_TURN) {
        while (bios_key_peek(&pending))
            bios_key_wait();        /* keys typed into "End of Turn" are void */
        bios_key_push(KEY_ENTER);
        return;
    }
    /* time is up: no unit has moves left, the order loop runs out */
    for (int u = 0; u < 128; u++)
        if (GB(UNIT(player, u) + UNIT_TYPE) != 0xff)
            WB(DS, (uint16_t)(UNIT(player, u) + UNIT_MOVES), 0);
    bios_key_push(MARKER_KEY);
}

/* hook before 0402:3e31: the human player's units are all done.  The game
   waits at "End of Turn" only with that option; in real time it always
   waits there, for the clock. */
void hook_realtime_end_of_turn(void)
{
    if (cfg.realtime || mp_in_game())
        WW(SS, (uint16_t)(BP - 6), 0);
}

/* ---- selecting a unit by clicking it ----
   The order loop of f_0402_000a works through the units by index
   ([BP-0x32]); clicking one of your units with moves left makes the loop
   continue with that unit.  Its standing orders (sentry, fortify,
   settler work, goto) are cancelled, as it now waits for new ones. */
static int order_unit = -1, order_goto_mode, select_unit = -1;

/* hook_unit_await (0402:0645), BP = order loop frame */
void realtime_order_wait(void)
{
    order_unit = (int16_t)RW(SS, (uint16_t)(BP - 0x32));
    order_goto_mode = RW(SS, (uint16_t)(BP - 0x36)) != 0;
}

int realtime_click(int x, int y)
{
    if (!realtime_on() || !video_is_map() || x < 80 || y < 8 || order_goto_mode ||
        order_unit < 0 || order_unit >= 128)
        return 0;
    int sx = ((x - 80) / 16 + VIEW_X) % MAP_W, sy = (y - 8) / 16 + VIEW_Y;
    int player = (int16_t)GW(0xe710);
    for (int u = 0; u < 128; u++) {
        uint16_t r = UNIT(player, u);
        if (u == order_unit || GB(r + UNIT_TYPE) == 0xff || GB(r + 1) != sx ||
            GB(r + 2) != sy || GB(r + UNIT_MOVES) == 0)
            continue;
        select_unit = u;
        bios_key_push(MARKER_KEY);
        return 1;
    }
    return 0;
}

/* hook_unit_order_key with the marker key, BP = order loop frame */
void realtime_apply_select(void)
{
    int u = select_unit;
    select_unit = -1;
    if (u < 0)
        return;
    uint16_t r = UNIT((int16_t)GW(0xe710), u);
    WB(DS, r, GB(r) & ~0xcf);       /* sentry, fortify, settler work */
    WB(DS, (uint16_t)(r + 6), 0xff); /* goto */
    WW(SS, (uint16_t)(BP - 0x32), (uint16_t)u);
}

int realtime_key(uint16_t key)
{
    if (!realtime_on() || !video_is_map())
        return 0;
    uint8_t ascii = (uint8_t)key, scan = (uint8_t)(key >> 8);
    if (mp_in_game() && scan != 0x0F)
        return 0;                   /* the host sets the pace */
    if (ascii == '[' || ascii == '-' || ascii == ']' || ascii == '=') {
        turn_ms();
        int faster = ascii == ']' || ascii == '=';
        speed += faster ? 1 : -1;
        if (speed < 0)
            speed = 0;
        if (speed >= NSPEEDS)
            speed = NSPEEDS - 1;
        ovl_msg("Turn: %d.%d seconds", speeds[speed] / 1000, speeds[speed] % 1000 / 100);
        return 1;
    }
    if (scan == 0x58 && ascii == 0) {   /* F12 */
        paused = !paused;
        ovl_msg(paused ? "Paused (F12)" : "Running");
        return 1;
    }
    if (scan == 0x0F) {                 /* Tab: next unit */
        bios_key_push(KEY_WAIT);
        return 1;
    }
    return 0;
}
