/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The PC port's shared interface on made-up data: the one rule for applying a write, and a frame in a file.
 * Usage: pc_frame_test <a scratch file>
 */
#include <stdio.h>
#include <stdlib.h>

#include "pc_frame.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static PcVdpState s;
static PcFrame a, b;

static void put(unsigned kind, unsigned addr, unsigned value)
{
    PcWrite w;
    memset(&w, 0, sizeof(w));
    w.kind = (uint8_t)kind;
    w.addr = (uint16_t)addr;
    w.value = (uint16_t)value;
    pc_state_apply(&s, &w);
}

static void test_apply(void)
{
    /* registers */
    put(PC_WRITE_REG, 12, 0x81);
    CHECK(s.regs[12] == 0x81);
    put(PC_WRITE_REG, 24, 0x55);        /* past the registers: nothing */
    CHECK(s.regs[23] == 0);

    /* colours keep the chip's 9 bits, and the address wraps */
    put(PC_WRITE_CRAM, 3, 0xFFFF);
    CHECK(s.cram[3] == 0x0EEE);
    put(PC_WRITE_CRAM, 64 + 4, 0x0222);
    CHECK(s.cram[4] == 0x0222);

    /* vertical scroll keeps 11 bits; past the table: nothing */
    put(PC_WRITE_VSRAM, 1, 0xFFFF);
    CHECK(s.vsram[1] == 0x07FF);
    put(PC_WRITE_VSRAM, 40, 0x0123);
    CHECK(s.vsram[39] == 0);

    /* VRAM is big-endian bytes */
    put(PC_WRITE_VRAM, 0x1234, 0xABCD);
    CHECK(s.vram[0x1234] == 0xAB && s.vram[0x1235] == 0xCD);

    /* The chip's own copy of the sprite table in the 320 pixel mode: register 5's low bit does not count, 80 entries, the first four bytes of each */
    s.regs[12] = 0x81;
    s.regs[5] = 0x79;
    put(PC_WRITE_VRAM, 0xF000, 0x0102);
    put(PC_WRITE_VRAM, 0xF002, 0x0304);
    put(PC_WRITE_VRAM, 0xF004, 0x0506);
    CHECK(s.sat_cache[0][0] == 1 && s.sat_cache[0][1] == 2 && s.sat_cache[0][2] == 3 && s.sat_cache[0][3] == 4);
    CHECK(s.vram[0xF004] == 5);
    put(PC_WRITE_VRAM, 0xF000 + 79 * 8, 0x1112);
    CHECK(s.sat_cache[79][0] == 0x11 && s.sat_cache[79][1] == 0x12);
    put(PC_WRITE_VRAM, 0xF000 + 80 * 8, 0x2122);
    CHECK(s.vram[0xF000 + 80 * 8] == 0x21);
    {
        int i, j, seen = 0;
        for (i = 0; i < 80; i++)
            for (j = 0; j < 4; j++)
                seen += s.sat_cache[i][j] == 0x21;
        CHECK(seen == 0);
    }

    /* In the 256 pixel mode the table is register 5 with every bit, and 64 entries: entry 63 at the very top of VRAM stays in range */
    memset(&s, 0, sizeof(s));
    s.regs[12] = 0;
    s.regs[5] = 0x7F;
    put(PC_WRITE_VRAM, 0xFE00 + 63 * 8, 0x3132);
    CHECK(s.sat_cache[63][0] == 0x31 && s.sat_cache[63][1] == 0x32);
    put(PC_WRITE_VRAM, 0xFE00 - 8, 0x4142);
    CHECK(s.vram[0xFE00 - 8] == 0x41);
    CHECK(s.sat_cache[0][0] == 0);

    /* A write the table is not at leaves the copy alone: the chip does not fetch it again when register 5 moves */
    s.regs[5] = 0x40;
    CHECK(s.sat_cache[63][0] == 0x31);
}

static void test_file(const char *path)
{
    FILE *f = fopen(path, "wb");
    unsigned i;
    CHECK(f != NULL);
    if (!f)
        return;
    memset(&a, 0, sizeof(a));
    a.number = 7;
    a.n_writes = 3;
    a.start.vram[5] = 0x5A;
    a.start.cram[2] = 0x0444;
    for (i = 0; i < a.n_writes; i++) {
        a.writes[i].vline = (uint16_t)(10u + i);
        a.writes[i].hclock = (uint16_t)(100u * i);
        a.writes[i].kind = PC_WRITE_VSRAM;
        a.writes[i].flags = PC_WF_IN_HINT;
        a.writes[i].addr = (uint16_t)i;
        a.writes[i].value = (uint16_t)(0x100u + i);
    }
    CHECK(pc_frame_write(f, &a) == 0);
    a.number = 8;
    a.n_writes = 0;
    CHECK(pc_frame_write(f, &a) == 0);
    fclose(f);

    f = fopen(path, "rb");
    CHECK(f != NULL);
    if (!f)
        return;
    CHECK(pc_frame_read(f, &b) == 0);
    CHECK(b.number == 7 && b.n_writes == 3 && b.start.vram[5] == 0x5A && b.start.cram[2] == 0x0444);
    CHECK(b.writes[2].vline == 12 && b.writes[2].hclock == 200 && b.writes[2].value == 0x102 && b.writes[2].flags == PC_WF_IN_HINT);
    CHECK(pc_frame_read(f, &b) == 0);
    CHECK(b.number == 8 && b.n_writes == 0);
    CHECK(pc_frame_read(f, &b) == 1);
    fclose(f);

    f = fopen(path, "wb");
    CHECK(f != NULL);
    if (!f)
        return;
    i = 0x12345678u;
    fwrite(&i, 4, 1, f);
    i = 0;
    fwrite(&i, 4, 1, f);
    fwrite(&i, 4, 1, f);
    fwrite(&i, 4, 1, f);
    fclose(f);
    f = fopen(path, "rb");
    CHECK(pc_frame_read(f, &b) == -1);
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    test_apply();
    test_file(argv[1]);
    printf("%d checks, %d failed\n", checks, fails);
    printf(fails ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    return fails ? 1 : 0;
}
