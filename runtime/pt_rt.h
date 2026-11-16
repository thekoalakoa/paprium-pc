/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The runtime that generated code runs on: the register context, the memory access functions, the
 * block macro and the dispatcher interface. It holds no game data. The ROM is loaded at start-up
 * from the file the player supplied.
 *
 * Build modes (one at a time):
 *   default       speed: no checks at all; the machine behind it is the port layer
 *   PT_LOCKSTEP   every bus access and every block goes through the lockstep rig in rig/
 *   PT_REPLAY     speed, with device answers and interrupt points replayed from a lockstep run's
 *                 record, so the translated code can be timed with no machine behind it
 *
 * PT_PC_BUILD, with the speed build: the PC program. The low page can come to hold another program than the one translated, because the cart can restart the
 * console on code it has just put in its RAM. Then the low page runs in the fallback, from the cart RAM.
 *
 * Memory layouts:
 *   default       words are native host words (byte address XOR 1 on a little-endian host)
 *   PT_RAM_BYTES  plain big-endian byte arrays, assembled byte by byte, any host; slower, for testing
 */
#ifndef PT_RT_H
#define PT_RT_H

#include <stdint.h>

/* The PC program can take the translated game from a DLL that the player's PC builds from their own ROM: see targets/pc/pc_game_dll.h. The
 * generated code is then compiled with PT_GAME_DLL, and the runtime's data it uses is imported from the program, which defines it. Every other
 * build sees a plain extern. */
#ifndef PT_DATA
#if defined(PT_GAME_DLL)
#define PT_DATA extern __declspec(dllimport)
#else
#define PT_DATA extern
#endif
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int16_t s16;
typedef int8_t s8;
typedef int32_t s32;
typedef uint64_t u64;

typedef struct PtCtx {
    u32 d[8];
    u32 a[8];           /* a[7] is the supervisor stack: the program this runtime is for stays in supervisor mode */
    u32 usp;
    u8 fn, fz, fv, fc;  /* one byte per condition flag. X is not kept, see the design notes */
    u8 imask;           /* the status register's interrupt mask */
    u8 rte_flag;        /* set by a translated rte, cleared by pt_take_irq */
    u8 stopped;         /* set when execution cannot go on; every caller unwinds to pt_run */
} PtCtx;

#if defined(__GNUC__)
#define PT_COLD __attribute__((cold))
#else
#define PT_COLD
#endif

#define D(n) (c->d[n])
#define A(n) (c->a[n])

/* The address map: one entry for each block that can be entered from outside its C function. */
typedef u32 (*PtFn)(PtCtx *, u32);
typedef struct PtEntry {
    u32 pc;
    PtFn fn;
    u32 entry;
} PtEntry;
extern const PtEntry pt_map[];
extern const u32 pt_map_n;

/* PT_GAME_FROM_DLL, with PT_PC_BUILD: the program holds no generated code. The address map then comes from the game DLL, which the program hands
 * to the runtime once, before pt_reset. PT_GAME_ABI is the version of what the DLL and the program share: the names the DLL imports and what
 * they mean. A DLL built for another version is refused. */
#define PT_GAME_ABI 2
void pt_game_attach(const PtEntry *map, u32 n);

/* RAM code: bytes the program copies from ROM into RAM and the translation decoded at the RAM address. */
typedef struct PtOverlay {
    u32 base, source, length, verified;
} PtOverlay;
extern const PtOverlay pt_overlays[];

/* ---- the dispatcher, in pt_rt.c */
u32 pt_enter(PtCtx *c, u32 pc);         /* run the code at pc, return the next program address */
u32 pt_run(PtCtx *c, u32 pc, u32 exit_pc);
u32 pt_take_irq(PtCtx *c, u32 pc);
u32 pt_stopped(PtCtx *c, u32 pc, const char *why);
u32 pt_halted(PtCtx *c);                /* the machine asked for a stop; no message */
void pt_reset(PtCtx *c);
int pt_load_rom(const char *path, u32 low_device_end);
int pt_ramcode_same(u32 pc, u32 source, u32 bytes);
int pt_map_has(u32 pc);
PT_DATA volatile u32 pt_irq_level;       /* the level the machine is asserting, 0 for none */
PT_DATA u32 pt_run_exit_pc;              /* the address pt_run was told to stop at */

/* ---- switches: patches of the patch source (tool/patchsrc.py, format 2) that the player turns on and off while the game runs. The generated
 * code at a switched site holds both ways, and takes the patched one while the switch's bit is set in pt_switches. pt_switches changes only at a
 * switch point, a place the patch source names where no switch's way can be running: there the generated code calls pt_switch_point, which takes
 * pt_switches_next, the value the program asks for, and then calls pt_switch_point_hook when the program gave one, for what the program changes at
 * that point itself. All zero, the game runs as it always did. The fallback runs no switch's way. In pt_rt.c. */
PT_DATA u32 pt_switches;
void pt_switch_point(void);
extern u32 pt_switches_next;
extern void (*pt_switch_point_hook)(void);
#define PT_SWITCH(mask) (pt_switches & (mask))

/* ---- the fallback for addresses the address map does not know, in pt_fallback.c */
u32 pt_fallback(PtCtx *c, u32 pc);
u32 pt_fallback_one(PtCtx *c, u32 pc);  /* exactly one instruction: for the form fuzz */
PT_DATA u64 pt_fallback_uses;
PT_DATA int pt_interp_only;              /* every instruction runs in the fallback: for speed comparisons and tests */
void pt_fallback_write_summary(const char *path);
void pt_fallback_write_summary_to(const char *dir);

/* ---- memory */
PT_DATA u8 *pt_rom;
PT_DATA u32 pt_rom_size;
PT_DATA u32 pt_low_device_end;           /* addresses below this are device space, not ROM */
PT_DATA u32 pt_rom_bus_end;              /* the 68000 sees the ROM only up to here; the rest is for the cart's chip */
PT_DATA u8 pt_ram[0x10000];

#define PT_BUS_MASK 0xFFFFFFu
#define PT_RAM_LOW 0xE00000u

#include "pt_port.h"

#if defined(PT_RAM_BYTES)
static inline u32 pt_mem_r8(const u8 *m, u32 a) { return m[a]; }
static inline u32 pt_mem_r16(const u8 *m, u32 a) { return ((u32)m[a] << 8) | m[a + 1]; }
static inline void pt_mem_w8(u8 *m, u32 a, u32 v) { m[a] = (u8)v; }
static inline void pt_mem_w16(u8 *m, u32 a, u32 v) { m[a] = (u8)(v >> 8); m[a + 1] = (u8)v; }
#else
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define PT_BYTE_XOR 0u
#else
#define PT_BYTE_XOR 1u
#endif
static inline u32 pt_mem_r8(const u8 *m, u32 a) { return m[a ^ PT_BYTE_XOR]; }
static inline u32 pt_mem_r16(const u8 *m, u32 a) { return *(const u16 *)(const void *)(m + a); }
static inline void pt_mem_w8(u8 *m, u32 a, u32 v) { m[a ^ PT_BYTE_XOR] = (u8)v; }
static inline void pt_mem_w16(u8 *m, u32 a, u32 v) { *(u16 *)(void *)(m + a) = (u16)v; }
#endif

#if defined(PT_LOCKSTEP)
#include "ls_core.h"
#define PT_DEV_R(sz, a) ls_read((sz), (a))
#define PT_DEV_W(sz, a, v) ((void)0)
#define PT_ON_WRITE(sz, a, v) ls_write((sz), (a), (v))
#define PT_CNT(name) (ls_counters.name++)
#define PT_LOW_CODE(pc, bytes) ls_low_code_check((pc), (bytes))
#else
#define PT_CNT(name) ((void)0)
#define PT_LOW_CODE(pc, bytes) ((void)0)
#define PT_DEV_R(sz, a) pt_port_read((sz), (a))
#define PT_DEV_W(sz, a, v) pt_port_write((sz), (a), (v))
#define PT_ON_WRITE(sz, a, v) ((void)0)
#endif

static inline int pt_is_rom(u32 a) { return a >= pt_low_device_end && a < pt_rom_bus_end; }

/* PT_STRICT_READS, with PT_LOCKSTEP: reads of ROM and work RAM are compared too, address and value. */
#if defined(PT_STRICT_READS)
#define PT_PLAIN_R(sz, a, v) ls_read_plain((sz), (a), (v))
#else
#define PT_PLAIN_R(sz, a, v) (v)
#endif

#if defined(PT_PC_BUILD)
PT_DATA int pt_low_from_ram;             /* the low page no longer holds what was translated: it runs in the fallback, from pt_low_ram */
PT_DATA const u8 *pt_low_ram;            /* the cart RAM in the same word layout as pt_rom */
PT_DATA u32 pt_low_ram_end;
#define PT_FROM_CART_RAM(a) (pt_low_from_ram && (a) < pt_low_ram_end)
#else
#define PT_FROM_CART_RAM(a) 0
static const u8 *const pt_low_ram = 0;
#endif

/* Program counter relative reads and the runtime's own look at the image: no device, never logged. */
static inline u32 IMG_R8(u32 a) {
    a &= PT_BUS_MASK;
    if (PT_FROM_CART_RAM(a)) return pt_mem_r8(pt_low_ram, a);
    return a >= PT_RAM_LOW ? pt_mem_r8(pt_ram, a & 0xFFFF) : pt_mem_r8(pt_rom, a);
}
static inline u32 IMG_R16(u32 a) {
    a &= PT_BUS_MASK;
    if (PT_FROM_CART_RAM(a)) return pt_mem_r16(pt_low_ram, a);
    return a >= PT_RAM_LOW ? pt_mem_r16(pt_ram, a & 0xFFFF) : pt_mem_r16(pt_rom, a);
}
static inline u32 IMG_R32(u32 a) { return (IMG_R16(a) << 16) | IMG_R16(a + 2); }

static inline u32 R8(u32 a) {
    a &= PT_BUS_MASK;
    if (a >= PT_RAM_LOW) return PT_PLAIN_R(1, a, pt_mem_r8(pt_ram, a & 0xFFFF));
    if (pt_is_rom(a)) return PT_PLAIN_R(1, a, pt_mem_r8(pt_rom, a));
    return PT_DEV_R(1, a);
}
static inline u32 R16(u32 a) {
    a &= PT_BUS_MASK;
    if (a >= PT_RAM_LOW) return PT_PLAIN_R(2, a, pt_mem_r16(pt_ram, a & 0xFFFF));
    if (pt_is_rom(a)) return PT_PLAIN_R(2, a, pt_mem_r16(pt_rom, a));
    return PT_DEV_R(2, a);
}
static inline u32 R32(u32 a) {
    a &= PT_BUS_MASK;
    if (a >= PT_RAM_LOW) return PT_PLAIN_R(4, a, (pt_mem_r16(pt_ram, a & 0xFFFF) << 16) | pt_mem_r16(pt_ram, (a + 2) & 0xFFFF));
    if (pt_is_rom(a)) return PT_PLAIN_R(4, a, (pt_mem_r16(pt_rom, a) << 16) | pt_mem_r16(pt_rom, a + 2));
    return PT_DEV_R(4, a);
}
static inline void W8(u32 a, u32 v) {
    a &= PT_BUS_MASK; v &= 0xFF;
    PT_ON_WRITE(1, a, v);
    if (a >= PT_RAM_LOW) pt_mem_w8(pt_ram, a & 0xFFFF, v);
    else if (!pt_is_rom(a)) PT_DEV_W(1, a, v);
}
static inline void W16(u32 a, u32 v) {
    a &= PT_BUS_MASK; v &= 0xFFFF;
    PT_ON_WRITE(2, a, v);
    if (a >= PT_RAM_LOW) pt_mem_w16(pt_ram, a & 0xFFFF, v);
    else if (!pt_is_rom(a)) PT_DEV_W(2, a, v);
}
static inline void W32(u32 a, u32 v) {
    a &= PT_BUS_MASK;
    PT_ON_WRITE(4, a, v);
    if (a >= PT_RAM_LOW) {
        pt_mem_w16(pt_ram, a & 0xFFFF, v >> 16);
        pt_mem_w16(pt_ram, (a + 2) & 0xFFFF, v & 0xFFFF);
    } else if (!pt_is_rom(a)) PT_DEV_W(4, a, v);
}

/* Accesses whose address the translator knows to be work RAM. */
#define RAM_R8(a) R8(a)
#define RAM_R16(a) R16(a)
#define RAM_R32(a) R32(a)
#define RAM_W8(a, v) W8((a), (v))
#define RAM_W16(a, v) W16((a), (v))
#define RAM_W32(a, v) W32((a), (v))

/* ---- values */
#define SX8(x) ((u32)(s32)(s8)(u8)(x))
#define SX16(x) ((u32)(s32)(s16)(u16)(x))
#define ROT16(x) (((u32)(x) << 16) | ((u32)(x) >> 16))
#define SETLO8(r, v) (((r) & 0xFFFFFF00u) | ((u32)(v) & 0xFFu))
#define SETLO16(r, v) (((r) & 0xFFFF0000u) | ((u32)(v) & 0xFFFFu))

#include "pt_flags.h"

/* ---- blocks. Each generated block starts with this.
 * pc = its address, n = its 68000 instructions, live = the flags it needs in the context,
 * safe = an interrupt may be taken here, clocks = its 68000 time from the published tables.
 * The order at a block: the interrupt check first, then the charge, then the deadline, so a handler does not pay for a block that has not run. */
#if defined(PT_LOCKSTEP)
#define PT_BLOCK(pc, n, live, safe, clocks) \
    for (;;) { \
        ls_block(c, (pc), (n), (live), (safe)); \
        if (!((safe) && pt_irq_level > c->imask)) break; \
        { u32 r_ = pt_take_irq(c, (pc)); if (r_ != (pc)) return r_; } \
    }
#elif defined(PT_REPLAY)
/* Interrupt points come from the record: every safe point counts down to the next one. */
extern u32 pt_replay_left, pt_replay_status;
int pt_replay_event(PtCtx *c);
#define PT_BLOCK(pc, n, live, safe, clocks) \
    while ((safe) && --pt_replay_left == 0 && pt_replay_event(c)) { \
        u32 r_ = pt_take_irq(c, (pc)); if (r_ != (pc)) return r_; \
    }
#else
#define PT_TIMED 1
#define PT_BLOCK(pc, n, live, safe, clocks) \
    while ((safe) && pt_irq_level > c->imask) { \
        u32 r_ = pt_take_irq(c, (pc)); if (r_ != (pc)) return r_; \
    } \
    pt_clock += (clocks); \
    if (pt_clock >= pt_deadline && pt_port_tick()) return pt_halted(c);
#endif

/* ---- time the run decides, and where a handler's device access falls in its block. All nothing unless the build counts time. */
#if defined(PT_TIMED) && defined(__TINYC__)
/* TinyCC, which builds the game DLL on a player's PC, has no __builtin_popcount. pt_popcount is defined beside the DLL's exports. */
int pt_popcount(unsigned v);
#define __builtin_popcount pt_popcount
#endif
#if defined(PT_TIMED)
void pt_watch_hit(int index);           /* a block the local configuration asked to hear about */
u32 pt_clocks_divu(u32 dividend, u32 divisor);
u32 pt_clocks_divs(u32 dividend, u32 divisor);
#define PT_AT(n) (pt_at = (n))
#define PT_WATCH(k) pt_watch_hit(k)
#define PT_CHARGE(n) (pt_clock += (u64)(s32)(n))
#define PT_CHARGE_SHIFT(count) (pt_clock += 2u * ((count) & 63u))
#define PT_CHARGE_MULU(src) (pt_clock += 2u * (u32)__builtin_popcount((src) & 0xFFFFu))
#define PT_CHARGE_MULS(src) (pt_clock += 2u * (u32)__builtin_popcount((((src) << 1) ^ (src)) & 0xFFFFu))
#define PT_CHARGE_DIVU(d, src) (pt_clock += pt_clocks_divu((d), (src) & 0xFFFFu))
#define PT_CHARGE_DIVS(d, src) (pt_clock += pt_clocks_divs((d), (src) & 0xFFFFu))
#else
#define PT_AT(n) ((void)0)
#define PT_WATCH(k) ((void)0)
#define PT_CHARGE(n) ((void)0)
#define PT_CHARGE_SHIFT(count) ((void)0)
#define PT_CHARGE_MULU(src) ((void)0)
#define PT_CHARGE_MULS(src) ((void)0)
#define PT_CHARGE_DIVU(d, src) ((void)0)
#define PT_CHARGE_DIVS(d, src) ((void)0)
#endif

/* A long NOP run ended: the program waited here on purpose. See the design notes. */
#define PT_DELAY(cycles) pt_port_delay(cycles)
/* Entering translated RAM code that RAM does not hold (yet, or any more): run what RAM holds, in the
 * fallback. The test looks at this block's own bytes, so it stays cheap on a hot routine. */
#define PT_RAMCODE_ENTRY(pc, source, bytes) \
    do { if (!pt_ramcode_same((pc), (source), (bytes))) return pt_fallback(c, (pc)); } while (0)
/* Under the rig, every block of RAM code checks its bytes, not just the ones entered from outside. */
#if defined(PT_LOCKSTEP)
#define PT_RAM_CODE(pc, source, bytes) ls_ram_code_check((pc), pt_ramcode_same((pc), (source), (bytes)))
#else
#define PT_RAM_CODE(pc, source, bytes) ((void)0)
#endif

#endif /* PT_RT_H */
