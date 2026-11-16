/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A short line of text drawn over the window for a moment, such as the name of the picture filter a key has just picked. The text is drawn with the program's one small font
 * (pc_font.h, which the controls screen uses too), 5 dots wide, in capitals, on a dark box that lets a little of the game show through. It only ever goes on the window: the
 * game's own picture is never touched.
 *
 * pc_osd_render needs no SDL, so a test can look at every dot.
 */
#ifndef PC_OSD_H
#define PC_OSD_H

#include <stdint.h>

#define PC_OSD_CELL_W 6     /* a letter's 5 dots and a gap */
#define PC_OSD_CELL_H 9     /* a capital's 7 dots and a gap of one above and below; a comma's tail takes the gap below */

/* The box for text broken into lines of at most columns letters (at spaces where it can), every dot scale x scale pixels. Fills out (0xAARRGGBB, w x h, h rows of w) when out is
   not NULL and capacity_pixels is enough. Returns 0, or -1 when the box would not fit in capacity_pixels. A letter the font does not have is drawn as a question mark. */
int pc_osd_render(const char *text, int columns, int scale, uint32_t *out, int capacity_pixels, int *w, int *h);

/* 1 when the font has a shape for c: every printable ASCII character (lower case is drawn as capitals). Any other byte is drawn as a question mark. */
int pc_osd_has(char c);

#endif /* PC_OSD_H */
