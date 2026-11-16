/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The controller ports' devices (targets/pc/pc_io.c) and what the Scum! menu's arcade entries put on them (targets/pc/pc_scum.c), against the
 * Pocket core's 0.3.0 release: its core_top.sv (the ports for each menu value), rtl/PAPRIUM/paprium_gs3.sv (the Grand Stick III) and
 * rtl/PAPRIUM/paprium_pads.sv with rtl/upstream/pad_io.sv (the EA 4-Way Play and its pads). Each group of checks is named, so a planted fault in
 * tests/pc_arcade_scenes.py can say which group must catch it. Times are master clocks.
 */
#include <stdio.h>
#include <string.h>

#include "pt_rt.h"
#include "pc_config.h"
#include "pc_io.h"
#include "pc_scum.h"
#include "pc_vdp.h"

/* What pc_io.c and pc_scum.c take from the machine and the runtime. */
PcCounts pc_counts;
u8 *pt_rom;
u32 pt_rom_size;
u32 pt_switches, pt_switches_next;
void (*pt_switch_point_hook)(void);
#define ROM_BYTES 0x0C0000u      /* a made-up program's size, big enough to hold the stage word; not a game address */
static u8 rom[ROM_BYTES];

static int checks, failed, group_failed[8];
static const char *group_names[8] = {"ports", "stick", "coins", "ext", "tap", "count", "float", "stage"};
static int group;
#define CHECK(cond) do { checks++; if (!(cond)) { failed++; group_failed[group]++; printf("FAIL %s line %d: %s\n", group_names[group], __LINE__, #cond); } } while (0)

/* Times in master clocks; none of these is a game address. */
#define START_TIME 1000000u      /* not a game address */
#define PAST_TIMEOUT 200000u     /* longer than every timeout here; not a game address */
#define JUST_PAST_COUNT 81300u   /* just past the pads' 81,200 clocks; not a game address */
static uint64_t now = START_TIME;

static void w8(int index, uint32_t v) { now += 200u; pc_io_write(1, 0xA10001u + 2u * (uint32_t)index, v, now); }
static uint32_t r8(int index) { now += 200u; return pc_io_read(1, 0xA10001u + 2u * (uint32_t)index, now) & 0xFFu; }

enum { DATA1 = 1, DATA2 = 2, CTRL1 = 4, CTRL2 = 5 };

/* ---- the ports for each menu value: core_top.sv's always block, as a table of the twelve distinct answers */
static void test_ports(void)
{
    int mode, coins, attract, three;
    group = 0;
    for (mode = 0; mode < PC_ARCADE_MODES; mode++)
        for (coins = 0; coins < PC_COINS_SETTINGS; coins++)
            for (attract = 0; attract < 2; attract++)
                for (three = 0; three < 2; three++) {
                    PcArcade a;
                    PcIoPorts p;
                    unsigned want_switches = 0;
                    memset(&a, 0, sizeof(a));
                    a.mode = mode;
                    a.coins = coins;
                    a.attract = attract;
                    a.three_players = three;
                    pc_scum_ports(&a, &p);
                    if (mode == 1 || mode == 2)
                        want_switches = 0x20u | (coins == 2 ? 0x10u : 0u) | (attract ? 0x08u : 0u) | (coins == 1 ? 0x04u : 0u) | (mode == 2 ? 0x01u : 0u);
                    CHECK(p.stick == (mode != 0) && p.switches == want_switches && p.stick_coin == (mode == 1 || mode == 2));
                    CHECK(p.stick_ext == (mode != 0 && (mode == 2 || three)) && p.tap == (three && mode == 0));
                }
}

/* The stick's frame: phase 0 with TH low, then a phase each TH edge. Returns the lines of phases 0 to 8 (bits 0-5). */
static void stick_frame(uint32_t lines[9])
{
    int k;
    now += PAST_TIMEOUT;                  /* past the stick's timeout */
    w8(CTRL1, 0x40);                 /* TH an output */
    w8(DATA1, 0x00);                 /* TH low: phase 0 */
    lines[0] = r8(DATA1) & 0x3Fu;
    for (k = 1; k < 9; k++) {
        w8(DATA1, (k & 1) ? 0x40 : 0x00);
        lines[k] = r8(DATA1) & 0x3Fu;
    }
}

static void stick_start(unsigned switches, int coin, int ext)
{
    PcIoPorts p;
    memset(&p, 0, sizeof(p));
    p.stick = 1;
    p.switches = switches;
    p.stick_coin = coin;
    p.stick_ext = ext;
    pc_io_set_ports(&p);
    pc_io_reset();
}

static void test_stick(void)
{
    uint32_t l[9];
    group = 1;
    pc_io_set_present(2, 0);
    stick_start(0x2Du, 1, 0);
    pc_io_set_pad(0, PC_PAD_UP | PC_PAD_C | PC_PAD_X | PC_PAD_START);
    stick_frame(l);
    CHECK(l[0] == 0x14u);                                   /* the ID: a stick, nothing in the EXT socket */
    CHECK(l[1] == (0x3Fu & ~(1u | 0x20u)));                 /* C B R L D U: Up and C held */
    CHECK(l[2] == (0x3Fu & ~(0x10u | 0x02u)));              /* Mode X Y Z Start A: X and Start held */
    CHECK(l[3] == 0x3Fu && l[4] == 0x3Fu);                  /* no EXT pad */
    CHECK(l[5] == 0x02u && l[6] == 0x3Fu && l[7] == 0x2Du && l[8] == 0x3Fu);
    /* Mode is the coin slot: the stick's own Mode bit reads not held. In Stick Only it is the stick's button. */
    pc_io_set_pad(0, PC_PAD_MODE);
    stick_frame(l);
    CHECK((l[2] & 0x20u) == 0x20u && (l[5] & 0x08u));
    stick_start(0x00u, 0, 0);
    pc_io_set_pad(0, 0);
    pc_io_set_pad(0, PC_PAD_MODE);
    stick_frame(l);
    CHECK((l[2] & 0x20u) == 0 && l[5] == 0x02u && l[7] == 0x00u);
    /* What the ports hold is taken at the next reset. */
    {
        PcIoPorts p;
        memset(&p, 0, sizeof(p));
        pc_io_set_ports(&p);
        CHECK(pc_io_stick());
        pc_io_reset();
        CHECK(!pc_io_stick());
    }
}

/* The coin: one waiting per press, up to 7; one taken per rise into phase 9 whose bit 2 is 1 after a 0. */
static void ack(unsigned bit)
{
    uint32_t l[9];
    stick_frame(l);
    w8(CTRL1, 0x4F);                 /* D0-D3 driven by the console for the acknowledge */
    w8(DATA1, 0x00 | (bit ? 0x04 : 0));
    w8(DATA1, 0x40 | (bit ? 0x04 : 0));     /* the rise into phase 9 */
    w8(CTRL1, 0x40);
}

static int waiting(void)
{
    uint32_t l[9];
    stick_frame(l);
    return (l[5] & 0x08u) != 0;
}

static void test_coins(void)
{
    int k;
    group = 2;
    stick_start(0x20u, 1, 0);
    pc_io_set_pad(0, 0);
    CHECK(!waiting());
    for (k = 0; k < 9; k++) {           /* nine presses: seven wait */
        pc_io_set_pad(0, PC_PAD_MODE);
        pc_io_set_pad(0, 0);
    }
    CHECK(waiting());
    ack(1);                              /* 0 -> 1: one taken */
    ack(1);                              /* held at 1: none */
    for (k = 0; k < 6; k++) {
        CHECK(waiting());
        ack(0);
        ack(1);
    }
    CHECK(!waiting());                  /* seven taken, two presses lost to the limit, as the stick's count of 7 loses them */
    /* Held, Mode is one press. */
    pc_io_set_pad(0, PC_PAD_MODE);
    pc_io_set_pad(0, PC_PAD_MODE);
    ack(0);
    ack(1);
    CHECK(!waiting());
}

/* Player 3 in the EXT socket: the ID's D1, then their buttons in phases 3 and 4; a three button pad holds no Mode, X, Y or Z. */
static void test_ext(void)
{
    uint32_t l[9];
    group = 3;
    stick_start(0x21u, 1, 1);
    pc_io_set_present(2, 0);
    stick_frame(l);
    CHECK(l[0] == 0x14u && l[3] == 0x3Fu && l[4] == 0x3Fu);
    pc_io_set_present(2, 1);
    pc_io_set_pad(2, PC_PAD_DOWN | PC_PAD_B | PC_PAD_A | PC_PAD_Y | PC_PAD_MODE);
    pc_io_set_three_button(2, 0);
    stick_frame(l);
    CHECK(l[0] == 0x16u);
    CHECK(l[3] == (0x3Fu & ~(0x02u | 0x10u)));                       /* Down, B */
    CHECK(l[4] == (0x3Fu & ~(0x01u | 0x08u | 0x20u)));               /* A, Y, Mode */
    pc_io_set_three_button(2, 1);
    stick_frame(l);
    CHECK(l[4] == (0x3Fu & ~0x01u));                                 /* A only */
    pc_io_set_three_button(2, 0);
    stick_start(0x21u, 1, 0);           /* the socket not wired: Arcade On without 3-Player Mode */
    stick_frame(l);
    CHECK(l[0] == 0x14u && l[3] == 0x3Fu);
}

static void tap_start(void)
{
    PcIoPorts p;
    memset(&p, 0, sizeof(p));
    p.tap = 1;
    pc_io_set_ports(&p);
    pc_io_reset();
    w8(CTRL1, 0x40);
    w8(CTRL2, 0x7F);
}

static void choose(unsigned socket) { w8(DATA2, socket << 4); }

/* The adapter: its ID on every line for the numbers 4 to 7; sockets A, B, C; D and an empty C read as nothing; port 2 reads back what is driven. */
static void test_tap(void)
{
    group = 4;
    pc_io_set_present(2, 1);
    pc_io_set_pad(0, PC_PAD_B);
    pc_io_set_pad(1, PC_PAD_C);
    pc_io_set_pad(2, PC_PAD_UP);
    pc_io_set_three_button(0, 0);
    pc_io_set_three_button(1, 0);
    pc_io_set_three_button(2, 0);
    tap_start();
    choose(4);
    CHECK((r8(DATA1) & 0x7Fu) == 0x7Cu);
    w8(CTRL1, 0x00);
    CHECK((r8(DATA1) & 0x7Fu) == 0x7Cu);       /* every line, inputs or not */
    w8(CTRL1, 0x40);
    choose(7);
    CHECK((r8(DATA1) & 0x7Fu) == 0x7Cu);
    now += PAST_TIMEOUT;
    choose(0);
    w8(DATA1, 0x40);
    CHECK((r8(DATA1) & 0x3Fu) == (0x3Fu & ~0x10u));     /* socket A, TH high: B held */
    choose(1);
    w8(DATA1, 0x40);
    CHECK((r8(DATA1) & 0x3Fu) == (0x3Fu & ~0x20u));     /* socket B: C held */
    choose(2);
    w8(DATA1, 0x40);
    CHECK((r8(DATA1) & 0x3Fu) == (0x3Fu & ~0x01u));     /* socket C: Up held */
    choose(3);
    CHECK((r8(DATA1) & 0x3Fu) == 0x3Fu);               /* socket D: nothing */
    pc_io_set_present(2, 0);
    choose(2);
    w8(DATA1, 0x00);
    CHECK((r8(DATA1) & 0x3Fu) == 0x3Fu);               /* an empty socket C, TH low too */
    pc_io_set_present(2, 1);
    /* The number is taken only while port 2 shows 0 on bits 1-0: here bit 0 driven high, so the socket stays C. */
    w8(DATA2, 0x01);
    w8(DATA1, 0x40);
    CHECK((r8(DATA1) & 0x3Fu) == (0x3Fu & ~0x01u));
    w8(CTRL2, 0x7C);                    /* bits 1-0 now inputs: they show 1, nothing taken */
    w8(DATA2, 0x00);
    CHECK((r8(DATA1) & 0x3Fu) == (0x3Fu & ~0x01u));
    CHECK((r8(DATA2) & 0x7Fu) == 0x03u);                /* port 2: what the console drives, 1 on the inputs */
    w8(CTRL2, 0x7F);
}

/* A socket's pad: the six button sequence, its count of rises in two bits, its timeout, a three button pad. */
static void test_count(void)
{
    int k;
    uint32_t v[8];
    group = 5;
    tap_start();
    pc_io_set_present(2, 1);
    pc_io_set_pad(1, PC_PAD_A | PC_PAD_START | PC_PAD_X | PC_PAD_MODE);
    choose(1);
    now += PAST_TIMEOUT;                     /* the count forgotten */
    w8(DATA1, 0x40);
    for (k = 0; k < 8; k++) {
        w8(DATA1, (k & 1) ? 0x40 : 0x00);
        v[k] = r8(DATA1) & 0x3Fu;
    }
    /* The rises: one before the loop, then k = 1, 3, 5 and 7. TH low with count 1 (k = 0): Start and A, D and U; count 2 (k = 2): the six button ID,
       the low four lines 0; count 3: Mode X Y Z (k = 3), then the low four lines 1 (k = 4); the fourth rise (k = 5) counts 0 again, the plain read. */
    CHECK(v[0] == 0x03u && v[1] == 0x3Fu && v[2] == 0x00u);
    CHECK(v[3] == (0x3Fu & ~(0x04u | 0x08u)));             /* X and Mode */
    CHECK(v[4] == 0x0Fu && v[5] == 0x3Fu && v[6] == 0x03u && v[7] == 0x3Fu);
    /* The sixth rise counts 2 again (the fifth was k = 7): TH low then reads the six button ID once more, where a count with no end would not. */
    w8(DATA1, 0x00);
    w8(DATA1, 0x40);
    w8(DATA1, 0x00);
    CHECK((r8(DATA1) & 0x0Fu) == 0x00u);
    /* After 81,200 clocks with no edge the count is gone. */
    now += JUST_PAST_COUNT;
    CHECK((r8(DATA1) & 0x0Fu) == 0x03u);                   /* TH low, count 0: D U not held */
    pc_io_set_three_button(1, 1);
    now += PAST_TIMEOUT;
    for (k = 0; k < 6; k++) {
        w8(DATA1, (k & 1) ? 0x00 : 0x40);
        v[k] = r8(DATA1) & 0x3Fu;
    }
    CHECK(v[0] == 0x3Fu && v[2] == 0x3Fu && v[4] == 0x3Fu && v[3] == 0x03u && v[5] == 0x03u);   /* a three button pad: never the ID */
    pc_io_set_three_button(1, 0);
}

/* A socket's TH no longer driven rises by itself 1,471 clocks later, and that rise counts. */
static void test_float(void)
{
    group = 6;
    tap_start();
    pc_io_set_pad(0, 0);
    choose(0);
    /* Left low for 1,000 clocks and taken back: no rise, the count stays 1, TH low reads D and U. */
    now += PAST_TIMEOUT;
    w8(DATA1, 0x40);
    w8(DATA1, 0x00);
    choose(1);
    now += 1000u;
    choose(0);
    CHECK((r8(DATA1) & 0x0Fu) == 0x03u);
    /* Left low for 1,600 clocks: the line rises by itself, which counts, so taken back low it reads the six button ID (count 2). */
    now += PAST_TIMEOUT;
    w8(DATA1, 0x40);
    w8(DATA1, 0x00);
    choose(1);
    now += 1600u;
    choose(0);
    CHECK((r8(DATA1) & 0x0Fu) == 0x00u);
}

/* The stage word: the menu's stage while Arcade Mode is On or Multiplayer, at start and at the switch point; otherwise the game's own. */
static void test_stage(void)
{
    PcArcade a;
    group = 7;
    pt_rom = rom;
    pt_rom_size = sizeof(rom);
    pt_mem_w16(pt_rom, 0x0B0A14u, 0x0001u);
    memset(&a, 0, sizeof(a));
    a.stage = 6;
    pc_scum_start(0, &a, 0);
    CHECK(pt_mem_r16(pt_rom, 0x0B0A14u) == 0x0001u && pc_scum_stage_served() == 0);
    a.mode = PC_ARCADE_ON;
    pc_scum_set_arcade(&a);
    CHECK(pt_mem_r16(pt_rom, 0x0B0A14u) == 0x0001u);       /* not before the switch point */
    pt_switch_point_hook();
    CHECK(pt_mem_r16(pt_rom, 0x0B0A14u) == 0x0006u);
    a.stage = 25;
    pc_scum_set_arcade(&a);
    pt_switch_point_hook();
    CHECK(pt_mem_r16(pt_rom, 0x0B0A14u) == 0x001Eu);       /* the Arena, the game's stage 0x1E */
    a.mode = PC_ARCADE_STICK_ONLY;
    pc_scum_set_arcade(&a);
    pt_switch_point_hook();
    CHECK(pt_mem_r16(pt_rom, 0x0B0A14u) == 0x0001u);
    a.mode = PC_ARCADE_MULTIPLAYER;
    a.stage = 0;
    pc_scum_start(0, &a, 1);
    CHECK(pt_mem_r16(pt_rom, 0x0B0A14u) == 0x0001u && pc_io_stick());
}

int main(void)
{
    int k;
    test_ports();
    test_stick();
    test_coins();
    test_ext();
    test_tap();
    test_count();
    test_float();
    test_stage();
    for (k = 0; k < 8; k++)
        printf("group %s: %s\n", group_names[k], group_failed[k] ? "FAIL" : "ok");
    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
