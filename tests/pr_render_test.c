/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The renderer's tests, on scenes made up here. No byte of any game is in them.
 *
 * Each test builds a scene, renders it, and checks pixels against an answer the test works out by its own simple rule, not through the renderer. Then the same
 * tests run again with one rule broken in the renderer at a time (a planted bug): the test named for that plant must fail, or the test is not testing anything.
 * The plants exist only in this test build. The program itself compiles them away.
 *
 *   pr_render_test                 runs the tests and the plants
 *   pr_render_test --export DIR    also writes each scene to DIR as a state file the video chip's bench can play, so the chip itself can check the answers
 *
 * Build: gcc -DPR_TEST_PLANTS -Itargets/pc tests/pr_render_test.c targets/pc/pr_render.c targets/pc/pr_planes.c targets/pc/pr_sprites.c
 */
#include "pr_local.h"

#include <stdarg.h>
#include <stdlib.h>

static PcFrame frame;
static PcPicture picture, picture2;
static int failures, quiet, n_checks;
static const char *export_dir;
static const char *current_test;

#define SAT_ADDR    0xF000u
#define HS_ADDR     0xF400u
#define PLANE_A     0xC000u
#define PLANE_B     0xE000u
#define WINDOW      0xB000u

/* ---- checks */

static void fail(const char *fmt, ...)
{
    va_list ap;
    failures++;
    if (quiet)
        return;
    printf("FAIL %s: ", current_test);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

/* The colour levels, typed here apart from the renderer's table. */
static const uint8_t test_normal[8]    = { 0, 49, 84, 114, 142, 172, 206, 255 };
static const uint8_t test_shadow[8]    = { 0, 27, 49, 67, 84, 100, 114, 128 };
static const uint8_t test_highlight[8] = { 128, 142, 156, 172, 188, 206, 228, 255 };

enum { NORMAL, SHADOW, HIGHLIGHT };

static uint32_t colour_of(unsigned index, int shade)
{
    unsigned e = frame.start.cram[index & 63u];
    unsigned r = (e >> 1) & 7u, g = (e >> 5) & 7u, b = (e >> 9) & 7u;
    const uint8_t *t = shade == SHADOW ? test_shadow : shade == HIGHLIGHT ? test_highlight : test_normal;
    return (uint32_t)t[r] << 16 | (uint32_t)t[g] << 8 | t[b];
}

static uint32_t grey_of(unsigned level)
{
    return (uint32_t)level << 16 | (uint32_t)level << 8 | level;
}

static void want_rgb(int line, int x, uint32_t want, const char *what)
{
    n_checks++;
    if (picture.rgb[line][x] != want)
        fail("%s: line %d x %d: %06X, want %06X", what, line, x, picture.rgb[line][x], want);
}

static void want_colour(int line, int x, unsigned index, const char *what)
{
    want_rgb(line, x, colour_of(index, NORMAL), what);
}

/* ---- building a scene */

static void put_word(unsigned address, unsigned value)
{
    frame.start.vram[address & 0xFFFFu] = (uint8_t)(value >> 8);
    frame.start.vram[(address + 1u) & 0xFFFFu] = (uint8_t)value;
}

/* A tile with one colour in all its pixels. */
static void tile_solid(unsigned tile, unsigned colour)
{
    unsigned i;
    for (i = 0; i < 32; i++)
        frame.start.vram[tile * 32u + i] = (uint8_t)(colour * 0x11u);
}

/* Colour by row: row r gets colours[r]. */
static void tile_rows(unsigned tile, const uint8_t *colours)
{
    unsigned r, i;
    for (r = 0; r < 8; r++)
        for (i = 0; i < 4; i++)
            frame.start.vram[tile * 32u + r * 4u + i] = (uint8_t)(colours[r] * 0x11u);
}

/* Colour by column: column c gets colours[c]. */
static void tile_cols(unsigned tile, const uint8_t *colours)
{
    unsigned r, c;
    for (r = 0; r < 8; r++)
        for (c = 0; c < 8; c += 2)
            frame.start.vram[tile * 32u + r * 4u + c / 2u] = (uint8_t)(colours[c] << 4 | colours[c + 1u]);
}

static void map_word(unsigned base, unsigned width_cells, unsigned row, unsigned col, unsigned word)
{
    put_word(base + (row * width_cells + col) * 2u, word);
}

/* Writes an entry to the table in VRAM and to the chip's copy. */
static void sprite(unsigned index, unsigned line, unsigned link, unsigned cols, unsigned rows, unsigned attr, unsigned screen_x)
{
    unsigned e = SAT_ADDR + index * 8u;
    unsigned y = line + 128u, x = screen_x + 128u, size = ((cols - 1u) << 2) | (rows - 1u);
    put_word(e, y);
    frame.start.vram[e + 2u] = (uint8_t)size;
    frame.start.vram[e + 3u] = (uint8_t)link;
    put_word(e + 4u, attr);
    put_word(e + 6u, x);
    if (index < PC_SAT_ENTRIES) {
        frame.start.sat_cache[index][0] = (uint8_t)(y >> 8);
        frame.start.sat_cache[index][1] = (uint8_t)y;
        frame.start.sat_cache[index][2] = (uint8_t)size;
        frame.start.sat_cache[index][3] = (uint8_t)link;
    }
}

/* An entry with X = 0, which masks. */
static void sprite_mask(unsigned index, unsigned line, unsigned link, unsigned cols, unsigned rows, unsigned attr)
{
    sprite(index, line, link, cols, rows, attr, 0);
    put_word(SAT_ADDR + index * 8u + 6u, 0);
}

static void set_link(unsigned index, unsigned link)
{
    frame.start.vram[SAT_ADDR + index * 8u + 3u] = (uint8_t)link;
    if (index < PC_SAT_ENTRIES)
        frame.start.sat_cache[index][3] = (uint8_t)link;
}

static void scene_start(const char *name)
{
    PcVdpState *s = &frame.start;
    unsigned i;
    current_test = name;
    memset(s, 0, sizeof(*s));
    frame.number = 1;
    frame.n_writes = 0;
    frame.dropped = 0;
    s->regs[0] = 0x04;
    s->regs[1] = 0x44;
    s->regs[2] = (uint8_t)(PLANE_A >> 10);
    s->regs[3] = (uint8_t)(WINDOW >> 10);
    s->regs[4] = (uint8_t)(PLANE_B >> 13);
    s->regs[5] = (uint8_t)(SAT_ADDR >> 9);
    s->regs[7] = 0;
    s->regs[12] = 0x81;
    s->regs[13] = (uint8_t)(HS_ADDR >> 10);
    s->regs[15] = 2;
    s->regs[16] = 0x01;     /* 64 by 32 cells */
    /* Every colour its own: red and green spell the colour number */
    for (i = 0; i < PC_CRAM_WORDS; i++)
        s->cram[i] = (uint16_t)((i & 7u) << 1 | ((i >> 3) & 7u) << 5 | ((i + 1u) & 7u) << 9);
    /* Entry 0 parked off the screen, link 0: an empty table */
    sprite(0, 300, 0, 1, 1, 0, 400);
}

static int export_count;

/* The scene as a state file the bench can play. The table goes in as it is in VRAM, so a scene whose chip copy differs from VRAM is not exported. */
static void export_scene(void)
{
    char path[512];
    FILE *f;
    uint32_t head[3], h[2];
    unsigned i;
    uint8_t cram[128], vsram[80], regs[32], stash[112];
    if (!export_dir)
        return;
    snprintf(path, sizeof(path), "%s/%s_%d.bin", export_dir, current_test, export_count++);
    f = fopen(path, "wb");
    if (!f)
        return;
    head[0] = head[1] = 0;
    head[2] = 1;
    h[0] = 1;
    h[1] = 0;
    for (i = 0; i < PC_CRAM_WORDS; i++) {
        cram[i * 2] = (uint8_t)(frame.start.cram[i] >> 8);
        cram[i * 2 + 1] = (uint8_t)frame.start.cram[i];
    }
    for (i = 0; i < PC_VSRAM_WORDS; i++) {
        vsram[i * 2] = (uint8_t)(frame.start.vsram[i] >> 8);
        vsram[i * 2 + 1] = (uint8_t)frame.start.vsram[i];
    }
    memset(regs, 0, sizeof(regs));
    memcpy(regs, frame.start.regs, PC_VDP_REGS);
    memset(stash, 0, sizeof(stash));
    fwrite(head, 4, 3, f);
    fwrite(h, 4, 2, f);
    fwrite(regs, 1, 32, f);
    fwrite(cram, 1, 128, f);
    fwrite(vsram, 1, 80, f);
    fwrite(frame.start.vram, 1, 65536, f);
    fwrite(stash, 1, 112, f);
    fclose(f);
    if (frame.n_writes) {
        snprintf(path, sizeof(path), "%s/%s_%d.script", export_dir, current_test, export_count - 1);
        f = fopen(path, "w");
        if (f) {
            for (i = 0; i < frame.n_writes; i++) {
                const PcWrite *w = &frame.writes[i];
                fprintf(f, "0 %u %u %u %u %u\n", w->vline, w->hclock, w->kind, w->addr, w->value);
            }
            fclose(f);
        }
    }
}

static void render(PrLimits limits)
{
    PrOptions o;
    o.limits = limits;
    o.want_layer = 1;
    if (limits == PR_LIMITS_CART)
        export_scene();
    pr_render(&frame, &o, &picture);
}

/* ---- the tests */

/* The answer to "which layer shows", by the hardware manuals' rule: a layer with priority beats one without; among equals sprite, then A, then B. 0 backdrop, 1 sprite, 2 A, 3 B */
static int top_layer(int ps, int pa, int pb, int os, int oa, int ob)
{
    if (os && ps) return 1;
    if (oa && pa) return 2;
    if (ob && pb) return 3;
    if (os) return 1;
    if (oa) return 2;
    if (ob) return 3;
    return 0;
}

/* Every mix of priority and of transparent or opaque, one 2-cell block for each. Plant 1: a sprite with priority under a plane with priority. Plant 2: B above A. */
static void test_priority(void)
{
    unsigned o, c, n = 1;
    scene_start("priority");
    frame.start.regs[7] = 2;
    tile_solid(1, 1);
    for (o = 0; o < 8; o++) {
        for (c = 0; c < 8; c++) {
            unsigned pa = c & 1u, pb = (c >> 1) & 1u, ps = (c >> 2) & 1u;
            unsigned ta = o & 1u ? 1u : 0u, tb = o & 2u ? 1u : 0u;
            map_word(PLANE_A, 64, o, c * 2u, pa << 15 | 1u << 13 | ta);
            map_word(PLANE_A, 64, o, c * 2u + 1u, pa << 15 | 1u << 13 | ta);
            map_word(PLANE_B, 64, o, c * 2u, pb << 15 | 2u << 13 | tb);
            map_word(PLANE_B, 64, o, c * 2u + 1u, pb << 15 | 2u << 13 | tb);
            if (o & 4u) {
                sprite(n, o * 8u, n + 1u, 2, 1, ps << 15 | 3u << 13 | 1u, c * 16u);
                n++;
            }
        }
    }
    set_link(n - 1u, 0);
    set_link(0, 1);
    render(PR_LIMITS_CART);
    for (o = 0; o < 8; o++) {
        for (c = 0; c < 8; c++) {
            unsigned pa = c & 1u, pb = (c >> 1) & 1u, ps = (c >> 2) & 1u;
            int top = top_layer((int)ps, (int)pa, (int)pb, (o >> 2) & 1u, o & 1u, (o >> 1) & 1u);
            static const unsigned colour[4] = { 2, 3u * 16u + 1u, 1u * 16u + 1u, 2u * 16u + 1u };
            want_colour((int)o * 8 + 3, (int)c * 16 + 7, colour[top], "the layer that shows");
        }
    }
}

/* The first opaque pixel on the chain wins, whatever its priority, and a transparent pixel lets the next sprite through. */
static void test_sprite_order(void)
{
    static const uint8_t half[8] = { 5, 5, 5, 5, 0, 0, 0, 0 };
    scene_start("sprite_order");
    tile_solid(1, 6);
    tile_cols(2, half);
    sprite(0, 10, 1, 1, 1, 0u << 15 | 2u, 50);      /* no priority, left half opaque */
    sprite(1, 10, 0, 1, 1, 1u << 15 | 1u << 13 | 1u, 50);    /* priority, solid */
    render(PR_LIMITS_CART);
    want_colour(12, 51, 5, "the first sprite's opaque pixel wins");
    want_colour(12, 55, 16 + 6, "through the first sprite's transparent pixel");
    render(PR_LIMITS_NONE);
    want_colour(12, 51, 5, "the first sprite's opaque pixel wins, no limit");
    want_colour(12, 55, 16 + 6, "through the first sprite's transparent pixel, no limit");
}

/* Flips and sizes: a sprite's cells run down the columns, and a flip takes the whole sprite. */
static void test_sprite_shape(void)
{
    static const uint8_t rows_up[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    static const uint8_t cols_up[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    unsigned t;
    scene_start("sprite_shape");
    /* A 2 by 2 sprite at tile 10: the four cells get colours 1, 2 (first column) and 3, 4 (second) */
    for (t = 0; t < 4; t++)
        tile_solid(10u + t, t + 1u);
    tile_rows(20, rows_up);
    tile_cols(21, cols_up);
    sprite(0, 20, 1, 2, 2, 10, 40);
    sprite(1, 20, 2, 2, 2, 0x0800u | 10u, 80);      /* H flip */
    sprite(2, 20, 3, 2, 2, 0x1000u | 10u, 120);     /* V flip */
    sprite(3, 40, 4, 1, 1, 20, 40);
    sprite(4, 40, 5, 1, 1, 0x1000u | 20u, 60);      /* V flip within a tile */
    sprite(5, 60, 0, 1, 1, 0x0800u | 21u, 40);      /* H flip within a tile */
    render(PR_LIMITS_CART);
    want_colour(20, 40, 1, "top left");
    want_colour(28, 40, 2, "bottom left");
    want_colour(20, 48, 3, "top right");
    want_colour(28, 48, 4, "bottom right");
    want_colour(20, 80, 3, "H flip top left");
    want_colour(20, 88, 1, "H flip top right");
    want_colour(28, 120, 1, "V flip bottom left");
    want_colour(20, 120, 2, "V flip top left");
    want_colour(43, 40, 4, "row 3 of the tile");
    want_colour(43, 60, 5, "row 3 of the tile V flipped");
    want_colour(60, 40, 8, "H flip within a tile, left pixel");
    want_colour(60, 47, 1, "H flip within a tile, right pixel");
}

/* The mask rule on one line. Plant 3: no mask. */
static void test_mask(void)
{
    scene_start("mask");
    tile_solid(1, 1);
    tile_solid(2, 2);
    sprite(0, 10, 1, 1, 1, 1, 40);
    sprite_mask(1, 10, 2, 1, 1, 0);
    sprite(2, 10, 3, 1, 1, 2, 80);          /* after the mask: hidden */
    sprite_mask(3, 30, 4, 1, 1, 0);         /* first on its line: masks nothing */
    sprite(4, 30, 0, 1, 1, 2, 80);
    render(PR_LIMITS_CART);
    want_colour(12, 40, 1, "before the mask");
    want_colour(12, 80, 0, "after the mask");
    want_colour(32, 80, 2, "an X = 0 first on its line masks nothing");
    render(PR_LIMITS_NONE);
    want_colour(12, 40, 1, "before the mask, no limit");
    want_colour(12, 80, 0, "after the mask, no limit");
    want_colour(32, 80, 2, "an X = 0 first on its line masks nothing, no limit");
}

/* The mask's memory of the line before. Plant 4: the walk forgets it. */
static void test_mask_line_before(void)
{
    unsigned i, n = 1;
    scene_start("mask_line_before");
    tile_solid(1, 1);
    tile_solid(2, 2);
    /* Lines 16-23: exactly 40 cells on X other than 0, so the line ends out of cells on such an entry */
    for (i = 0; i < 10; i++, n++)
        sprite(n, 16, n + 1u, 4, 1, 1, i * 32u);
    /* Line 24 on: first an X = 0, then a sprite that must hide on line 24, the only line right after the full ones */
    sprite_mask(n, 24, n + 1u, 1, 1, 0);
    n++;
    sprite(n, 24, 0, 1, 1, 2, 200);
    set_link(0, 1);
    render(PR_LIMITS_CART);
    want_colour(23, 3, 1, "the full line");
    want_colour(24, 200, 0, "hidden by the line before's memory");
    want_colour(25, 200, 2, "the memory lasts one line");
    render(PR_LIMITS_NONE);
    want_colour(24, 200, 2, "no limit: the mask holds on its own line only");
}

/* The cell limit, with the last sprite drawn in part, flipped and not. Plant 5: the last sprite drawn whole. Plant 7: 41 cells. */
static void test_part_drawn(void)
{
    unsigned i, n = 1;
    scene_start("part_drawn");
    tile_solid(1, 1);
    tile_solid(2, 2);
    tile_solid(3, 3);
    tile_solid(4, 4);
    tile_solid(5, 5);
    /* Lines 16-23: 9 sprites of 4 cells = 36, one of 2 = 38, then one of 4 cells of which 2 are drawn. The first ten sit past the right edge, where they still cost cells */
    for (i = 0; i < 9; i++, n++)
        sprite(n, 16, n + 1u, 4, 1, 1, 372);
    sprite(n, 16, n + 1u, 2, 1, 1, 372);
    n++;
    /* A sprite's cells run down the columns of tiles, so a one-row sprite of 4 cells shows tiles 2, 3, 4, 5 */
    sprite(n, 16, 0, 4, 1, 2, 100);
    set_link(0, 1);
    render(PR_LIMITS_CART);
    want_colour(16, 100, 2, "first cell of the part-drawn sprite");
    want_colour(16, 108, 3, "second cell of the part-drawn sprite");
    want_colour(16, 116, 0, "third cell: out of cells");
    want_colour(16, 124, 0, "fourth cell: out of cells");
    n_checks++;
    if (picture.lines_over != 8)
        fail("lines over the limit: %u, want 8", picture.lines_over);
    render(PR_LIMITS_NONE);
    want_colour(16, 116, 4, "no limit draws the third cell");
    want_colour(16, 124, 5, "no limit draws the fourth cell");
    n_checks++;
    if (picture.lines_over != 8)
        fail("lines over the limit in no limit mode: %u, want 8", picture.lines_over);
}

static void test_part_drawn_flipped(void)
{
    unsigned i, n = 1;
    scene_start("part_drawn_flip");
    tile_solid(1, 1);
    tile_solid(2, 2);
    tile_solid(3, 3);
    tile_solid(4, 4);
    tile_solid(5, 5);
    for (i = 0; i < 9; i++, n++)
        sprite(n, 16, n + 1u, 4, 1, 1, 372);    /* X past the right edge still costs cells */
    sprite(n, 16, n + 1u, 2, 1, 1, 372);
    n++;
    /* Flipped: cell colours 2, 3, 4, 5 from right to left on screen, and the two fetched first are the rightmost */
    sprite(n, 16, 0, 4, 1, 0x0800u | 2u, 100);
    set_link(0, 1);
    render(PR_LIMITS_CART);
    want_colour(16, 124, 2, "rightmost cell");
    want_colour(16, 116, 3, "next cell");
    want_colour(16, 108, 0, "out of cells");
    want_colour(16, 100, 0, "out of cells");
}

/* The sprite limit. Plant 6: 19 sprites. */
static void test_twenty_sprites(void)
{
    unsigned i, n = 1;
    scene_start("twenty_sprites");
    tile_solid(1, 1);
    tile_solid(2, 2);
    for (i = 0; i < 20; i++, n++)
        sprite(n, 16, n + 1u, 1, 1, 1, i * 10u);
    sprite(n, 16, 0, 1, 1, 2, 300);         /* the 21st */
    set_link(0, 1);
    render(PR_LIMITS_CART);
    want_colour(18, 190, 1, "the 20th sprite");
    want_colour(18, 300, 0, "the 21st sprite");
    n_checks++;
    if (picture.lines_over != 8)
        fail("lines over the limit: %u, want 8", picture.lines_over);
    render(PR_LIMITS_NONE);
    want_colour(18, 300, 2, "no limit draws the 21st");
}

/* The ghost: a record fetched but not drawn comes back on a later line with cells to spare. Plant 8: no ghost. */
static void test_ghost(void)
{
    static const uint8_t rows[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    unsigned i, n = 1;
    scene_start("ghost");
    tile_solid(1, 9);
    tile_rows(2, rows);
    /* Lines 16-23: 10 sprites of 4 cells use up the 40, so the next one is fetched and not drawn, record 10 */
    for (i = 0; i < 10; i++, n++)
        sprite(n, 16, n + 1u, 4, 1, 1, 372);
    sprite(n, 16, n + 1u, 1, 1, 2, 200);
    n++;
    /* Lines 24-31: 10 narrow sprites overwrite records 0 to 9 and leave cells over, which go on to record 10 */
    for (i = 0; i < 10; i++, n++)
        sprite(n, 24, n + 1u, 1, 1, 1, 372);
    set_link(n - 1u, 0);
    set_link(0, 1);
    render(PR_LIMITS_CART);
    want_colour(23, 200, 0, "not drawn on its own line");
    want_colour(24, 200, 8, "the ghost: row 7 of its tile, from the last line it was fetched");
    want_colour(25, 200, 0, "drawn once: its X is now clear");
    render(PR_LIMITS_NONE);
    want_colour(23, 200, 8, "no limit: drawn on its own line");
    want_colour(24, 200, 0, "no limit has no ghost");
}

/* The sprite layer keeps the chip's seven bits, a transparent pixel's priority and palette too. Plant 9: transparent pixels not written. */
static void test_layer_bits(void)
{
    static const uint8_t half[8] = { 5, 5, 5, 5, 0, 0, 0, 0 };
    scene_start("layer_bits");
    tile_cols(1, half);
    sprite(0, 10, 0, 1, 1, 1u << 15 | 2u << 13 | 1u, 50);
    render(PR_LIMITS_CART);
    n_checks += 2;
    if (picture.sprite_layer[12][50] != (1u << 6 | 2u << 4 | 5u))
        fail("opaque pixel in the layer: %02X", picture.sprite_layer[12][50]);
    if (picture.sprite_layer[12][56] != (1u << 6 | 2u << 4))
        fail("transparent pixel in the layer: %02X", picture.sprite_layer[12][56]);
    render(PR_LIMITS_NONE);
    n_checks++;
    if (picture.sprite_layer[12][56] != 0)
        fail("no limit writes no transparent pixel: %02X", picture.sprite_layer[12][56]);
}

/* No limit reads Y, size and link from VRAM, past entry 79 too; the chip reads its own copy and stops at 80. Plant 10: no limit reads the copy. */
static void test_no_limit_walk(void)
{
    scene_start("no_limit_walk");
    tile_solid(1, 1);
    tile_solid(2, 2);
    sprite(0, 300, 1, 1, 1, 0, 400);
    /* Entry 1 is on line 10 in VRAM, and on line 40 in the chip's copy */
    sprite(1, 10, 90, 1, 1, 1, 40);
    frame.start.sat_cache[1][0] = 0;
    frame.start.sat_cache[1][1] = (uint8_t)(40u + 128u);
    sprite(90, 10, 0, 1, 1, 2, 80);
    current_test = "no_limit_walk_nx";  /* a copy that differs from VRAM cannot be played on the bench */
    {
        const char *keep = export_dir;
        export_dir = NULL;
        render(PR_LIMITS_CART);
        export_dir = keep;
    }
    current_test = "no_limit_walk";
    want_colour(42, 40, 1, "limits on: Y from the chip's copy");
    want_colour(12, 40, 0, "limits on: not where VRAM says");
    render(PR_LIMITS_NONE);
    want_colour(12, 40, 1, "no limit: Y from VRAM");
    want_colour(42, 40, 0, "no limit: not where the copy says");
    want_colour(12, 80, 2, "no limit follows a link past entry 79");
}

/* Registers 11's H scroll modes: whole plane, first eight lines repeating, per cell, per line, for A and B apart. Plant 11: the modes mixed up. Plant 12: A reads B's word. */
static void test_h_scroll_modes(void)
{
    unsigned line, mode;
    for (mode = 0; mode < 4; mode++) {
        static const char *names[4] = { "h_scroll_whole", "h_scroll_8lines", "h_scroll_cell", "h_scroll_line" };
        scene_start(names[mode]);
        frame.start.regs[11] = (uint8_t)mode;
        tile_solid(1, 1);
        tile_solid(2, 2);
        /* One opaque cell in each plane's row map, all rows: A at map column 8, B at map column 24 */
        for (line = 0; line < 32; line++) {
            map_word(PLANE_A, 64, line, 8, 1);
            map_word(PLANE_B, 64, line, 24, 1u << 13 | 2u);
        }
        for (line = 0; line < 224; line++) {
            put_word(HS_ADDR + line * 4u, line + 3u);
            put_word(HS_ADDR + line * 4u + 2u, line / 2u + 5u);
        }
        render(PR_LIMITS_CART);
        for (line = 0; line < 224; line += 13) {
            unsigned index = mode == 0 ? 0 : mode == 1 ? (line & 7u) : mode == 2 ? (line & 0xF8u) : line;
            unsigned sa = index + 3u, sb = index / 2u + 5u;
            /* A's cell starts at map pixel 64, so on screen at 64 + scroll; B's at 192 + scroll */
            want_colour((int)line, (int)(64u + sa), 1, "plane A's first pixel");
            want_colour((int)line, (int)(64u + sa + 7u), 1, "plane A's last pixel");
            want_colour((int)line, (int)(192u + sb), 16u + 2u, "plane B's first pixel");
            want_colour((int)line, (int)(192u + sb + 7u), 16u + 2u, "plane B's last pixel");
            want_colour((int)line, (int)(64u + sa + 8u), 0, "after plane A's cell");
        }
    }
}

/* Register 11's V scroll: a word per 2-cell column, or one for the plane. Plant 13: a column reads the next column's word. */
static void test_v_scroll(void)
{
    unsigned col;
    scene_start("v_scroll_columns");
    frame.start.regs[11] = 4;
    tile_solid(1, 1);
    tile_solid(2, 2);
    /* Plane A has one row of tile 1 at map row 2, across all columns */
    for (col = 0; col < 64; col++)
        map_word(PLANE_A, 64, 2, col, 1);
    /* Column c scrolled up by -4c: the row at map line 16 shows at screen line 16 + 4c */
    for (col = 0; col < 20; col++)
        frame.start.vsram[col * 2u] = (uint16_t)(256u - col * 4u);
    render(PR_LIMITS_CART);
    for (col = 0; col < 20; col++) {
        int top = 16 + (int)col * 4;
        want_colour(top - 1, (int)col * 16 + 7, 0, "a column's own scroll, above it");
        want_colour(top, (int)col * 16 + 7, 1, "a column's own scroll, first row");
        want_colour(top + 7, (int)col * 16 + 7, 1, "a column's own scroll, last row");
        want_colour(top + 8, (int)col * 16 + 7, 0, "a column's own scroll, below it");
    }
    scene_start("v_scroll_whole");
    tile_solid(1, 1);
    for (col = 0; col < 64; col++)
        map_word(PLANE_A, 64, 2, col, 1);
    frame.start.vsram[0] = 5;
    frame.start.vsram[2] = 9;   /* not read in this mode */
    render(PR_LIMITS_CART);
    want_colour(11, 100, 1, "whole plane scroll, first row");
    want_colour(10, 100, 0, "whole plane scroll, above it");
}

/* Fine scroll with the plane's map wrapping at its width, and tile flips in a plane. */
static void test_plane_shape(void)
{
    static const uint8_t cols[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    scene_start("plane_shape");
    tile_cols(1, cols);
    /* Map column 63 of row 0 shows at the left edge when the plane is scrolled right by 3 */
    map_word(PLANE_A, 64, 0, 63, 1);
    map_word(PLANE_A, 64, 1, 0, 0x0800u | 1u);  /* H flip */
    map_word(PLANE_A, 64, 2, 0, 0x1000u | 1u);
    put_word(HS_ADDR, 3);
    render(PR_LIMITS_CART);
    want_colour(0, 0, 6, "map column 63, pixel 5, at screen x 0");
    want_colour(0, 2, 8, "map column 63, pixel 7, at screen x 2");
    want_colour(0, 3, 0, "map column 0 next");
    want_colour(8, 3, 8, "H flip, first pixel");
    want_colour(8, 10, 1, "H flip, last pixel");
    want_colour(16, 3, 1, "V flip keeps the columns");
}

/* The window. Plant 14: top edge off by one cell. Plant 15: left edge off by one column. */
static void test_window(void)
{
    unsigned col, row;
    scene_start("window_left");
    tile_solid(1, 1);
    tile_solid(2, 2);
    frame.start.regs[17] = 3;           /* left of column 3: cells 0-5 */
    for (row = 0; row < 32; row++)
        for (col = 0; col < 64; col++) {
            map_word(PLANE_A, 64, row, col, 1);
            map_word(WINDOW, 64, row, col, 1u << 13 | 2u);
        }
    render(PR_LIMITS_CART);
    want_colour(10, 47, 16 + 2, "last window pixel");
    want_colour(10, 48, 1, "first plane A pixel");

    scene_start("window_right");
    tile_solid(1, 1);
    tile_solid(2, 2);
    frame.start.regs[17] = 0x80u | 3u;  /* right from column 3 */
    for (row = 0; row < 32; row++)
        for (col = 0; col < 64; col++) {
            map_word(PLANE_A, 64, row, col, 1);
            map_word(WINDOW, 64, row, col, 1u << 13 | 2u);
        }
    render(PR_LIMITS_CART);
    want_colour(10, 47, 1, "last plane A pixel");
    want_colour(10, 48, 16 + 2, "first window pixel");

    scene_start("window_top");
    tile_solid(1, 1);
    tile_solid(2, 2);
    frame.start.regs[18] = 5;           /* above cell row 5 */
    for (row = 0; row < 32; row++)
        for (col = 0; col < 64; col++) {
            map_word(PLANE_A, 64, row, col, 1);
            map_word(WINDOW, 64, row, col, 1u << 13 | 2u);
        }
    render(PR_LIMITS_CART);
    want_colour(39, 100, 16 + 2, "last window line");
    want_colour(40, 100, 1, "first plane A line");

    scene_start("window_bottom");
    tile_solid(1, 1);
    tile_solid(2, 2);
    frame.start.regs[18] = 0x80u | 5u;  /* from cell row 5 down */
    for (row = 0; row < 32; row++)
        for (col = 0; col < 64; col++) {
            map_word(PLANE_A, 64, row, col, 1);
            map_word(WINDOW, 64, row, col, 1u << 13 | 2u);
        }
    /* The window does not scroll, plane A does */
    put_word(HS_ADDR, 16);
    frame.start.vsram[0] = 16;
    render(PR_LIMITS_CART);
    want_colour(39, 100, 1, "last plane A line");
    want_colour(40, 100, 16 + 2, "first window line");
}

/* Where a window on the left ends and plane A has a fine scroll, the gap shows the end of plane A's own first column. Plant 30: the gap left empty. */
static void test_window_gap(void)
{
    static const uint8_t cols[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    unsigned col, row;
    scene_start("window_gap");
    tile_cols(1, cols);
    tile_solid(2, 9);
    frame.start.regs[17] = 3;           /* left of column 3 */
    put_word(HS_ADDR, 3);               /* plane A moved 3 to the right */
    for (row = 0; row < 32; row++) {
        for (col = 0; col < 64; col++) {
            map_word(PLANE_A, 64, row, col, 1);
            map_word(WINDOW, 64, row, col, 1u << 13 | 2u);
        }
    }
    render(PR_LIMITS_CART);
    want_colour(10, 47, 16 + 9, "the window's last pixel");
    want_colour(10, 48, 6, "the gap: pixel 13 of plane A's first column, which is pixel 5 of its second tile");
    want_colour(10, 49, 7, "the gap");
    want_colour(10, 50, 8, "the gap's last pixel");
    want_colour(10, 51, 1, "plane A's first pixel, 3 to the right of the column's place");
}

/* The column hidden at the left edge in the per-column V scroll mode has no word of its own. Plant 29: it takes column 0's plane word. */
static void test_hidden_column(void)
{
    unsigned col, row;
    scene_start("hidden_column");
    frame.start.regs[11] = 4;
    tile_solid(1, 1);
    tile_solid(2, 2);
    /* Odd map rows are tile 2, even map rows tile 1 */
    for (row = 0; row < 32; row++)
        for (col = 0; col < 64; col++)
            map_word(PLANE_A, 64, row, col, 1u + (row & 1u));
    put_word(HS_ADDR, 8);
    /* Words 38 and 39 anded give 8, which moves the hidden column one cell, and column 0 has no scroll */
    frame.start.vsram[38] = 0x0018;
    frame.start.vsram[39] = 0x0008;
    render(PR_LIMITS_CART);
    want_colour(3, 4, 2, "the hidden column reads map row 1");
    want_colour(3, 8, 1, "column 0 reads map row 0");

    scene_start("hidden_column_256");
    frame.start.regs[12] = 0x00;
    frame.start.regs[16] = 0x00;
    frame.start.regs[11] = 4;
    tile_solid(1, 1);
    tile_solid(2, 2);
    for (row = 0; row < 32; row++)
        for (col = 0; col < 32; col++)
            map_word(PLANE_A, 32, row, col, 1u + (row & 1u));
    put_word(HS_ADDR, 8);
    frame.start.vsram[35] = 0x0018;
    frame.start.vsram[36] = 0x0008;
    render(PR_LIMITS_CART);
    want_colour(3, 4, 2, "the hidden column reads map row 1");
    want_colour(3, 8, 1, "column 0 reads map row 0");
}

/* Shadow and highlight: every mix of priority, with plain sprites and the two operators. Plants 16-19: each of the rules broken. */
static void test_shadow_highlight(void)
{
    /* Sprite kinds: none, plain low, plain high, operator 14 low, operator 14 high, operator 15 low, operator 15 high, colour 14 of palette 1 low */
    static const struct { unsigned present, high, palette, colour; } kinds[8] = {
        { 0, 0, 0, 0 }, { 1, 0, 1, 3 }, { 1, 1, 1, 3 }, { 1, 0, 3, 14 }, { 1, 1, 3, 14 }, { 1, 0, 3, 15 }, { 1, 1, 3, 15 }, { 1, 0, 1, 14 }
    };
    unsigned row, k, n = 1;
    scene_start("shadow_highlight");
    frame.start.regs[12] = 0x89;
    frame.start.regs[7] = 2;
    tile_solid(1, 1);       /* plane A's colour */
    tile_solid(2, 3);
    tile_solid(3, 14);
    tile_solid(4, 15);
    /* Rows: both planes low; A high; B high; both high. Each with both planes opaque in the first half of the row's columns, and transparent in the second */
    for (row = 0; row < 4; row++) {
        unsigned pa = row & 1u, pb = (row >> 1) & 1u, col;
        for (col = 0; col < 40; col++) {
            unsigned opaque = col < 16;
            map_word(PLANE_A, 64, row * 2u, col, pa << 15 | (opaque ? 1u : 0u));
            map_word(PLANE_B, 64, row * 2u, col, pb << 15 | 2u << 13 | (opaque ? 1u : 0u));
            map_word(PLANE_A, 64, row * 2u + 1u, col, pa << 15 | (opaque ? 1u : 0u));
            map_word(PLANE_B, 64, row * 2u + 1u, col, pb << 15 | 2u << 13 | (opaque ? 1u : 0u));
        }
        for (k = 0; k < 8; k++) {
            if (kinds[k].present) {
                unsigned tile = kinds[k].colour == 3 ? 2u : kinds[k].colour == 14 ? 3u : 4u;
                sprite(n, row * 16u, n + 1u, 2, 2, kinds[k].high << 15 | kinds[k].palette << 13 | tile, k * 16u);
                sprite(n + 1u, row * 16u, n + 2u, 2, 2, kinds[k].high << 15 | kinds[k].palette << 13 | tile, 136u + k * 16u);
                n += 2;
            }
        }
    }
    set_link(n - 1u, 0);
    set_link(0, 1);
    render(PR_LIMITS_CART);
    for (row = 0; row < 4; row++) {
        unsigned pa = row & 1u, pb = (row >> 1) & 1u;
        int side;
        for (side = 0; side < 2; side++) {
            for (k = 0; k < 8; k++) {
                const int opaque = side == 0;
                const int x = (side ? 136 : 0) + (int)k * 16 + 4;
                const int line = (int)row * 16 + 4;
                const unsigned high = kinds[k].high;
                const unsigned present = kinds[k].present;
                const unsigned op = present && kinds[k].palette == 3 && kinds[k].colour >= 14;
                const int sprite_top = present && (high || (!pa && !pb) || (!opaque && (pa || pb)));
                unsigned index;
                int shade;
                /* What is under it */
                if (opaque) {
                    index = (pb && !pa) ? 2u * 16u + 1u : 1u;
                } else {
                    index = 2;
                }
                if (sprite_top && !op)
                    index = kinds[k].palette * 16u + kinds[k].colour;
                /* The shade, by the rules at compose_shaded in the renderer, worked out on its own here */
                if (sprite_top && op) {
                    if (kinds[k].colour == 15)
                        shade = SHADOW;
                    else
                        shade = (pa || pb) ? HIGHLIGHT : NORMAL;
                } else if (pa || pb) {
                    shade = NORMAL;
                } else if (sprite_top && (high || kinds[k].colour == 14)) {
                    shade = NORMAL;
                } else {
                    shade = SHADOW;
                }
                want_rgb(line, x, colour_of(index, shade), "shade");
            }
        }
    }
}

/* The colour levels. Plant 20: one level wrong. */
static void test_levels(void)
{
    unsigned i;
    scene_start("levels");
    frame.start.regs[12] = 0x89;
    frame.start.regs[7] = 0;
    for (i = 0; i < 8; i++) {
        frame.start.cram[i + 1u] = (uint16_t)(i << 1 | i << 5 | i << 9);
        tile_solid(2u + i, i + 1u);
        /* Plane A has priority in the top band and none in the second, and the third gets a highlight operator */
        map_word(PLANE_A, 64, 0, i, 1u << 15 | (2u + i));
        map_word(PLANE_A, 64, 8, i, 2u + i);
        map_word(PLANE_A, 64, 16, i, 1u << 15 | (2u + i));
    }
    /* Two sprites of 4 by 1 cells cover the third band's eight cells */
    for (i = 20; i < 24; i++)
        tile_solid(i, 14);
    sprite(1, 16u * 8u, 2, 4, 1, 1u << 15 | 3u << 13 | 20u, 0);
    sprite(2, 16u * 8u, 0, 4, 1, 1u << 15 | 3u << 13 | 20u, 32);
    set_link(0, 1);
    render(PR_LIMITS_CART);
    for (i = 0; i < 8; i++) {
        uint32_t n = grey_of(test_normal[i]), s = grey_of(test_shadow[i]), h = grey_of(test_highlight[i]);
        want_rgb(2, (int)i * 8 + 3, n, "normal level");
        want_rgb(66, (int)i * 8 + 3, s, "shadow level");
        want_rgb(130, (int)i * 8 + 3, h, "highlight level");
    }
}

/* With the display off the picture is the backdrop, with no sprites and no walk. Register 0's bit 2 clear keeps only the low bit of each channel. */
static void test_display_off_and_palette_bit(void)
{
    scene_start("display_off");
    frame.start.regs[1] = 0x04;
    frame.start.regs[7] = 5;
    tile_solid(1, 1);
    map_word(PLANE_A, 64, 0, 0, 1);
    sprite(0, 0, 0, 1, 1, 1, 100);
    render(PR_LIMITS_CART);
    want_colour(3, 3, 5, "plane hidden with the display off");
    want_colour(3, 100, 5, "sprite hidden with the display off");

    scene_start("palette_bit");
    frame.start.regs[0] = 0x00;
    frame.start.regs[7] = 5;
    frame.start.cram[5] = 0x0EEE;
    render(PR_LIMITS_CART);
    want_rgb(3, 3, grey_of(49), "only the low bit of each channel");
}

/* The 256 pixel mode: 16 sprites, 32 cells, a picture 256 wide, and the switch to no limit does nothing. */
static void test_h32(void)
{
    unsigned i, n = 1;
    scene_start("h32");
    frame.start.regs[12] = 0x00;
    frame.start.regs[5] = (uint8_t)(SAT_ADDR >> 9);
    frame.start.regs[16] = 0x00;     /* 32 by 32 cells */
    tile_solid(1, 1);
    tile_solid(2, 2);
    for (i = 0; i < 32; i++)
        map_word(PLANE_B, 32, 3, i, 1u << 13 | 2u);
    for (i = 0; i < 16; i++, n++)
        sprite(n, 40, n + 1u, 1, 1, 1, i * 10u);
    sprite(n, 40, n + 1u, 1, 1, 2, 200);        /* the 17th: not drawn */
    n++;
    /* lines 60-67: 8 sprites of 4 cells = 32 cells, and one more */
    for (i = 0; i < 8; i++, n++)
        sprite(n, 60, n + 1u, 4, 1, 1, 300);
    sprite(n, 60, 0, 1, 1, 2, 200);
    set_link(0, 1);
    render(PR_LIMITS_CART);
    n_checks++;
    if (picture.width != 256)
        fail("width %u, want 256", picture.width);
    want_colour(24, 255, 16 + 2, "plane B reaches the last pixel");
    want_colour(42, 150, 1, "the 16th sprite");
    want_colour(42, 200, 0, "the 17th sprite");
    want_colour(62, 200, 0, "past 32 cells");
    {
        PrOptions o = { PR_LIMITS_NONE, 0 };
        pr_render(&frame, &o, &picture2);
        n_checks++;
        if (memcmp(picture.rgb, picture2.rgb, sizeof(picture.rgb)))
            fail("the switch must do nothing in the 256 pixel mode");
    }
}

static void add_write(unsigned vline, unsigned hclock, unsigned kind, unsigned addr, unsigned value, unsigned flags)
{
    PcWrite *w = &frame.writes[frame.n_writes++];
    w->vline = (uint16_t)vline;
    w->hclock = (uint16_t)hclock;
    w->kind = (uint8_t)kind;
    w->flags = (uint8_t)flags;
    w->addr = (uint16_t)addr;
    w->value = (uint16_t)value;
}

/* Writes during the picture, each with its time. A handler raised at the start of line L writes H scroll late in L's blank at the earliest, so it shows from L + 1.
   Plant 21: the list is taken in list order, not in time. Plant 22: a write shows a line late. */
static void test_raster(void)
{
    unsigned col;
    scene_start("raster_h_scroll");
    tile_solid(1, 1);
    for (col = 0; col < 32; col++)
        map_word(PLANE_A, 64, col, 8, 1);
    add_write(100, 1800, PC_WRITE_VRAM, HS_ADDR, 40, PC_WF_IN_HINT);
    render(PR_LIMITS_CART);
    want_colour(100, 64, 1, "before the write's line ends: the old scroll");
    want_colour(101, 104, 1, "the next line: the new scroll");
    want_colour(101, 64, 0, "the next line: the old place is empty");

    scene_start("raster_order");
    tile_solid(1, 1);
    tile_solid(2, 2);
    for (col = 0; col < 32; col++)
        map_word(PLANE_A, 64, col, 8, 1);
    /* The later list entry has the earlier time, as a late handler's write does after a main program write. Applied in time order, 80 ends up on top */
    add_write(50, 2000, PC_WRITE_VRAM, HS_ADDR, 80, 0);
    add_write(50, 1990, PC_WRITE_VRAM, HS_ADDR, 40, PC_WF_IN_HINT);
    add_write(80, 2000, PC_WRITE_VRAM, HS_ADDR, 8, 0);
    add_write(80, 2000, PC_WRITE_VRAM, HS_ADDR, 16, 0);   /* equal times keep the list's order */
    render(PR_LIMITS_CART);
    want_colour(60, 144, 1, "time order: the later time wins");
    want_colour(90, 80, 1, "equal times: the later entry wins");

    scene_start("raster_v_scroll");
    tile_solid(1, 1);
    for (col = 0; col < 64; col++)
        map_word(PLANE_A, 64, 10, col, 1);
    add_write(40, 1800, PC_WRITE_VSRAM, 0, 8, PC_WF_IN_HINT);
    render(PR_LIMITS_CART);
    want_colour(71, 100, 0, "above the row, moved up by 8");
    want_colour(72, 100, 1, "line 72 + 8 = map line 80");
    want_colour(79, 100, 1, "line 79 + 8 = map line 87");
    want_colour(80, 100, 0, "below the row, moved up by 8");

    scene_start("raster_table");
    tile_solid(1, 1);
    tile_solid(2, 2);
    sprite(0, 60, 0, 1, 1, 1, 100);
    /* X of entry 0 moved in the blank well above the sprite, and its Y in the chip's copy */
    add_write(30, 1000, PC_WRITE_VRAM, SAT_ADDR + 6u, 128u + 200u, 0);
    add_write(30, 1100, PC_WRITE_VRAM, SAT_ADDR, 128u + 90u, 0);
    render(PR_LIMITS_CART);
    want_colour(92, 200, 1, "the entry after its writes");
    want_colour(62, 100, 0, "not at its old place");

    scene_start("raster_colour");
    tile_solid(1, 1);
    map_word(PLANE_A, 64, 0, 0, 1);
    frame.start.regs[7] = 3;
    add_write(120, 2500, PC_WRITE_CRAM, 3, 0x0EEE, 0);
    add_write(150, 200, PC_WRITE_REG, 7, 4, 0);     /* before the first pixel: later the chip changes it partway across the line, which the renderer leaves out */
    render(PR_LIMITS_CART);
    want_colour(100, 100, 3, "the old colour");
    want_rgb(130, 100, 0xFFFFFFu, "the colour as written");
    want_colour(160, 100, 4, "the new backdrop");
}

/* When a write counts: it waits for an access slot, a word lands low byte first over two slots, and each fetch has its own time. Times are in master clocks since the V
   counter stepped, and the answers were measured on the exact chip model. Plant 23: a write lands at once. Plant 24: the plane's V scroll word read after the blank's slot.
   Plant 25: a colour takes effect for the whole line. Plant 26: X and tile all read at the start of the chip's reads. Plant 27: a word lands high byte first. Plant 28: every
   entry of the walk reads its copy before the blank's slot. */
static void test_landing(void)
{
    unsigned col;
    scene_start("landing_h_scroll");
    tile_solid(1, 1);
    for (col = 0; col < 32; col++)
        map_word(PLANE_A, 64, col, 8, 1);
    /* Early in the blank: the blank's slot takes the low byte before H scroll is fetched, and the high byte comes later */
    put_word(HS_ADDR, 0x0010);
    add_write(100, 200, PC_WRITE_VRAM, HS_ADDR, 0x0020, 0);
    /* Too late for that slot: the next slot is after the fetch */
    add_write(140, 400, PC_WRITE_VRAM, HS_ADDR, 0x0030, 0);
    render(PR_LIMITS_CART);
    want_colour(99, 64 + 0x10, 1, "before the write");
    want_colour(100, 64 + 0x20, 1, "the low byte reached this line");
    want_colour(140, 64 + 0x20, 1, "the slot was out of reach");
    want_colour(141, 64 + 0x30, 1, "the next line");

    scene_start("landing_byte_order");
    tile_solid(1, 1);
    for (col = 0; col < 32; col++)
        map_word(PLANE_A, 64, col, 8, 1);
    put_word(HS_ADDR, 0x0010);
    /* Low byte in the blank's slot, high byte after H scroll's fetch: this one line takes the new low byte with the old high byte */
    add_write(100, 200, PC_WRITE_VRAM, HS_ADDR, 0x0120, 0);
    add_write(110, 200, PC_WRITE_VRAM, HS_ADDR, 0x0010, 0);
    render(PR_LIMITS_CART);
    want_colour(100, 64 + 0x20, 1, "the new low byte with the old high byte");
    want_colour(105, 64 + 0x120 - 256, 0, "the whole new word is far to the right of the old place");

    scene_start("landing_v_scroll");
    tile_solid(1, 1);
    for (col = 0; col < 64; col++)
        map_word(PLANE_A, 64, 10, col, 1);
    /* Very early in the line: the V scroll word for this line was already read */
    add_write(72, 0, PC_WRITE_VSRAM, 0, 8, 0);
    render(PR_LIMITS_CART);
    want_colour(72, 100, 0, "this line kept the old word");
    want_colour(73, 100, 1, "the next line: 73 + 8 = map line 81");

    scene_start("landing_colour");
    frame.start.regs[7] = 3;
    add_write(120, 1216, PC_WRITE_CRAM, 3, 0x0EEE, 0);
    render(PR_LIMITS_CART);
    want_colour(120, 44, 3, "the colour before its pixel");
    want_rgb(120, 45, 0xFFFFFFu, "the slot at 1341 reaches pixel 45");
    want_rgb(121, 0, 0xFFFFFFu, "the next line");

    scene_start("landing_attribute");
    tile_solid(1, 1);
    tile_solid(2, 1);
    tile_solid(3, 1);
    tile_solid(4, 1);
    /* One sprite of 4 cells tall on a line: the chip reads its X at the last of its 20 reads, at 3323 */
    sprite(0, 100, 1, 1, 4, 1, 40);
    sprite(1, 300, 0, 1, 1, 0, 400);
    add_write(110, 2760, PC_WRITE_VRAM, SAT_ADDR + 6u, 128u + 41u, 0);  /* lands at 2877 and 3005: in time */
    add_write(120, 2990, PC_WRITE_VRAM, SAT_ADDR + 6u, 128u + 42u, 0);  /* lands at 3133 and 3373: the low byte is in time, and only that changes */
    render(PR_LIMITS_CART);
    want_colour(110, 40, 1, "this line is drawn already");
    want_colour(111, 48, 1, "read after it landed");
    want_colour(121, 49, 1, "the low byte was read after it landed");

    scene_start("landing_attribute_many");
    {
        unsigned i;
        tile_solid(1, 1);
        tile_solid(2, 1);
        tile_solid(3, 1);
        tile_solid(4, 1);
        /* Twenty sprites on the line, so the first of them is read first, at 891, and the write misses it */
        for (i = 0; i < 20; i++)
            sprite(i, 100, i + 1u, 1, 4, 1, 100u + i);
        set_link(19, 0);
        add_write(110, 920, PC_WRITE_VRAM, SAT_ADDR + 6u, 128u + 10u, 0);   /* lands at 1085 and 1341 */
        render(PR_LIMITS_CART);
        want_colour(111, 10, 0, "the read came before it landed");
        want_colour(112, 10, 1, "the next line reads it");
    }

    scene_start("landing_walk");
    {
        unsigned i;
        tile_solid(1, 1);
        /* A chain of 60 entries with Y high byte 1 below the picture. Entries low on the chain are read before the blank's slot, entries high on it after */
        for (i = 0; i < 60; i++)
            sprite(i, 372, i + 1u, 1, 1, 1, i * 5u);
        set_link(59, 0);
        add_write(130, 100, PC_WRITE_VRAM, SAT_ADDR + 20u * 8u, 128u + 131u, 0);    /* its low byte lands at 362, too late for walk position 20 */
        add_write(150, 100, PC_WRITE_VRAM, SAT_ADDR + 50u * 8u, 128u + 151u, 0);    /* in time for position 50 */
        render(PR_LIMITS_CART);
        want_colour(131, 100, 0, "position 20 was read before the slot");
        want_colour(132, 100, 1, "position 20, the next line");
        want_colour(151, 250, 1, "position 50 was read after the slot");
    }
}

/* The sprite walk's memory goes from frame to frame: a record fetched and never drawn can come back in a later frame. Plant 31: pr_render_kept forgets it. */
static void test_memory(void)
{
    static const uint8_t rows[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    static PrMemory memory;
    PrOptions cart = { PR_LIMITS_CART, 0 };
    unsigned i, n = 1;
    memset(&memory, 0, sizeof(memory));
    scene_start("memory_first");
    tile_solid(1, 9);
    tile_rows(2, rows);
    /* Lines 16-23: 10 sprites of 4 cells use up the 40, so the next one is fetched and not drawn. Later lines of this frame spend their cells on those 10 records and never reach it */
    for (i = 0; i < 10; i++, n++)
        sprite(n, 16, n + 1u, 4, 1, 1, 372);
    sprite(n, 16, 0, 1, 1, 2, 200);
    set_link(0, 1);
    pr_render_kept(&frame, &cart, &memory, &picture);
    want_colour(23, 200, 0, "not drawn on its own line");
    want_colour(100, 200, 0, "this frame's later lines never reach it");

    /* The next frame: 10 narrow sprites overwrite records 0 to 9 and leave cells over */
    scene_start("memory_second");
    tile_solid(1, 9);
    tile_rows(2, rows);
    n = 1;
    for (i = 0; i < 10; i++, n++)
        sprite(n, 40, n + 1u, 1, 1, 1, 372);
    set_link(n - 1u, 0);
    set_link(0, 1);
    pr_render_kept(&frame, &cart, &memory, &picture);
    want_colour(40, 200, 8, "the record from the frame before, its last row");
    want_colour(41, 200, 0, "drawn once");

    memset(&memory, 0, sizeof(memory));
    pr_render(&frame, &cart, &picture);
    want_colour(40, 200, 0, "pr_render starts clean");
}

/* The renderer leaves the frame alone, and the same frame gives the same picture, whatever was drawn before. */
static void test_pure(void)
{
    static PcVdpState copy;
    PrOptions cart = { PR_LIMITS_CART, 1 }, none = { PR_LIMITS_NONE, 1 };
    scene_start("pure");
    tile_solid(1, 1);
    sprite(0, 10, 0, 1, 1, 1, 100);
    add_write(40, 1800, PC_WRITE_VSRAM, 0, 8, PC_WF_IN_HINT);
    memcpy(&copy, &frame.start, sizeof(copy));
    pr_render(&frame, &cart, &picture);
    pr_render(&frame, &none, &picture2);
    pr_render(&frame, &cart, &picture2);
    n_checks += 2;
    if (memcmp(&copy, &frame.start, sizeof(copy)))
        fail("the frame changed");
    if (memcmp(picture.rgb, picture2.rgb, sizeof(picture.rgb)))
        fail("the same frame gave another picture");
}

/* ---- running them */

typedef void (*TestFn)(void);
static const struct { const char *name; TestFn fn; } tests[] = {
    { "priority", test_priority },
    { "sprite_order", test_sprite_order },
    { "sprite_shape", test_sprite_shape },
    { "mask", test_mask },
    { "mask_line_before", test_mask_line_before },
    { "part_drawn", test_part_drawn },
    { "part_drawn_flip", test_part_drawn_flipped },
    { "twenty_sprites", test_twenty_sprites },
    { "ghost", test_ghost },
    { "layer_bits", test_layer_bits },
    { "no_limit_walk", test_no_limit_walk },
    { "h_scroll", test_h_scroll_modes },
    { "v_scroll", test_v_scroll },
    { "plane_shape", test_plane_shape },
    { "window", test_window },
    { "window_gap", test_window_gap },
    { "hidden_column", test_hidden_column },
    { "shadow_highlight", test_shadow_highlight },
    { "levels", test_levels },
    { "display_palette", test_display_off_and_palette_bit },
    { "h32", test_h32 },
    { "raster", test_raster },
    { "landing", test_landing },
    { "memory", test_memory },
    { "pure", test_pure },
};
#define N_TESTS (sizeof(tests) / sizeof(tests[0]))

/* Which test must catch which plant. The plant numbers are the ones in the renderer's PR_PLANT calls. */
static const struct { int plant; const char *what; const char *test; } plants[] = {
    { 1, "a sprite with priority no longer beats a plane", "priority" },
    { 2, "plane B above plane A", "priority" },
    { 3, "no mask rule", "mask" },
    { 4, "the mask forgets the line before", "mask_line_before" },
    { 5, "the last sprite drawn whole", "part_drawn" },
    { 6, "19 sprites a line", "twenty_sprites" },
    { 7, "41 cells a line", "part_drawn" },
    { 8, "no ghost", "ghost" },
    { 9, "transparent pixels not written", "layer_bits" },
    { 10, "no limit reads the chip's copy", "no_limit_walk" },
    { 11, "H scroll modes mixed up", "h_scroll" },
    { 12, "plane A reads plane B's scroll word", "h_scroll" },
    { 13, "a column reads the next column's scroll word", "v_scroll" },
    { 14, "window's top edge a cell off", "window" },
    { 15, "window's left edge a column off", "window" },
    { 16, "operator 15 does not shadow", "shadow_highlight" },
    { 17, "operator 14 does not highlight", "shadow_highlight" },
    { 18, "low priority planes not shadowed", "shadow_highlight" },
    { 19, "colour 14 of another palette shadowed", "shadow_highlight" },
    { 20, "one colour level wrong", "levels" },
    { 21, "writes taken in list order", "raster" },
    { 22, "a write shows a line late", "raster" },
    { 23, "a write lands at once", "landing" },
    { 24, "whole plane V scroll read after the blank's slot", "landing" },
    { 25, "a colour reaches the whole line at once", "landing" },
    { 26, "X and tile read all at one time", "landing" },
    { 27, "a word lands high byte first", "landing" },
    { 28, "the walk reads its whole copy before the blank's slot", "landing" },
    { 29, "the hidden column reads its plane's first word", "hidden_column" },
    { 30, "a gap where a left window ends", "window_gap" },
    { 31, "the walk's memory does not cross frames", "memory" },
};
#define N_PLANTS (sizeof(plants) / sizeof(plants[0]))

int main(int argc, char **argv)
{
    unsigned i, bad_plants = 0;
    int total_failures = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 3 && !strcmp(argv[1], "--export"))
        export_dir = argv[2];

    for (i = 0; i < N_TESTS; i++) {
        failures = 0;
        current_test = tests[i].name;
        tests[i].fn();
        printf("%-18s %s\n", tests[i].name, failures ? "FAIL" : "ok");
        total_failures += failures;
    }
    printf("%d checks, %d failed\n", n_checks, total_failures);

    export_dir = NULL;
    quiet = 1;
#ifdef PR_TEST_PLANTS
    for (i = 0; i < N_PLANTS; i++) {
        unsigned t;
        int caught = 0;
        pr_plant = plants[i].plant;
        for (t = 0; t < N_TESTS; t++) {
            if (!strcmp(tests[t].name, plants[i].test)) {
                failures = 0;
                tests[t].fn();
                caught = failures;
            }
        }
        pr_plant = 0;
        printf("plant %2d %-48s %s: %s (%d checks failed)\n", plants[i].plant, plants[i].what, plants[i].test, caught ? "caught" : "NOT CAUGHT", caught);
        if (!caught)
            bad_plants++;
    }
#else
    printf("built without the plants\n");
    (void)bad_plants;
#endif
    printf("RESULT: %s\n", total_failures || bad_plants ? "FAIL" : "PASS");
    return total_failures || bad_plants ? 1 : 0;
}
