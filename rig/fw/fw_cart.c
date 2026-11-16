/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The cart as the firmware answers it. The reference machine's cart model steps aside and this file stands on the
 * cart's bus: the firmware's host build (its core and its x86 board, board/host/s1_board.c, unchanged) serves every
 * mailbox, and the parts the real cart does in logic are done here. No brain is linked: this is the firmware with
 * CPU MATE off.
 *
 * Time. The firmware runs in a fiber of its own, one board call at a time, on the board timer. The board timer is the
 * machine's master clock, so a frame is 262 x 3,420 ticks on both. When the 68000, the Z80 or a VDP DMA touches the
 * cart at clock T, the firmware first runs until its timer reaches T, and then the access is answered. A job that
 * waits for time or for the VDP's frames just spends its turns waiting, while the 68000 goes on. Only the firmware's
 * board calls cost it time (the host board's figure, 8 ticks each): its own arithmetic costs nothing, which is the
 * biggest thing this model leaves out. Idle stretches are skipped to the next timer deadline.
 *
 * What the logic does on the real board, and what stands in here:
 *   - the cart bus: RAM $0000-$1FDF, the register page $1FE0-$1FFF, the FM view of $2000-$7FFF, the window $C000-$FFFF
 *     as a read FIFO whose pointer keeps going past the stream's end, and the rule that a byte write stores the byte
 *     on both halves of its word. docs/CART_FPGA_SPEC.md, as the RTL builds it;
 *   - the sprite decoder: the host board's own model, which decodes at the START in no time;
 *   - the synth: a stand-in that only keeps its schedule (synth_step below). It mixes nothing, so wave voices make no
 *     sound, their envelopes do not move, SFX channels never end by themselves, and the output rings stay zero.
 * Nothing else of the machine is here: the VDP, the Z80, the YM and the pads stay the reference machine's.
 *
 * Both sides of the lockstep get the same answers from here by construction, as they did from the cart model, because
 * the reference alone talks to the cart and the translated side reads its answers from the queue.
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

#include "fw_cart.h"

/* The Windows fiber calls, declared here because <windows.h> and the firmware's headers do not mix. */
extern void *__attribute__((stdcall)) CreateFiberEx(size_t, size_t, unsigned long, void (__attribute__((stdcall)) *)(void *), void *);
extern void __attribute__((stdcall)) SwitchToFiber(void *);
extern int __attribute__((stdcall)) QueryPerformanceCounter(long long *);
extern int __attribute__((stdcall)) QueryPerformanceFrequency(long long *);
#define FLOAT_SWITCH 1ul

/* ---- the cart bus map, as the real cart's logic decodes it */
#define BUS_RAM_END     0x1FE0u     /* cart RAM below this */
#define BUS_REG_END     0x2000u     /* the register page below this */
#define BUS_FM_END      0x8000u     /* the FM view below this */
#define BUS_WINDOW      0xC000u     /* the window from here to 64 KB */
#define FM_XOR          0xA8A2u

#define MCLK_LINE       3420u
#define MCLK_FRAME      (262u * MCLK_LINE)
/* The VDP's vertical sync start on the first frame: line 224's VBlank start plus the lag the firmware names in
   core/dispatch/jobs.c (10 lines + 3,372 clocks). */
#define VS_FIRST        (224u * MCLK_LINE + 10u * MCLK_LINE + 3372u)

static FwConfig cfg;
static uint8_t low[0x10000];        /* what the machine fetches code from: cart RAM, then the file */
uint8_t rig_fw_mwtext[0x30000];     /* the MegaWire text view (board_impl.h) */

static void *fw_fiber, *fw_caller;
static uint32_t limit;              /* the firmware runs until its timer reaches this */
static int running;
static uint32_t frame_base;

static uint64_t n_reads, n_writes, n_window, n_over, n_runs, n_ignored_writes;
static long long host_ticks;        /* the PC's own time spent inside the firmware's turns */
static uint32_t fatal_code, fatal_count;
static uint32_t bank_state;         /* 0 not asked, 1 unpacked, 2 failed, 3 left empty */

/* ---- the synth's schedule, nothing more.
 * The real synth takes a block instant 300 times a second (48 kHz, 160 samples), each waiting until every 68000 write
 * made before it has been served (MCU_INTERFACE.md v1.3 3.10.5), and BUSY falls when it is done. Here the wait is
 * kept, the block itself takes no time and does nothing, BLKCNT moves and CAUSE.BLK rises. That is what lets the
 * firmware's main-loop pass run at the rate it does on the cart: $1FE6 bit 3, $1B00 and the Boom Box feed. */
#define SY_BLOCK_MCLK   178977u
#define SY_TIMEOUT      268465u
#define SY_Q            16u
static struct { uint16_t need; uint32_t t; } sy_q[SY_Q];
static uint32_t sy_h, sy_n, sy_next, sy_blk, sy_on, sy_timeouts, sy_dropped;

static void synth_step(void)
{
    uint32_t mode = hb.sy_config & 3u;
    if (mode != 1u && mode != 2u)
        return;
    if (!sy_on) {
        sy_on = 1u;
        sy_next = hb.now + SY_BLOCK_MCLK;
    }
    while ((int32_t)(hb.now - sy_next) >= 0) {
        if (sy_n < SY_Q) {
            uint32_t k = (sy_h + sy_n) % SY_Q;
            sy_q[k].need = (uint16_t)hb.arr;
            sy_q[k].t = sy_next;
            sy_n++;
        } else {
            sy_dropped++;
        }
        sy_next += SY_BLOCK_MCLK;
    }
    while (sy_n) {
        int served = (int16_t)((uint16_t)hb.done - sy_q[sy_h].need) >= 0;
        if (!served) {
            if ((uint32_t)(hb.now - sy_q[sy_h].t) < SY_TIMEOUT)
                break;
            sy_timeouts++;
        }
        sy_blk = (sy_blk + 1u) & 0xFFFu;
        hb.cause |= BRD_CAUSE_BLK;
        sy_h = (sy_h + 1u) % SY_Q;
        sy_n--;
    }
    hb.sy_status = (hb.sy_status & ~(0x0FFF0000u | 0x1E0u)) | sy_blk << 16 | (sy_n < 15u ? sy_n : 15u) << 5;
}

/* ---- the firmware's turn */
static void fw_on_point(void)
{
    synth_step();
    if (running && (int32_t)(hb.now - limit) >= 0)
        SwitchToFiber(fw_caller);
}

/* Nothing to do until the next deadline: jump there. A post can only come from the 68000, and the 68000 runs only
   while this fiber is parked. */
static void fw_idle_skip(void)
{
    uint32_t next = limit;
    if (hb.ten && (int32_t)(hb.tcmp - hb.now) > 0 && (int32_t)(next - hb.tcmp) > 0)
        next = hb.tcmp;
    if (sy_on && (int32_t)(sy_next - hb.now) > 0 && (int32_t)(next - sy_next) > 0)
        next = sy_next;
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
    printf("firmware asked for its own reset at board time %u: the rig has no way to do that\n", (unsigned)hb.now);
    fflush(stdout);
    exit(7);
}

/* The earlier firmware's audio entries. This build's dispatcher never calls the bodies that name them: the Pocket's
   linker drops those bodies, and this link keeps them, so they need something to point at. */
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

/* ---- the wave bank. The Pocket unpacks it before the 68000 runs and checks at C6 that it came from the stream the
   game names. Here it is unpacked when the game names it, in no firmware time, with the firmware's own code. */
#include "wvbank.h"

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

uint32_t board_bank_ok(uint32_t stream)
{
    wvbank_result r;
    if (bank_state)
        return bank_state == 1u;
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

/* ---- the board's boot, in the order of board/pocket/s1_board.c brd_boot for this build, without what only the
   Pocket has: the boot cache's bar, the debug save, the sub-CPU's link. */
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

static void __attribute__((stdcall)) fw_main(void *arg)
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

void fwc_configure(const FwConfig *c)
{
    cfg = *c;
}

void fwc_boot(uint8_t *rom_words, uint32_t size)
{
    uint32_t i;
    (void)size;
    hb.flash8 = cfg.rom_bytes;
    hb.flash16 = rom_words;
    hb.rom = (uint16_t *)(void *)rom_words;
    hb_reset();
    hb.rom = (uint16_t *)(void *)rom_words;
    hb.options = cfg.options;
    if (cfg.step)
        hb.step = cfg.step;
    hb.vs_next = VS_FIRST;
    hb.on_point = fw_on_point;
    if (cfg.save) {
        uint32_t n = cfg.save_size < BRD_SAVE_BYTES ? cfg.save_size : BRD_SAVE_BYTES;
        memcpy(hb.save, cfg.save, n);
    }
    /* What the machine fetches from outside the cart RAM is the file. A fetch in the FM view does not happen. */
    memcpy(low, rom_words, sizeof(low));
    fw_fiber = CreateFiberEx(1u << 20, 1u << 24, FLOAT_SWITCH, fw_main, NULL);
    if (!fw_fiber) {
        fprintf(stderr, "no fiber for the firmware\n");
        exit(2);
    }
    fw_caller = cfg.current_fiber();
    SwitchToFiber(fw_fiber);    /* it comes back at its first board call after the release */
    memcpy(low, hb.ram, BRD_CART_RAM_BYTES);
    for (i = 0; i < 0x20u; i++)
        low[BUS_RAM_END + i] = 0;
}

uint8_t *fwc_low(void)
{
    return low;
}

static void fw_run_to(int t)
{
    uint32_t to = frame_base + (uint32_t)t;
    if ((int32_t)(to - hb.now) <= 0)
        return;
    limit = to;
    n_runs++;
    fw_caller = cfg.current_fiber();
    {
        long long t0, t1;
        QueryPerformanceCounter(&t0);
        SwitchToFiber(fw_fiber);
        QueryPerformanceCounter(&t1);
        host_ticks += t1 - t0;
    }
    memcpy(low, hb.ram, BUS_RAM_END);
}

/* One word out of the window. The pointer steps on every word delivered, also past the stream's end, as the RTL does. */
static unsigned int window_word(void)
{
    unsigned int v = *(const uint16_t *)(const void *)(hb.workspace + (hb.sptr & BRD_STREAM_MAX_WORDS));
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

static int window_on(unsigned int a)
{
    return a >= BUS_WINDOW && hb.window_en && !hb.mini_view;
}

static unsigned int file_word(unsigned int a)
{
    unsigned int v = ((const uint16_t *)(const void *)hb.rom)[a >> 1];
    if (a >= BUS_REG_END && a < BUS_FM_END && !hb.mini_view)
        v ^= FM_XOR;
    return v;
}

unsigned int fwc_r16(unsigned int a, int t)
{
    a &= 0xFFFEu;
    fw_run_to(t);
    n_reads++;
    if (a < BUS_RAM_END)
        return hb.ram[a >> 1];
    if (a < BUS_REG_END)
        return hb_68k_read(a - BUS_RAM_END);
    if (window_on(a))
        return window_word();
    return file_word(a);
}

unsigned int fwc_r8(unsigned int a, int t)
{
    unsigned int w;
    a &= 0xFFFFu;
    fw_run_to(t);
    n_reads++;
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

void fwc_w16(unsigned int a, unsigned int v, int t)
{
    a &= 0xFFFEu;
    fw_run_to(t);
    n_writes++;
    if (a < BUS_RAM_END) {
        hb.ram[a >> 1] = (uint16_t)v;
        *(uint16_t *)(void *)(low + a) = (uint16_t)v;
    } else if (a < BUS_REG_END) {
        hb_68k_write(a - BUS_RAM_END, (uint16_t)v);
    } else {
        n_ignored_writes++;
    }
}

void fwc_w8(unsigned int a, unsigned int v, int t)
{
    v &= 0xFFu;
    fwc_w16(a, v | v << 8, t);      /* the 68000 drives the byte on both halves; the cart stores the whole word */
}

void fwc_frame_end(unsigned int frame_mclk, int t)
{
    frame_base += frame_mclk;
    fw_run_to(t);
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

void fwc_frame_line(void *file, int frame)
{
    fprintf((FILE *)file, "%d cart %016llX now %u vs %u arr %u done %u blk %u window %llu fe6 %04X fe4 %04X\n", frame,
            (unsigned long long)fnv(hb.ram, BRD_CART_RAM_BYTES), (unsigned)hb.now, (unsigned)hb.vs, (unsigned)hb.arr,
            (unsigned)hb.done, (unsigned)sy_blk, (unsigned long long)n_window, (unsigned)hb.freg[BRD_FREG_FE6 >> 1],
            (unsigned)hb.freg[BRD_FREG_FE4 >> 1]);
}

void fwc_report(void)
{
    long long freq = 1;
    QueryPerformanceFrequency(&freq);
    printf("firmware: board time %u, %llu turns taking %lld ms of this PC, levels taken MEI %u MTI %u MSI %u, deepest %u\n",
           (unsigned)hb.now, (unsigned long long)n_runs, host_ticks * 1000 / freq, (unsigned)hb.taken[0], (unsigned)hb.taken[1],
           (unsigned)hb.taken[2], (unsigned)hb.depth_max);
    printf("firmware: arrivals %u done %u (16 bits), frames seen %u, decoder starts %u flags %08X, mini view %u, hold %u\n",
           (unsigned)hb.arr, (unsigned)hb.done, (unsigned)hb.vs, (unsigned)hb.dec_starts, (unsigned)hb.dec_flags,
           (unsigned)hb.mini_view, (unsigned)hb.hold);
    printf("firmware: cart reads %llu writes %llu (%llu nowhere), window words %llu (%llu past a stream's end)\n",
           (unsigned long long)n_reads, (unsigned long long)n_writes, (unsigned long long)n_ignored_writes,
           (unsigned long long)n_window, (unsigned long long)n_over);
    printf("firmware: synth stand-in: blocks %u (mod 4096), timeouts %u, instants dropped %u, wave bank state %u\n",
           (unsigned)sy_blk, (unsigned)sy_timeouts, (unsigned)sy_dropped, (unsigned)bank_state);
    printf("firmware: fatal halts %u, first code %08X, MegaWire on %u\n", (unsigned)fatal_count, (unsigned)fatal_code,
           (unsigned)s3_mw_on);
    fflush(stdout);
}
