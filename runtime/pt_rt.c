/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The dispatcher, the interrupt entry and the ROM loader.
 *
 * Generated functions return the next program address. A caller that expected that address goes
 * on; anything else travels up the C stack until pt_run takes it and looks it up here. That covers
 * every case with one rule: a routine that drops its return address, a stack reset, a jump into the
 * middle of a function.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pt_rt.h"

u8 *pt_rom;
u32 pt_rom_size;
u32 pt_low_device_end;
u32 pt_rom_bus_end;
u8 pt_ram[0x10000];
volatile u32 pt_irq_level;
u32 pt_run_exit_pc = 0xFFFFFFFEu;
uint64_t pt_clock;
uint64_t pt_deadline = ~(uint64_t)0;
uint32_t pt_at;
u32 pt_switches;
u32 pt_switches_next;
void (*pt_switch_point_hook)(void);

/* See pt_rt.h: a switch changes only here. */
void pt_switch_point(void) {
    pt_switches = pt_switches_next;
    if (pt_switch_point_hook) pt_switch_point_hook();
}
#if defined(PT_PC_BUILD)
int pt_low_from_ram;
const u8 *pt_low_ram;
u32 pt_low_ram_end;
#endif

#define PT_PC_STOP 0xFFFFFFFFu

/* The address map: the generated code's own, or, when the program takes the game from a DLL, the one that DLL handed over. With no DLL attached
 * the map is empty, and every address runs in the fallback. */
#if defined(PT_GAME_FROM_DLL)
static const PtEntry *pt_game_map;
static u32 pt_game_map_n;
void pt_game_attach(const PtEntry *map, u32 n) {
    pt_game_map = map;
    pt_game_map_n = map ? n : 0;
}
#define PT_MAP pt_game_map
#define PT_MAP_N pt_game_map_n
#else
#define PT_MAP pt_map
#define PT_MAP_N pt_map_n
#endif

/* The map is sorted by address. Lookups are rare: indirect calls and returns that left their frame. */
static const PtEntry *pt_map_find(u32 pc) {
    u32 lo = 0, hi = PT_MAP_N;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (PT_MAP[mid].pc == pc) return &PT_MAP[mid];
        if (PT_MAP[mid].pc < pc) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

int pt_map_has(u32 pc) {
    return pt_map_find(pc & PT_BUS_MASK) != NULL;
}

u32 pt_enter(PtCtx *c, u32 pc) {
    const PtEntry *e;
    if (pt_interp_only) return pt_fallback(c, pc);
#if defined(PT_PC_BUILD)
    if (pt_low_from_ram && (pc & PT_BUS_MASK) < pt_low_device_end) return pt_fallback(c, pc);
#endif
    e = pt_map_find(pc & PT_BUS_MASK);
    if (e) {
        PT_CNT(map_hit);
        return e->fn(c, e->entry);
    }
    PT_CNT(map_miss);
    return pt_fallback(c, pc);
}

u32 pt_stopped(PtCtx *c, u32 pc, const char *why) {
    fprintf(stderr, "stopped at %06X: %s\n", (unsigned)pc, why);
    c->stopped = 1;
    return PT_PC_STOP;
}

u32 pt_halted(PtCtx *c) {
    c->stopped = 1;
    return PT_PC_STOP;
}

/* The time of the 68000's two divides, in clocks, for the whole instruction apart from its source operand. It depends on the values, because the chip works out one quotient bit at a
 * time. These rules and figures were measured: made-up operands run on the FX68K core in simulation, each rule fitted to the answers and then checked on a second set it had not seen. See
 * THIRD_PARTY.md. A divide by zero stops the run elsewhere, so its figure does not matter. */
enum { DIVU_TOO_LARGE = 10, DIVU_BASE = 76, DIVU_BIT_ONE = 2, DIVU_BIT_ZERO = 4, DIVS_TOO_LARGE = 16, DIVS_TOO_LARGE_NEGATIVE = 18, DIVS_ZERO_BIT = 2 };

u32 pt_clocks_divu(u32 dividend, u32 divisor) {
    /* Long division, top bit first. Each step brings down one bit, so the partial remainder can reach 17 bits. When the 17th is set the chip subtracts at once and that bit costs nothing
     * extra. Otherwise it has to compare first, and a quotient bit that comes out 0 costs more than one that comes out 1. The last bit costs the same either way. */
    u32 remainder = dividend >> 16, clocks = DIVU_BASE;
    int bit;
    if (remainder >= divisor) return DIVU_TOO_LARGE;
    for (bit = 15; bit >= 1; bit--) {
        remainder = remainder << 1 | ((dividend >> bit) & 1u);
        if (remainder > 0xFFFFu) {
            remainder -= divisor;
        } else if (remainder >= divisor) {
            remainder -= divisor;
            clocks += DIVU_BIT_ONE;
        } else {
            clocks += DIVU_BIT_ZERO;
        }
    }
    return clocks;
}

u32 pt_clocks_divs(u32 dividend, u32 divisor) {
    /* By the signs, then for each 0 among bits 15 to 1 of the quotient of the two sizes. The first index is 1 for a negative dividend, the second for a negative divisor. */
    static const u8 base[2][2] = { { 120, 122 }, { 126, 124 } };
    const int negative = (dividend >> 31) != 0, negative_divisor = (divisor & 0x8000u) != 0;
    const u32 size = negative ? 0u - dividend : dividend, size_divisor = negative_divisor ? 0x10000u - (divisor & 0xFFFFu) : divisor & 0xFFFFu;
    u32 quotient;
    if (!size_divisor) return 0;
    if ((size >> 16) >= size_divisor) return negative ? DIVS_TOO_LARGE_NEGATIVE : DIVS_TOO_LARGE;
    quotient = size / size_divisor;
    return base[negative][negative_divisor] + DIVS_ZERO_BIT * (15u - (u32)__builtin_popcount(quotient >> 1));
}

u32 pt_run(PtCtx *c, u32 pc, u32 exit_pc) {
    pt_run_exit_pc = exit_pc;
    while (!c->stopped && pc != exit_pc) {
        pc = pt_enter(c, pc);
        c->rte_flag = 0;
        PT_CNT(unwind);
    }
    return pc;
}

/* The interrupt entry, in the reference 68000's order: read the vector, push the program counter,
 * push the status register. flags are the ones in the context: at a safe point every flag something
 * can read is there. The handler runs until its rte. */
u32 pt_take_irq(PtCtx *c, u32 pc) {
    u32 level = pt_irq_level, target, sr, r;
    sr = 0x2000u | ((u32)c->imask << 8) | ((u32)c->fn << 3) | ((u32)c->fz << 2) | ((u32)c->fv << 1) | c->fc;
    c->imask = (u8)level;
    pt_irq_level = 0;
    pt_port_irq_ack((int)level);
#if !defined(PT_LOCKSTEP) && !defined(PT_REPLAY)
    pt_clock += 44u;                    /* the interrupt's own time, from the published table */
#endif
    target = R32((24u + level) * 4u);
    A(7) -= 4;
    W32(A(7), pc);
    A(7) -= 2;
#if defined(PT_LOCKSTEP)
    pt_mem_w16(pt_ram, A(7) & 0xFFFF, ls_srframe(A(7) & PT_BUS_MASK, sr));
#elif defined(PT_REPLAY)
    /* The recorded word, so dead flags in old stack frames cannot make the final state differ. */
    W16(A(7), pt_replay_status);
#else
    W16(A(7), sr);
#endif
    r = pt_enter(c, target);
    while (!c->rte_flag && !c->stopped) r = pt_enter(c, r);
    c->rte_flag = 0;
#if !defined(PT_LOCKSTEP) && !defined(PT_REPLAY)
    pt_port_irq_return((int)level);
#endif
    return r;
}

/* Returns 1 when RAM holds what was translated for the block at pc, from its ROM source. */
int pt_ramcode_same(u32 pc, u32 source, u32 bytes) {
    u32 i;
    for (i = 0; i < bytes; i += 2) {
        if (pt_mem_r16(pt_ram, (pc + i) & 0xFFFF) != pt_mem_r16(pt_rom, source + i)) return 0;
    }
    return 1;
}

int pt_load_rom(const char *path, u32 low_device_end) {
    FILE *f = fopen(path, "rb");
    long n;
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    pt_rom = (u8 *)malloc((size_t)n + 16);
    if (!pt_rom || fread(pt_rom, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        return -1;
    }
    fclose(f);
    pt_rom_size = (u32)n;
    pt_rom_bus_end = pt_rom_size < 0x400000u ? pt_rom_size : 0x400000u;
    pt_low_device_end = low_device_end;
#if !defined(PT_RAM_BYTES) && defined(PT_BYTE_XOR)
    if (PT_BYTE_XOR) {
        long i;
        for (i = 0; i + 1 < n; i += 2) {
            u8 t = pt_rom[i];
            pt_rom[i] = pt_rom[i + 1];
            pt_rom[i + 1] = t;
        }
    }
#endif
    return 0;
}

void pt_reset(PtCtx *c) {
    memset(c, 0, sizeof(*c));
    memset(pt_ram, 0, sizeof(pt_ram));
    c->imask = 7;
    A(7) = IMG_R32(0);
}
