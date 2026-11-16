/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See pc_osd.h. The letters are the program's one small font, pc_font.c's, the one the controls screen draws with too: each character up to 8 rows
 * of 5 dots, the top row first, and in each row bit 4 is the leftmost dot. Small letters are drawn as capitals here.
 */
#include <string.h>

#include "pc_font.h"
#include "pc_osd.h"

#define BOX 0xB0000000u         /* the box: black, about two thirds covering */
#define INK 0xFFFFFFFFu         /* the letters: white */
#define PAD 2                   /* dots of box around the text */

/* The shape a character is drawn with: small letters as capitals, and pc_font_glyph draws any byte outside printable ASCII as '?'. */
static const uint8_t *shape(char c)
{
    unsigned char u = (unsigned char)c;
    if (u >= 'a' && u <= 'z')
        u = (unsigned char)(u - 'a' + 'A');
    return pc_font_glyph(u);
}

int pc_osd_has(char c) { return (unsigned char)c >= 32 && (unsigned char)c <= 126; }

/* Where each line starts and how long it is, breaking at the last space that fits, or in the middle of a word too long for a line. */
#define MAX_LINES 8

static int lay_out(const char *text, int columns, int start[MAX_LINES], int length[MAX_LINES])
{
    int n = 0, at = 0, total = (int)strlen(text);
    if (columns < 1)
        columns = 1;
    while (at < total && n < MAX_LINES) {
        int end = at, cut;
        while (end < total && text[end] != '\n' && end - at < columns)
            end++;
        cut = end;
        if (end < total && text[end] != '\n' && text[end] != ' ') {
            int k = end;
            while (k > at && text[k - 1] != ' ')
                k--;
            if (k > at)
                cut = k - 1;    /* the space itself is left out */
        }
        start[n] = at;
        length[n] = cut - at;
        n++;
        at = cut;
        while (at < total && (text[at] == ' ' || text[at] == '\n'))
            at++;
    }
    return n;
}

int pc_osd_render(const char *text, int columns, int scale, uint32_t *out, int capacity, int *w, int *h)
{
    int start[MAX_LINES], length[MAX_LINES], lines, longest = 0, i, bw, bh, line, k, row, dot, sy, sx;
    if (scale < 1)
        scale = 1;
    lines = lay_out(text, columns, start, length);
    for (i = 0; i < lines; i++)
        if (length[i] > longest)
            longest = length[i];
    bw = (longest * PC_OSD_CELL_W - 1 + 2 * PAD) * scale;
    bh = (lines * PC_OSD_CELL_H + 2 * PAD - 2) * scale;
    if (bw < (2 * PAD + 1) * scale)
        bw = (2 * PAD + 1) * scale;
    if (bh < (2 * PAD + 1) * scale)
        bh = (2 * PAD + 1) * scale;
    *w = bw;
    *h = bh;
    if (!out)
        return 0;
    if (bw * bh > capacity)
        return -1;
    for (i = 0; i < bw * bh; i++)
        out[i] = BOX;
    for (line = 0; line < lines; line++) {
        for (k = 0; k < length[line]; k++) {
            const uint8_t *g = shape(text[start[line] + k]);
            for (row = 0; row < 8; row++) {     /* a comma's tail reaches the eighth row, in the gap below the line */
                for (dot = 0; dot < 5; dot++) {
                    if (!(g[row] >> (4 - dot) & 1))
                        continue;
                    for (sy = 0; sy < scale; sy++)
                        for (sx = 0; sx < scale; sx++) {
                            int x = (PAD + k * PC_OSD_CELL_W + dot) * scale + sx;
                            int y = (PAD - 1 + line * PC_OSD_CELL_H + 1 + row) * scale + sy;
                            if (x < bw && y < bh)
                                out[y * bw + x] = INK;
                        }
                }
            }
        }
    }
    return 0;
}
