/* Multiplayer over the network (mp.c, mp_net.c). */
#pragma once
#include <stdint.h>

#define MP_MAX_HOSTS 8
struct mp_host {
    char ip[48];
    char info[128];         /* "name|mapwidth" */
};

/* mp_net.c */
int net_host_start(void);
void net_set_info(const char *info);
void net_scan(void);
int net_hosts(struct mp_host *h, int max);
int net_connect(const char *ip);
int net_connected(void);
int net_send(int type, const void *data, uint32_t len);
int net_recv(int *type, uint8_t **data, uint32_t *len);
void net_poll(void);
void net_close(void);

/* mp.c */
int mp_in_game(void);       /* a multiplayer game is running */
int mp_turn_due(void);      /* the clock says this turn is over */
int mp_progress(void);      /* 0..1000 of the turn used */
int mp_turn_secs10(void);
void mp_note_idle(void);    /* the game waits for orders on the map */
int mp_key(uint16_t key);   /* lobby input */
int mp_overlay_active(void);
void mp_overlay_draw(uint8_t *buf, int W, int H, int ox, int oy,
                     void (*fill)(uint8_t *, int, int, int, int, int, int, uint8_t),
                     void (*text)(uint8_t *, int, int, int, int, const char *, uint8_t),
                     uint8_t white, uint8_t black, uint8_t yellow, uint8_t grey);
