/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Replays a lockstep run's record with no machine behind it, to time the translated code. Device reads
 * come from the record, device writes are dropped, and interrupts happen at the recorded points. The
 * run ends where the lockstep run ended, and the final state must match its hash, so a timed run is
 * also a checked run.
 *
 * Usage: pt_replay <run folder> [interp]
 *   interp: every instruction runs in the fallback interpreter, for the speed comparison.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>

#include "pt_rt.h"

u32 pt_replay_left;
extern u64 pt_replay_icount;

typedef struct IrqRow { u64 safe_point, instruction, level, status; } IrqRow;
typedef struct PokeRow { u64 safe_point, instruction, where, value; } PokeRow;
u32 pt_replay_status;

static u32 *reads;
static size_t reads_n, reads_at;
static IrqRow *irqs;
static size_t irqs_n, irqs_at;
static PokeRow *pokes;
static size_t pokes_n, pokes_at;
static u64 end_safe, end_instructions, end_reads, end_irqs;
static unsigned long long end_hash;
static u64 safe_seen;
static LARGE_INTEGER t0, freq;
static const char *mode_name;

static void *load(const char *dir, const char *name, size_t *size) {
    char path[1024];
    FILE *f;
    long long n;
    void *b;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "no %s\n", path); exit(2); }
    _fseeki64(f, 0, SEEK_END);
    n = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    b = malloc((size_t)n + 16);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "cannot read %s\n", path); exit(2); }
    fclose(f);
    *size = (size_t)n;
    return b;
}

static void finish(PtCtx *c) {
    LARGE_INTEGER t1;
    double seconds;
    unsigned long long h = 0xCBF29CE484222325ull, full;
    const u8 *p;
    u32 i;
    QueryPerformanceCounter(&t1);
    seconds = (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
    /* RAM as the reference's layout holds it, whatever this build's own layout is. */
    for (i = 0; i < sizeof(pt_ram); i++) { h ^= pt_mem_r8(pt_ram, i ^ 1u); h *= 0x100000001B3ull; }
    p = (const u8 *)c->d;
    for (i = 0; i < sizeof(c->d) + sizeof(c->a); i++) { h ^= p[i]; h *= 0x100000001B3ull; }
    /* The same with the rest of the context, to compare builds of the same code on different hosts. */
    full = h;
    p = (const u8 *)&c->usp;
    for (i = 0; i < 4; i++) { full ^= p[i]; full *= 0x100000001B3ull; }
    full ^= c->fn; full *= 0x100000001B3ull;
    full ^= c->fz; full *= 0x100000001B3ull;
    full ^= c->fv; full *= 0x100000001B3ull;
    full ^= c->fc; full *= 0x100000001B3ull;
    full ^= c->imask; full *= 0x100000001B3ull;
    printf("mode %s seconds %.3f instructions %llu mips %.1f\n", mode_name, seconds,
           (unsigned long long)end_instructions, (double)end_instructions / seconds / 1e6);
    printf("reads used %llu of %llu; interrupts %llu of %llu; fallback entries %llu\n",
           (unsigned long long)reads_at, (unsigned long long)end_reads, (unsigned long long)irqs_at,
           (unsigned long long)end_irqs, (unsigned long long)pt_fallback_uses);
    printf("final state %016llX, the lockstep run's %016llX\n", h, end_hash);
    printf("final state with flags and usp %016llX\n", full);
    if (h != end_hash || reads_at != end_reads || irqs_at != end_irqs) {
        printf("RESULT: FAIL: the replay did not end in the lockstep run's state\n");
        fflush(stdout);
        ExitProcess(1);
    }
    printf("RESULT: PASS\n");
    fflush(stdout);
    ExitProcess(0);
}

/* The next stop on the safe point count: a route's RAM change, an interrupt, or the end of the run. */
static void arm_next(void) {
    u64 next = irqs_at < irqs_n ? irqs[irqs_at].safe_point : end_safe + 1;
    if (pokes_at < pokes_n && pokes[pokes_at].safe_point < next) next = pokes[pokes_at].safe_point;
    pt_replay_left = (u32)(next - safe_seen);
    safe_seen = next;
}

static void apply_poke(const PokeRow *p) {
    u32 a = (u32)p->where & 0xFFFFu;
    if ((p->where >> 32) == 1) pt_mem_w8(pt_ram, a, (u32)p->value);
    else pt_mem_w16(pt_ram, a & 0xFFFEu, (u32)p->value);
}

/* Called when the count reaches a stop. Returns 1 to take an interrupt now. */
int pt_replay_event(PtCtx *c) {
    u64 here = safe_seen;
    while (pokes_at < pokes_n && pokes[pokes_at].safe_point == here) apply_poke(&pokes[pokes_at++]);
    if (irqs_at < irqs_n && irqs[irqs_at].safe_point == here) {
        pt_irq_level = (u32)irqs[irqs_at].level;
        pt_replay_status = (u32)irqs[irqs_at].status;
        irqs_at++;
        arm_next();
        return 1;
    }
    if (here > end_safe) {
        finish(c);
        return 0;
    }
    arm_next();
    return 0;
}

/* The interpreter's version, counting instructions. */
int pt_replay_icount_event(PtCtx *c) {
    while (pokes_at < pokes_n && pokes[pokes_at].instruction == pt_replay_icount) apply_poke(&pokes[pokes_at++]);
    if (irqs_at < irqs_n && pt_replay_icount == irqs[irqs_at].instruction) {
        pt_irq_level = (u32)irqs[irqs_at].level;
        pt_replay_status = (u32)irqs[irqs_at].status;
        irqs_at++;
        return 1;
    }
    if (pt_replay_icount == end_instructions) finish(c);
    return 0;
}

uint32_t pt_port_read(int size, uint32_t address) {
    (void)size;
    (void)address;
    if (reads_at >= reads_n) {
        printf("RESULT: FAIL: the replay asked for more device reads than the record holds\n");
        fflush(stdout);
        ExitProcess(1);
    }
    return reads[reads_at++];
}

void pt_port_write(int size, uint32_t address, uint32_t value) { (void)size; (void)address; (void)value; }
void pt_port_delay(int cycles) { (void)cycles; }
void pt_port_irq_ack(int level) { (void)level; }

int main(int argc, char **argv) {
    static PtCtx ctx;
    const char *dir;
    size_t size;
    u8 *start;
    u32 pc, rom_size;
    FILE *f;
    char path[1024];
    if (argc < 2) return 2;
    dir = argv[1];
    pt_interp_only = argc > 2 && !strcmp(argv[2], "interp");
    mode_name = pt_interp_only ? "interpreter" : "translated";

    start = (u8 *)load(dir, "replay_start.bin", &size);
    memcpy(&pc, start, 4);
    memcpy(&rom_size, start + 4, 4);
    memcpy(&ctx, start + 8, sizeof(ctx));
    memcpy(pt_ram, start + 8 + sizeof(ctx), sizeof(pt_ram));
    pt_rom = start + 8 + sizeof(ctx) + sizeof(pt_ram);
    pt_rom_size = rom_size;
#if defined(PT_RAM_BYTES)
    /* The record holds 16-bit words of the host that made it. This layout keeps the bytes in bus order. */
    {
        u32 k;
        for (k = 0; k + 1 < rom_size; k += 2) {
            u8 x = pt_rom[k];
            pt_rom[k] = pt_rom[k + 1];
            pt_rom[k + 1] = x;
        }
        for (k = 0; k < sizeof(pt_ram); k += 2) {
            u8 x = pt_ram[k];
            pt_ram[k] = pt_ram[k + 1];
            pt_ram[k + 1] = x;
        }
    }
#endif
    pt_rom_bus_end = pt_rom_size < 0x400000u ? pt_rom_size : 0x400000u;
    pt_low_device_end = 0x10000;

    reads = (u32 *)load(dir, "replay_reads.bin", &size);
    reads_n = size / 4;
    irqs = (IrqRow *)load(dir, "replay_irqs.bin", &size);
    irqs_n = size / sizeof(IrqRow);
    pokes = (PokeRow *)load(dir, "replay_pokes.bin", &size);
    pokes_n = size / sizeof(PokeRow);
    snprintf(path, sizeof(path), "%s/replay_end.txt", dir);
    f = fopen(path, "r");
    if (!f) return 2;
    if (fscanf(f, "safe_points %llu instructions %llu reads %llu irqs %llu state %llx",
               (unsigned long long *)&end_safe, (unsigned long long *)&end_instructions,
               (unsigned long long *)&end_reads, (unsigned long long *)&end_irqs, &end_hash) != 5) return 2;
    fclose(f);
    snprintf(path, sizeof(path), "%s/fallback_replay_%s.log", dir, mode_name);
    {
        extern const char *pt_fallback_log_path;
        pt_fallback_log_path = _strdup(path);
    }

    arm_next();
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    pt_run(&ctx, pc, 0xFFFFFFFEu);
    printf("RESULT: FAIL: the replay stopped early\n");
    return 1;
}
