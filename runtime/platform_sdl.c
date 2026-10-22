/*
 * SDL2 platform layer: window, rendering of the emulated VGA frame buffer,
 * keyboard / mouse input and audio output.
 */
#include <SDL.h>
#include <stdio.h>
#include "cpu.h"
#include "runtime.h"
#include "audio.h"
#include "video.h"
#include "addons.h"

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex, *upscaled;
static int tex_w, tex_h, up_w, up_h;
static SDL_AudioDeviceID audio_dev;
static SDL_Rect dst;
static uint32_t *pixels;

static void audio_cb(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    audio_render((int16_t *)stream, len / 2);
}

int plat_init(void)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS | SDL_INIT_TIMER) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return -1;
    }
    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (cfg.fullscreen)
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    win = SDL_CreateWindow("Civilization", SDL_WINDOWPOS_CENTERED,
                           SDL_WINDOWPOS_CENTERED, cfg.win_w, cfg.win_h, flags);
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return -1;
    }
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC |
                             SDL_RENDERER_TARGETTEXTURE);
    if (!ren)
        ren = SDL_CreateRenderer(win, -1, 0);
    SDL_ShowCursor(SDL_DISABLE);

    SDL_AudioSpec want = {0}, have;
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 512;
    want.callback = audio_cb;
    audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (audio_dev) {
        audio_init(have.freq);
        SDL_PauseAudioDevice(audio_dev, 0);
    }
    return 0;
}

void plat_shutdown(void)
{
    if (audio_dev)
        SDL_CloseAudioDevice(audio_dev);
    if (win)
        SDL_DestroyWindow(win);
    win = NULL;
    SDL_Quit();
}

void plat_audio_lock(int lock)
{
    if (!audio_dev)
        return;
    if (lock)
        SDL_LockAudioDevice(audio_dev);
    else
        SDL_UnlockAudioDevice(audio_dev);
}

uint64_t plat_time_us(void)
{
    static uint64_t freq;
    if (!freq)
        freq = SDL_GetPerformanceFrequency();
    return SDL_GetPerformanceCounter() * 1000000ULL / freq;
}

void plat_sleep_us(int us)
{
    SDL_Delay(us >= 1000 ? (Uint32)(us / 1000) : 1);
}

/* ------------------------------------------------------------------ */

static void ensure_textures(int w, int h, int ow, int oh)
{
    if (!tex || tex_w != w || tex_h != h) {
        if (tex)
            SDL_DestroyTexture(tex);
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STREAMING, w, h);
        SDL_SetTextureScaleMode(tex, SDL_ScaleModeNearest);
        tex_w = w;
        tex_h = h;
        free(pixels);
        pixels = malloc((size_t)w * h * 4);
    }
    /* integer pre-scale for "sharp bilinear" output */
    int sx = ow / w, sy = oh / h;
    if (sx < 1) sx = 1;
    if (sy < 1) sy = 1;
    int uw = w * sx, uh = h * sy;
    if (!upscaled || up_w != uw || up_h != uh) {
        if (upscaled)
            SDL_DestroyTexture(upscaled);
        upscaled = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                     SDL_TEXTUREACCESS_TARGET, uw, uh);
        SDL_SetTextureScaleMode(upscaled, cfg.scale_filter ? SDL_ScaleModeLinear
                                                           : SDL_ScaleModeNearest);
        up_w = uw;
        up_h = uh;
    }
}

static void draw_cursor(int w, int h)
{
    if (!mouse_cursor_visible())
        return;
    int hx, hy, x, y;
    const uint16_t *shape = mouse_cursor_shape(&hx, &hy);
    mouse_cursor_pos(&x, &y);
    x = video_map_mouse_x(x);
    y = video_map_mouse_y(y);
    x -= hx;
    y -= hy;
    for (int r = 0; r < 16; r++) {
        int py = y + r;
        if (py < 0 || py >= h)
            continue;
        uint16_t scr = shape ? shape[r] : 0x3FFF >> (r > 13 ? 16 : 0);
        uint16_t cur = shape ? shape[16 + r] : 0;
        if (!shape) {
            /* default arrow */
            static const uint16_t arrow_s[16] = {0x3FFF, 0x1FFF, 0x0FFF, 0x07FF,
                0x03FF, 0x01FF, 0x00FF, 0x007F, 0x003F, 0x001F, 0x01FF, 0x10FF,
                0x30FF, 0xF87F, 0xF87F, 0xFC7F};
            static const uint16_t arrow_c[16] = {0x0000, 0x4000, 0x6000, 0x7000,
                0x7800, 0x7C00, 0x7E00, 0x7F00, 0x7F80, 0x7C00, 0x6C00, 0x4600,
                0x0600, 0x0300, 0x0300, 0x0000};
            scr = arrow_s[r];
            cur = arrow_c[r];
        }
        for (int c = 0; c < 16; c++) {
            int px = x + c;
            if (px < 0 || px >= w)
                continue;
            int sbit = (scr >> (15 - c)) & 1, cbit = (cur >> (15 - c)) & 1;
            uint32_t *p = &pixels[py * w + px];
            if (!sbit)
                *p = 0xFF000000;
            if (cbit)
                *p ^= 0x00FFFFFF;
        }
    }
}

void plat_present(void)
{
    if (!win)
        return;
    int w, h;
    const uint8_t *fb = video_framebuffer(&w, &h);
    int ow, oh;
    SDL_GetRendererOutputSize(ren, &ow, &oh);

    /* destination rectangle: keep the original 4:3 display aspect for
       320x200 (pixels 1.2x taller than wide) unless square pixels are
       requested; never stretch to fill. */
    double par = cfg.aspect ? video_pixel_aspect() : 1.0;
    double disp_w = w, disp_h = h * par;
    double scale = (double)ow / disp_w;
    if (disp_h * scale > oh)
        scale = (double)oh / disp_h;
    if (cfg.integer_scale && !cfg.aspect) {
        int is = (int)scale;
        if (is >= 1)
            scale = is;
    }
    dst.w = (int)(disp_w * scale + 0.5);
    dst.h = (int)(disp_h * scale + 0.5);
    dst.x = (ow - dst.w) / 2;
    dst.y = (oh - dst.h) / 2;
    ensure_textures(w, h, dst.w, dst.h);

    uint32_t pal[256];
    for (int i = 0; i < 256; i++) {
        uint32_t r = vga_dac[i][0], g = vga_dac[i][1], b = vga_dac[i][2];
        r = (r << 2) | (r >> 4);
        g = (g << 2) | (g >> 4);
        b = (b << 2) | (b >> 4);
        pal[i] = 0xFF000000u | r << 16 | g << 8 | b;
    }
    if (fb) {
        for (int i = 0; i < w * h; i++)
            pixels[i] = pal[fb[i]];
    } else {
        memset(pixels, 0, (size_t)w * h * 4);
    }
    draw_cursor(w, h);
    SDL_UpdateTexture(tex, NULL, pixels, w * 4);
    SDL_SetRenderTarget(ren, upscaled);
    SDL_RenderCopy(ren, tex, NULL, NULL);
    SDL_SetRenderTarget(ren, NULL);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, upscaled, NULL, &dst);
    SDL_RenderPresent(ren);
}

/* ------------------------------------------------------------------ */
/* input */

static int xt_scancode(SDL_Scancode s, int *ext)
{
    *ext = 0;
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) {
        static const uint8_t letters[26] = {0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21,
            0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10,
            0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C};
        return letters[s - SDL_SCANCODE_A];
    }
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_9)
        return 0x02 + (s - SDL_SCANCODE_1);
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F10)
        return 0x3B + (s - SDL_SCANCODE_F1);
    switch (s) {
    case SDL_SCANCODE_0: return 0x0B;
    case SDL_SCANCODE_ESCAPE: return 0x01;
    case SDL_SCANCODE_MINUS: return 0x0C;
    case SDL_SCANCODE_EQUALS: return 0x0D;
    case SDL_SCANCODE_BACKSPACE: return 0x0E;
    case SDL_SCANCODE_TAB: return 0x0F;
    case SDL_SCANCODE_LEFTBRACKET: return 0x1A;
    case SDL_SCANCODE_RIGHTBRACKET: return 0x1B;
    case SDL_SCANCODE_RETURN: return 0x1C;
    case SDL_SCANCODE_LCTRL: return 0x1D;
    case SDL_SCANCODE_SEMICOLON: return 0x27;
    case SDL_SCANCODE_APOSTROPHE: return 0x28;
    case SDL_SCANCODE_GRAVE: return 0x29;
    case SDL_SCANCODE_LSHIFT: return 0x2A;
    case SDL_SCANCODE_BACKSLASH: return 0x2B;
    case SDL_SCANCODE_COMMA: return 0x33;
    case SDL_SCANCODE_PERIOD: return 0x34;
    case SDL_SCANCODE_SLASH: return 0x35;
    case SDL_SCANCODE_RSHIFT: return 0x36;
    case SDL_SCANCODE_KP_MULTIPLY: return 0x37;
    case SDL_SCANCODE_LALT: return 0x38;
    case SDL_SCANCODE_SPACE: return 0x39;
    case SDL_SCANCODE_CAPSLOCK: return 0x3A;
    case SDL_SCANCODE_NUMLOCKCLEAR: return 0x45;
    case SDL_SCANCODE_SCROLLLOCK: return 0x46;
    case SDL_SCANCODE_KP_7: return 0x47;
    case SDL_SCANCODE_KP_8: return 0x48;
    case SDL_SCANCODE_KP_9: return 0x49;
    case SDL_SCANCODE_KP_MINUS: return 0x4A;
    case SDL_SCANCODE_KP_4: return 0x4B;
    case SDL_SCANCODE_KP_5: return 0x4C;
    case SDL_SCANCODE_KP_6: return 0x4D;
    case SDL_SCANCODE_KP_PLUS: return 0x4E;
    case SDL_SCANCODE_KP_1: return 0x4F;
    case SDL_SCANCODE_KP_2: return 0x50;
    case SDL_SCANCODE_KP_3: return 0x51;
    case SDL_SCANCODE_KP_0: return 0x52;
    case SDL_SCANCODE_KP_PERIOD: return 0x53;
    case SDL_SCANCODE_F11: return 0x57;
    case SDL_SCANCODE_F12: return 0x58;
    case SDL_SCANCODE_RCTRL: *ext = 1; return 0x1D;
    case SDL_SCANCODE_RALT: *ext = 1; return 0x38;
    case SDL_SCANCODE_KP_ENTER: *ext = 1; return 0x1C;
    case SDL_SCANCODE_KP_DIVIDE: *ext = 1; return 0x35;
    case SDL_SCANCODE_HOME: *ext = 1; return 0x47;
    case SDL_SCANCODE_UP: *ext = 1; return 0x48;
    case SDL_SCANCODE_PAGEUP: *ext = 1; return 0x49;
    case SDL_SCANCODE_LEFT: *ext = 1; return 0x4B;
    case SDL_SCANCODE_RIGHT: *ext = 1; return 0x4D;
    case SDL_SCANCODE_END: *ext = 1; return 0x4F;
    case SDL_SCANCODE_DOWN: *ext = 1; return 0x50;
    case SDL_SCANCODE_PAGEDOWN: *ext = 1; return 0x51;
    case SDL_SCANCODE_INSERT: *ext = 1; return 0x52;
    case SDL_SCANCODE_DELETE: *ext = 1; return 0x53;
    default: return 0;
    }
}

static void window_to_game(int wx, int wy, int *gx, int *gy)
{
    int w, h;
    video_framebuffer(&w, &h);
    /* convert window (logical) coordinates to renderer output pixels */
    int ww, wh, ow, oh;
    SDL_GetWindowSize(win, &ww, &wh);
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    double fx = ww ? (double)ow / ww : 1, fy = wh ? (double)oh / wh : 1;
    double px = wx * fx, py = wy * fy;
    int x = dst.w ? (int)((px - dst.x) * w / dst.w) : 0;
    int y = dst.h ? (int)((py - dst.y) * h / dst.h) : 0;
    if (x < 0) x = 0;
    if (x >= w) x = w - 1;
    if (y < 0) y = 0;
    if (y >= h) y = h - 1;
    *gx = x;
    *gy = y;
}

void plat_pump_events(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            quit_requested = 1;
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_RETURN &&
                (e.key.keysym.mod & KMOD_ALT)) {
                cfg.fullscreen = !cfg.fullscreen;
                SDL_SetWindowFullscreen(win, cfg.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                break;
            }
            int ext;
            int sc = xt_scancode(e.key.keysym.scancode, &ext);
            if (!sc)
                break;
            if (e.type == SDL_KEYDOWN && e.key.repeat && (sc == 0x2A || sc == 0x36 ||
                                                          sc == 0x1D || sc == 0x38))
                break;
            if (ext)
                kbd_scancode(0xE0);
            kbd_scancode((uint8_t)(sc | (e.type == SDL_KEYUP ? 0x80 : 0)));
            break;
        }
        case SDL_MOUSEMOTION: {
            int gx, gy;
            window_to_game(e.motion.x, e.motion.y, &gx, &gy);
            gx = video_unmap_mouse_x(gx);
            gy = video_unmap_mouse_y(gy);
            mouse_event_motion(e.motion.xrel, e.motion.yrel, gx * 2, gy);
            break;
        }
        case SDL_MOUSEWHEEL:
            if (e.wheel.y)
                mapnav_wheel(e.wheel.y > 0 ? 1 : -1);
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            int b = e.button.button == SDL_BUTTON_LEFT ? 0 :
                    e.button.button == SDL_BUTTON_RIGHT ? 1 :
                    e.button.button == SDL_BUTTON_MIDDLE ? 2 : -1;
            int gx, gy;
            window_to_game(e.button.x, e.button.y, &gx, &gy);
            gx = video_unmap_mouse_x(gx);
            gy = video_unmap_mouse_y(gy);
            mouse_event_motion(0, 0, gx * 2, gy);
            mouse_event_button(b, e.type == SDL_MOUSEBUTTONDOWN);
            break;
        }
        default:
            break;
        }
    }
}
