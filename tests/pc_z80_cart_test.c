/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Z80 against the firmware's turns, on a made-up ROM and a made-up Z80 program, with the real firmware as the cart. The firmware stores a word at a known time. The 68000 then touches the cart at a
 * time past that, which runs the firmware. The Z80 has been polling that word through its window and must see the store at its own time: its reads before the store see the old word, and its reads after
 * see the new one. So the Z80 must have caught up before the firmware takes its turn. Built by tests/test_pc_machine.py with one rule broken as well.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "pt_rt.h"
#include "pc_cart.h"
#include "pc_clock.h"
#include "pc_fiber.h"
#include "pc_io.h"
#include "pc_vdp.h"
#include "pc_z80.h"

/* What the machine wants of a program. */
PtCtx pc_ctx;
void pt_watch_hit(int index) { (void)index; }
int pt_interp_only;
u32 pt_fallback(PtCtx *c, u32 pc) { (void)c; (void)pc; return 0xFFFFFFFFu; }
void pc_machine_frame_start(uint32_t frame) { (void)frame; }
void pc_machine_frame_end(uint32_t frame) { (void)frame; }

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

#define ROM_BYTES 0x800000u
static uint8_t file_bytes[ROM_BYTES];
static uint8_t rom_words[ROM_BYTES];

/* The firmware's store, made on its own clock inside its own turn. */
#define WATCHED_WORD (0x0100u / 2u)
static void (*real_point)(void);
static uint32_t store_at;
static int stored;
static void my_point(void)
{
    if (!stored && (int32_t)(hb.now - store_at) >= 0) {
        hb.ram[WATCHED_WORD] = 0x5A5Au;
        stored = 1;
    }
    real_point();
}

/* LD HL,0200h 10; loop: INC (HL) 11; LD A,(8100h) 13 + wait; OR A 4; JP Z,loop 10; JR $. Bank 0 puts the window on the cart's first page. */
static const uint8_t polling[] = {0x21, 0x00, 0x02, 0x34, 0x3A, 0x00, 0x81, 0xB7, 0xCA, 0x03, 0x00, 0x18, 0xFE};

int main(void)
{
    static PcCartConfig cfg;
    uint32_t a;
    uint64_t t, start;
    setvbuf(stdout, NULL, _IONBF, 0);
    for (a = 0; a < ROM_BYTES; a += 2) {
        uint32_t w = (a >> 1) * 40503u & 0xFFFFu;
        file_bytes[a] = (uint8_t)(w >> 8);
        file_bytes[a + 1] = (uint8_t)w;
        rom_words[a] = (uint8_t)w;
        rom_words[a + 1] = (uint8_t)(w >> 8);
    }
    pt_rom = rom_words;
    pt_rom_size = ROM_BYTES;
    pt_rom_bus_end = ROM_BYTES;
    pt_low_device_end = 0x10000u;
    pc_ctx.imask = 7;
    cfg.rom_bytes = file_bytes;
    cfg.rom_size = ROM_BYTES;
    cfg.rom_words = rom_words;
    pc_fiber_main_init();
    pc_clock_reset();
    pc_tick_init();
    pc_vdp_reset();
    pc_io_reset();
    pc_z80_reset();
    pc_cart_boot(&cfg);
    hb.ram[WATCHED_WORD] = 0;
    real_point = hb.on_point;
    hb.on_point = my_point;
    store_at = 0xFFFFFFFFu;     /* not yet */
    stored = 1;

    pt_port_write(1, 0xA11100u, 1u);
    for (a = 0; a < sizeof(polling); a++)
        pt_port_write(1, 0xA00000u + a, polling[a]);
    pt_port_write(1, 0xA11100u, 0u);
    pt_port_write(1, 0xA11200u, 1u);
    start = pc_clock_now();

    /* The store comes well after this, and the 68000's next touch of the cart comes well after the store. */
    t = start + 100u * 1000u;
    store_at = (uint32_t)t;
    stored = 0;
    t += 100u * 1000u;
    pc_clock_stall(t - pc_clock_now());
    (void)pt_port_read(2, 0x1000u);
    CHECK(stored == 1);

    /* The Z80 has reached the 68000's touch, or it has caught up the rest here. Its count of passes says when it saw the store. */
    pc_z80_run_to(pc_clock_now());
    {
        double pass_mclk = (38.0 + 2.676) * 15.0;
        double expect = (double)(uint32_t)(store_at - (uint32_t)start) / pass_mclk;
        uint32_t lo = pc_z80_ram()[0x200];
        double got;
        /* The count runs modulo 256, so the expected whole passes put the top back. */
        got = lo + 256.0 * (double)(int)((expect - lo + 128.0) / 256.0);
        printf("store seen after %.1f passes, expected %.1f\n", got, expect);
        CHECK(got > expect - 2.0 && got < expect + 2.0);
    }
    CHECK(pc_z80_counts()->window_reads > 100u);

    /* A post to the mailbox goes in the sound log with its time, so a song's time can be counted from the post that starts it. */
    {
        static const char path[] = "pc_z80_cart_test_log.bin";
        uint8_t r[16];
        FILE *f;
        int found = 0;
        uint64_t when = pc_clock_now();
        pc_sound_log_open(path);
        pt_port_write(2, 0x1FEAu, 0x8802u);
        pc_sound_log_open(NULL);
        f = fopen(path, "rb");
        CHECK(f != NULL);
        while (f && fread(r, 1, 16, f) == 16) {
            uint64_t ticks;
            uint16_t address;
            uint32_t word;
            memcpy(&ticks, r, 8);
            memcpy(&address, r + 10, 2);
            memcpy(&word, r + 12, 4);
            if (r[8] == 16 && r[9] == 2 && address == 0x1FEAu && word == 0x8802u && ticks / 2u >= when)
                found = 1;
        }
        if (f)
            fclose(f);
        remove(path);
        CHECK(found);
    }

    printf("%d checks, %d failed\n", checks, fails);
    printf(fails ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    exit(fails ? 1 : 0);
}
