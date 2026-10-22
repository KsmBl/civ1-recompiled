/*
 * "Produce gold": a city can turn its production into money instead of
 * building something.  Chosen as an entry of the production list, or
 * toggled on the city screen with the GOLD button or the G key.  Each turn the shields the city would add to its production
 * box go to the owner's treasury; the box does not fill, so nothing is
 * completed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "native.h"
#include "runtime.h"
#include "addons.h"
#include "mp.h"

#define CITY(c) (0x706c + 0x1c * (c))     /* 28 byte city records */
#define CITY_X 4
#define CITY_Y 5
#define CITY_OWNER 11
#define GOLD(p) (0xc0f6 + 2 * (p))        /* treasury of each player */

static struct { uint8_t on, x, y, owner; } cap[128];
static int screen_city = -1;

int city_screen_city(void)
{
    return screen_city;
}

int city_produces_gold(int c)
{
    if (c < 0 || c >= 128 || !cap[c].on)
        return 0;
    uint16_t r = CITY(c);
    /* the flag belongs to the city that was there when it was set */
    return cap[c].x == GB(r + CITY_X) && cap[c].y == GB(r + CITY_Y) &&
           cap[c].owner == GB(r + CITY_OWNER);
}

static void set_gold(int c, int on)
{
    uint16_t r = CITY(c);
    cap[c].on = (uint8_t)on;
    cap[c].x = GB(r + CITY_X);
    cap[c].y = GB(r + CITY_Y);
    cap[c].owner = GB(r + CITY_OWNER);
    ovl_msg(on ? "Production goes to the treasury" : "City builds again");
}

/* without a message (multiplayer: the other player's settings) */
void city_set_gold(int c, int on)
{
    if (c < 0 || c >= 128 || city_produces_gold(c) == !!on)
        return;
    uint16_t r = CITY(c);
    cap[c].on = (uint8_t)!!on;
    cap[c].x = GB(r + CITY_X);
    cap[c].y = GB(r + CITY_Y);
    cap[c].owner = GB(r + CITY_OWNER);
}

static void toggle(int c)
{
    set_gold(c, !city_produces_gold(c));
}

/* ---- "Gold" in the "What shall we build" list (f_0ad5_041c) ----
   The list is text at DS:C926 ("title?\n item (...)\n item ...") plus the
   item ids in a word array at BP-0xDE (entry 0 = current build, entry i =
   line i), count+1 at BP-0xA.  The choice comes back as an index. */
#define MENU_TEXT 0xc926
#define GOLD_ITEM 0x70                  /* unused item id */
static int menu_city = -1;

void hook_build_menu(void)              /* 0ad5:1433, human player's list */
{
    int city = (int16_t)RW(SS, (uint16_t)(BP + 8));
    menu_city = -1;
    if (getenv("CIV_ORIGINAL") || city < 0 || city >= 128)
        return;
    int len = 0, title = -1;
    while (len < 2000 && GB(MENU_TEXT + len)) {
        if (title < 0 && GB(MENU_TEXT + len) == '\n')
            title = len + 2;            /* after "?\n " */
        len++;
    }
    const char *line = city_produces_gold(city) ? "GOLD (now): production to treasury\n "
                                                : "Gold: production to treasury\n ";
    int n = (int)strlen(line);
    int count = (int16_t)RW(SS, (uint16_t)(BP - 0xa));
    if (title < 0 || title > len || len + n > 1400 || count < 1 || count > 60)
        return;
    for (int i = len; i >= title; i--)  /* includes the terminating 0 */
        WB(DS, (uint16_t)(MENU_TEXT + i + n), GB(MENU_TEXT + i));
    for (int i = 0; i < n; i++)
        WB(DS, (uint16_t)(MENU_TEXT + title + i), (uint8_t)line[i]);
    uint16_t list = (uint16_t)(BP - 0xde);
    for (int i = count - 1; i >= 1; i--)
        WW(SS, (uint16_t)(list + 2 * (i + 1)), RW(SS, (uint16_t)(list + 2 * i)));
    WW(SS, (uint16_t)(list + 2), GOLD_ITEM);
    WW(SS, (uint16_t)(BP - 0xa), (uint16_t)(count + 1));
    menu_city = city;
}

void hook_build_choice(void)            /* 0ad5:1459, AX = chosen item */
{
    int city = menu_city;
    menu_city = -1;
    if (city < 0)
        return;
    uint16_t item = AX;
    if (item == GOLD_ITEM) {
        if (!city_produces_gold(city))
            set_gold(city, 1);
        /* keep building what was set before: the box does not fill anyway */
        AX = RW(SS, (uint16_t)(BP - 0xde));
        WW(SS, (uint16_t)(BP - 0x134), AX);
    } else if (item != 0x62 && item != 0x63 && SI != 0xfffe && city_produces_gold(city))
        set_gold(city, 0);              /* a real build was chosen */
}

/* f_0d07_0043(city, mode): city turn processing (mode 0) and the city
   screen (mode 1).  Wrapped to know which city is on screen. */
void f_0d07_0043(void)
{
    uint16_t city = FARG(0), lo = FARG(1), hi = FARG(2);
    int prev = screen_city;
    if (lo == 1 && hi == 0)
        screen_city = city;
    lifted_f_0d07_0043();       /* in the caller's frame, returns for it */
    screen_city = prev;
}

/* hook before 0d07:1520 "add [city.shields], ax" */
void hook_city_production(void)
{
    int city = (int16_t)RW(SS, (uint16_t)(BP + 6));
    if (RW(SS, (uint16_t)(BP + 8)) != 0 || !city_produces_gold(city))
        return;
    int16_t amount = (int16_t)AX;
    if (amount <= 0)
        return;
    int owner = GB(CITY(city) + CITY_OWNER) & 7;
    int32_t g = (int16_t)GW(GOLD(owner)) + amount;
    if (g > 30000)
        g = 30000;
    WW(DS, GOLD(owner), (uint16_t)g);
    AX = 0;
}

int native_key_filter(uint16_t key)
{
    uint8_t ascii = (uint8_t)key;
    if (mp_key(key))
        return 1;
    if (screen_city < 0 && realtime_key(key))
        return 1;
    if (screen_city >= 0 && (ascii == 'g' || ascii == 'G')) {
        toggle(screen_city);
        return 1;
    }
    return 0;
}

int native_mouse_filter(int button, int x, int y)
{
    if (screen_city < 0 && button == 0 && realtime_click(x, y))
        return 1;
    if (screen_city < 0 || button != 0)
        return 0;
    if (x >= GOLD_BTN_X && x < GOLD_BTN_X + GOLD_BTN_W &&
        y >= GOLD_BTN_Y && y < GOLD_BTN_Y + GOLD_BTN_H) {
        toggle(screen_city);
        return 1;
    }
    return 0;
}

void native_save_state(void *fp)
{
    FILE *f = fp;
    fwrite("CIVN1", 1, 5, f);
    fwrite(cap, sizeof cap, 1, f);
    autowork_save(f);
}

void native_load_state(void *fp)
{
    FILE *f = fp;
    char tag[5];
    memset(cap, 0, sizeof cap);
    if (!f || fread(tag, 1, 5, f) != 5 || memcmp(tag, "CIVN1", 5))
        return;
    if (fread(cap, sizeof cap, 1, f) != 1)
        memset(cap, 0, sizeof cap);
    autowork_load(f);
}
