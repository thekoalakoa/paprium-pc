/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Z80 and its bus. The processor is z80.h from floooh's chips, zlib licence, from its author's source at the commit pc_build.py pins. It is not in this repository. It takes one call per Z80 clock, and
 * each bus access has its own clock, so a chip write or a cart read happens at its true time.
 *
 * TIME. The Z80 keeps its own time in master clocks, 15 to a clock, and runs behind the machine's clock. pc_z80_run_to brings it up to a moment, and these call it:
 *   - the tick, at every line, so the Z80 is never more than a line behind;
 *   - every 68000 access that the Z80 can see or that can see the Z80: its bus request, reset and RAM, and the video chip's ports;
 *   - the cart, before the firmware takes a turn, so the Z80's reads of the cart and the firmware's writes land in time order.
 * The Z80 never runs past an interrupt that has been raised and not taken: see pc_sound_time in pc_clock.h. A handler that writes to the Z80 runs at an earlier effective time than the clock, and the
 * Z80 must not have gone past that moment yet. Every caller gets this rule, because the rule is applied here.
 *
 * THE BUS. The Z80 reaches the 68000's bus through a window of 32 KB at a bank it sets itself, nine bits written one at a time. On the real console such an access takes the bus from the 68000: the Z80
 * waits for it, and the 68000 stops for it. So each access costs both of them time, which is what the clock's share is for. Those numbers are measured:
 *   - The Z80's wait comes from the whole-core simulation's own event log: 2.676 Z80 clocks a window read, on average, over 266,066 reads of an attract run. It does not follow the clock's phase. It
 *     follows what the 68000 is doing on its bus, which this machine does not model. So the average is kept with an accumulator, and each read waits 2 or 3 clocks.
 *   - The 68000's stop is PC_Z80_BUS_SHARE_MCLK in pc_clock.h.
 * A window access that falls inside a video chip DMA waits for the DMA's end, because the DMA holds the 68000's bus throughout. The game's driver is built so that this does not happen.
 *
 * Left out by name: the Z80's interrupt, because the game's driver turns interrupts off at its first instruction and never turns them on; the grant's delay, because the 68000's next look at the grant
 * comes later than the grant does.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHIPS_IMPL
#define CHIPS_ASSERT(c) ((void)0)
#include "chips/z80.h"

#include "pt_rt.h"
#include "pc_cart.h"
#include "pc_clock.h"
#include "pc_psg.h"
#include "pc_sound.h"
#include "pc_vdp.h"
#include "pc_ym.h"
#include "pc_z80.h"

#define Z_MCLK 15u              /* master clocks a Z80 clock */
#define Z80_WAIT_Q16 (2676u * 65536u / 1000u)   /* the mean wait of a window access, in 65,536ths of a Z80 clock */
#define CART_RAM_END 0x1FE0u

static z80_t cpu;
static uint64_t pins;
static uint8_t ram[0x2000];
static int bus_requested;
static int in_reset;
static int inside;              /* a read of the cart runs the firmware, and the firmware's turn asks for the Z80 first: it is already here */
static uint64_t z_time;
static uint32_t wait_acc;
static uint32_t bank;
static PcZ80Counts counts;

static uint64_t cart_write_time[CART_RAM_END / 2u];     /* when the 68000 last wrote each word of cart RAM, plus one; 0 = never */

/* ---- the sound log: the 16-byte record of the whole-core simulation's event log, so one analysis reads both. Times there are in two ticks a master clock. */
enum { LOG_WRITE = 1, LOG_READ = 2, LOG_FRAME = 5, LOG_LINES = 9, LOG_POST = 16 };    /* the simulation's own log has no record 16 */
enum { LOG_BY_Z80 = 1, LOG_BY_68000 = 2, LOG_CHIP = 4 };
static FILE *sound_log;

static void log_put(uint64_t t, unsigned kind, unsigned flags, uint32_t a, uint32_t d)
{
    if (sound_log) {
        uint8_t r[16];
        uint64_t ticks = t * 2u;
        uint16_t a16 = (uint16_t)a;
        memcpy(r, &ticks, 8);
        r[8] = (uint8_t)kind;
        r[9] = (uint8_t)flags;
        memcpy(r + 10, &a16, 2);
        memcpy(r + 12, &d, 4);
        fwrite(r, 1, 16, sound_log);
    }
}

static void log_lines(uint64_t t)
{
    /* The simulation's three lines: reset, request and grant, each 1 when not asserted. */
    log_put(t, LOG_LINES, 0, 0, (uint32_t)((in_reset ? 0u : 1u) | (bus_requested ? 0u : 6u)));
}

void pc_sound_log_open(const char *path)
{
    if (sound_log)
        fclose(sound_log);
    sound_log = path ? fopen(path, "wb") : NULL;
    if (path && !sound_log) {
        printf("cannot write the sound log %s\n", path);
        fflush(stdout);
    }
}

void pc_sound_log_post(uint64_t t, uint32_t address, uint32_t word)
{
    log_put(t, LOG_POST, LOG_BY_68000, address, word);
}

void pc_z80_frame_mark(uint32_t frame, uint64_t t)
{
    log_put(t, LOG_FRAME, 0, 0, frame);
}

static void report_at_exit(void)
{
    printf("Z80: %llu clocks and %llu waiting; window reads %llu (%llu off the cart), writes %llu, %llu waited for a DMA; FM writes %llu reads %llu, PSG writes %llu, bank writes %llu, video chip writes %llu reads %llu, port cycles %llu\n",
           (unsigned long long)counts.clocks, (unsigned long long)counts.wait_clocks, (unsigned long long)counts.window_reads, (unsigned long long)counts.other_pages,
           (unsigned long long)counts.window_writes, (unsigned long long)counts.dma_waits, (unsigned long long)counts.ym_writes, (unsigned long long)counts.ym_reads,
           (unsigned long long)counts.psg_writes, (unsigned long long)counts.bank_writes, (unsigned long long)counts.chip_writes, (unsigned long long)counts.chip_reads, (unsigned long long)counts.io_cycles);
    printf("Z80: resets %llu, bus requests %llu, 68000 writes with no grant %llu, reads of cart RAM within a line of a 68000 write %llu\n", (unsigned long long)counts.resets,
           (unsigned long long)counts.bus_requests, (unsigned long long)counts.no_grant_writes, (unsigned long long)counts.near_cart_writes);
    if (sound_log)
        fclose(sound_log);
    sound_log = NULL;
    fflush(stdout);
}

const PcZ80Counts *pc_z80_counts(void) { return &counts; }

/* ---- what an access to the 68000's bus costs. Both sides pay here. */
static void bus_pay(void)
{
    uint64_t dma_start = pc_vdp_dma_start(), dma_end = pc_vdp_dma_end();
    if (z_time >= dma_start && z_time < dma_end) {
        counts.dma_waits++;
        z_time += (dma_end - z_time + Z_MCLK - 1u) / Z_MCLK * Z_MCLK;
    }
    wait_acc += Z80_WAIT_Q16;
    z_time += (uint64_t)(wait_acc >> 16) * Z_MCLK;
    counts.wait_clocks += wait_acc >> 16;
    wait_acc &= 0xFFFFu;
    pc_clock_share(PC_Z80_BUS_SHARE_MCLK);
}

static void note_near_write(uint32_t a)
{
    if (a < CART_RAM_END) {
        uint64_t w = cart_write_time[a >> 1];
        if (w) {
            uint64_t t = w - 1u, gap = t > z_time ? t - z_time : z_time - t;
            if (gap < PC_MCLK_LINE)
                counts.near_cart_writes++;
        }
    }
}

void pc_z80_note_cart_write(uint32_t a, uint64_t t)
{
    if (a < CART_RAM_END)
        cart_write_time[a >> 1] = t + 1u;
}

/* The bank window. The cart answers as it answers the 68000, the same RAM, register page and FM view, but the frame log's cart reads stay the 68000's. */
static uint8_t window_read(uint16_t a, uint32_t pc)
{
    uint32_t a68 = bank << 15 | (a & 0x7FFFu);
    uint32_t v;
    counts.window_reads++;
    bus_pay();
    if (a68 < 0x10000u) {
        uint64_t keep = pc_counts.cart_reads;
        note_near_write(a68);
        v = pc_cart_r8(a68, z_time);
        pc_counts.cart_reads = keep;
    } else {
        counts.other_pages++;
        if (a68 >= PT_RAM_LOW)
            v = pt_mem_r8(pt_ram, a68 & 0xFFFFu);
        else if (a68 < pt_rom_bus_end)
            v = pt_mem_r8(pt_rom, a68);
        else
            v = 0xFFu;
    }
    log_put(z_time, LOG_READ, LOG_BY_Z80, a, (v & 0xFFu) | pc << 16);
    return (uint8_t)v;
}

static void window_write(uint16_t a, uint8_t v)
{
    uint32_t a68 = bank << 15 | (a & 0x7FFFu);
    counts.window_writes++;
    bus_pay();
    log_put(z_time, LOG_WRITE, LOG_BY_Z80, a, v);
    if (a68 < 0x10000u) {
        uint64_t keep = pc_counts.cart_writes;
        pc_cart_w8(a68, v, z_time);
        pc_counts.cart_writes = keep;
    }
}

/* ---- the Z80's own bus */
static uint8_t z_read(uint16_t a, uint32_t pc)
{
    if (a < 0x4000u)
        return ram[a & 0x1FFFu];
    if (a < 0x6000u) {
        uint32_t v = pc_ym_read(a, z_time);
        counts.ym_reads++;
        log_put(z_time, LOG_READ, LOG_BY_Z80 | LOG_CHIP, a, v | pc << 16);
        return (uint8_t)v;
    }
    if (a >= 0x8000u)
        return window_read(a, pc);
    if (a >= 0x7F00u) {
        /* The video chip's ports. The driver reads none, so a read counts and reads as nothing there. */
        bus_pay();
        counts.chip_reads++;
    }
    return 0xFFu;
}

static void z_write(uint16_t a, uint8_t v)
{
    if (a < 0x4000u) {
        ram[a & 0x1FFFu] = v;
    } else if (a < 0x6000u) {
        pc_ym_write(a, v, z_time);
        counts.ym_writes++;
        log_put(z_time, LOG_WRITE, LOG_BY_Z80 | LOG_CHIP, a, v);
    } else if (a < 0x6100u) {
        bank = (bank >> 1 | (uint32_t)(v & 1u) << 8) & 0x1FFu;
        counts.bank_writes++;
        log_put(z_time, LOG_WRITE, LOG_BY_Z80, a, v);
    } else if (a >= 0x8000u) {
        window_write(a, v);
    } else if (a >= 0x7F00u) {
        bus_pay();
        log_put(z_time, LOG_WRITE, LOG_BY_Z80, a, v);
        if ((a & 0xF8u) == 0x10u) {
            pc_psg_write(v, z_time);
            counts.psg_writes++;
        } else {
            pc_vdp_z80_write(a, v, z_time);
            counts.chip_writes++;
        }
    }
}

void pc_z80_run_to(uint64_t t)
{
    uint64_t limit = pc_sound_time();
    if (t > limit)
        t = limit;
    if (inside || z_time + Z_MCLK > t)
        return;
    inside = 1;
    if (bus_requested || in_reset || pc_cart_holding()) {
        z_time += (t - z_time) / Z_MCLK * Z_MCLK;
    } else {
        uint64_t p = pins;
        while (z_time + Z_MCLK <= t) {
            p = z80_tick(&cpu, p);
            z_time += Z_MCLK;
            counts.clocks++;
            if (p & Z80_MREQ) {
                if (p & Z80_RD) {
                    uint8_t v = z_read(Z80_GET_ADDR(p), cpu.pc);
                    Z80_SET_DATA(p, v);
                } else if (p & Z80_WR) {
                    z_write(Z80_GET_ADDR(p), Z80_GET_DATA(p));
                }
            } else if (p & Z80_IORQ) {
                counts.io_cycles++;
                if (p & Z80_RD)
                    Z80_SET_DATA(p, 0xFFu);
            }
        }
        pins = p;
    }
    pc_ym_run_to(t);
    pc_psg_run_to(t);
    inside = 0;
}

/* ---- the 68000's side */
void pc_z80_reset(void)
{
    static int once;
    uint64_t now = pc_clock_now();
    memset(ram, 0, sizeof(ram));
    memset(cart_write_time, 0, sizeof(cart_write_time));
    bus_requested = 0;
    in_reset = 1;
    inside = 0;
    bank = 0;
    wait_acc = 0;
    z_time = now;
    pins = z80_init(&cpu);
    pc_ym_power(now);
    pc_psg_power(now);
    if (!once) {
        once = 1;
        atexit(report_at_exit);
    }
}

const uint8_t *pc_z80_ram(void) { return ram; }

/* On the Z80's 8 bit bus a 68000 word reaches one byte, the high one. */
static uint32_t high_byte(int size, uint32_t v)
{
    return (size == 1 ? v : size == 2 ? v >> 8 : v >> 24) & 0xFFu;
}

uint32_t pc_z80_read(int size, uint32_t a)
{
    uint32_t v = 0u;
    pc_counts.z80_reads++;
    a &= 0xFFFFu;
    if (bus_requested && a < 0x4000u) {
        v = ram[a & 0x1FFFu];
        v = size == 1 ? v : size == 2 ? v * 0x101u : v * 0x01010101u;
    }
    return v;
}

static void write_byte_from_68000(uint32_t a, uint32_t v, uint64_t t)
{
    if (a < 0x4000u) {
        ram[a & 0x1FFFu] = (uint8_t)v;
        log_put(t, LOG_WRITE, LOG_BY_68000, a & 0x1FFFu, v);
    } else if (a < 0x6000u) {
        pc_ym_write(a, v, t);
        log_put(t, LOG_WRITE, LOG_BY_68000 | LOG_CHIP, a, v);
    } else if (a < 0x6100u) {
        bank = (bank >> 1 | (v & 1u) << 8) & 0x1FFu;
    }
}

void pc_z80_write(int size, uint32_t a, uint32_t v)
{
    uint64_t t = pc_sound_time();
    pc_counts.z80_writes++;
    pc_z80_run_to(t);
    if (!bus_requested) {
        counts.no_grant_writes++;
        return;
    }
    a &= 0xFFFFu;
    write_byte_from_68000(a, high_byte(size, v), t);
    if (size == 4)
        write_byte_from_68000((a + 2u) & 0xFFFFu, (v >> 8) & 0xFFu, t);
}

uint32_t pc_z80_busreq_read(int size)
{
    uint32_t bit = bus_requested ? 0u : 1u;    /* 0 = the bus is ours; the game only ever tests this bit */
    return size == 1 ? bit : bit << 8;
}

void pc_z80_busreq_write(int size, uint32_t v)
{
    int want = (int)(high_byte(size, v) & 1u);
    pc_z80_run_to(pc_sound_time());
    if (want != bus_requested) {
        bus_requested = want;
        counts.bus_requests += (uint64_t)want;
        log_lines(pc_sound_time());
    }
}

void pc_z80_reset_write(int size, uint32_t v)
{
    int want = !(high_byte(size, v) & 1u);
    uint64_t t = pc_sound_time();
    pc_z80_run_to(t);
    if (want != in_reset) {
        in_reset = want;
        if (in_reset) {
            /* The line also resets the FM chip. It does not reach the bank register or the PSG. */
            pins = z80_reset(&cpu);
            pc_ym_reset(t);
            counts.resets++;
        }
        log_lines(t);
    }
}

/* The console's reset button: the bus arbiter's reset one-shot clears the Z80's reset register, so the Z80, and the FM chip on the same line, are held
   until the game lets them go. Its bank register and the bus request stay as they were. */
void pc_z80_console_reset(void) { pc_z80_reset_write(1, 0u); }

void pc_z80_psg_write_from_68000(uint32_t v)
{
    uint64_t t = pc_sound_time();
    pc_z80_run_to(t);
    pc_psg_write(v, t);
    log_put(t, LOG_WRITE, LOG_BY_68000, 0x7F11u, v & 0xFFu);
}
