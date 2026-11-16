/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The sprite line buffer: the chip's walk of the sprite table with its limits, and the walk with none.
 *
 * WITH THE CHIP'S LIMITS
 * The chip prepares each line from its own copy of the first four bytes of every table entry (Y, size, link). It follows the links from
 * entry 0, and for each entry on the line it reads X and the tile from VRAM. These are its rules, as the exact chip model shows them:
 *
 *   - It looks at 80 entries at most (64 in the 256 pixel mode) and keeps 20 sprites a line (16). The next sprite on a line stops the walk.
 *   - It draws 40 cells a line (32). The cell budget can run out in the middle of a sprite, which is then drawn in part.
 *   - The first opaque pixel on the chain wins, whatever its priority. A transparent pixel is written too, so its priority and palette
 *     bits sit in the buffer until an opaque pixel replaces them. Those bits select nothing in the colour logic.
 *   - The mask rule: an entry with X = 0, found after an entry with X other than 0, stops everything later on the line from being drawn.
 *     It still uses up cells. The earlier entry can be on the line before: if that line ran out of cells while on an entry with X other than
 *     0, an X = 0 first on this line masks too.
 *   - The ghost: the chip keeps its 20 per-line records and clears a record's X when it draws it. A record that was fetched but never drawn,
 *     because its line ran out of cells, keeps its X. A later line with few sprites goes on through its records while it has cells left, so it
 *     draws that old record again, with the tile and row it had.
 *
 * A LINE IN TWO STEPS
 * First the chip reads the table: Y, size and link of each entry on the chain, then X and tile of the sprites it found, while the line before is on screen. Only
 * later does it read the tiles. A table write that lands in the meantime reaches some of those reads and not others, which is what the mid-frame table swap does
 * on the lines it lands on. So pr_sprites_fetch reads the table and pr_sprites_draw draws, and between the two the caller applies what landed.
 *
 * WITH NO LIMIT
 * This is what the Pocket core's menu row that lifts the sprite limits does, and the player can flip it while the game runs. It walks the table
 * in VRAM itself, up to 128 links, and draws everything on the line. The mask rule holds on the same line only, and there are no ghosts. It acts in the 320 pixel mode
 * only: in the 256 pixel mode that row leaves the chip's own pixels on screen, and so does this. The game cannot tell which is in use.
 */
#include "pr_local.h"

#define RECORDS 20u

/* ---- reading the table while writes land */

/* A byte of VRAM at time t, with what has landed by then. */
static unsigned table_byte(const PcVdpState *s, const PrLanding *landings, unsigned n, unsigned address, uint32_t t)
{
    unsigned value = s->vram[address & 0xFFFFu], i;
    for (i = 0; i < n; i++)
        if (landings[i].time < t && landings[i].kind == PC_WRITE_VRAM && landings[i].addr == (address & 0xFFFFu))
            value = landings[i].value;
    return value;
}

/* The chip's copy of an entry, the same. The copy follows writes to the table's first four bytes of each entry. */
static void copy_entry(const PcVdpState *s, const PrLanding *landings, unsigned n, unsigned table, unsigned entry, uint32_t t, uint8_t *out)
{
    memcpy(out, s->sat_cache[entry], 4);
    if (n) {
        unsigned i;
        for (i = 0; i < n; i++) {
            unsigned a = landings[i].addr;
            if (landings[i].time < t && landings[i].kind == PC_WRITE_VRAM && a >= table + entry * 8u && a < table + entry * 8u + 4u)
                out[a & 3u] = (uint8_t)landings[i].value;
        }
    }
}

/* ---- drawing */

/* Writes one cell into the buffer. keep_transparent: a transparent pixel writes its bits too, where nothing opaque is there yet. */
static void draw_cell(uint8_t *buffer, unsigned width, unsigned x, const uint8_t *row, unsigned bits, unsigned flip, int keep_transparent)
{
    unsigned i;
    for (i = 0; i < 8; i++, x = (x + 1u) & 0x1FFu) {
        unsigned nibble;
        if (x >= width || (buffer[x] & 15u))
            continue;
        if (flip)
            nibble = (i & 1u) ? row[3u - i / 2u] >> 4 : row[3u - i / 2u] & 15u;
        else
            nibble = (i & 1u) ? row[i / 2u] & 15u : row[i / 2u] >> 4;
        if (nibble || keep_transparent)
            buffer[x] = (uint8_t)(bits | nibble);
    }
}

static inline unsigned first_cell_x(const PrRecord *r)
{
    /* Flipped, the first cell fetched is the rightmost on screen */
    return ((unsigned)r->x + 512u - 128u + (r->flip ? (unsigned)r->cols * 8u : 0u)) & 0x1FFu;
}

/* Goes through the cells of a sprite while the budget lasts, the way the chip's pattern fetch does. masked: count them, draw nothing. */
static void draw_record(const uint8_t *vram, const PrRecord *r, unsigned *cells, int masked, uint8_t *buffer, unsigned width)
{
    unsigned x = first_cell_x(r), tile = r->tile, col;
    for (col = 0; col <= r->cols; col++) {
        if (*cells == 0) {
            if (!PR_PLANT(5))
                return;
        } else {
            *cells -= 1u;
        }
        if (!masked)
            draw_cell(buffer, width, x, &vram[(tile & 0x7FFu) * 32u + r->row * 4u], r->bits, r->flip, !PR_PLANT(9));
        tile += r->rows + 1u;
        x = (x + (r->flip ? 512u - 8u : 8u)) & 0x1FFu;
    }
}

/* Fills a record from an entry's second long and the offset of this line inside the sprite. */
static void fill_record(PrRecord *r, unsigned attr, unsigned x, unsigned size, unsigned offset)
{
    unsigned rows = size & 3u, vflip = (attr >> 12) & 1u, cell_row = offset >> 3;
    r->x = (uint16_t)(x & 0x1FFu);
    r->rows = (uint8_t)rows;
    r->cols = (uint8_t)(size >> 2);
    r->flip = (uint8_t)((attr >> 11) & 1u);
    r->bits = (uint8_t)(((attr >> 15) << 6) | (((attr >> 13) & 3u) << 4));
    r->tile = (uint16_t)((attr + (vflip ? (rows - cell_row) & 3u : cell_row)) & 0x7FFu);
    r->row = (uint8_t)(vflip ? ~offset & 7u : offset & 7u);
}

/* ---- reading the table */

/* When the chip reads X and the tile of the j-th sprite it found, of n, in master clocks since the V counter took the value of the line before. Its 20 reads (16 in the
   256 pixel mode) come one per two cells across the line, and the sprites take the last n of them. */
static uint32_t attribute_time(unsigned h40, unsigned j, unsigned n)
{
    if (PR_PLANT(26))
        return 891u;
    return h40 ? 891u + 128u * (20u - n + j) : 899u + 160u * (16u - n + j);
}

/* When the chip reads its copy of the entry the walk comes to after k others, in master clocks since the V counter took the value of the line before. It reads about one
   entry a dot through the blank, and the only access slots a write can land on in that stretch are the blank's: one in the 320 pixel mode, which comes after 34 entries, and
   two in the 256 pixel mode, placed here by the same rate and not measured. So this needs to place an entry only on the right side of those slots. */
static uint32_t walk_time(unsigned h40, unsigned k)
{
    if (PR_PLANT(28))
        return 0;
    if (h40)
        return k < 34u ? 300u : 400u;
    return k < 16u ? 200u : k < 43u ? 400u : 600u;
}

void pr_sprites_fetch(const PcVdpState *s, PrWalk *w, int line, PrLimits limits, const PrLanding *landings, unsigned n_landings)
{
    const unsigned h40 = s->regs[12] & 1u;
    const unsigned most_entries = h40 ? 80u : 64u;
    const unsigned most_sprites = PR_PLANT(6) ? 19u : (h40 ? 20u : 16u), most_cells = h40 ? 40u : 32u;
    const unsigned table = (unsigned)(s->regs[5] & (h40 ? 0x7Eu : 0x7Fu)) << 9;
    const uint32_t before = (uint32_t)(line - 1) * PC_LINE_CLOCKS;
    unsigned link = 0, walked = 0, found = 0, cells = 0, over = 0;
    PrFound list[20];
    unsigned j;

    for (;;) {
        uint8_t e[4];
        unsigned y, size, next, offset;
        if (n_landings && link < PC_SAT_ENTRIES) {
            copy_entry(s, landings, n_landings, table, link, before + walk_time(h40, walked), e);
        } else if (link < PC_SAT_ENTRIES) {
            memcpy(e, s->sat_cache[link], 4);
        } else {
            /* Past the end of the copy the chip reads a mix of what it read last: the output so far, and what it last read for an entry with the same low two bits */
            uint32_t mixed = w->memory->out & w->memory->lane[link & 3u];
            e[0] = (uint8_t)(mixed >> 24);
            e[1] = (uint8_t)(mixed >> 16);
            e[2] = (uint8_t)(mixed >> 8);
            e[3] = (uint8_t)mixed;
        }
        w->memory->out = (uint32_t)e[0] << 24 | (uint32_t)e[1] << 16 | (uint32_t)e[2] << 8 | e[3];
        if (link < PC_SAT_ENTRIES)
            w->memory->lane[link & 3u] = w->memory->out;
        y = ((unsigned)e[0] << 8 | e[1]) & 0x1FFu;
        size = e[2] & 15u;
        next = e[3] & 0x7Fu;
        offset = ((unsigned)line + 128u - y) & 0x1FFu;
        walked++;
        if (offset < ((size & 3u) + 1u) * 8u) {
            if (found == most_sprites) {
                over = 1;
                break;
            }
            list[found].entry = (uint8_t)link;
            list[found].size = (uint8_t)size;
            list[found].offset = (uint8_t)offset;
            found++;
            cells += (size >> 2) + 1u;
            if (cells > most_cells)
                over = 1;
        }
        if (next == 0 || walked == most_entries)
            break;
        link = next;
    }
    w->found = (uint8_t)found;
    w->over = (uint8_t)over;

    if (limits == PR_LIMITS_NONE && h40) {
        /* No limit reads the table when it draws, so keep what the table was and what lands on it */
        w->free_copy = 0;
        if (n_landings) {
            memcpy(w->table_copy, &s->vram[table], sizeof(w->table_copy));
            w->free_landings = landings;
            w->free_n_landings = n_landings;
            w->free_copy = 1;
        }
        return;
    }

    /* X and the tile of each sprite found */
    for (j = 0; j < found; j++) {
        const unsigned e = table + list[j].entry * 8u;
        unsigned attr, x, size = list[j].size;
        if (n_landings) {
            /* The chip takes the size from its copy again at this read, so a table write between its two reads gives a sprite the new size at the old line offset */
            uint32_t t = before + attribute_time(h40, j, found);
            if (list[j].entry < PC_SAT_ENTRIES) {
                uint8_t again[4];
                copy_entry(s, landings, n_landings, table, list[j].entry, t, again);
                size = again[2] & 15u;
            }
            attr = table_byte(s, landings, n_landings, e + 4u, t) << 8 | table_byte(s, landings, n_landings, e + 5u, t);
            x = table_byte(s, landings, n_landings, e + 6u, t) << 8 | table_byte(s, landings, n_landings, e + 7u, t);
        } else {
            attr = pr_word(s->vram, e + 4u);
            x = pr_word(s->vram, e + 6u);
        }
        fill_record(&w->fetched[j], attr, x, size, list[j].offset);
    }
}

/* ---- drawing what was fetched */

/* A byte of the table as it stood before this line's writes, plus those landed by time t. */
static unsigned table_byte_from(const PrWalk *w, unsigned table, unsigned address, uint32_t t)
{
    unsigned value = w->table_copy[(address - table) & 1023u], i;
    for (i = 0; i < w->free_n_landings; i++)
        if (w->free_landings[i].time < t && w->free_landings[i].kind == PC_WRITE_VRAM && w->free_landings[i].addr == address)
            value = w->free_landings[i].value;
    return value;
}

static void draw_cart(const PcVdpState *s, PrWalk *w, uint8_t *buffer)
{
    PrMemory *m = w->memory;
    const unsigned h40 = s->regs[12] & 1u;
    const unsigned width = h40 ? 320u : 256u;
    unsigned cells = PR_PLANT(7) ? 41u : (h40 ? 40u : 32u);
    int seen_nz = !PR_PLANT(4) && m->previous_nz, masked = 0;
    unsigned j, next_record;

    for (j = 0; j < w->found; j++) {
        PrRecord drawn = w->fetched[j];
        PrRecord *r = &m->records[j];
        *r = drawn;
        if (cells == 0)
            continue;   /* out of cells: the chip still fetched this record and never draws it, so it keeps its X */
        r->x = 0;
        m->last_nz = drawn.x != 0;
        if (drawn.x == 0) {
            if (seen_nz && !PR_PLANT(3))
                masked = 1;
        } else {
            seen_nz = 1;
        }
        draw_record(s->vram, &drawn, &cells, masked, buffer, width);
    }

    /* Cells left over go on to the next records. Each holds X = 0 once it has been drawn, so normally that draws nothing on screen, but a record that
       was fetched and never drawn still holds its X, and this is where it comes back. */
    next_record = w->found;
    while (cells > 0) {
        PrRecord now;
        if (next_record >= RECORDS) {
            m->last_nz = 0;
            break;
        }
        now = m->records[next_record];
        m->records[next_record].x = 0;
        m->last_nz = now.x != 0;
        if (now.x == 0) {
            if (seen_nz)
                masked = 1;
        } else {
            seen_nz = 1;
        }
        draw_record(s->vram, &now, &cells, masked || (PR_PLANT(8) && now.x != 0), buffer, width);
        next_record++;
    }
    m->previous_nz = m->last_nz;
}

static void draw_free(const PcVdpState *s, const PrWalk *w, int line, uint8_t *buffer)
{
    const unsigned table = (unsigned)(s->regs[5] & 0x7Eu) << 9;
    unsigned link = 0;
    int seen_nz = 0;
    uint32_t visited[4] = { 1u, 0, 0, 0 };

    for (;;) {
        const uint8_t *e = &s->vram[table + link * 8u];
        unsigned y = ((unsigned)e[0] << 8 | e[1]) & 0x1FFu;
        unsigned size = e[2] & 15u, next = e[3] & 0x7Fu;
        unsigned offset = ((unsigned)line + 128u - y) & 0x1FFu;
        if (PR_PLANT(10) && link < PC_SAT_ENTRIES) {
            y = ((unsigned)s->sat_cache[link][0] << 8 | s->sat_cache[link][1]) & 0x1FFu;
            offset = ((unsigned)line + 128u - y) & 0x1FFu;
        }
        if (offset < ((size & 3u) + 1u) * 8u) {
            PrRecord r;
            unsigned x, tile, col;
            fill_record(&r, (unsigned)e[4] << 8 | e[5], (unsigned)e[6] << 8 | e[7], size, offset);
            if (r.x == 0 && seen_nz && !PR_PLANT(3))
                return;
            if (r.x != 0)
                seen_nz = 1;
            x = first_cell_x(&r);
            tile = r.tile;
            for (col = 0; col <= r.cols; col++) {
                draw_cell(buffer, 320u, x, &s->vram[(tile & 0x7FFu) * 32u + r.row * 4u], r.bits, r.flip, 0);
                tile += r.rows + 1u;
                x = (x + (r.flip ? 512u - 8u : 8u)) & 0x1FFu;
            }
        }
        if (next == 0 || (visited[next >> 5] >> (next & 31u)) & 1u)
            return;
        visited[next >> 5] |= 1u << (next & 31u);
        link = next;
    }
}

/* ---- no limit, on a line where table writes land */

/* The Pocket core's renderer reads the table as it goes, so on the lines the mid-frame table swap lands on it sees part of the old table and part of the new. This is the same
   walk with the renderer's own progress counted, as the exact chip bench shows it, in its clocks, two to a master clock since the V counter stepped. An off-line entry takes 6
   clocks. Of each 256 clocks 32 give it no reads, around the chip's access slots. */
#define FREE_FIRST_READ 1866u
static unsigned free_read_time(unsigned clocks)
{
    unsigned phase = (clocks + 25600u - 1900u) & 255u;
    return phase < 32u ? clocks + 32u - phase : clocks;
}

static void draw_free_timed(const PcVdpState *s, const PrWalk *w, int line, uint8_t *buffer)
{
    const unsigned table = (unsigned)(s->regs[5] & 0x7Eu) << 9;
    const uint32_t before = (uint32_t)(line - 1) * PC_LINE_CLOCKS;
    unsigned link = 0, clocks = FREE_FIRST_READ;
    int seen_nz = 0;
    uint32_t visited[4] = { 1u, 0, 0, 0 };

/* A table byte as the renderer reads it now */
#define TABLE_BYTE(a) table_byte_from(w, table, (a), before + clocks / 2u)
    for (;;) {
        unsigned e = table + link * 8u, y, size, next, offset;
        clocks = free_read_time(clocks);
        y = (TABLE_BYTE(e) << 8 | TABLE_BYTE(e + 1u)) & 0x1FFu;
        size = TABLE_BYTE(e + 2u) & 15u;
        next = TABLE_BYTE(e + 3u) & 0x7Fu;
        offset = ((unsigned)line + 128u - y) & 0x1FFu;
        clocks += 3u;
        if (offset < ((size & 3u) + 1u) * 8u) {
            PrRecord r;
            unsigned x, tile, col, attr, screen;
            clocks = free_read_time(clocks + 2u);
            attr = TABLE_BYTE(e + 4u) << 8 | TABLE_BYTE(e + 5u);
            screen = TABLE_BYTE(e + 6u) << 8 | TABLE_BYTE(e + 7u);
            clocks += 3u;
            fill_record(&r, attr, screen, size, offset);
            if (r.x == 0 && seen_nz)
                return;
            if (r.x != 0)
                seen_nz = 1;
            x = first_cell_x(&r);
            tile = r.tile;
            for (col = 0; col <= r.cols; col++) {
                const uint8_t *row = &s->vram[(tile & 0x7FFu) * 32u + r.row * 4u];
                unsigned i, cx = x;
                clocks = free_read_time(clocks + 2u) + 2u;
                for (i = 0; i < 8u; i++, cx = (cx + 1u) & 0x1FFu) {
                    unsigned nibble = r.flip ? ((i & 1u) ? row[3u - i / 2u] >> 4 : row[3u - i / 2u] & 15u) : ((i & 1u) ? row[i / 2u] & 15u : row[i / 2u] >> 4);
                    clocks += (cx < 320u && nibble) ? 3u : 2u;
                }
                clocks += 1u;
                draw_cell(buffer, 320u, x, row, r.bits, r.flip, 0);
                tile += r.rows + 1u;
                x = (x + (r.flip ? 512u - 8u : 8u)) & 0x1FFu;
            }
        }
        clocks += 3u;
        if (next == 0 || (visited[next >> 5] >> (next & 31u)) & 1u)
            return;
        visited[next >> 5] |= 1u << (next & 31u);
        link = next;
    }
#undef TABLE_BYTE
}

int pr_sprites_draw(const PcVdpState *s, PrWalk *w, int line, PrLimits limits, uint8_t *buffer)
{
    if (limits == PR_LIMITS_NONE && (s->regs[12] & 1u)) {
        if (w->free_copy)
            draw_free_timed(s, w, line, buffer);
        else
            draw_free(s, w, line, buffer);
    } else
        draw_cart(s, w, buffer);
    return w->over;
}
