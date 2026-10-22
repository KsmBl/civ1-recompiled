/* Declarations shared by the runtime (DOS/BIOS/hardware emulation). */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "cpu.h"

#define BIOS_SEG 0xF000
#define PSPSEG ((uint16_t)(LOADSEG - 0x10))
#define ENVSEG 0x07F1

#if defined(__GNUC__)
#define PRINTF_LIKE(a, b) __attribute__((format(printf, a, b)))
#else
#define PRINTF_LIKE(a, b)
#endif

void fatal(const char *fmt, ...) PRINTF_LIKE(1, 2) NORETURN;
void warn(const char *fmt, ...) PRINTF_LIKE(1, 2);
void logmsg(const char *fmt, ...) PRINTF_LIKE(1, 2);
extern int verbose;

/* loader */
struct overlay {
    uint8_t *image;
    uint32_t size;
    int nrel;
    uint16_t *rel;
};
extern struct overlay overlays[32];
void load_program(const char *exe_path, const char *cmdline);
extern uint16_t entry_cs, entry_ip, entry_ss, entry_sp;

/* dispatch */
void dispatch_init(void);
typedef void (*func_t)(void);
func_t lookup_code(uint16_t seg, uint16_t off);
void raise_irq_vector(uint8_t n);

/* interpreter / testing */
void interp_exec(uint16_t seg, uint16_t off);
extern int interp_ref_mode;
extern long interp_insns;
extern int deterministic;
void trace_event(const char *what, int n);
long trace_count(void);

/* bios / dos */
void bios_init(void);
void bios_int(uint8_t n);
uint16_t bios_key_wait(void);
void bios_key_push(uint16_t w);
int bios_key_peek(uint16_t *w);
void dos_int21(void);
void dos_redirect_buffer(uint16_t off, uint16_t seg);
void dos_init(void);
void mouse_int33(void);
void mouse_event_motion(int dx, int dy, int abs_x, int abs_y);
void mouse_event_button(int button, int down);
void mouse_poll_callback(void);
int mouse_cursor_visible(void);
void mouse_cursor_pos(int *x, int *y);
const uint16_t *mouse_cursor_shape(int *hx, int *hy);
int dos_resolve_path(const char *dos, char *host, size_t n, int must_exist);
void dos_load_save_sidecar(void);
void native_frame(void);          /* src/native/mp.c, every frame */
int load_module_mz(const char *host_path, const char *dos_name,
                   uint16_t load_seg, uint16_t reloc);

/* hardware */
void hw_init(void);
void hlt_wait(void);
uint64_t now_us(void);
void kbd_scancode(uint8_t sc);       /* host -> emulated keyboard */
extern uint8_t vga_dac[256][3];
extern int video_mode;
extern int quit_requested;
extern int irq_hold;
void timer_set_divisor(uint16_t div);

/* test / automation scripting */
void script_load(const char *path);
void script_tick(void);
void screenshot_png(const char *path);
int key_by_name(const char *name, int *ext);

/* platform (SDL / DOS) */
int plat_init(void);
void plat_shutdown(void);
void plat_present(void);
void plat_pump_events(void);
void plat_sleep_us(int us);
uint64_t plat_time_us(void);
void plat_audio_lock(int lock);

/* configuration */
struct config {
    int win_w, win_h;       /* window size */
    int fullscreen;
    int widescreen;         /* render a wider game view */
    int scale_filter;       /* 0 nearest, 1 linear */
    int aspect;             /* 1 = 4:3 correct pixel aspect, 0 = square */
    int integer_scale;      /* only integer scale factors */
    int zoom;               /* widescreen pixel size, 0 = auto */
    int autostart;          /* answer the text mode setup questions */
    int sound;              /* 1 none, 2 PC speaker, 3 Tandy, 4 AdLib, 5 MT-32 */
    int mouse;              /* 1 = mouse and keyboard */
    int realtime;           /* turns pass on a clock (src/native/realtime.c) */
    int turn_ms;            /* realtime: length of a turn */
    char gamedir[1024];
};
extern struct config cfg;
