/*
 * Multiplayer networking: host discovery by UDP broadcast on the LAN and
 * one TCP connection between host and player carrying framed messages
 * (4 byte little endian length, 1 byte type, payload).  Everything is
 * non-blocking and polled from the game loop.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mp.h"

#ifdef __DJGPP__
/* no networking in the DOS build */
int net_host_start(void) { return -1; }
void net_scan(void) {}
int net_hosts(struct mp_host *h, int max) { (void)h; (void)max; return 0; }
int net_connect(const char *ip) { (void)ip; return -1; }
int net_connected(void) { return 0; }
int net_send(int type, const void *data, uint32_t len) { (void)type; (void)data; (void)len; return -1; }
int net_recv(int *type, uint8_t **data, uint32_t *len) { (void)type; (void)data; (void)len; return 0; }
void net_poll(void) {}
void net_close(void) {}
void net_set_info(const char *info) { (void)info; }
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#define TCP_PORT 28400
#define UDP_PORT 28401
#define QUERY "CIVMP1?"
#define ANSWER "CIVMP1!"

static int listen_fd = -1, udp_fd = -1, conn_fd = -1, scan_fd = -1;
static char host_info[128];

static uint8_t *rbuf;
static uint32_t rlen, rcap;
static uint8_t *wbuf;
static uint32_t wlen, wcap;

static struct mp_host found[MP_MAX_HOSTS];
static int nfound;

static void nonblock(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

void net_set_info(const char *info)
{
    snprintf(host_info, sizeof host_info, "%s", info);
}

int net_host_start(void)
{
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    int one = 1;
    if (listen_fd < 0) {
        listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        a.sin_port = htons(TCP_PORT);
        if (bind(listen_fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(listen_fd, 1) < 0) {
            close(listen_fd);
            listen_fd = -1;
            return -1;
        }
        nonblock(listen_fd);
    }
    if (udp_fd < 0) {
        udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
        setsockopt(udp_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        a.sin_port = htons(UDP_PORT);
        if (bind(udp_fd, (struct sockaddr *)&a, sizeof a) < 0) {
            close(udp_fd);
            udp_fd = -1;
        } else
            nonblock(udp_fd);
    }
    return 0;
}

void net_scan(void)
{
    if (scan_fd < 0) {
        scan_fd = socket(AF_INET, SOCK_DGRAM, 0);
        int one = 1;
        setsockopt(scan_fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
        nonblock(scan_fd);
        nfound = 0;
    }
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(UDP_PORT);
    a.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(scan_fd, QUERY, strlen(QUERY), 0, (struct sockaddr *)&a, sizeof a);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);     /* a host on this PC */
    sendto(scan_fd, QUERY, strlen(QUERY), 0, (struct sockaddr *)&a, sizeof a);
}

int net_hosts(struct mp_host *h, int max)
{
    int n = nfound < max ? nfound : max;
    memcpy(h, found, sizeof *h * (size_t)n);
    return n;
}

int net_connect(const char *ip)
{
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(TCP_PORT);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1)
        return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        close(fd);
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    nonblock(fd);
    conn_fd = fd;
    rlen = wlen = 0;
    return 0;
}

int net_connected(void)
{
    return conn_fd >= 0;
}

static void drop(void)
{
    if (conn_fd >= 0)
        close(conn_fd);
    conn_fd = -1;
    rlen = wlen = 0;
}

void net_close(void)
{
    drop();
    if (listen_fd >= 0)
        close(listen_fd);
    if (udp_fd >= 0)
        close(udp_fd);
    if (scan_fd >= 0)
        close(scan_fd);
    listen_fd = udp_fd = scan_fd = -1;
}

static void flush(void)
{
    while (conn_fd >= 0 && wlen) {
        ssize_t n = send(conn_fd, wbuf, wlen, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                drop();
            return;
        }
        memmove(wbuf, wbuf + n, wlen - (uint32_t)n);
        wlen -= (uint32_t)n;
    }
}

int net_send(int type, const void *data, uint32_t len)
{
    if (conn_fd < 0)
        return -1;
    if (wlen + len + 5 > wcap) {
        wcap = (wlen + len + 5) * 2;
        wbuf = realloc(wbuf, wcap);
    }
    uint32_t l = len + 1;
    uint8_t *p = wbuf + wlen;
    p[0] = (uint8_t)l;
    p[1] = (uint8_t)(l >> 8);
    p[2] = (uint8_t)(l >> 16);
    p[3] = (uint8_t)(l >> 24);
    p[4] = (uint8_t)type;
    memcpy(p + 5, data, len);
    wlen += len + 5;
    flush();
    return 0;
}

void net_poll(void)
{
    if (listen_fd >= 0 && conn_fd < 0) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd >= 0) {
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            nonblock(fd);
            conn_fd = fd;
            rlen = wlen = 0;
        }
    }
    if (udp_fd >= 0) {                  /* answer scans */
        char b[64];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        ssize_t n;
        while ((n = recvfrom(udp_fd, b, sizeof b - 1, 0, (struct sockaddr *)&from, &fl)) > 0) {
            b[n] = 0;
            if (!strcmp(b, QUERY)) {
                char ans[160];
                snprintf(ans, sizeof ans, "%s%s", ANSWER, host_info);
                sendto(udp_fd, ans, strlen(ans), 0, (struct sockaddr *)&from, fl);
            }
            fl = sizeof from;
        }
    }
    if (scan_fd >= 0) {                 /* collect answers */
        char b[200];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        ssize_t n;
        while ((n = recvfrom(scan_fd, b, sizeof b - 1, 0, (struct sockaddr *)&from, &fl)) > 0) {
            b[n] = 0;
            fl = sizeof from;
            if (strncmp(b, ANSWER, strlen(ANSWER)))
                continue;
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
            int i;
            for (i = 0; i < nfound; i++)
                if (!strcmp(found[i].ip, ip))
                    break;
            if (i == nfound) {
                if (nfound == MP_MAX_HOSTS)
                    continue;
                nfound++;
            }
            snprintf(found[i].ip, sizeof found[i].ip, "%s", ip);
            snprintf(found[i].info, sizeof found[i].info, "%s", b + strlen(ANSWER));
        }
    }
    flush();
    while (conn_fd >= 0) {
        if (rlen + 65536 > rcap) {
            rcap = (rlen + 65536) * 2;
            rbuf = realloc(rbuf, rcap);
        }
        ssize_t n = recv(conn_fd, rbuf + rlen, rcap - rlen, 0);
        if (n == 0) {
            drop();
            break;
        }
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                drop();
            break;
        }
        rlen += (uint32_t)n;
    }
}

/* next complete message; *data stays valid until the next call */
int net_recv(int *type, uint8_t **data, uint32_t *len)
{
    static uint8_t *msg;
    static uint32_t mcap;
    if (rlen < 4)
        return 0;
    uint32_t l = rbuf[0] | rbuf[1] << 8 | rbuf[2] << 16 | (uint32_t)rbuf[3] << 24;
    if (l < 1 || rlen < 4 + l)
        return 0;
    if (l > mcap) {
        mcap = l;
        msg = realloc(msg, mcap);
    }
    memcpy(msg, rbuf + 4, l);
    memmove(rbuf, rbuf + 4 + l, rlen - 4 - l);
    rlen -= 4 + l;
    *type = msg[0];
    *data = msg + 1;
    *len = l - 1;
    return 1;
}
#endif
