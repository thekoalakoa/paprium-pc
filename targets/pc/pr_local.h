/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * What the renderer's three files share. Outside them only the renderer's tests include this, and the program's frame loop for pr_render_kept.
 *
 * A line buffer holds one byte per pixel, in the form the chip's own line buffer uses:
 *   bit 6     priority
 *   bits 5-4  palette
 *   bits 3-0  colour index, 0 = transparent
 * A plane's buffer starts PR_MARGIN bytes to the left of pixel 0, so a column of a scrolled plane can be drawn in one go and the part left of the screen simply
 * never gets read.
 */
#ifndef PR_LOCAL_H
#define PR_LOCAL_H

#include "pc_frame.h"

#define PR_MARGIN       32
#define PR_BUFFER_W     (PR_MARGIN + PC_PICTURE_W + 64)

/* The tests can break one rule on purpose, to prove that they notice. In the program itself PR_PLANT is always false, so the compiler
   drops the branch and no trace of it is left. The numbers are listed in tests/pr_render_test.c. */
#ifdef PR_TEST_PLANTS
extern int pr_plant;
#define PR_PLANT(n) (pr_plant == (n))
#else
#define PR_PLANT(n) 0
#endif

static inline unsigned pr_word(const uint8_t *vram, unsigned address)
{
    return (unsigned)vram[address & 0xFFFFu] << 8 | vram[(address + 1u) & 0xFFFFu];
}

/* Time inside a frame, in master clocks since the V counter became 0. */
static inline uint32_t pr_time(unsigned vline, unsigned hclock)
{
    return vline * PC_LINE_CLOCKS + hclock;
}

/* ---- sprites */

/* One of the chip's per-line sprite records. It keeps a record for each sprite it finds on a line, and clears the X of a record when it draws it.
   See pr_sprites.c for why that matters. */
typedef struct PrRecord {
    uint16_t x;
    uint16_t tile;      /* the tile of the sprite's first cell on this line */
    uint8_t  row;       /* the pixel row inside the tile */
    uint8_t  cols;      /* width in cells, less one */
    uint8_t  rows;      /* height in cells, less one */
    uint8_t  bits;      /* priority and palette, in their line buffer places */
    uint8_t  flip;      /* 1 when flipped left to right */
} PrRecord;

/* A sprite the walk found on a line. */
typedef struct PrFound {
    uint8_t entry;      /* its entry in the table */
    uint8_t size;       /* the size byte */
    uint8_t offset;     /* this line's distance from the sprite's top */
} PrFound;

/* What the chip's sprite walk remembers from line to line, and so from frame to frame. */
typedef struct PrMemory {
    PrRecord  records[20];
    uint32_t  out;              /* the chip's last read of its copy of the table, and its last read for each value of the entry number's low two bits */
    uint32_t  lane[4];
    uint8_t   previous_nz;      /* the chip's memory for the mask rule: the last line ended on an entry with X other than 0 */
    uint8_t   last_nz;          /* the entry the walk loaded last had X other than 0 */
} PrMemory;

/* pr_render, for a program that draws frame after frame: the walk's memory carries over, as it does on the chip. The chip's walk keeps a record it fetched and never drew,
   and a line of a later frame can draw it. Start with a memory of zeros. pr_render is this with a fresh memory each time, which gives the same picture but for those lines. */
void pr_render_kept(const PcFrame *frame, const PrOptions *options, PrMemory *memory, PcPicture *out);

/* One thing that lands in the chip's memories during the walk of a line, in time order. kind is PC_WRITE_*. A VRAM word lands one byte at a time, on two access
   slots, so for VRAM addr is a byte address and value a byte. */
typedef struct PrLanding {
    uint32_t time;
    uint16_t addr;
    uint16_t value;
    uint8_t  kind;
} PrLanding;

/* The walk's working state for the next line. */
typedef struct PrWalk {
    PrMemory *memory;
    PrRecord  fetched[20];
    uint8_t   found;            /* sprites found for the next line */
    uint8_t   over;             /* the chip's rules cut something on that line */
    uint8_t   free_copy;        /* table_copy holds the table before free_landings */
    const PrLanding *free_landings;
    unsigned  free_n_landings;
    uint8_t   table_copy[1024];
} PrWalk;

/* Reads the table for one picture line, from the state as it stands plus the table writes in landings, which land while the walk goes on. Times are the frame's. */
void pr_sprites_fetch(const PcVdpState *s, PrWalk *w, int line, PrLimits limits, const PrLanding *landings, unsigned n_landings);
/* Draws the sprite line buffer from what was read. Returns 1 when the chip's rules cut something on the line. */
int  pr_sprites_draw(const PcVdpState *s, PrWalk *w, int line, PrLimits limits, uint8_t *buffer);

/* ---- planes */

#define PR_COLUMNS      21  /* 2-cell columns a line fetches in the 320 pixel mode: one hidden at the left edge, then 20 */
#define PR_COLUMNS_H32  17  /* the same in the 256 pixel mode */

/* The H scroll word of a plane for a line. plane is 0 for A, 1 for B. */
unsigned pr_h_scroll(const PcVdpState *s, int line, int plane);
/* The V scroll value each fetched column of a plane reads. */
void pr_scroll_columns(const PcVdpState *s, int plane, uint16_t *scroll_columns);
/* Draws one plane's line buffer. */
void pr_plane_a_line(const PcVdpState *s, int line, unsigned scroll, const uint16_t *scroll_columns, uint8_t *buffer);
void pr_plane_b_line(const PcVdpState *s, int line, unsigned scroll, const uint16_t *scroll_columns, uint8_t *buffer);

#endif /* PR_LOCAL_H */
