/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The replay of pt_main_replay.c with no operating system: for pricing the translated code on another CPU
 * under an instruction set simulator (tool/price.py). The simulator loads the program and writes a run's
 * record to fixed addresses before it starts, and reads the result back at the end. It holds no game data.
 *
 * bare_mark() is called when the replay reaches the interrupt number the simulator asked for, twice over,
 * so the simulator can count only a window of the run.
 */
#include "pt_rt.h"

#define BARE_INFO_ADDRESS 0x0F000000u

typedef struct IrqRow { u64 safe_point, instruction, level, status; } IrqRow;
typedef struct PokeRow { u64 safe_point, instruction, where, value; } PokeRow;

typedef struct BareInfo {
    u32 pc, rom_size;
    u32 rom, start_ctx, start_ram;
    u32 reads, reads_n;
    u32 irqs, irqs_n;
    u32 pokes, pokes_n;
    u32 end_safe_low, end_safe_high;
    u32 mark_from, mark_to;
    /* results */
    u32 done, reads_at, irqs_at;
    u32 hash_low, hash_high;
    u32 full_low, full_high;
} BareInfo;

#define INFO ((volatile BareInfo *)BARE_INFO_ADDRESS)

u32 pt_replay_left, pt_replay_status;

static const u32 *reads;
static u32 reads_at;
static const IrqRow *irqs;
static u32 irqs_n, irqs_at;
static const PokeRow *pokes;
static u32 pokes_n, pokes_at;
static u64 end_safe, safe_seen;
static PtCtx ctx;

/* The simulator stops at these addresses. */
void __attribute__((noinline)) bare_mark_from(void) { __asm__ volatile(""); }
void __attribute__((noinline)) bare_mark_to(void) { __asm__ volatile(""); }
void __attribute__((noinline)) bare_stop(void) { for (;;) __asm__ volatile(""); }

static void finish(PtCtx *c) {
    unsigned long long h = 0xCBF29CE484222325ull, full;
    const u8 *p;
    u32 i;
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
    INFO->reads_at = reads_at;
    INFO->irqs_at = irqs_at;
    INFO->hash_low = (u32)h;
    INFO->hash_high = (u32)(h >> 32);
    INFO->full_low = (u32)full;
    INFO->full_high = (u32)(full >> 32);
    INFO->done = 1;
    bare_stop();
}

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

int pt_replay_event(PtCtx *c) {
    u64 here = safe_seen;
    while (pokes_at < pokes_n && pokes[pokes_at].safe_point == here) apply_poke(&pokes[pokes_at++]);
    if (irqs_at < irqs_n && irqs[irqs_at].safe_point == here) {
        if (irqs_at == INFO->mark_from) bare_mark_from();
        if (irqs_at == INFO->mark_to) bare_mark_to();
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

int pt_replay_icount_event(PtCtx *c) {
    (void)c;
    return 0;
}

uint32_t pt_port_read(int size, uint32_t address) {
    (void)size;
    (void)address;
    return reads[reads_at++];
}
void pt_port_write(int size, uint32_t address, uint32_t value) { (void)size; (void)address; (void)value; }
void pt_port_delay(int cycles) { (void)cycles; }
void pt_port_irq_ack(int level) { (void)level; }

const char *pt_bare_note;

int main(void) {
    u32 i;
    const u8 *from;
    u8 *to;
    pt_rom = (u8 *)INFO->rom;
    pt_rom_size = INFO->rom_size;
    pt_rom_bus_end = pt_rom_size < 0x400000u ? pt_rom_size : 0x400000u;
    pt_low_device_end = 0x10000;
    from = (const u8 *)INFO->start_ctx;
    to = (u8 *)&ctx;
    for (i = 0; i < sizeof(ctx); i++) to[i] = from[i];
    from = (const u8 *)INFO->start_ram;
#if defined(PT_RAM_BYTES)
    /* The record holds 16-bit words of the host that made it. This layout keeps the bytes in bus order. */
    for (i = 0; i < sizeof(pt_ram); i++) pt_ram[i] = from[i ^ 1];
    for (i = 0; i + 1 < pt_rom_size; i += 2) {
        u8 x = pt_rom[i];
        pt_rom[i] = pt_rom[i + 1];
        pt_rom[i + 1] = x;
    }
#else
    for (i = 0; i < sizeof(pt_ram); i++) pt_ram[i] = from[i];
#endif
    reads = (const u32 *)INFO->reads;
    irqs = (const IrqRow *)INFO->irqs;
    irqs_n = INFO->irqs_n;
    pokes = (const PokeRow *)INFO->pokes;
    pokes_n = INFO->pokes_n;
    end_safe = ((u64)INFO->end_safe_high << 32) | INFO->end_safe_low;
    arm_next();
    pt_run(&ctx, INFO->pc, 0xFFFFFFFEu);
    finish(&ctx);
    return 0;
}

__asm__(
    ".section .text.start\n"
    ".globl _start\n"
    "_start:\n"
    ".option push\n.option norelax\n"
    "  la gp, __global_pointer$\n"
    ".option pop\n"
    "  li sp, 0x0EFFFFF0\n"
    "  call main\n"
    "  call bare_stop\n"
    ".text\n");
