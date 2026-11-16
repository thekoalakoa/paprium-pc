/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The program's one small text font: the controls screen draws with it into a picture of the game's size, and pc_osd.c draws the words over the
 * window with the same shapes. Each character is 5 pixels wide and up to 8 tall in a cell of 6 by 8, so a 320 pixel line holds 53 characters and
 * 224 lines hold 28 rows. The letters were drawn for this program. Only the printable ASCII characters have shapes; any other byte is drawn as '?'.
 */
#ifndef PC_FONT_H
#define PC_FONT_H

#include <stdint.h>

#include "pc_frame.h"

#define PC_FONT_CELL_W 6
#define PC_FONT_CELL_H 8

/* Draws text with its first cell's top left corner at x, y, in colour 0x00RRGGBB. Pixels outside the picture are left out. Returns the x after the
   last character. */
int pc_font_draw(PcPicture *picture, int x, int y, const char *text, uint32_t colour);

/* The rows of one character's shape, top first, bit 4 the leftmost pixel. Returns 8 rows. */
const uint8_t *pc_font_glyph(unsigned char c);

#endif /* PC_FONT_H */
