/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The window and the picture in it, through SDL2. See pc_window.h for what the two shapes do.
 *
 * WHICH RENDERER. The one the configuration names goes first, then the rest of try_order below, in that order. Each try gets a new window, since an OpenGL window has to be made as one. A renderer that
 * will not start leaves a line for the console and the freeze log, and the game goes on with the next. The window stays hidden until one has started, so a failed try shows nothing.
 *
 * OpenGL leads because of a freeze measured on a laptop with Intel graphics beside an NVIDIA GPU that sleeps. Each time the NVIDIA GPU wakes, Intel's Direct3D 9 and 11 drivers hold the present for
 * 0.3 to 3 seconds, inside a call that opens the graphics adapter. The game is not at fault and Windows wakes that GPU every few minutes on that PC. In the one shown OpenGL run so far, six wakes held
 * up no present. That is a small sample. A hidden window cannot meet this freeze on either renderer, because the driver makes that call during play only for a window that is shown, so hidden runs do
 * not settle it. In an hour of a hidden OpenGL window one present took about a second at a wake, in another Windows call, so OpenGL is not proven free of every such hold.
 * Direct3D 9 comes next because it is the renderer that has been played the most. Software is the last resort: it cannot hold the game's pace in a large window.
 *
 * LOOKS (pc_filter.h). off is drawn here as it always was. sharp is drawn here too, through SDL on any renderer: the picture made larger by whole numbers with no filter, then brought to the
 * window's height with a linear filter, as the 4:3 shape already does. The shader looks go to pc_filter.c, which draws in the OpenGL renderer's own context after SDL has cleared the
 * window. The words a look shows on the window are drawn last, over everything, by pc_osd.c with the program's one small font (pc_font.c).
 *
 * THE MENU BAR (pc_menu.h). With the menu bar on, a window is taller by the bar's strip, and the picture is placed in the rest, below it, exactly as it
 * would be in a window of that size with no bar. In full screen there is no strip: the bar and its menus are drawn over the picture while they are
 * shown. The bar and the open menus are drawn after the picture and before the look's words, from pc_menu.c's pixels, which are drawn again only when
 * they change.
 */
#define SDL_MAIN_HANDLED
#define COBJMACROS
#include <SDL.h>
#include <SDL_syswm.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

#include "pc_filter.h"
#include "pc_menu.h"
#include "pc_osd.h"
#include "pc_window.h"

#define LINES 224

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *small_texture, *large_texture;
static int small_w, large_w, large_h;
static int shape, hidden, fullscreen, wants_readback;
#define SQUARE (shape == PC_SHAPE_SQUARE)
static const PcPicture *last_picture;
static int vsync_on, vsync_dropped;
static int vsync_refused;             /* a test: the renderer refuses to switch vsync */
static int vsync_slow;                /* a test: each switch of vsync takes 300 ms in the renderer */
static double open_sdl_ms, open_renderer_ms;
static int last_used_large;
static SDL_Texture *draw_target;      /* where draw puts the picture: NULL for the window, or a test's offscreen picture */

/* The look (pc_filter.h). gl_state: 0 when not asked yet, 1 when the shader looks can draw, -1 when they cannot, with gl_why. */
static int look = PC_LOOK_OFF;
static int gl_state;
static char gl_why[300];
static int preset_ready;
static char preset_label[120];
static char note_text[700];
static int note_waiting;
static uint32_t frames_drawn;

/* Words over the window, for a while: what pc_window_show_text asked for, and the texture they are drawn in. */
static char text_now[1200];
static Uint64 text_until;
static SDL_Texture *text_texture;
static int text_w, text_h, text_scale, text_columns, text_changed;

/* The menu bar: whether a window keeps a strip for it, and its pixels as textures (the bar, a menu, a list beside it), with the version of pc_menu.c's
   model they were drawn from. */
static int menu_strip;
static SDL_Texture *menu_texture[3];
static int menu_x[3], menu_y[3], menu_w[3], menu_h[3], menu_have[3];
static unsigned menu_version_drawn;
static int menu_size_drawn_w, menu_size_drawn_h;

static const char *const try_order[] = {"opengl", "direct3d", "direct3d11", "direct3d12", "software"};
#define TRY_MAX (sizeof(try_order) / sizeof(try_order[0]) + 1u)
static char fallback_lines[TRY_MAX][240];
static int fallback_n;

/* SDL's dummy video driver has no GPU, and Direct3D would open the real one to find that out. A run on it goes straight to software and leaves the graphics drivers alone. */
static int on_dummy_driver(void)
{
    const char *driver = SDL_GetCurrentVideoDriver();
    if (!driver)
        driver = SDL_getenv("SDL_VIDEODRIVER");
    return driver && !strcmp(driver, "dummy");
}

static int driver_index(const char *name)
{
    int i, n = SDL_GetNumRenderDrivers();
    for (i = 0; i < n; i++) {
        SDL_RendererInfo info;
        if (SDL_GetRenderDriverInfo(i, &info) == 0 && !strcmp(info.name, name))
            return i;
    }
    return -1;
}

/* A text from OpenGL itself, with the renderer's context current. The program does not link OpenGL: SDL has loaded it. */
#define GL_TEXT_VENDOR 0x1F00u
#define GL_TEXT_RENDERER 0x1F01u
#define GL_TEXT_VERSION 0x1F02u
static const char *gl_text(unsigned which)
{
    typedef const unsigned char *(APIENTRY *GetString)(unsigned);
    GetString get = (GetString)SDL_GL_GetProcAddress("glGetString");
    const unsigned char *text = get ? get(which) : NULL;
    return text ? (const char *)text : "";
}

static int named_in_list(const char *list, const char *name)
{
    size_t n = strlen(name);
    const char *at = list;
    while (at && (at = strstr(at, name)) != NULL) {
        if ((at == list || at[-1] == ',') && (at[n] == 0 || at[n] == ','))
            return 1;
        at += n;
    }
    return 0;
}

/* One try: a window and a renderer on it, or neither, with the reason in why. */
static int try_renderer(const char *name, const PcWindowOptions *o, int w, int h, char *why, size_t why_size)
{
    int soft = !strcmp(name, "software"), any = !strcmp(name, "auto"), index = -1;
    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | (!strcmp(name, "opengl") ? SDL_WINDOW_OPENGL : 0);
    Uint32 render_flags = soft ? SDL_RENDERER_SOFTWARE | SDL_RENDERER_TARGETTEXTURE : SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE | (o->vsync ? SDL_RENDERER_PRESENTVSYNC : 0);
    SDL_RendererInfo info;
    memset(&info, 0, sizeof(info));
    if (o->test_fail && named_in_list(o->test_fail, name)) {
        snprintf(why, why_size, "a test asked for this");
        return -1;
    }
    if (!soft && on_dummy_driver()) {
        snprintf(why, why_size, "SDL's dummy video driver has no GPU");
        return -1;
    }
    if (!soft && !any) {
        index = driver_index(name);
        if (index < 0) {
            snprintf(why, why_size, "this SDL has no renderer of that name");
            return -1;
        }
    }
    window = SDL_CreateWindow("Paprium", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, flags);
    if (!window) {
        snprintf(why, why_size, "no window: %s", SDL_GetError());
        return -1;
    }
    renderer = SDL_CreateRenderer(window, index, render_flags);
    if (renderer) {
        const char *refused = NULL;
        SDL_GetRendererInfo(renderer, &info);
        /* Windows' own OpenGL, on a PC with no driver for it or over a remote desktop, starts but draws on the processor, and has no target to make the 4:3 enlargement in. */
        if (!(info.flags & SDL_RENDERER_TARGETTEXTURE))
            refused = "it cannot draw into a texture";
        else if (info.name && !strcmp(info.name, "opengl") && strstr(gl_text(GL_TEXT_RENDERER), "GDI Generic"))
            refused = "this PC has only Windows' own OpenGL, which draws on the processor";
        if (refused) {
            snprintf(why, why_size, "%s", refused);
            SDL_DestroyRenderer(renderer);
            renderer = NULL;
        }
    } else {
        snprintf(why, why_size, "%s", SDL_GetError());
    }
    if (!renderer) {
        SDL_DestroyWindow(window);
        window = NULL;
        return -1;
    }
    vsync_on = o->vsync && !soft && (info.flags & SDL_RENDERER_PRESENTVSYNC);
    return 0;
}

int pc_window_available(void) { return 1; }

int pc_window_open(const PcWindowOptions *o, char *error, size_t error_size)
{
    int scale = o->scale > 0 ? o->scale : 3;
    int w = o->shape == PC_SHAPE_SQUARE || o->shape == PC_SHAPE_CORRECTED ? 320 * scale :
            o->shape == PC_SHAPE_ORIGINAL ? LINES * scale * 64 / 49 : LINES * scale * 4 / 3, h = LINES * scale;
    const char *first = o->renderer && o->renderer[0] ? o->renderer : try_order[0];
    char why[160] = "";
    unsigned k;
    hidden = o->hidden;
    shape = o->shape >= 0 && o->shape < PC_SHAPES ? o->shape : PC_SHAPE_4_3;
    wants_readback = o->want_readback;
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    vsync_refused = o->test_fail && named_in_list(o->test_fail, "vsync");
    vsync_slow = o->test_fail && named_in_list(o->test_fail, "vsyncslow");
    {
        /* How long SDL's own start takes, with the game controllers' search, for the freeze log's line about the start-up. */
        Uint64 t0 = SDL_GetPerformanceCounter();
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
            snprintf(error, error_size, "SDL could not start: %s", SDL_GetError());
            return -1;
        }
        open_sdl_ms = (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
    }
    menu_strip = o->menu_bar != 0;
    if (menu_strip && !o->fullscreen)
        h += pc_menu_bar_height(h);
    if (o->test_w > 0 && o->test_h > 0) {
        w = o->test_w;
        h = o->test_h;
    }
    fallback_n = 0;
    open_renderer_ms = (double)SDL_GetPerformanceCounter();
    for (k = 0; !renderer && k < TRY_MAX; k++) {
        /* The one asked for, then the list without it. */
        const char *name = k == 0 ? first : try_order[k - 1];
        const char *next = NULL;
        unsigned j;
        if (k && !strcmp(name, first))
            continue;
        if (try_renderer(name, o, w, h, why, sizeof(why)) == 0)
            break;
        for (j = k; j < TRY_MAX - 1u && !next; j++)
            if (strcmp(try_order[j], first))
                next = try_order[j];
        snprintf(fallback_lines[fallback_n++], sizeof(fallback_lines[0]), "renderer %s could not start (%s)%s%s", name, why, next ? ", so next is " : ", and there is no other", next ? next : "");
    }
    open_renderer_ms = ((double)SDL_GetPerformanceCounter() - open_renderer_ms) * 1000.0 / (double)SDL_GetPerformanceFrequency();
    if (!renderer) {
        snprintf(error, error_size, "No renderer would start. The last said: %s", why);
        return -1;
    }
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    if (!o->hidden)
        SDL_ShowWindow(window);
    if (o->fullscreen)
        pc_window_toggle_fullscreen();
    return 0;
}

void pc_window_close(void)
{
    if (gl_state == 1)
        pc_filter_gl_stop();
    gl_state = 0;
    look = PC_LOOK_OFF;
    preset_ready = 0;
    note_waiting = 0;
    text_until = 0;
    if (text_texture) SDL_DestroyTexture(text_texture);
    text_texture = NULL;
    {
        int k;
        for (k = 0; k < 3; k++) {
            if (menu_texture[k]) SDL_DestroyTexture(menu_texture[k]);
            menu_texture[k] = NULL;
            menu_have[k] = 0;
        }
        menu_version_drawn = 0;
    }
    if (large_texture) SDL_DestroyTexture(large_texture);
    if (small_texture) SDL_DestroyTexture(small_texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    large_texture = small_texture = NULL;
    renderer = NULL;
    window = NULL;
    small_w = large_w = large_h = 0;
    SDL_Quit();
}

static int small_for(int width)
{
    if (small_texture && small_w == width)
        return 0;
    if (small_texture)
        SDL_DestroyTexture(small_texture);
    small_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, width, LINES);
    if (!small_texture)
        return -1;
    SDL_SetTextureScaleMode(small_texture, SDL_ScaleModeNearest);
    SDL_SetTextureBlendMode(small_texture, SDL_BLENDMODE_NONE);
    small_w = width;
    return 0;
}

static int large_for(int w, int h)
{
    if (large_texture && large_w == w && large_h == h)
        return 0;
    if (large_texture)
        SDL_DestroyTexture(large_texture);
    large_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h);
    if (!large_texture)
        return -1;
    SDL_SetTextureScaleMode(large_texture, SDL_ScaleModeLinear);
    SDL_SetTextureBlendMode(large_texture, SDL_BLENDMODE_NONE);
    large_w = w;
    large_h = h;
    return 0;
}

/* The largest height that is a whole multiple of the lines and fits, with its width for the shape. A window smaller than one multiple gets a plain fit, whatever it costs. */
static void place(int picture_w, int out_w, int out_h, SDL_Rect *r)
{
    int k = out_h / LINES;
    if (shape == PC_SHAPE_CORRECTED || shape == PC_SHAPE_ORIGINAL) {
        /* The Pocket core's shapes. The frame that must fit is the 320 pixel line's: square pixels for corrected, 64:49 for original, whose width at k
           lines is LINES * k * 64 / 49 rounded down, as pc_window_open rounds the window's own width. A 256 pixel line is 64:49 in both. */
        int kw = shape == PC_SHAPE_CORRECTED ? out_w / 320 : (out_w * 49 + 48) / (LINES * 64);
        if (kw < k) k = kw;
        if (k < 1) k = 1;
        r->h = LINES * k;
        r->w = shape == PC_SHAPE_CORRECTED && picture_w == 320 ? 320 * k : LINES * k * 64 / 49;
    } else if (SQUARE) {
        int kw = out_w / 320;
        if (kw < k) k = kw;
        if (k < 1) k = 1;
        r->w = picture_w * k;
        r->h = LINES * k;
    } else {
        /* k lines fit across when their 4:3 width, LINES * k * 4 / 3 rounded down as pc_window_open rounds the window's own width, is at most
           out_w. Without the rounding, a window opened at scale 2, 4 or 5 (597, 1194 or 1493 pixels wide, where 4:3 is 597.3, 1194.7 and
           1493.3) got one multiple fewer than its height gives. */
        int kw = (out_w * 3 + 2) / (LINES * 4);
        if (kw < k) k = kw;
        if (k < 1) k = 1;
        r->h = LINES * k;
        r->w = (LINES * k * 4 + 1) / 3;
    }
    if (r->w > out_w) r->w = out_w;
    if (r->h > out_h) r->h = out_h;
    r->x = (out_w - r->w) / 2;
    r->y = (out_h - r->h) / 2;
}

/* Every look but off fills the window's height, or its width when that is the narrower: 4:3, or with square pixels a 320 pixel line as wide as the height gives, and a 256 pixel line in
   the middle of that width, as place does. */
static void fill_place(int picture_w, int out_w, int out_h, SDL_Rect *r)
{
    int frame_w;
    if (shape == PC_SHAPE_CORRECTED || shape == PC_SHAPE_ORIGINAL) {
        /* The Pocket core's shapes, as place gives them: the 320 pixel line's frame fills the height, or the width when that is the narrower. */
        int wide = shape == PC_SHAPE_CORRECTED;
        frame_w = wide ? (out_h * 320 + LINES / 2) / LINES : (out_h * 64 + 24) / 49;
        r->h = out_h;
        if (frame_w > out_w) {
            frame_w = out_w;
            r->h = wide ? (out_w * LINES + 160) / 320 : (out_w * 49 + 32) / 64;
        }
        r->w = wide && picture_w == 320 ? frame_w : wide ? (r->h * 64 + 24) / 49 : frame_w;
        r->x = (out_w - r->w) / 2;
        r->y = (out_h - r->h) / 2;
        return;
    }
    frame_w = SQUARE ? (out_h * 320 + LINES / 2) / LINES : (out_h * 4 + 1) / 3;
    r->h = out_h;
    if (frame_w > out_w) {
        frame_w = out_w;
        r->h = SQUARE ? (out_w * LINES + 160) / 320 : (out_w * 3 + 2) / 4;
    }
    r->w = SQUARE ? (frame_w * picture_w + 160) / 320 : frame_w;
    r->x = (out_w - r->w) / 2;
    r->y = (out_h - r->h) / 2;
}

static void set_note(const char *text)
{
    snprintf(note_text, sizeof(note_text), "%s", text);
    note_waiting = 1;
}

/* A look other than off. Returns 0 when it drew, or -1 to draw as with off: a shader that fails while the game runs puts the look back to off, and says why on the window and the console. */
static int draw_look(const PcPicture *p, int out_w, int out_h, int top)
{
    SDL_Rect r;
    char why[400], text[700];
    fill_place(p->width, out_w, out_h - top, &r);
    r.y += top;
    if (look == PC_LOOK_SHARP) {
        int across = (r.w + p->width - 1) / p->width, down = (r.h + LINES - 1) / LINES;
        if (large_for(p->width * across, LINES * down) != 0)
            return -1;
        SDL_SetRenderTarget(renderer, large_texture);
        SDL_RenderCopy(renderer, small_texture, NULL, NULL);
        SDL_SetRenderTarget(renderer, draw_target);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, large_texture, NULL, &r);
        last_used_large = 1;
        return 0;
    }
    if (gl_state != 1)
        return -1;
    SDL_SetRenderTarget(renderer, draw_target);
    SDL_RenderClear(renderer);
    SDL_RenderFlush(renderer);
    if (pc_filter_gl_draw(look, &p->rgb[0][0], PC_PICTURE_W, p->width, LINES, r.x, r.y, r.w, r.h, out_h, draw_target != NULL, frames_drawn, why, sizeof(why)) == 0)
        return 0;
    snprintf(text, sizeof(text), "The filter %s stopped working (%s), so the picture is drawn as with off.", pc_window_filter_label(), why);
    set_note(text);
    pc_window_show_text(text, 8000);
    look = PC_LOOK_OFF;
    return -1;
}

/* top: the menu bar's strip above the picture, 0 for none. */
static void draw(const PcPicture *p, int out_w, int out_h, int top)
{
    SDL_Rect place_rect;
    if (small_for(p->width))
        return;
    SDL_UpdateTexture(small_texture, NULL, p->rgb, (int)sizeof(p->rgb[0]));
    last_used_large = 0;
    if (look != PC_LOOK_OFF && draw_look(p, out_w, out_h, top) == 0)
        return;
    place(p->width, out_w, out_h - top, &place_rect);
    place_rect.y += top;
    if (place_rect.w % p->width == 0 && place_rect.h % LINES == 0) {
        SDL_SetRenderTarget(renderer, draw_target);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, small_texture, NULL, &place_rect);
        return;
    }
    {
        int kw = (place_rect.w + p->width - 1) / p->width;
        int kh = (place_rect.h + LINES - 1) / LINES;
        if (large_for(p->width * kw, LINES * kh) == 0) {
            SDL_SetRenderTarget(renderer, large_texture);
            SDL_RenderCopy(renderer, small_texture, NULL, NULL);
            SDL_SetRenderTarget(renderer, draw_target);
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, large_texture, NULL, &place_rect);
            last_used_large = 1;
        } else {
            SDL_SetRenderTarget(renderer, draw_target);
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, small_texture, NULL, &place_rect);
        }
    }
}

/* The menu bar's strip at this height: in a window with the bar on. */
static int strip(int out_h) { return menu_strip && !fullscreen ? pc_menu_bar_height(out_h) : 0; }

/* The menu bar and its open boxes, over what is drawn, from pc_menu.c's pixels: drawn into textures again only when they change. */
static void draw_menu(int out_w, int out_h)
{
    int k;
    pc_menu_layout(out_w, out_h, strip(out_h) > 0);
    if (pc_menu_version() != menu_version_drawn || out_w != menu_size_drawn_w || out_h != menu_size_drawn_h) {
        menu_version_drawn = pc_menu_version();
        menu_size_drawn_w = out_w;
        menu_size_drawn_h = out_h;
        for (k = 0; k < 3; k++) {
            uint32_t *pixels;
            int x = 0, y = 0, w = 0, h = 0;
            menu_have[k] = 0;
            if (menu_texture[k])
                SDL_DestroyTexture(menu_texture[k]);
            menu_texture[k] = NULL;
            if ((k == 0 ? pc_menu_render_bar(NULL, 0, &w, &h) : pc_menu_render_box(k - 1, NULL, 0, &x, &y, &w, &h)) != 0 || w <= 0 || h <= 0)
                continue;
            pixels = (uint32_t *)SDL_malloc((size_t)w * (size_t)h * 4u);
            if (pixels && (k == 0 ? pc_menu_render_bar(pixels, w * h, &w, &h) : pc_menu_render_box(k - 1, pixels, w * h, &x, &y, &w, &h)) == 0) {
                menu_texture[k] = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
                if (menu_texture[k]) {
                    SDL_UpdateTexture(menu_texture[k], NULL, pixels, w * 4);
                    SDL_SetTextureBlendMode(menu_texture[k], SDL_BLENDMODE_BLEND);
                    menu_x[k] = x;
                    menu_y[k] = y;
                    menu_w[k] = w;
                    menu_h[k] = h;
                    menu_have[k] = 1;
                }
            }
            SDL_free(pixels);
        }
    }
    SDL_SetRenderTarget(renderer, NULL);
    for (k = 0; k < 3; k++) {
        SDL_Rect r;
        if (!menu_have[k] || (k == 0 && !pc_menu_bar_shown()))
            continue;
        r.x = menu_x[k];
        r.y = menu_y[k];
        r.w = menu_w[k];
        r.h = menu_h[k];
        SDL_RenderCopy(renderer, menu_texture[k], NULL, &r);
    }
}

/* The words pc_window_show_text asked for, over the top left of the window, a little larger on a larger window, below the menu bar while it is shown. */
static void draw_text_over(int out_w, int out_h)
{
    int scale = out_h / 270, columns, w, h;
    SDL_Rect r;
    if (!text_until)
        return;
    if (SDL_GetTicks64() >= text_until) {
        text_until = 0;
        return;
    }
    if (scale < 1)
        scale = 1;
    columns = out_w * 9 / 10 / (PC_OSD_CELL_W * scale);
    if (columns < 8)
        columns = 8;
    if (text_changed || !text_texture || scale != text_scale || columns != text_columns) {
        uint32_t *pixels;
        pc_osd_render(text_now, columns, scale, NULL, 0, &w, &h);
        pixels = (uint32_t *)SDL_malloc((size_t)w * (size_t)h * 4u);
        if (text_texture)
            SDL_DestroyTexture(text_texture);
        text_texture = NULL;
        if (pixels && pc_osd_render(text_now, columns, scale, pixels, w * h, &w, &h) == 0) {
            text_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
            if (text_texture) {
                SDL_UpdateTexture(text_texture, NULL, pixels, w * 4);
                SDL_SetTextureBlendMode(text_texture, SDL_BLENDMODE_BLEND);
            }
        }
        SDL_free(pixels);
        text_w = w;
        text_h = h;
        text_scale = scale;
        text_columns = columns;
        text_changed = 0;
    }
    if (!text_texture)
        return;
    r.x = r.y = 3 * scale;
    if (pc_menu_bar_shown())
        r.y += pc_menu_bar_height(out_h);
    r.w = text_w;
    r.h = text_h;
    SDL_SetRenderTarget(renderer, NULL);
    SDL_RenderCopy(renderer, text_texture, NULL, &r);
}

const char *pc_window_shown_text(void) { return text_until && SDL_GetTicks64() < text_until ? text_now : ""; }

void pc_window_show_text(const char *text, int ms)
{
    snprintf(text_now, sizeof(text_now), "%s", text);
    text_until = SDL_GetTicks64() + (Uint64)(ms > 0 ? ms : 1);
    text_changed = 1;
}

/* Whether the shader looks can draw here, asked once a window. */
static int looks_ready(void)
{
    if (gl_state == 0) {
        if (!renderer || strcmp(pc_window_renderer(), "opengl")) {
            snprintf(gl_why, sizeof(gl_why), "they need the OpenGL renderer (renderer=opengl, the default), and this run draws with %s", pc_window_renderer());
            gl_state = -1;
        } else {
            /* SDL's OpenGL renderer keeps its context current on this thread; the flush makes sure it has drawn all it holds before this file adds to it. */
            SDL_RenderFlush(renderer);
            gl_state = pc_filter_gl_start(gl_why, sizeof(gl_why)) == 0 ? 1 : -1;
        }
    }
    return gl_state == 1;
}

const char *pc_window_filter_label(void) { return look == PC_LOOK_PRESET ? preset_label : pc_filter_name(look); }
int pc_window_filter(void) { return look; }

int pc_window_take_note(char *text, size_t size)
{
    if (!note_waiting)
        return 0;
    snprintf(text, size, "%s", note_text);
    note_waiting = 0;
    return 1;
}

int pc_window_set_filter(const char *value, const char *librashader_dll, char *note, size_t size)
{
    char why[600];
    int k;
    note[0] = 0;
    look = PC_LOOK_OFF;
    if (!value || !value[0] || !strcmp(value, "off"))
        return 0;
    if (pc_config_is_preset(value)) {
        const char *name = value + strlen(value);
        while (name > value && name[-1] != '\\' && name[-1] != '/')
            name--;
        snprintf(preset_label, sizeof(preset_label), "%s", name);
        if (!looks_ready())
            snprintf(why, sizeof(why), "a preset is drawn on the graphics card, and %s", gl_why);
        else if (pc_filter_preset_load(value, librashader_dll, why, sizeof(why)) == 0) {
            preset_ready = 1;
            look = PC_LOOK_PRESET;
            snprintf(note, size, "filter: your preset %s, through librashader", preset_label);
            return 0;
        }
        snprintf(note, size, "The filter preset %s could not be used: %s. The picture is drawn as with off.", value, why);
        pc_window_show_text(note, 10000);
        return -1;
    }
    k = pc_filter_find(value);
    if (k < 0) {
        snprintf(note, size, "There is no filter called %s, so the picture is drawn as with off.", value);
        pc_window_show_text(note, 10000);
        return -1;
    }
    if (pc_filter_needs_gl(k)) {
        if (!looks_ready())
            snprintf(why, sizeof(why), "%s", gl_why);
        else if (pc_filter_gl_build(k, why, sizeof(why)) == 0)
            why[0] = 0;
        if (why[0]) {
            snprintf(note, size, "The filter %s could not be used: %s. The picture is drawn as with off.", value, why);
            pc_window_show_text(note, 10000);
            return -1;
        }
    }
    look = k;
    snprintf(note, size, "filter: %s, %s", pc_filter_name(k), pc_filter_about(k));
    return 0;
}

int pc_window_next_filter(const char *key, char *note, size_t size)
{
    char why[400] = "", text[600];
    int next = look, tries, skipped_gl = 0, ready = looks_ready();
    for (tries = 0; tries < PC_LOOKS + 2; tries++) {
        next = next >= PC_LOOK_PRESET ? PC_LOOK_OFF : next + 1;
        if (next == PC_LOOK_PRESET && !preset_ready)
            continue;
        if (next != PC_LOOK_PRESET && pc_filter_needs_gl(next)) {
            if (!ready) {
                skipped_gl = 1;
                continue;
            }
            if (pc_filter_gl_build(next, why, sizeof(why)) != 0)
                continue;
        }
        break;
    }
    look = next;
    /* The key that changes the look is named with it, as the player may have moved that key on the controls screen. */
    snprintf(text, sizeof(text), "Filter: %s%s%s%s%s%s", look == PC_LOOK_PRESET ? preset_label : pc_filter_title(look), skipped_gl ? " (the CRT looks need the OpenGL renderer)" : "",
             why[0] ? " (one look would not build)" : "", key && key[0] ? ". " : "", key && key[0] ? key : "", key && key[0] ? ": the next filter" : "");
    pc_window_show_text(text, skipped_gl || why[0] ? 5000 : 2500);
    snprintf(note, size, "filter: %s, %s%s%s%s", pc_window_filter_label(), pc_filter_about(look), skipped_gl ? "; the CRT looks are left out, as " : "", skipped_gl ? gl_why : "",
             why[0] ? "; a look was left out, as it would not build: " : "");
    if (why[0] && strlen(note) + 1 < size)
        snprintf(note + strlen(note), size - strlen(note), "%s", why);
    return look;
}

int pc_window_draw_offscreen(const PcPicture *p, int w, int h, int frames, int every_ms, uint32_t *out, int capacity, double *gpu_ms_mean, double *gpu_ms_worst, double *gpu_ms_each,
                             double *cpu_ms_mean)
{
    Uint64 next = SDL_GetPerformanceCounter();
    SDL_Texture *target;
    static int timers[4096];
    double gpu = 0, worst = 0, cpu = 0;
    int i, n = 0, result = 0;
    if (!renderer || frames < 1 || frames > 4096 || w * h > capacity)
        return -1;
    target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h);
    if (!target)
        return -1;
    looks_ready();
    draw_target = target;
    for (i = 0; i < frames; i++) {
        Uint64 t0;
        int timer = gl_state == 1 ? pc_filter_gl_timer_begin() : -1;
        t0 = SDL_GetPerformanceCounter();
        draw(p, w, h, 0);
        SDL_RenderFlush(renderer);
        cpu += (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
        if (timer >= 0) {
            pc_filter_gl_timer_end();
            timers[n++] = timer;
        }
        frames_drawn++;
        if (every_ms > 0) {
            Uint64 now = SDL_GetPerformanceCounter();
            next += SDL_GetPerformanceFrequency() * (Uint64)every_ms / 1000u;
            if (next > now)
                SDL_Delay((Uint32)((next - now) * 1000u / SDL_GetPerformanceFrequency()));
        }
    }
    SDL_SetRenderTarget(renderer, target);
    if (SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888, out, w * 4) != 0)
        result = -1;
    for (i = 0; result == 0 && i < w * h; i++)
        out[i] &= 0x00FFFFFFu;
    for (i = 0; i < n; i++) {
        double ms = pc_filter_gl_timer_ms(timers[i]);
        if (gpu_ms_each)
            gpu_ms_each[i] = ms;
        gpu += ms;
        if (ms > worst)
            worst = ms;
    }
    SDL_SetRenderTarget(renderer, NULL);
    draw_target = NULL;
    SDL_DestroyTexture(target);
    *gpu_ms_mean = n ? gpu / n : -1;
    *gpu_ms_worst = n ? worst : -1;
    *cpu_ms_mean = cpu / frames;
    return result;
}

static uint32_t *read_buffer;
static int read_w, read_h;

/* What is on the back buffer has to be read before the present: after it, its contents are not promised. */
static void keep_for_readback(void)
{
    int w, h;
    SDL_GetRendererOutputSize(renderer, &w, &h);
    if (w != read_w || h != read_h || !read_buffer) {
        SDL_free(read_buffer);
        read_buffer = (uint32_t *)SDL_malloc((size_t)w * (size_t)h * 4u);
        read_w = w;
        read_h = h;
    }
    if (read_buffer)
        SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888, read_buffer, w * 4);
}

void pc_window_present(const PcPicture *p)
{
    int out_w, out_h;
    last_picture = p;
    draw_target = NULL;
    SDL_GetRendererOutputSize(renderer, &out_w, &out_h);
    draw(p, out_w, out_h, strip(out_h));
    draw_menu(out_w, out_h);
    draw_text_over(out_w, out_h);
    frames_drawn++;
    if (wants_readback)
        keep_for_readback();
    SDL_RenderPresent(renderer);
}

/* A monitor slower than the game would hold the game back through the present, and the game's speed must never follow the monitor. pc_pace.h says when the run loop turns vsync off and when it
   turns it back on. Only a renderer that started with vsync can have it back. */
int pc_window_set_vsync(int on)
{
    on = on != 0;
    if (!renderer || on == vsync_on || (on && !vsync_dropped))
        return 0;
    /* SDL 2.30.9 never refuses this for a renderer with a window: it falls back to waiting in its own code. Another SDL might, and a test asks for it by the name vsync. */
    if (vsync_slow)
        SDL_Delay(300);
    if (vsync_refused || SDL_RenderSetVSync(renderer, on) != 0)
        return -1;
    vsync_on = on;
    vsync_dropped = !on;
    return 0;
}

void pc_window_redraw(void)
{
    if (last_picture)
        pc_window_present(last_picture);
}

void pc_window_set_title(const char *text) { if (window) SDL_SetWindowTitle(window, text); }

void pc_window_toggle_fullscreen(void)
{
    fullscreen = !fullscreen;
    if (window)
        SDL_SetWindowFullscreen(window, fullscreen && !hidden ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

/* The shape key: the Aspect Ratio list's order, Original, Corrected, 4:3, square pixels, and round again; from the default 4:3 the next is square, as it
   always was. */
void pc_window_toggle_shape(void)
{
    static const int next[PC_SHAPES] = {PC_SHAPE_SQUARE, PC_SHAPE_ORIGINAL, PC_SHAPE_CORRECTED, PC_SHAPE_4_3};
    shape = next[shape];
}
int pc_window_is_square(void) { return SQUARE; }
void pc_window_set_shape(int s) { if (s >= 0 && s < PC_SHAPES) shape = s; }
int pc_window_shape(void) { return shape; }
int pc_window_is_fullscreen(void) { return fullscreen; }
int pc_window_menu_bar(void) { return menu_strip; }

void pc_window_set_menu_bar(int on)
{
    int w, h;
    on = on != 0;
    if (on == menu_strip)
        return;
    menu_strip = on;
    if (!window || fullscreen)
        return;
    /* The picture keeps its size: the window grows or shrinks by the strip. */
    SDL_GetWindowSize(window, &w, &h);
    if (on)
        SDL_SetWindowSize(window, w, h + pc_menu_bar_height(h));
    else
        SDL_SetWindowSize(window, w, h - pc_menu_bar_height(h - pc_menu_bar_height(h)));
}

void pc_window_point(int wx, int wy, int *x, int *y)
{
    int ww = 0, wh = 0, ow = 0, oh = 0;
    *x = wx;
    *y = wy;
    if (!window || !renderer)
        return;
    SDL_GetWindowSize(window, &ww, &wh);
    SDL_GetRendererOutputSize(renderer, &ow, &oh);
    if (ww > 0 && wh > 0 && (ww != ow || wh != oh)) {
        *x = (int)((long long)wx * ow / ww);
        *y = (int)((long long)wy * oh / wh);
    }
}
int pc_window_vsync_dropped(void) { return vsync_dropped; }
int pc_window_vsync_on(void) { return vsync_on; }
int pc_window_fallbacks(void) { return fallback_n; }
void pc_window_open_times(double *sdl_ms, double *renderer_ms) { *sdl_ms = open_sdl_ms; *renderer_ms = open_renderer_ms; }
const char *pc_window_fallback(int which) { return which >= 0 && which < fallback_n ? fallback_lines[which] : ""; }

const char *pc_window_renderer(void)
{
    SDL_RendererInfo info;
    if (renderer && SDL_GetRendererInfo(renderer, &info) == 0 && info.name)
        return info.name;
    return "none";
}

void pc_window_describe(char *text, size_t size)
{
    SDL_RendererInfo info;
    int w = 0, h = 0;
    memset(&info, 0, sizeof(info));
    if (renderer) {
        SDL_GetRendererInfo(renderer, &info);
        SDL_GetRendererOutputSize(renderer, &w, &h);
    }
    snprintf(text, size, "SDL %d.%d.%d, video driver %s, renderer %s, window %dx%d, %s, vsync %s", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL, SDL_GetCurrentVideoDriver(),
             info.name ? info.name : "none", w, h, hidden ? "hidden" : "shown", vsync_on ? "on" : "off");
}

void pc_window_displays(char *text, size_t size)
{
    int n = SDL_GetNumVideoDisplays(), here = window ? SDL_GetWindowDisplayIndex(window) : -1, i;
    size_t used = 0;
    text[0] = 0;
    for (i = 0; i < n && used + 1 < size; i++) {
        SDL_DisplayMode mode;
        if (SDL_GetCurrentDisplayMode(i, &mode) == 0)
            used += (size_t)snprintf(text + used, size - used, "%s%dx%d at %d Hz%s", used ? "; " : "", mode.w, mode.h, mode.refresh_rate, i == here ? ", the window is on this one" : "");
    }
}

int pc_window_refresh_hz(void)
{
    SDL_DisplayMode mode;
    int at = window ? SDL_GetWindowDisplayIndex(window) : -1;
    if (at < 0 || SDL_GetCurrentDisplayMode(at, &mode) != 0 || mode.refresh_rate <= 0)
        return 0;
    return mode.refresh_rate;
}

/* OpenGL and the Direct3D 9 renderer can name their own adapter. The other backends say nothing here, and pc_stall_adapters still lists what the system has. */
void pc_window_gpu(char *text, size_t size)
{
    IDirect3DDevice9 *device;
    snprintf(text, size, "this renderer does not say");
    if (!strcmp(pc_window_renderer(), "opengl")) {
        snprintf(text, size, "%s, %s, OpenGL %s", gl_text(GL_TEXT_RENDERER), gl_text(GL_TEXT_VENDOR), gl_text(GL_TEXT_VERSION));
        return;
    }
    device = renderer ? SDL_RenderGetD3D9Device(renderer) : NULL;
    if (device) {
        IDirect3D9 *d3d = NULL;
        D3DDEVICE_CREATION_PARAMETERS made;
        D3DADAPTER_IDENTIFIER9 id;
        if (SUCCEEDED(IDirect3DDevice9_GetDirect3D(device, &d3d))) {
            if (SUCCEEDED(IDirect3DDevice9_GetCreationParameters(device, &made)) && SUCCEEDED(IDirect3D9_GetAdapterIdentifier(d3d, made.AdapterOrdinal, 0, &id)))
                snprintf(text, size, "%s, driver %s %u.%u.%u.%u", id.Description, id.Driver, (unsigned)HIWORD(id.DriverVersion.HighPart), (unsigned)LOWORD(id.DriverVersion.HighPart),
                         (unsigned)HIWORD(id.DriverVersion.LowPart), (unsigned)LOWORD(id.DriverVersion.LowPart));
            IDirect3D9_Release(d3d);
        }
        IDirect3DDevice9_Release(device);
    }
}

void *pc_window_native(void)
{
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (window && SDL_GetWindowWMInfo(window, &info) && info.subsystem == SDL_SYSWM_WINDOWS)
        return info.info.win.window;
    return NULL;
}

int pc_window_read_screen(uint32_t *out, int capacity, int *w, int *h)
{
    int y, x;
    if (!read_buffer || read_w * read_h > capacity)
        return -1;
    for (y = 0; y < read_h; y++)
        for (x = 0; x < read_w; x++)
            out[y * read_w + x] = read_buffer[y * read_w + x] & 0x00FFFFFFu;
    *w = read_w;
    *h = read_h;
    return 0;
}

int pc_window_read_large(uint32_t *out, int capacity, int *w, int *h)
{
    int i, n;
    if (!last_used_large || !large_texture || large_w * large_h > capacity)
        return -1;
    SDL_SetRenderTarget(renderer, large_texture);
    if (SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888, out, large_w * 4) != 0) {
        SDL_SetRenderTarget(renderer, NULL);
        return -1;
    }
    SDL_SetRenderTarget(renderer, NULL);
    n = large_w * large_h;
    for (i = 0; i < n; i++)
        out[i] &= 0x00FFFFFFu;
    *w = large_w;
    *h = large_h;
    return 0;
}

/* SDL's message box is the system's own whatever the video driver is, so a run on the dummy driver must not reach it. */
void pc_window_message(const char *text)
{
    if (!hidden && !on_dummy_driver())
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Paprium", text, window);
}
