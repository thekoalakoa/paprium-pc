/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The window, the picture in it, the keys and the game controllers, through SDL2. The rest of the program sees these two small interfaces and nothing of SDL, so it also builds
 * without SDL, in which case pc_nowindow.c stands in and only headless runs work.
 *
 * A window opened with hidden set is never shown and never takes the keyboard. Automated runs use that, or SDL's dummy video driver. They go through all the same code but the
 * showing of the window.
 *
 * THE PICTURE
 *   Colours are never changed and frames are never blended. Four shapes (the display line, pc_config.h):
 *   - 4:3, the default: the shape a television gives the picture, so 320 pixels a line and 256 pixels a line fill the same width. A pixel's width is then not a whole number of the
 *     screen's pixels. To keep it crisp the picture is first made larger by a whole number with no filter, then that large picture is reduced to the final size with a linear filter,
 *     so an edge between two game pixels takes at most one screen pixel and no pixel comes out wider than its neighbours by a whole screen pixel.
 *   - square: each game pixel is a whole number of screen pixels, nearest neighbour only. A 256 pixel line sits in the middle of the 320 pixel width.
 *   - original and corrected, the Pocket core's Aspect Ratio, from its video.json: original gives the 224 line picture the shape 64:49, a 320 pixel line and a 256 pixel line
 *     alike (a 240 line picture would be 128:105); corrected gives a 320 pixel line square pixels (10:7, or 4:3 for 240 lines) and keeps a 256 pixel line at original's 64:49,
 *     as the Pocket core keeps that mode out of its corrected pair. A width that is not a whole number of screen pixels is drawn as 4:3 is.
 *   The height is a whole multiple of the picture's lines, as large as fits.
 *   That is the look called off. The other looks (pc_filter.h) fill the window's height, whole multiple or not, and may change the colours on the window: never the game's picture.
 */
#ifndef PC_WINDOW_H
#define PC_WINDOW_H

#include <stdint.h>
#include <stdio.h>

#include "pc_config.h"
#include "pc_frame.h"

typedef struct PcWindowOptions {
    int hidden;
    int scale;
    int shape;              /* PC_SHAPE_* (pc_config.h): 0 is 4:3 */
    int vsync;
    int fullscreen;
    int want_readback;      /* keep a way to read pictures back, for tests */
    const char *renderer;   /* the configuration's renderer. NULL or empty means the default, OpenGL */
    const char *test_fail;  /* a test: renderers by name, with commas between, that act as if they will not start; and vsync, a renderer that refuses to switch vsync */
    int test_w, test_h;     /* a test: the window's size in place of scale's, as a hidden window cannot go full screen */
    int menu_bar;           /* a strip above the picture for the menu bar (pc_menu.h): the window is that much taller. In full screen the bar has none */
} PcWindowOptions;

/* Opens SDL, the window and its renderer. If the renderer asked for will not start, the next on pc_window.c's list is tried. Returns 0, or -1 with the reason in error when none will. */
int pc_window_open(const PcWindowOptions *options, char *error, size_t error_size);
void pc_window_close(void);
int pc_window_available(void);              /* 0 in a program built without SDL */

/* Draws a picture and shows it. picture stays the caller's; pc_window_redraw shows the last one again. */
void pc_window_present(const PcPicture *picture);
void pc_window_redraw(void);

void pc_window_set_title(const char *text);
void pc_window_toggle_fullscreen(void);
void pc_window_toggle_shape(void);         /* the shape key: the next shape, in the Aspect Ratio list's order; from 4:3, square */
int  pc_window_is_square(void);
void pc_window_set_shape(int shape);        /* PC_SHAPE_* */
int  pc_window_shape(void);
int  pc_window_is_fullscreen(void);
/* The menu bar's strip in a window, on or off: the window grows or shrinks by the strip, and the picture keeps its size. */
void pc_window_set_menu_bar(int on);
int  pc_window_menu_bar(void);
/* A point of the window as SDL's mouse gives it, in the window's own pixels, as the menu bar is drawn. */
void pc_window_point(int window_x, int window_y, int *x, int *y);
int  pc_window_set_vsync(int on);           /* stop waiting for the monitor, or wait for it again after that. 0, or -1 when the renderer refused */
int  pc_window_vsync_dropped(void);         /* vsync was on and the run has turned it off */
int  pc_window_vsync_on(void);

/* The renderer in use, by SDL's name for it. pc_window_fallbacks: how many would not start before it, and pc_window_fallback a line of text for each, in order. */
const char *pc_window_renderer(void);
int  pc_window_fallbacks(void);
const char *pc_window_fallback(int which);

/* How long pc_window_open took: SDL's start, with its search for game controllers, and the renderer's tries, each of which makes a window and a renderer on it. For the freeze
   log's line about the start-up. */
void pc_window_open_times(double *sdl_ms, double *renderer_ms);

/* What the renderer is, for a log line: the video driver and the renderer's names. */
void pc_window_describe(char *text, size_t size);

/* For the freeze log's first lines. pc_window_displays: each monitor's size and refresh rate, and which one holds the window. pc_window_gpu: the adapter the renderer really draws with and its
   driver, when the renderer can say. pc_window_native: the window's system handle, or NULL. */
void pc_window_displays(char *text, size_t size);
/* The refresh rate of the monitor that holds the window now, in whole Hz as the system gives it, or 0 when it is not known. For the vsync rule (pc_pace.h). */
int pc_window_refresh_hz(void);
void pc_window_gpu(char *text, size_t size);
void *pc_window_native(void);

/* For tests, with want_readback: reads what was last drawn, in 0x00RRGGBB, into out. Returns its width and height through w and h, 0 on success. pc_window_read_screen reads the
   window's whole picture as it was before the last present; pc_window_read_large reads the whole-number enlargement of 4:3, or fails when the last picture did not use one. */
int pc_window_read_screen(uint32_t *out, int capacity_pixels, int *w, int *h);
int pc_window_read_large(uint32_t *out, int capacity_pixels, int *w, int *h);
void pc_window_message(const char *text);   /* a plain message box, for a player's error; nothing when hidden */

/* ---- picture filters, called looks: see pc_filter.h */

/* Sets the look by a configuration's value: a look's name, or a preset file's full path, with librashader_dll the full path where librashader.dll would be. Returns 0, or -1 when that look
   cannot be used on this PC or renderer: the picture is then drawn as with off. note gets a plain line for the console either way, and a refusal is shown on the window for a while. */
int pc_window_set_filter(const char *value, const char *librashader_dll, char *note, size_t note_size);

/* The filter key: the next look this renderer can draw, after the last back to off. Its name is shown on the window for a moment with key, the name of the key that changes the
   look (NULL or empty when no key does), and note gets a line for the console. Returns the look. */
int pc_window_next_filter(const char *key, char *note, size_t note_size);
int pc_window_filter(void);                 /* the look in use: a PC_LOOK_ number */
const char *pc_window_filter_label(void);   /* its name, or the preset file's name */

/* A line for the console when a look stopped working during the run and the picture went back to off. Returns 1 when there was one, and forgets it. */
int pc_window_take_note(char *note, size_t note_size);

/* Shows text over the top left of the window for ms milliseconds. */
void pc_window_show_text(const char *text, int ms);
/* The text shown over the window now, or "" when none is: for tests. */
const char *pc_window_shown_text(void);

/* For tests and the timing tool: draws picture with the look in use into an offscreen picture of w x h, as a window of that size shows it without any text over it, frames times,
   one each every_ms milliseconds (0: back to back), and reads the last into out. gpu_ms_mean and gpu_ms_worst get the GPU's own time a frame from OpenGL's timer queries, or -1 on
   another renderer, and gpu_ms_each, when not NULL, each frame's; cpu_ms_mean the program's time a frame to hand it all to the GPU. Returns 0, or -1. */
int pc_window_draw_offscreen(const PcPicture *picture, int w, int h, int frames, int every_ms, uint32_t *out, int capacity_pixels, double *gpu_ms_mean, double *gpu_ms_worst,
                             double *gpu_ms_each, double *cpu_ms_mean);

/* ---- keys and controllers */

enum {
    PC_ACTION_QUIT = 1,
    PC_ACTION_LIMITS = 2,       /* flip the sprite limits */
    PC_ACTION_PAUSE = 4,
    PC_ACTION_FULLSCREEN = 8,
    PC_ACTION_SHAPE = 16,       /* 4:3 or square, see pc_window_toggle_shape */
    PC_ACTION_FILTER = 256,     /* the next picture filter. 32 is pc_audio.h's mute, 64 and 128 pc_controls.h's controls key and the controls screen closing */
    PC_ACTION_MENU = 512,       /* the menu key: opens the menu bar's first menu, or closes what is open (pc_menu.h) */
    PC_ACTION_MENU_COMMAND = 1024   /* a menu item asked for one of the program's own commands: pc_menu_take_command has it */
};

/* Reads the configuration's keys. Returns 0, or -1 with the reason in error when a key's name is not known. */
int pc_input_open(const PcConfig *config, char *error, size_t error_size);
void pc_input_close(void);

/* Takes everything SDL has waiting. pad gets the PC_PAD_* bits each player holds: players 1, 2 and 3. Returns the PC_ACTION_* bits that happened since
   the last call. */
unsigned pc_input_poll(uint32_t pad[3]);

/* A test's key press, by SDL's name for the key, into the same queue the keyboard feeds. Returns 0, or -1 when the name is not known. */
int pc_input_push_key(const char *name, int down);
/* A test's mouse: moved to x, y of the window, or the left button pressed there (press 1). Returns 0, or -1. */
int pc_input_push_mouse(int x, int y, int press);
/* After a poll that gave PC_ACTION_MENU_COMMAND: the command a menu item asked for (pc_menu.h's PC_MENU_DO_*) and its argument, one at a time, 0 when
   none is left. */
int pc_input_take_menu_command(int *arg);

int pc_input_controllers(void);             /* controllers open now */
void pc_input_print_keys(FILE *to);

#endif /* PC_WINDOW_H */
