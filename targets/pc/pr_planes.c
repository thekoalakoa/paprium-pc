/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Planes A and B and the window, one line at a time.
 *
 * The chip fetches a plane in 2-cell columns. H scroll moves the plane as a whole: its low four bits shift the pixels, the rest choose which columns of
 * the map are fetched. With a shift, one more column shows at the left edge, so a line fetches a column numbered -1 first. V scroll comes from one word per plane
 * or from one word per column, by register 11.
 */
#include "pr_local.h"

/* Plane sizes in cells, from register 16's two fields. The value the hardware manual forbids is taken as 64 here. */
static const uint16_t plane_cells[4] = { 32, 64, 64, 128 };

/* One cell's eight pixels into the buffer. entry is the map word: priority, palette, V flip, H flip, tile. */
static inline void draw_tile(uint8_t *dst, const uint8_t *vram, unsigned entry, unsigned pixel_row)
{
    const uint8_t *p = &vram[(entry & 0x7FFu) * 32u + ((entry & 0x1000u) ? 7u - pixel_row : pixel_row) * 4u];
    const uint8_t bits = (uint8_t)(((entry >> 15) << 6) | (((entry >> 13) & 3u) << 4));
    if (entry & 0x0800u) {
        dst[0] = (uint8_t)(bits | (p[3] & 15u));
        dst[1] = (uint8_t)(bits | (p[3] >> 4));
        dst[2] = (uint8_t)(bits | (p[2] & 15u));
        dst[3] = (uint8_t)(bits | (p[2] >> 4));
        dst[4] = (uint8_t)(bits | (p[1] & 15u));
        dst[5] = (uint8_t)(bits | (p[1] >> 4));
        dst[6] = (uint8_t)(bits | (p[0] & 15u));
        dst[7] = (uint8_t)(bits | (p[0] >> 4));
    } else {
        dst[0] = (uint8_t)(bits | (p[0] >> 4));
        dst[1] = (uint8_t)(bits | (p[0] & 15u));
        dst[2] = (uint8_t)(bits | (p[1] >> 4));
        dst[3] = (uint8_t)(bits | (p[1] & 15u));
        dst[4] = (uint8_t)(bits | (p[2] >> 4));
        dst[5] = (uint8_t)(bits | (p[2] & 15u));
        dst[6] = (uint8_t)(bits | (p[3] >> 4));
        dst[7] = (uint8_t)(bits | (p[3] & 15u));
    }
}

unsigned pr_h_scroll(const PcVdpState *s, int line, int plane)
{
    /* By register 11: one word for the whole plane; the first eight lines' words repeating; a word per cell; a word per line */
    static const uint8_t line_mask[4] = { 0x00, 0x07, 0xF8, 0xFF };
    unsigned table = (unsigned)(s->regs[13] & 0x3Fu) << 10;
    unsigned mode = PR_PLANT(11) ? (s->regs[11] & 3u) & 2u : s->regs[11] & 3u;
    unsigned index = (unsigned)line & line_mask[mode];
    return pr_word(s->vram, table + index * 4u + (PR_PLANT(12) ? 2u : (unsigned)plane * 2u)) & 0x3FFu;
}

void pr_scroll_columns(const PcVdpState *s, int plane, uint16_t *scroll_columns)
{
    int column;
    if (s->regs[11] & 4u) {
        /* A scroll word per 2-cell column. The column hidden at the left edge has no word of its own: for both planes it takes two words of the memory anded together, the last
           two in the 320 pixel mode, and in the 256 pixel mode the two the exact chip model shows */
        scroll_columns[0] = PR_PLANT(29) ? s->vsram[plane] : (s->regs[12] & 1u) ? (uint16_t)(s->vsram[38] & s->vsram[39]) : (uint16_t)(s->vsram[35] & s->vsram[36]);
        for (column = 1; column < PR_COLUMNS; column++) {
            int word = column - 1;
            if (PR_PLANT(13))
                word = column;
            scroll_columns[column] = s->vsram[(unsigned)(word * 2 + plane) % PC_VSRAM_WORDS];
        }
    } else {
        for (column = 0; column < PR_COLUMNS; column++)
            scroll_columns[column] = s->vsram[plane];
    }
}

/* Draws the scrolled plane's columns, first to last. skip, when given, marks columns the window takes. */
static void draw_scrolled(const PcVdpState *s, int line, unsigned base, unsigned scroll, const uint16_t *scroll_columns, uint8_t *buffer, int columns,
                          const uint8_t *skip)
{
    const unsigned width = plane_cells[s->regs[16] & 3u], height = plane_cells[(s->regs[16] >> 4) & 3u];
    const unsigned fine = scroll & 15u;
    unsigned source = (0u - (scroll >> 4)) * 2u - 2u;       /* the map column of the hidden column's left cell */
    int column;

    for (column = 0; column < columns; column++, source += 2u) {
        unsigned y, row_base;
        uint8_t *dst;
        if (skip && skip[column])
            continue;
        y = (unsigned)line + scroll_columns[column];
        row_base = base + ((y >> 3) & (height - 1u)) * width * 2u;
        dst = buffer + (PR_MARGIN - 16 + column * 16 + (int)fine);
        draw_tile(dst, s->vram, pr_word(s->vram, row_base + (source & (width - 1u)) * 2u), y & 7u);
        draw_tile(dst + 8, s->vram, pr_word(s->vram, row_base + ((source + 1u) & (width - 1u)) * 2u), y & 7u);
    }
}

void pr_plane_b_line(const PcVdpState *s, int line, unsigned scroll, const uint16_t *scroll_columns, uint8_t *buffer)
{
    const int columns = (s->regs[12] & 1u) ? PR_COLUMNS : PR_COLUMNS_H32;
    draw_scrolled(s, line, (unsigned)(s->regs[4] & 7u) << 13, scroll, scroll_columns, buffer, columns, NULL);
}

/* Plane A, with the window in the columns or lines that show it. The window does not scroll and takes the place of plane A in its columns. */
void pr_plane_a_line(const PcVdpState *s, int line, unsigned scroll, const uint16_t *scroll_columns, uint8_t *buffer)
{
    const unsigned h40 = s->regs[12] & 1u;
    const int columns = h40 ? PR_COLUMNS : PR_COLUMNS_H32;
    const unsigned base = (unsigned)(s->regs[2] & 0x38u) << 10;
    const unsigned win_h = s->regs[17], win_v = s->regs[18];
    const unsigned cell_line = (unsigned)line >> 3;
    const unsigned at_v = win_v & 0x1Fu, at_h = win_h & 0x1Fu;
    uint8_t in_window[PR_COLUMNS];
    int any = 0, column;
    int whole_line = (win_v & 0x80u) ? cell_line >= at_v : cell_line < at_v;

    if (PR_PLANT(14))
        whole_line = (win_v & 0x80u) ? cell_line > at_v : cell_line <= at_v;
    if (!whole_line && at_h == 0 && !(win_h & 0x80u)) {
        /* The common case: no window on this line */
        draw_scrolled(s, line, base, scroll, scroll_columns, buffer, columns, NULL);
        return;
    }
    for (column = 0; column < columns; column++) {
        int screen = column - 1;
        int inside = whole_line || ((win_h & 0x80u) ? screen >= (int)at_h : screen < (int)at_h);
        if (PR_PLANT(15) && !whole_line)
            inside = (win_h & 0x80u) ? screen > (int)at_h : screen <= (int)at_h;
        in_window[column] = (uint8_t)inside;
        any |= inside;
    }
    draw_scrolled(s, line, base, scroll, scroll_columns, buffer, columns, in_window);
    if (any) {
        const unsigned table = (unsigned)(s->regs[3] & (h40 ? 0x3Cu : 0x3Eu)) << 10;
        const unsigned row_base = table + cell_line * (h40 ? 64u : 32u) * 2u;
        for (column = 1; column < columns; column++) {
            if (in_window[column]) {
                unsigned cell = (unsigned)(column - 1) * 2u;
                uint8_t *dst = buffer + PR_MARGIN + cell * 8u;
                draw_tile(dst, s->vram, pr_word(s->vram, row_base + cell * 2u), (unsigned)line & 7u);
                draw_tile(dst + 8, s->vram, pr_word(s->vram, row_base + (cell + 1u) * 2u), (unsigned)line & 7u);
            }
        }
        /* Where a window on the left ends, plane A's first column comes later than its place by the fine scroll, and the gap this leaves shows the end of that same column */
        if (!whole_line && !(win_h & 0x80u)) {
            const unsigned fine = scroll & 15u, edge = PR_MARGIN + at_h * 16u;
            unsigned i;
            for (i = 0; i < fine; i++)
                buffer[edge + i] = PR_PLANT(30) ? 0 : buffer[edge + 16u + i];
        }
    }
}
