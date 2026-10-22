/* Features added to the game (src/native). */
#pragma once
#include <stdint.h>

/* city screen "GOLD" button (game coordinates, 320x200 city screen) */
#define GOLD_BTN_X 238
#define GOLD_BTN_Y 189
#define GOLD_BTN_W 44
#define GOLD_BTN_H 9

int city_screen_city(void);          /* city shown on the city screen or -1 */
int city_produces_gold(int city);
void city_set_gold(int city, int on);
void ovl_msg(const char *fmt, ...);
int native_overlay_active(void);
void native_overlay_draw(uint8_t *buf, int W, int H, int ox, int oy);

/* input filters: return 1 if the event was used by a feature */
int native_key_filter(uint16_t key);           /* BIOS key word scan<<8|ascii */
int native_mouse_filter(int button, int x, int y);

/* real-time mode (realtime.c) */
int realtime_on(void);
void realtime_idle(void);
int realtime_key(uint16_t key);
int realtime_progress(void);         /* 0..1000 of the turn used */
int realtime_paused(void);
int realtime_turn_secs10(void);      /* turn length in 1/10 s */
void realtime_order_wait(void);
int realtime_click(int x, int y);
void realtime_apply_select(void);

/* map navigation (mapnav.c) */
int mapnav_button(int b, int down, int x, int y);
void mapnav_motion(int x, int y);
void mapnav_wheel(int steps);
int mapnav_is_marker(uint16_t key);
void mouse_deliver_button(int b, int down);
void mouse_deliver_click(int b);     /* press now, release a moment later */
void mouse_screen_resized(void);

void native_state_dump(void);
void native_state_dump_to(const char *path);

/* save games: extra state stored in CIVILn.NAT next to CIVILn.SVE */
/* f is a FILE * (kept opaque so this header needs no system headers) */
void native_save_state(void *f);
void native_load_state(void *f);
void autowork_save(void *f);
void autowork_load(void *f);
