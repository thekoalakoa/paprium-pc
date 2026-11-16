/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A small program for tests: load a ROM image, reset, run from the reset vector until the program
 * reaches the given exit address, then print the final state. It has no machine behind it.
 *
 * Usage: pt_host <rom> <exit address in hex> <file for the RAM contents> [interp] [switches=HEX]
 * With "interp" the fallback interpreter runs everything, so it can be tested on the same programs. switches=HEX is the value the program asks
 * for at the switch points (pt_switches_next, see pt_rt.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pt_rt.h"

extern const char *pt_fallback_log_path;

int main(int argc, char **argv) {
    static PtCtx ctx;
    PtCtx *c = &ctx;
    u32 pc, exit_pc;
    int i;
    FILE *f;
    if (argc < 4) return 2;
    if (pt_load_rom(argv[1], 0) != 0) return 2;
    exit_pc = (u32)strtoul(argv[2], NULL, 16);
    for (i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "interp")) pt_interp_only = 1;
        else if (!strncmp(argv[i], "switches=", 9)) pt_switches_next = (u32)strtoul(argv[i] + 9, NULL, 16);
        else return 2;
    }
    pt_reset(c);
    pc = pt_run(c, IMG_R32(4), exit_pc);
    printf("pc %06X\n", (unsigned)pc);
    for (i = 0; i < 8; i++) printf("d%d %08X\n", i, (unsigned)c->d[i]);
    for (i = 0; i < 8; i++) printf("a%d %08X\n", i, (unsigned)c->a[i]);
    printf("flags %d%d%d%d\n", c->fn, c->fz, c->fv, c->fc);
    printf("stopped %d\n", c->stopped);
    printf("fallback %llu\n", (unsigned long long)pt_fallback_uses);
    f = fopen(argv[3], "wb");
    if (!f) return 2;
    for (i = 0; i < 0x10000; i++) {
        u8 b = (u8)R8(0xFF0000u + (u32)i);
        fwrite(&b, 1, 1, f);
    }
    fclose(f);
    return c->stopped ? 4 : 0;
}
