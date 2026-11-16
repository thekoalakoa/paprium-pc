/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The cart as the firmware answers it. The firmware's host build (its core and its x86 board, board/host/s1_board.c, unchanged) serves every mailbox, and the parts the
 * real cart does in logic are done here. No CPU MATE brain is linked: this is the firmware with CPU MATE off.
 *
 * Time. The firmware runs in a fiber of its own, one board call at a time, on the board timer. The board timer is the machine's master clock, so a frame is 262 x 3,420
 * ticks on both. When the 68000 or a video chip DMA touches the cart at clock T, the firmware first runs until its timer reaches T, and then the access is answered. A job
 * that waits for time or for the video chip's frames just spends its turns waiting, while the 68000 goes on. Only the firmware's board calls cost it time (the host board's
 * figure, 8 ticks each): its own arithmetic costs nothing, which is the biggest thing this model leaves out. Idle stretches are skipped to the next timer deadline.
 *
 * What the logic does on the real board, and what stands in here:
 *   - the cart bus: RAM $0000-$1FDF, the register page $1FE0-$1FFF, the FM view of $2000-$7FFF, the window $C000-$FFFF as a read queue whose pointer keeps going past the
 *     stream's end, and the rule that a byte write stores the byte on both halves of its word;
 *   - the sprite decoder: the host board's own model, which decodes at the START in no time;
 *   - the synth: pc_synth.c, a copy in C of the logic, on the firmware's own views. Its blocks take no time, as in the reference runner.
 *   - the hold: the firmware's MegaWire screen holds the whole console, puts a program in the cart RAM, and lets go. pc_cart_holding tells the machine.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "sched.h"
#include "sched_hooks.h"
#include "appinit.h"
#include "s1_interim.h"
#include "wdog.h"
#include "mw.h"
#include "song.h"

#include "pc_cart.h"
#include "pc_clock.h"
#include "pc_fiber.h"
#include "pc_sound.h"
#include "pc_synth.h"
#include "pc_vdp.h"

/* ---- the cart bus map, as the real cart's logic decodes it */
#define BUS_RAM_END     0x1FE0u     /* cart RAM below this */
#define BUS_REG_END     0x2000u     /* the register page below this */
#define BUS_FM_END      0x8000u     /* the FM view below this */
#define BUS_WINDOW      0xC000u     /* the window from here to 64 KB */
#define FM_XOR          0xA8A2u

/* The video chip's vertical sync start on the first frame: line 224's VBlank start plus the lag the firmware names in core/dispatch/jobs.c (10 lines + 3,372 clocks). The
   machine's line 0 starts at T = 0, so that is the time on the board's timer too. */
#define VS_FIRST        (224u * PC_MCLK_LINE + 10u * PC_MCLK_LINE + 3372u)

static PcCartConfig cfg;
uint8_t pc_mwtext[3u << 16];         /* the MegaWire text view (pc_board_impl.h) */

static PcFiber fw_fiber, main_fiber;
static uint32_t limit;              /* the firmware runs until its timer reaches this */
static int running;
static int holding;

static uint64_t n_reads, n_writes, n_window, n_over, n_runs, n_ignored_writes;
static uint64_t host_ticks;         /* the PC's own time spent inside the firmware's turns */
volatile uint8_t pc_cart_in_turn;
static uint32_t fatal_code, fatal_count;
static uint32_t bank_state;         /* 0 not asked, 1 unpacked, 2 failed, 3 left empty */

/* ---- the firmware's turn */
static void fw_on_point(void)
{
    int at_limit = running && (int32_t)(hb.now - limit) >= 0;
    pc_synth_step(at_limit ? limit : hb.now);   /* no further than the turn's end, so an arrival there is ahead of every later instant */
    if (at_limit)
        pc_fiber_switch(main_fiber);
}

/* Nothing to do until the next deadline: jump there. A post can only come from the 68000, and the 68000 runs only while this fiber is parked. */
static void fw_idle_skip(void)
{
    uint32_t next = limit, wake = pc_synth_next_wake();
    if (hb.ten && (int32_t)(hb.tcmp - hb.now) > 0 && (int32_t)(next - hb.tcmp) > 0)
        next = hb.tcmp;
    if ((int32_t)(wake - hb.now) > 0 && (int32_t)(next - wake) > 0)
        next = wake;
    if ((int32_t)(next - hb.now) > (int32_t)hb.step)
        hb.now = next - hb.step;
    hb_point();
}

/* The fatal halt with its waits and its LEDs left out: interrupts off for good, the 68000 runs on what it has. */
void fatal_halt(uint32_t code, uint32_t a, uint32_t b)
{
    (void)a;
    (void)b;
    (void)brd_irq_save();
    if (!fatal_count++)
        fatal_code = code;
    printf("firmware fatal halt, code %08X, at board time %u\n", (unsigned)code, (unsigned)hb.now);
    fflush(stdout);
    for (;;)
        fw_idle_skip();
}

void hb_full_reset_hook(void)
{
    printf("firmware asked for its own reset at board time %u: this program has no way to do that\n", (unsigned)hb.now);
    fflush(stdout);
    exit(7);
}

/* The earlier firmware's audio entries. This build's dispatcher never calls the bodies that name them: the Pocket's linker drops those bodies, and this link keeps them, so
   they need something to point at. */
static void never(const char *what)
{
    printf("firmware called %s, which this build should not reach\n", what);
    fflush(stdout);
    exit(7);
}
void sfx_play(unsigned char a) { (void)a; never("sfx_play"); }
void sfx_loop(unsigned char a) { (void)a; never("sfx_loop"); }
void sfx_d2(unsigned char a) { (void)a; never("sfx_d2"); }
void sfx_mode88(unsigned int a) { (void)a; never("sfx_mode88"); }
void s1_sfx_kick(void) { never("s1_sfx_kick"); }
void s1_vu_stop(void) { never("s1_vu_stop"); }
void mdp_stop(void) { never("mdp_stop"); }

/* The MegaWire code lives in the Pocket's SRAM image and is checked at boot. Here it is linked in, so it passes. */
uint32_t brd_sram_image_ok(void) { return 1u; }
uint32_t s3_sram_cold_ok(void) { return 1u; }

/* ---- the wave bank. The Pocket unpacks it before the 68000 runs and checks at the game's first post that it came from the stream the game names. Here it is unpacked when
   the game names it, in no firmware time, with the firmware's own code. The same path serves the sound round. */
#include "wvbank.h"
#include "boot/c4b.h"

static uint32_t bank_src;
static int32_t bank_rd(void *dst, int32_t n)
{
    if (n < 0)
        n = 0;
    if ((uint32_t)n > cfg.rom_size - bank_src)
        n = (int32_t)(cfg.rom_size - bank_src);
    memcpy(dst, cfg.rom_bytes + bank_src, (size_t)n);
    bank_src += (uint32_t)n;
    return n;
}

static void bank_put(uint32_t off, const uint8_t *b, uint32_t n)
{
    uint8_t *d = (uint8_t *)hb.cram + BRD_CRAM_BANK_OFF + off;
    if (b)
        memcpy(d, b, n);
    else
        memset(d, off < WVBANK_DIR_BYTES ? 0x00 : 0x80, n);
}

/* The SFX bank: the ROM's words to the sound memory, where the synth reads them. The Pocket's boot overlay does this at its first boot, with these same checks. A big-endian long of the game file: */
static uint32_t rom_be32(uint32_t a)
{
    const uint8_t *p = cfg.rom_bytes + a;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void sfx_bank_copy(void)
{
    uint32_t a0, lo, hi;
    if (cfg.rom_size < C4B_ROM_TABLE_PTR + 4u)
        return;
    a0 = rom_be32(C4B_ROM_TABLE_PTR);
    if ((a0 & 1u) || a0 > cfg.rom_size - 0x800u)
        return;
    lo = rom_be32(a0 + C4B_A0_SFX_LO);
    hi = rom_be32(a0 + C4B_A0_SFX_HI);
    if (((lo | hi) & 3u) || hi <= lo || hi > cfg.rom_size || hi - lo > C4B_SFX_ROOM)
        return;
    memcpy((uint8_t *)hb.cram + BRD_CRAM_SFXBANK_OFF, cfg.rom_words + lo, hi - lo);
}

uint32_t board_bank_ok(uint32_t stream)
{
    wvbank_result r;
    if (bank_state)
        return bank_state == 1u;
    sfx_bank_copy();
    if (cfg.no_bank) {
        bank_state = 3u;
        return 1u;
    }
    bank_src = stream;
    wvbank_unpack(bank_rd, bank_put, &r);
    bank_state = r.why == WVBANK_OK ? 1u : 2u;
    printf("wave bank: %u samples, %u block errors, result %u\n", (unsigned)r.total, (unsigned)r.crc_errors, (unsigned)r.why);
    fflush(stdout);
    return bank_state == 1u;
}

/* ---- the board's boot, in the order of the Pocket's board for this build, without what only the Pocket has: the boot cache's bar, the debug save, the sub-CPU's link. */
#define FW_DTOUT_OFF    0x03Cu
#define FW_DTOUT_WORD   (1410u | 0u << 16 | 11u << 17)

static void fw_boot_steps(void)
{
    sched_init();
    boot_page();
    s1_interim_boot();
    app_start();
    brd_menu_mute(1u);
    brd_opt_at_reset(0u);
    brd_synth_blkie(1u);
    brd_synth_wr32(FW_DTOUT_OFF, FW_DTOUT_WORD);
    brd_cause_ack(BRD_CAUSE_ALL | BRD_CAUSE_BLK);
    brd_msi(0u);
    brd_timer_disarm();
    wdog_start();
    brd_lines_on(BRD_LINES_ALL);
    brd_irq_enable();
    sched_work_post();
    brd_md_hold(0u);
}

static void PC_FIBER_CALL fw_main(void *arg)
{
    (void)arg;
    fw_boot_steps();
    running = 1;
    limit = hb.now;
    for (;;) {
        if (!sched_thread_once())
            fw_idle_skip();
        else
            hb_point();
    }
}

void pc_cart_boot(const PcCartConfig *c)
{
    cfg = *c;
    hb.flash8 = cfg.rom_bytes;
    hb.flash16 = cfg.rom_words;
    hb.rom = (uint16_t *)(void *)cfg.rom_words;
    hb_reset();
    hb.rom = (uint16_t *)(void *)cfg.rom_words;
    hb.options = cfg.options;
    if (cfg.step)
        hb.step = cfg.step;
    hb.vs_next = VS_FIRST;
    hb.on_point = fw_on_point;
    pc_synth_reset();
    if (cfg.save) {
        uint32_t n = cfg.save_size < BRD_SAVE_BYTES ? cfg.save_size : BRD_SAVE_BYTES;
        memcpy(hb.save, cfg.save, n);
    }
    fw_fiber = pc_fiber_create(1u << 20, 1u << 24, fw_main);
    if (!fw_fiber) {
        fprintf(stderr, "no fiber for the firmware\n");
        exit(2);
    }
    main_fiber = pc_fiber_current();
    pc_fiber_switch(fw_fiber);      /* it comes back at its first board call after the release */
    holding = hb.hold != 0u;
}

void pc_cart_copy_boot_page(uint8_t *low_page, uint32_t bytes)
{
    uint32_t i;
    memcpy(low_page, hb.ram, BRD_CART_RAM_BYTES < bytes ? BRD_CART_RAM_BYTES : bytes);
    for (i = 0; i < 0x20u; i++)
        low_page[BUS_RAM_END + i] = 0;
}

const uint8_t *pc_cart_low(uint32_t *ram_bytes)
{
    if (ram_bytes)
        *ram_bytes = BUS_RAM_END;
    return (const uint8_t *)hb.ram;
}

static inline void fw_run_to(uint64_t t)
{
    uint32_t to = (uint32_t)t;
    pc_z80_run_to(t);   /* so its reads of the cart and this turn's writes land in time order */
    if ((int32_t)(to - hb.now) <= 0)
        return;
    limit = to;
    n_runs++;
    main_fiber = pc_fiber_current();
    {
        uint64_t t0 = pc_fiber_host_time(), t1;
        pc_cart_in_turn = 1;
        pc_fiber_switch(fw_fiber);
        pc_cart_in_turn = 0;
        t1 = pc_fiber_host_time();
        host_ticks += t1 - t0;
    }
    if ((hb.hold != 0u) != holding) {
        holding = hb.hold != 0u;
        if (holding)
            pc_tick_stop_request();
    }
}

void pc_cart_run_to(uint64_t t) { fw_run_to(t); }
int pc_cart_holding(void) { return holding; }

/* The level latch the cart's logic raises for the console's reset button, as the Pocket core raises it; the firmware's level 1 handler takes it. */
void pc_cart_console_reset(uint64_t t)
{
    fw_run_to(t);
    hb.cause |= BRD_CAUSE_V;
}

int pc_cart_console_reset_pending(void) { return (hb.cause & BRD_CAUSE_V) != 0u; }

/* One word out of the window. The pointer steps on every word delivered, also past the stream's end, as the real cart's logic does. */
static uint32_t window_word(void)
{
    uint32_t v = *(const uint16_t *)(const void *)(hb.workspace + (hb.sptr & BRD_STREAM_MAX_WORDS));
    hb.sptr = (hb.sptr + 2u) & BRD_STREAM_MAX_WORDS;
    n_window++;
    if (hb.remaining) {
        if (--hb.remaining == 0u)
            hb.stream_valid = 0u;
    } else {
        n_over++;
    }
    return v;
}

static int window_on(uint32_t a)
{
    return a >= BUS_WINDOW && hb.window_en && !hb.mini_view;
}

static uint32_t file_word(uint32_t a)
{
    uint32_t v = ((const uint16_t *)(const void *)hb.rom)[a >> 1];
    if (a >= BUS_REG_END && a < BUS_FM_END && !hb.mini_view)
        v ^= FM_XOR;
    return v;
}

uint32_t pc_cart_r16(uint32_t a, uint64_t t)
{
    a &= 0xFFFEu;
    fw_run_to(t);
    n_reads++;
    pc_counts.cart_reads++;
    if (a < BUS_RAM_END)
        return hb.ram[a >> 1];
    if (a < BUS_REG_END)
        return hb_68k_read(a - BUS_RAM_END);
    if (window_on(a))
        return window_word();
    return file_word(a);
}

uint32_t pc_cart_r8(uint32_t a, uint64_t t)
{
    uint32_t w;
    a &= 0xFFFFu;
    fw_run_to(t);
    n_reads++;
    pc_counts.cart_reads++;
    if (a < BUS_RAM_END)
        w = hb.ram[a >> 1];
    else if (a < BUS_REG_END)
        w = hb_68k_read((a & ~1u) - BUS_RAM_END);
    else if (window_on(a))
        w = window_word();
    else
        w = file_word(a & ~1u);
    return (a & 1u) ? (w & 0xFFu) : (w >> 8);
}

void pc_cart_w16(uint32_t a, uint32_t v, uint64_t t)
{
    a &= 0xFFFEu;
    fw_run_to(t);
    n_writes++;
    pc_counts.cart_writes++;
    if (holding)
        return;     /* the turn above took the console off the bus */
    if (a < BUS_RAM_END)
        hb.ram[a >> 1] = (uint16_t)v;
    else if (a < BUS_REG_END) {
        uint32_t arrivals = hb.arr;
        hb_68k_write(a - BUS_RAM_END, (uint16_t)v);
        if (hb.arr != arrivals) {
            pc_synth_arrival(t);
            pc_sound_log_post(t, a, v);
        }
    } else {
        n_ignored_writes++;
    }
}

void pc_cart_w8(uint32_t a, uint32_t v, uint64_t t)
{
    v &= 0xFFu;
    pc_cart_w16(a, v | v << 8, t);      /* the 68000 drives the byte on both halves; the cart stores the whole word */
}

static uint64_t fnv(const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    uint64_t h = 0xCBF29CE484222325ull;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= b[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

void pc_cart_frame_line(void *file, uint32_t frame)
{
    fprintf((FILE *)file, "%u cart %016llX now %u vs %u arr %u done %u blk %u window %llu fe6 %04X fe4 %04X\n", (unsigned)frame,
            (unsigned long long)fnv(hb.ram, BRD_CART_RAM_BYTES), (unsigned)hb.now, (unsigned)hb.vs, (unsigned)hb.arr,
            (unsigned)hb.done, (unsigned)BRD_SYNTH_BLKCNT(hb.sy_status), (unsigned long long)n_window, (unsigned)hb.freg[BRD_FREG_FE6 >> 1],
            (unsigned)hb.freg[BRD_FREG_FE4 >> 1]);
}

uint32_t pc_cart_fatal_count(void) { return fatal_count; }
uint32_t pc_cart_fatal_code(void) { return fatal_code; }
uint64_t pc_cart_host_ticks(void) { return host_ticks; }

void pc_cart_save_copy(uint8_t *out, uint32_t bytes)
{
    memcpy(out, hb.save, bytes < BRD_SAVE_BYTES ? bytes : BRD_SAVE_BYTES);
}

/* The save's last byte, outside the four places pc_save.c checks, so a save that checked good still does after the change. */
void pc_cart_save_test_change(void) { hb.save[BRD_SAVE_BYTES - 1u] ^= 0xFFu; }

void pc_cart_report(void)
{
    printf("firmware: board time %u, %llu turns, levels taken MEI %u MTI %u MSI %u, deepest %u\n",
           (unsigned)hb.now, (unsigned long long)n_runs, (unsigned)hb.taken[0], (unsigned)hb.taken[1],
           (unsigned)hb.taken[2], (unsigned)hb.depth_max);
    printf("firmware: arrivals %u done %u (16 bits), frames seen %u, decoder starts %u flags %08X, mini view %u, hold %u\n",
           (unsigned)hb.arr, (unsigned)hb.done, (unsigned)hb.vs, (unsigned)hb.dec_starts, (unsigned)hb.dec_flags,
           (unsigned)hb.mini_view, (unsigned)hb.hold);
    printf("firmware: cart reads %llu writes %llu (%llu nowhere), window words %llu (%llu past a stream's end)\n",
           (unsigned long long)n_reads, (unsigned long long)n_writes, (unsigned long long)n_ignored_writes,
           (unsigned long long)n_window, (unsigned long long)n_over);
    printf("firmware: wave bank state %u\n", (unsigned)bank_state);
    pc_synth_report();
    printf("firmware: fatal halts %u, first code %08X, MegaWire on %u\n", (unsigned)fatal_count, (unsigned)fatal_code,
           (unsigned)s3_mw_on);
    fflush(stdout);
}
