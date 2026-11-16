/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The firmware answering as the cart, on a made-up ROM with nothing from the game: the bus rules of rig/fw/fw_cart.c,
 * a post served through the firmware's own dispatcher, the board timer following the caller's clock, and the synth
 * stand-in's schedule. Built by tests/test_all.py when the local configuration names a firmware tree.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "fw_cart.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static void *current_fiber(void) { return GetCurrentFiber(); }

#define ROM_BYTES 0x800000u
static uint8_t file_bytes[ROM_BYTES];
static uint8_t rom_words[ROM_BYTES];

static unsigned file_word(unsigned a) { return (unsigned)file_bytes[a] << 8 | file_bytes[a + 1]; }

int main(void)
{
    static FwConfig cfg;
    unsigned a;
    int t = 0;
    ConvertThreadToFiber(NULL);
    for (a = 0; a < ROM_BYTES; a += 2) {
        unsigned w = (a >> 1) * 40503u & 0xFFFFu;
        file_bytes[a] = (uint8_t)(w >> 8);
        file_bytes[a + 1] = (uint8_t)w;
        rom_words[a] = (uint8_t)w;          /* 16-bit host words, as the machine keeps a ROM */
        rom_words[a + 1] = (uint8_t)(w >> 8);
    }
    cfg.rom_bytes = file_bytes;
    cfg.rom_size = ROM_BYTES;
    cfg.current_fiber = current_fiber;
    fwc_configure(&cfg);
    fwc_boot(rom_words, ROM_BYTES);

    /* cart RAM: the boot page from the ROM, whole words, a byte write on both halves */
    CHECK(fwc_r16(0x0200, t) == file_word(0x0200));
    CHECK(fwc_r8(0x0201, t) == (file_word(0x0200) & 0xFF));
    fwc_w16(0x0300, 0x1234, t);
    CHECK(fwc_r16(0x0300, t) == 0x1234);
    fwc_w8(0x0301, 0x5A, t);
    CHECK(fwc_r16(0x0300, t) == 0x5A5A);
    CHECK(fwc_low()[0x0300] == 0x5A);

    /* above the RAM: the FM view, the plain ROM, writes go nowhere */
    CHECK(fwc_r16(0x2000, t) == (file_word(0x2000) ^ 0xA8A2u));
    CHECK(fwc_r8(0x7FFE, t) == ((file_word(0x7FFE) ^ 0xA8A2u) >> 8));
    CHECK(fwc_r16(0x8000, t) == file_word(0x8000));
    CHECK(fwc_r16(0xC000, t) == file_word(0xC000));
    fwc_w16(0x8000, 0xFFFF, t);
    CHECK(fwc_r16(0x8000, t) == file_word(0x8000));

    /* a post the firmware does not know: refused and finished by its own dispatcher, as time passes */
    fwc_w16(0x1FEA, 0x8E00, t);
    CHECK(fwc_r16(0x1FEA, t) == 0x8E00);
    t += 100000;
    CHECK(fwc_r16(0x1FEA, t) == 0);
    CHECK(hb.arr == 1 && hb.done == 1);
    CHECK(fwc_r16(0x1FE4, t) == 4);         /* busy at rest, no model bits */

    /* the window is a read FIFO: any address in it, past the end too, and busy once the stream is out */
    *(uint16_t *)(void *)(hb.workspace + 0x100) = 0xAAAA;
    *(uint16_t *)(void *)(hb.workspace + 0x102) = 0xBBBB;
    *(uint16_t *)(void *)(hb.workspace + 0x104) = 0xCCCC;
    hb.window_en = 1;
    hb.sptr = 0x100;
    hb.remaining = 2;
    hb.stream_valid = 1;
    CHECK(fwc_r16(0x1FE4, t) == 0);
    CHECK(fwc_r16(0xC000, t) == 0xAAAA);
    CHECK(fwc_r16(0xD234, t) == 0xBBBB);
    CHECK(fwc_r16(0x1FE4, t) == 4);
    CHECK(fwc_r16(0xC000, t) == 0xCCCC);
    CHECK(hb.sptr == 0x106);

    /* the mini view: the file everywhere, no window */
    hb.mini_view = 1;
    CHECK(fwc_r16(0xC000, t) == file_word(0xC000));
    CHECK(fwc_r16(0x2000, t) == file_word(0x2000));
    hb.mini_view = 0;
    hb.window_en = 0;

    /* frames: the board timer and the VS count follow the caller's clock; the housekeeping timer runs */
    {
        unsigned vs0 = hb.vs, taken0 = hb.taken[1];
        int i;
        for (i = 0; i < 10; i++)
            fwc_frame_end(896040u, 0);
        (void)fwc_r16(0x0200, 1);
        CHECK(hb.vs - vs0 == 10);
        CHECK(hb.taken[1] > taken0);
        CHECK(hb.now >= 10u * 896040u);
    }

    /* the synth stand-in: no block before CONFIG; after it the count moves at its rate, and waits for what the 68000 posted */
    {
        unsigned b0 = BRD_SYNTH_BLKCNT(hb.sy_status), b1, b2;
        CHECK(b0 == 0);
        hb.sy_config = 1;
        fwc_frame_end(896040u, 0);          /* the schedule starts at the first board call that sees the switch */
        (void)fwc_r16(0x0200, 1);
        b0 = BRD_SYNTH_BLKCNT(hb.sy_status);
        fwc_frame_end(896040u, 0);
        (void)fwc_r16(0x0200, 1);
        b1 = BRD_SYNTH_BLKCNT(hb.sy_status);
        CHECK(b1 - b0 == 5);                /* 300 instants a second, 60 frames */
        hb.no_irq = 1;                      /* the firmware cannot serve what is posted now */
        fwc_w16(0x1FEA, 0x8E00, 2);
        fwc_frame_end(896040u, 0);
        (void)fwc_r16(0x0200, 1);
        b2 = BRD_SYNTH_BLKCNT(hb.sy_status);
        CHECK(b2 - b1 >= 2 && b2 - b1 <= 4);    /* only the 5 ms timeout lets any through */
        hb.no_irq = 0;
    }

    printf("%d checks, %d failed\n", checks, fails);
    printf(fails ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    return fails ? 1 : 0;
}
