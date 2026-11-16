/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Picture filters, called looks here: how the game's picture is drawn on the window. A look changes only the window. The game, its picture, its sound and every log stay exactly as they
 * are, so the same run gives the same frames with any look.
 *
 * The looks, in the order the filter key goes through them:
 *   off        the picture as it has always been drawn: sharp pixels, a whole number of screen lines for each game line, black bars where that leaves room
 *   sharp      sharp pixels that fill the window's height: each pixel stays crisp, with at most one soft screen pixel at its edge
 *   scanlines  the dark lines between the picture's lines on a flat RGB monitor, with no colour mask
 *   crt        a home television: soft beam, a little glow, and a slot mask (columns of red, green and blue broken up like brickwork)
 *   pvm        a Sony PVM or BVM broadcast monitor: sharp picture, clear dark scanlines and an aperture grille (unbroken red, green and blue stripes)
 *   curved     an older television with curved glass, rounded corners and a fine dot mask
 * A player's own preset (a RetroArch .slangp file) comes after them when the configuration names one.
 *
 * off and sharp work on every renderer, through SDL alone. The other looks are small programs that run on the graphics card (shaders), written for RetroArch's shader format by their
 * authors and kept as they wrote them in the shaders folder beside this file, with their notices; this file only feeds them the picture and their settings. They need the OpenGL renderer,
 * the default. With OpenGL, SDL's renderer draws in a context the program can draw in too, once SDL has finished what it holds. With Direct3D 9, 11 and 12, SDL 2 hands out its
 * device, but not the textures or the target it draws with, and it keeps its own copy of the device's state; a pass there would need textures of its own, all of that state put back,
 * and these shaders written again for Direct3D, which is not done. So on those, and on the software renderer, these looks are refused with a plain message and the picture is drawn
 * as with off.
 *
 * A player's own preset runs through librashader, a library that runs RetroArch's slang presets in other programs (Mozilla Public License 2.0, or GPL 3 at our choice). It is not built into
 * this program: the library, librashader.dll, must sit beside the program, and the preset's files wherever the player keeps them. A preset that cannot be read or built, or a missing
 * library, leaves the picture as with off and says why in a plain message.
 */
#ifndef PC_FILTER_H
#define PC_FILTER_H

#include <stddef.h>
#include <stdint.h>

enum { PC_LOOK_OFF, PC_LOOK_SHARP, PC_LOOK_SCANLINES, PC_LOOK_CRT, PC_LOOK_PVM, PC_LOOK_CURVED, PC_LOOKS };
/* Their names, in that order, for a configuration's filter line. pc_config.c checks a line against these without SDL. */
#define PC_LOOK_NAMES {"off", "sharp", "scanlines", "crt", "pvm", "curved"}
#define PC_LOOK_PRESET PC_LOOKS     /* a player's preset file, after the built-in looks */

const char *pc_filter_name(int look);       /* the name a configuration and the console use: "crt" */
const char *pc_filter_title(int look);      /* the words shown on the window: "Consumer TV" */
const char *pc_filter_about(int look);      /* one line: what it imitates and whose shader draws it */
int pc_filter_needs_gl(int look);           /* 1 for the looks that are shaders, and a preset */
int pc_filter_find(const char *name);       /* the look of that name, or -1. Whether a value names a preset file instead: pc_config_is_preset */

/* ---- the OpenGL side, for pc_window.c. Every call needs the renderer's OpenGL context current, and leaves the OpenGL state as it found it, as SDL's renderer counts on that. */

/* Takes the OpenGL functions the looks need. 0, or -1 with the reason in why, when this OpenGL is too old (3.0 is the least). */
int pc_filter_gl_start(char *why, size_t why_size);
void pc_filter_gl_stop(void);       /* lets go of everything made on the GPU, and of the preset and the library */

/* Draws the picture (rgb, stride pixels a row, pw x ph) with a look into place (x, y, w, h, from the top left) of the framebuffer bound for drawing now, fb_h lines tall. top_down is 1 when
   that framebuffer keeps its top line first, as SDL's target textures do, and 0 for the window. frame counts the frames shown, for looks that move. Returns 0, or -1 with why when the look
   could not be built or drawn: the caller then draws as with off. */
int pc_filter_gl_draw(int look, const uint32_t *rgb, int stride, int pw, int ph, int x, int y, int w, int h, int fb_h, int top_down, uint32_t frame, char *why, size_t why_size);

/* Builds a look's shader now rather than at its first draw, so a failure can be told at once. 0 (also for a look with no shader), or -1 with why. pc_filter_gl_build_all builds
   every built-in one, and gives the first failure. */
int pc_filter_gl_build(int look, char *why, size_t why_size);
int pc_filter_gl_build_all(char *why, size_t why_size);

/* A player's preset: loads librashader.dll from dll_path, a full path, and builds path's preset with it. 0, or -1 with why. */
int pc_filter_preset_load(const char *path, const char *dll_path, char *why, size_t why_size);
int pc_filter_preset_loaded(void);

/* The look at a preset file that comes before the library is loaded, and needs neither OpenGL nor the library: the file is there, it has a line shaders = N, and every shader,
   texture and #reference file it names is there, from the preset's own folder. 0, or -1 with why in plain words. */
int pc_filter_preset_check(const char *path, char *why, size_t why_size);

/* For tests and the timing tool: the GPU's own time for what is drawn between begin and end, through OpenGL's timer queries (OpenGL 3.3). begin returns -1 when this OpenGL has none.
   ms waits for the GPU and gives its time in milliseconds, or -1. */
int pc_filter_gl_timer_begin(void);
void pc_filter_gl_timer_end(void);
double pc_filter_gl_timer_ms(int which);

#endif /* PC_FILTER_H */
