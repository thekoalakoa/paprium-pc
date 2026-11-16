/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The PC program's machine alone, on made-up data: the clock, the video chip's ports and its log, the interrupts and the tick, the pads, the Z80 seam, the bus, the save. No game
 * byte and no firmware: a small cart stands in here. Built and run by tests/test_all.py, which also builds it with one rule broken at a time and checks that a test here fails.
 * tests/pc_cart_test.c covers the firmware as the cart.
 *
 * It prints one line for each failed check, then the count, then RESULT: PASS or RESULT: FAIL. An argument names one group of checks to run.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pt_rt.h"
#include "pc_cart.h"
#include "pc_clock.h"
#include "pc_frame.h"
#include "pc_io.h"
#include "pc_save.h"
#include "pc_vdp.h"
#include "pc_z80.h"

PtCtx pc_ctx;

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* ---- what the runtime gives the machine */
u8 *test_rom;
void pt_watch_hit(int index) { (void)index; }
int pt_interp_only;

/* No translated code is linked here, so the runtime sends an interrupt's handler to this stand-in. It does the scene's work, then what a translated rte does. */
static void (*handler_work)(void);
static uint32_t handler_entries;
u32 pt_fallback(PtCtx *c, u32 pc)
{
    u32 sr, back;
    (void)pc;
    handler_entries++;
    if (handler_work)
        handler_work();
    if (c->stopped)
        return 0xFFFFFFFFu;
    sr = R16(c->a[7]);
    c->a[7] += 2;
    back = R32(c->a[7]);
    c->a[7] += 4;
    c->imask = (u8)((sr >> 8) & 7u);
    c->fn = (u8)((sr >> 3) & 1u);
    c->fz = (u8)((sr >> 2) & 1u);
    c->fv = (u8)((sr >> 1) & 1u);
    c->fc = (u8)(sr & 1u);
    c->rte_flag = 1;
    return back;
}

/* ---- a small cart: 64 KB of words, and a switch for its hold */
static uint16_t cart[0x8000];
static int cart_hold;
static uint64_t cart_last_t;
uint32_t pc_cart_r16(uint32_t a, uint64_t t) { cart_last_t = t; pc_counts.cart_reads++; return cart[(a & 0xFFFFu) >> 1]; }
uint32_t pc_cart_r8(uint32_t a, uint64_t t) { uint32_t w = pc_cart_r16(a & ~1u, t); return (a & 1u) ? w & 0xFFu : w >> 8; }
void pc_cart_w16(uint32_t a, uint32_t v, uint64_t t) { cart_last_t = t; cart[(a & 0xFFFFu) >> 1] = (uint16_t)v; }
void pc_cart_w8(uint32_t a, uint32_t v, uint64_t t) { pc_cart_w16(a, (v & 0xFFu) * 0x101u, t); }
int pc_cart_holding(void) { return cart_hold; }

/* ---- the frame's two ends */
static PcFrame *rec;
static uint32_t frame_starts, frame_ends, last_end_frame;
static uint32_t ended_writes;
void pc_machine_frame_start(uint32_t frame) { frame_starts++; pc_vdp_frame_start(frame); }
void pc_machine_frame_end(uint32_t frame) { frame_ends++; last_end_frame = frame; ended_writes = rec->n_writes; }

/* ---- helpers. The machine's clock is moved by hand here, as the running program would move it. */
#define CPU_PER_LINE 480u
static void reset_all(void)
{
    memset(&pc_counts, 0, sizeof(pc_counts));
    memset(&pc_vdp_seen, 0, sizeof(pc_vdp_seen));
    memset(&pc_ctx, 0, sizeof(pc_ctx));
    memset(rec, 0, sizeof(*rec));
    memset(cart, 0, sizeof(cart));
    cart_hold = 0;
    frame_starts = frame_ends = 0;
    handler_work = NULL;
    handler_entries = 0;
    pc_ctx.imask = 7;
    pc_frame_rec = rec;
    pc_clock_reset();
    pc_tick_init();
    pc_vdp_reset();
    pc_io_reset();
    pc_z80_reset();
    pt_irq_level = 0;
    pt_at = 0;
    pc_vread_first = pc_vread_last = -1;
}

/* Moves the machine's clock to the master clock t, by a stall, so the 68000's scale plays no part. Runs the tick as a block's deadline check would. */
static void go_to(uint64_t t)
{
    uint64_t now = pc_clock_now();
    if (t > now)
        pc_clock_stall(t - now);
    (void)pt_port_tick();
}

static void w16(uint32_t a, uint32_t v) { pt_port_write(2, a, v); }
static uint32_t r16(uint32_t a) { return pt_port_read(2, a); }
#define DATA 0xC00000u
#define CTRL 0xC00004u
static void reg(uint32_t r, uint32_t v) { w16(CTRL, 0x8000u | r << 8 | (v & 0xFFu)); }
static void cmd(uint32_t code, uint32_t a)
{
    w16(CTRL, ((code & 3u) << 14) | (a & 0x3FFFu));
    w16(CTRL, ((code >> 2) << 4) | (a >> 14));
}
static uint64_t line_t(uint32_t frame, uint32_t line, uint32_t h) { return (uint64_t)frame * PC_MCLK_FRAME + (uint64_t)line * PC_MCLK_LINE + h; }
static void display_on(void) { reg(1, 0x44); reg(12, 0x81); reg(15, 2); }

static const char *scratch = ".";

/* ---- the clock */
static void test_clock(void)
{
    reset_all();
    CHECK(pc_clock_now() == 0);
    pt_clock = 1000;
    {
        uint64_t want = (uint64_t)1000 * 7u * (65536u + PC_SCALE_REFRESH_Q16) >> 16;
        CHECK(pc_clock_now() == want);
        CHECK(pc_clock_now() > 7000u && pc_clock_now() < 7200u);    /* refresh is about 1.6 % */
    }
    {
        uint64_t before = pc_clock_now();
        pc_clock_stall(500);
        CHECK(pc_clock_now() == before + 500);                      /* a stall is real time, not scaled */
    }
    /* A Z80 access to the 68000's bus moves the clock like a stall, but it is behind the 68000, so a handler is not held back by it. */
    {
        uint64_t before = pc_clock_now();
        pc_clock_share(91u);
        CHECK(pc_clock_now() == before + 91u);
    }
    /* time as frame, line, position */
    CHECK(pc_time_frame(line_t(3, 7, 11)) == 3);
    CHECK(pc_time_vline(line_t(3, 7, 11)) == 7);
    CHECK(pc_time_hclock(line_t(3, 7, 11)) == 11);
    CHECK(PC_MCLK_FRAME == 262u * 3420u);
}

/* ---- ports */
static void test_ports(void)
{
    const PcVdpState *s;
    reset_all();
    s = pc_vdp_state();
    /* power-on: the H-int register reads as all ones, the rest as 0 */
    CHECK(s->regs[10] == 0xFF && s->regs[7] == 0 && s->regs[1] == 0);
    /* a register write */
    reg(7, 0x3C);
    CHECK(s->regs[7] == 0x3C);
    /* a two word command, VRAM, even and odd addresses, the address's step */
    reg(15, 2);
    cmd(1, 0x1234);
    w16(DATA, 0xABCD);
    CHECK(s->vram[0x1234] == 0xAB && s->vram[0x1235] == 0xCD);
    w16(DATA, 0x1122);
    CHECK(s->vram[0x1236] == 0x11 && s->vram[0x1237] == 0x22);
    cmd(1, 0x2001);
    w16(DATA, 0x5566);                  /* an odd address lands the word swapped, at the even address below */
    CHECK(s->vram[0x2000] == 0x66 && s->vram[0x2001] == 0x55);
    /* a high address bit in the second word */
    cmd(1, 0xC002);
    w16(DATA, 0x7788);
    CHECK(s->vram[0xC002] == 0x77 && s->vram[0xC003] == 0x88);
    /* colours: 64, the unused bits gone */
    cmd(3, 0x0006);
    w16(DATA, 0xFFFF);
    CHECK(s->cram[3] == 0x0EEE);
    /* VSRAM: 40 entries, 11 bits */
    cmd(5, 0x0004);
    w16(DATA, 0xFFFF);
    CHECK(s->vsram[2] == 0x07FF);
    /* a status read clears a half written command, so the next word is a first word again */
    w16(CTRL, 0x4000);
    (void)r16(CTRL);
    w16(CTRL, 0x8710);
    CHECK(s->regs[7] == 0x10);
    /* a data read back, as the game's memory checks do */
    cmd(1, 0x0000);
    w16(DATA, 0xA501);
    cmd(0, 0x0000);
    CHECK(r16(DATA) == 0xA501);
    /* a long is two words, high first */
    pt_port_write(4, CTRL, 0x45000000u);    /* VRAM write command for 0x0500, written as one long */
    pt_port_write(2, DATA, 0xBEEF);
    CHECK(s->vram[0x0500] == 0xBE && s->vram[0x0501] == 0xEF);
    /* the chip's copy of the sprite table, first four bytes of an entry only */
    reg(5, 0x78);
    reg(12, 0x81);
    cmd(1, 0xF000);
    w16(DATA, 0x0123);
    w16(DATA, 0x0F45);
    w16(DATA, 0x6789);
    CHECK(s->sat_cache[0][0] == 0x01 && s->sat_cache[0][1] == 0x23 && s->sat_cache[0][2] == 0x0F && s->sat_cache[0][3] == 0x45);
    CHECK(s->vram[0xF004] == 0x67);
    /* a byte write puts the byte on both halves */
    cmd(1, 0x0100);
    pt_port_write(1, DATA, 0x5A);
    CHECK(s->vram[0x0100] == 0x5A && s->vram[0x0101] == 0x5A);
    /* the PSG address writes go to the seam, not the chip */
    pt_port_write(1, 0xC00011u, 0x9F);
    CHECK(pc_counts.psg_writes == 1);
}

/* ---- status and the V counter */
static void test_status(void)
{
    uint32_t i;
    reset_all();
    /* the H counter, at the points the exact chip's H byte was read */
    CHECK((pc_vdp_hv_at(line_t(0, 5, 130)) & 0xFFu) == 173u);
    CHECK((pc_vdp_hv_at(line_t(0, 5, 431)) & 0xFFu) == 236u);
    CHECK((pc_vdp_hv_at(line_t(0, 5, 2027)) & 0xFFu) == 77u);
    /* display off: the VBlank bit reads 1 everywhere */
    CHECK((pc_vdp_status_at(line_t(0, 100, 2000)) & 8u) != 0);
    display_on();
    CHECK((pc_vdp_status_at(line_t(0, 100, 2000)) & 8u) == 0);
    CHECK((pc_vdp_status_at(line_t(0, 223, 3000)) & 8u) == 0);
    CHECK((pc_vdp_status_at(line_t(0, 224, 2000)) & 8u) != 0);
    CHECK((pc_vdp_status_at(line_t(0, 250, 2000)) & 8u) != 0);
    CHECK((pc_vdp_status_at(line_t(1, 0, 2000)) & 8u) == 0);
    CHECK((pc_vdp_status_at(line_t(0, 261, 2000)) & 8u) == 0);
    /* FIFO empty at rest, not full */
    CHECK((pc_vdp_status_at(line_t(0, 100, 2000)) & 0x300u) == 0x200u);
    /* the V counter: 0-234, then 229-255 */
    for (i = 0; i <= 234u; i++)
        CHECK(pc_vdp_hv_at(line_t(0, i, 100)) >> 8 == i);
    for (i = 235; i < 262u; i++)
        CHECK(pc_vdp_hv_at(line_t(0, i, 100)) >> 8 == i - 6u);
    /* its read through the port, low byte is the H counter */
    go_to(line_t(0, 50, 100));
    CHECK(r16(0xC00008u) >> 8 == 50);
    CHECK(pc_vread_first == 50 && pc_vread_last == 50);
    CHECK(pc_counts.hv_reads == 1);
    /* the status byte alone, as the game's wait loops read it */
    go_to(line_t(0, 225, 100));
    CHECK((pt_port_read(1, 0xC00005u) & 8u) != 0);
    /* a V-int flag shows in bit 7 with its enable off */
    CHECK((pc_vdp_status_at(pc_clock_now()) & 0x80u) != 0);
}

/* ---- the write log: filing by the stamp */
static void test_log(void)
{
    const PcVdpState *s;
    reset_all();
    s = pc_vdp_state();
    display_on();
    go_to(line_t(0, 0, 0));
    CHECK(rec->number == 0);
    go_to(line_t(0, 10, 500));
    cmd(3, 0x0002);
    w16(DATA, 0x0E0E);
    CHECK(rec->n_writes == 1);
    CHECK(rec->writes[0].kind == PC_WRITE_CRAM && rec->writes[0].addr == 1 && rec->writes[0].value == 0x0E0E);
    CHECK(rec->writes[0].vline == 10 && rec->writes[0].hclock == 500);     /* when the 68000 made it: the renderer works out which slot that reaches */
    CHECK(s->cram[1] == 0x0E0E);
    /* the frame's state stays what it was at V = 0 */
    CHECK(rec->start.cram[1] == 0);
    /* a register in the picture is logged at once */
    reg(7, 0x21);
    CHECK(rec->n_writes == 2 && rec->writes[1].kind == PC_WRITE_REG && rec->writes[1].addr == 7 && rec->writes[1].value == 0x21);
    /* in the vertical blank nothing is logged, and the state still takes it */
    go_to(line_t(0, 230, 0));
    {
        uint32_t n = rec->n_writes;
        cmd(3, 0x0004);
        w16(DATA, 0x0222);
        CHECK(rec->n_writes == n);
        CHECK(s->cram[2] == 0x0222);
    }
    CHECK(frame_ends == 1 && last_end_frame == 0);
    /* the next frame's start holds it */
    go_to(line_t(1, 0, 1));
    CHECK(rec->number == 1 && rec->n_writes == 0 && rec->start.cram[2] == 0x0222 && rec->start.cram[1] == 0x0E0E);
    CHECK(frame_starts == 2);
}

/* ---- the queue */
static void test_queue(void)
{
    uint32_t i;
    uint64_t before;
    reset_all();
    display_on();
    go_to(line_t(0, 100, 900));
    cmd(1, 0x8000);
    before = pc_clock_now();
    /* with the picture drawn a VRAM word takes two slots of 18 a line: the fifth write waits */
    for (i = 0; i < 4u; i++)
        w16(DATA, 0x1100u + i);
    CHECK(pc_clock_now() == before);
    CHECK((pc_vdp_status_at(before) & 0x100u) != 0);   /* full */
    w16(DATA, 0x1104u);
    CHECK(pc_clock_now() > before);
    /* in the log, the first four carry the moment they were written. The fifth carries the moment a place came free: when the first had taken its two slots after position 900, 957 and 1085 */
    for (i = 0; i < 4u; i++)
        CHECK(rec->writes[i].vline == 100 && rec->writes[i].hclock == 900);
    CHECK(rec->writes[4].vline == 100 && rec->writes[4].hclock == 1085);
    /* a line's worth of words spreads over about a line: nine words, each held back by the queue */
    {
        uint32_t n = rec->n_writes;
        for (i = 0; i < 9u; i++)
            w16(DATA, 0x2200u + i);
        i = rec->writes[n + 8].vline * PC_MCLK_LINE + rec->writes[n + 8].hclock - (rec->writes[n].vline * PC_MCLK_LINE + rec->writes[n].hclock);
        CHECK(i > 2u * PC_MCLK_LINE / 3u && i < 3u * PC_MCLK_LINE / 2u);
    }
    /* in the blank the slots are many: no stall for a 68000 writing at its best rate, a word each 8 of its clocks */
    go_to(line_t(0, 230, 0));
    before = pc_counts.stall_mclk;
    for (i = 0; i < 64u; i++) {
        w16(DATA, i);
        pc_clock_stall(56);
    }
    CHECK(pc_counts.stall_mclk == before);
}

/* ---- DMA */
static void dma_regs(uint32_t words, uint32_t src)
{
    reg(19, words & 0xFFu);
    reg(20, words >> 8);
    reg(21, (src >> 1) & 0xFFu);
    reg(22, (src >> 9) & 0xFFu);
    reg(23, (src >> 17) & 0x7Fu);
}

static void test_dma(void)
{
    const PcVdpState *s;
    uint32_t i;
    uint64_t before;
    reset_all();
    s = pc_vdp_state();
    display_on();
    reg(1, 0x54);       /* DMA on, display on */
    for (i = 0; i < 16u; i++) {
        pt_mem_w16(pt_ram, 0x1000u + i * 2u, 0x1000u + i);
        cart[(0x0200u + i * 2u) >> 1] = (uint16_t)(0x2000u + i);
        pt_mem_w16(pt_rom, (2u << 16) + i * 2u, 0x3000u + i);
    }
    /* work RAM to VRAM in the blank: the words land, and the 68000 is stopped about as long as the public rate says */
    go_to(line_t(0, 230, 0));
    before = pc_clock_now();
    dma_regs(16, (0xFFu << 16) + 0x1000u);
    cmd(0x21, 0x4000);
    CHECK(s->vram[0x4000] == 0x10 && s->vram[0x4001] == 0x00 && s->vram[0x401E] == 0x10 && s->vram[0x401F] == 0x0F);
    CHECK(pc_clock_now() - before >= 16u * 25u && pc_clock_now() - before <= 16u * 45u);
    CHECK(s->regs[19] == 0 && s->regs[20] == 0);
    CHECK(pc_counts.dma_runs == 1 && pc_counts.dma_words == 16);
    /* the next data write is a plain write again */
    w16(DATA, 0x7777);
    CHECK(s->vram[0x4020] == 0x77);
    /* the cart path */
    dma_regs(16, 0x0200u);
    cmd(0x21, 0x5000);
    CHECK(s->vram[0x5000] == 0x20 && s->vram[0x501F] == 0x0F);
    /* ROM to CRAM, a word a slot */
    dma_regs(4, 2u << 16);
    cmd(0x23, 0x0000);
    CHECK(s->cram[0] == (0x3000u & 0x0EEEu) && s->cram[3] == (0x3003u & 0x0EEEu));
    /* DMA disabled: the code bit does nothing */
    reg(1, 0x44);
    {
        uint64_t runs = pc_counts.dma_runs;
        cmd(0x21, 0x6000);
        CHECK(pc_counts.dma_runs == runs);
    }
    reg(1, 0x54);
    /* a DMA in the picture runs at the picture's rate, and its words go in the log */
    go_to(line_t(1, 4, 100));
    before = pc_clock_now();
    {
        uint32_t n = rec->n_writes;
        dma_regs(9, (0xFFu << 16) + 0x1000u);
        cmd(0x21, 0x7000);
        CHECK(rec->n_writes >= n + 9);
        CHECK(rec->n_writes && rec->writes[rec->n_writes - 1].kind == PC_WRITE_VRAM);
    }
    CHECK(pc_clock_now() - before >= PC_MCLK_LINE / 2u && pc_clock_now() - before <= 2u * PC_MCLK_LINE);
    CHECK(pc_counts.dma_picture_words >= 9);
    /* fill and copy are counted and do nothing else */
    reg(23, 0x80);
    cmd(0x21, 0x0000);
    CHECK(pc_vdp_seen.fill == 1);
    reg(23, 0xC0);
    cmd(0x21, 0x0000);
    CHECK(pc_vdp_seen.copy == 1);
    /* the length wraps at zero: a whole 64 K words */
    reg(23, 0);
    go_to(line_t(2, 225, 0));
    pt_mem_w16(pt_ram, 0x0000u, 0xCAFEu);
    dma_regs(0, 0xFF0000u);
    before = pc_counts.dma_words;
    reg(15, 0);
    cmd(0x21, 0x8000);
    reg(15, 2);
    CHECK(pc_counts.dma_words - before == 0x10000u);
    /* the source wraps inside its 128 KB window */
    go_to(line_t(4, 225, 0));
    pt_mem_w16(pt_rom, (5u << 16) | 0xFFFEu, 0x4141u);
    pt_mem_w16(pt_rom, 4u << 16, 0x4242u);
    dma_regs(2, (5u << 16) | 0xFFFEu);
    cmd(0x21, 0x9000);
    CHECK(s->vram[0x9000] == 0x41 && s->vram[0x9002] == 0x42);
}

/* ---- the H-int counter and the flags */
static void test_interrupts(void)
{
    uint32_t i;
    reset_all();
    pc_ctx.imask = 0;
    display_on();
    /* register 10 = 3: an H-int every fourth line, and the flag rises with its enable off */
    reg(10, 3);
    go_to(line_t(0, 240, 0));
    pt_irq_level = 0;
    pc_ctx.imask = 7;
    {
        /* one frame with the mask up: the requests are visible only through the counts */
        uint64_t h0 = pc_counts.hints;
        go_to(line_t(1, 239, 0));
        CHECK(pc_counts.hints - h0 == 225u / 4u);
    }
    /* the enables: a flag up with its enable off raises the level on the write that enables it */
    reset_all();
    pc_ctx.imask = 2;
    display_on();
    reg(10, 0xFF);
    go_to(line_t(0, 226, 0));
    CHECK(pt_irq_level == 0);
    CHECK((pc_vdp_status_at(pc_clock_now()) & 0x80u) != 0);
    reg(1, 0x64);
    CHECK(pt_irq_level == 6);
    /* the 68000 takes it: the flag goes */
    pt_port_irq_ack(6);
    CHECK(pt_irq_level == 0);
    CHECK((pc_vdp_status_at(pc_clock_now()) & 0x80u) == 0);
    pt_port_irq_return(6);
    /* V-int above H-int */
    reset_all();
    pc_ctx.imask = 2;
    display_on();
    reg(10, 0);
    reg(0, 0x14);
    reg(1, 0x64);
    go_to(line_t(0, 230, 0));
    pt_port_irq_ack(6);
    pt_port_irq_return(6);
    go_to(line_t(1, 0, PC_VDP_HINT_HCLOCK));
    CHECK(pt_irq_level == 4);
    pt_port_irq_ack(4);
    pt_port_irq_return(4);
    CHECK(pt_irq_level == 0);
    /* both up: the request is V-int's until it is taken, then H-int's */
    pc_ctx.imask = 7;
    go_to(line_t(1, 225, 0));
    CHECK(pt_irq_level == 6);
    pt_port_irq_ack(6);
    CHECK(pt_irq_level == 4);
    pt_port_irq_return(6);
    /* the counter takes register 10 as it stands at the underflow, so a value written ahead sets the gap after the next */
    reset_all();
    pc_ctx.imask = 7;
    display_on();
    reg(10, 1);
    go_to(line_t(0, 230, 0));       /* the blank loads the counter from the register */
    go_to(line_t(1, 0, 0));
    reg(10, 5);
    {
        uint64_t h0 = pc_counts.hints;
        go_to(line_t(1, 1, PC_VDP_HINT_HCLOCK));    /* counter was 1: line 0 takes it to 0, line 1 fires and loads 5 */
        CHECK(pc_counts.hints - h0 == 1);
        go_to(line_t(1, 6, PC_MCLK_LINE - 1u));
        CHECK(pc_counts.hints - h0 == 1);
        go_to(line_t(1, 7, PC_VDP_HINT_HCLOCK));    /* five lines down, and fires on the sixth */
        CHECK(pc_counts.hints - h0 == 2);
    }
    /* no H-int counts in the blank, and the counter loads there */
    {
        uint64_t h0 = pc_counts.hints;
        go_to(line_t(1, 225, 0));
        h0 = pc_counts.hints;
        go_to(line_t(1, 261, 3000));
        CHECK(pc_counts.hints == h0);
    }
    /* a single V-int a frame, at line 224, at its place */
    {
        uint64_t v0;
        reset_all();
        pc_ctx.imask = 7;
        display_on();
        go_to(line_t(0, 224, PC_VDP_VINT_HCLOCK - 1u));
        v0 = pc_counts.vints;
        go_to(line_t(0, 224, PC_VDP_VINT_HCLOCK));
        CHECK(pc_counts.vints == v0 + 1);
        for (i = 1; i < 4u; i++)
            go_to(line_t(i, 224, PC_VDP_VINT_HCLOCK));
        CHECK(pc_counts.vints == v0 + 4);
    }
}

/* ---- the tick: holding at a raised interrupt, and handlers in effective time */
static void test_tick(void)
{
    uint64_t t, lag;
    uint32_t n;
    reset_all();
    pc_ctx.imask = 2;
    display_on();
    reg(10, 0);
    reg(0, 0x14);       /* an H-int on every line, once the blank has loaded the counter */
    go_to(line_t(0, 230, 0));
    go_to(line_t(1, 0, PC_VDP_HINT_HCLOCK));
    CHECK(pt_irq_level == 4);
    /* The program runs on to a safe point, a whole line late. The tick waits: the line's next event has not run. */
    pt_clock += (PC_MCLK_LINE + 100u) / 7u;
    (void)pt_port_tick();
    CHECK(pc_tick_position() <= line_t(1, 1, 0));
    CHECK(pc_counts.hints == 1);
    /* It takes the interrupt. A write in the handler carries the time it would have had: raise + half an instruction, not the clock. */
    t = pc_clock_now();
    pc_ctx.imask = 4;
    pt_irq_level = 0;
    pt_port_irq_ack(4);
    (void)pt_port_tick();
    CHECK(pc_handler_level() == 4);
    lag = t - pc_access_time();
    CHECK(lag + 300u > PC_MCLK_LINE + 100u && lag < PC_MCLK_LINE + 400u);
    n = rec->n_writes;
    reg(10, 7);
    CHECK(rec->n_writes == n + 1);
    CHECK(rec->writes[n].vline == 0);       /* filed where the chip would have had it */
    CHECK((rec->writes[n].flags & PC_WF_IN_HINT) != 0);
    /* and the tick follows the handler's time, so the next line's underflow has not been passed yet with the old register */
    CHECK(pc_counts.hints == 1);
    /* an access says what is left of its block: its time is that much earlier */
    {
        uint64_t a0, a1;
        a0 = pc_access_time();
        pt_at = 100;
        a1 = pc_access_time();
        CHECK(a0 - a1 >= 700u && a0 - a1 <= 720u);
        CHECK(pt_at == 0);                  /* used up */
    }
    /* the handler ends. The tick runs free up to the clock, and stops at the next raised interrupt it meets, with no merge. */
    pc_ctx.imask = 2;
    pt_port_irq_return(4);
    CHECK(pc_handler_level() == 0);
    (void)pt_port_tick();
    CHECK(pc_counts.hints_merged == 0);
    CHECK(pt_irq_level == 4);
    /* register 10 = 7 reached the counter: 7 lines of nothing follow this H-int */
    pc_ctx.imask = 4;
    pt_irq_level = 0;
    pt_port_irq_ack(4);
    pc_ctx.imask = 2;
    pt_port_irq_return(4);
    (void)pt_port_tick();
    n = (uint32_t)pc_counts.hints;
    go_to(line_t(1, 7, PC_MCLK_LINE - 1u));
    CHECK(pc_counts.hints == n);
    /* A handler held off by the one before starts at that one's end, not at its own raise. */
    reset_all();
    pc_ctx.imask = 2;
    display_on();
    reg(10, 0);
    reg(0, 0x14);
    go_to(line_t(0, 230, 0));
    pt_clock += (line_t(1, 5, 0) - line_t(0, 230, 0)) / 7u;
    (void)pt_port_tick();
    CHECK(pc_counts.hints == 1);
    pc_ctx.imask = 4;
    pt_irq_level = 0;
    pt_port_irq_ack(4);
    /* this handler is long: a line and a half, past the next line's raise, which its mask holds back */
    pt_clock += (PC_MCLK_LINE + PC_MCLK_LINE / 2u) / 7u;
    (void)pt_port_tick();
    CHECK(pc_counts.hints == 2 && pc_counts.hints_merged == 0);
    {
        uint64_t end = pc_access_time();
        pc_ctx.imask = 2;
        pt_port_irq_return(4);
        (void)pt_port_tick();
        CHECK(pt_irq_level == 4);
        /* the second handler starts where the first ended */
        pc_ctx.imask = 4;
        pt_irq_level = 0;
        pt_port_irq_ack(4);
        CHECK(pc_access_time() == end);
        pc_ctx.imask = 2;
        pt_port_irq_return(4);
    }
    /* An interrupt raised while the 68000 is stopped for a device starts when the 68000 is back. */
    reset_all();
    pc_ctx.imask = 2;
    display_on();
    reg(10, 0);
    reg(0, 0x14);
    go_to(line_t(0, 230, 0));
    go_to(line_t(1, 5, PC_VDP_HINT_HCLOCK));
    pc_ctx.imask = 4;
    pt_irq_level = 0;
    pc_clock_stall(1000);
    t = pc_clock_now();
    pc_clock_stall(0);
    pt_port_irq_ack(4);
    CHECK(pc_access_time() == t);
    pc_ctx.imask = 2;
    pt_port_irq_return(4);
    /* An interrupt the program itself had masked when it rose starts when it is taken: the chip would have held it that long too. */
    reset_all();
    pc_ctx.imask = 7;
    display_on();
    reg(10, 0);
    reg(0, 0x14);
    go_to(line_t(0, 230, 0));
    go_to(line_t(1, 5, PC_VDP_HINT_HCLOCK));
    CHECK(pc_counts.hints >= 1);
    pt_clock += PC_MCLK_LINE / 7u;
    pc_ctx.imask = 4;
    t = pc_clock_now();
    pt_port_irq_ack(4);
    CHECK(pc_access_time() == t);
    pt_port_irq_return(4);
    /* a stop request stops the next deadline check, and clears */
    reset_all();
    pc_tick_stop_request();
    CHECK(pt_port_tick() != 0);
    pc_tick_stop_clear();
    CHECK(pt_port_tick() == 0);
}

/* ---- a chip write and an interrupt raised just ahead of it. A block's whole time is charged at its start, so an interrupt can rise inside a block that still has a chip write to make.
 * On a 68000 the handler runs first, whether the write is the program's command or a data word. Here the same. */
#define SCENE_STACK ((0xFFu << 16) + (1u << 15))
#define SCENE_VRAM 0x2400u
#define SCENE_COMMAND ((1u << 30) | (SCENE_VRAM << 16))     /* a VRAM write command, as one long */

/* What a handler does on the chip: a command and a colour of its own, then register 10. A register write changes the low bits of the chip's command too. */
static void handler_writes_the_chip(void)
{
    cmd(3, 0x0004);
    w16(DATA, 0x0246);
    reg(10, 0x20);
}

static void handler_holds_the_console(void) { cart_hold = 1; }

/* The stack holds the status word, then the address pushed. */
static void handler_changes_its_return(void) { pt_mem_w16(pt_ram, (pc_ctx.a[7] + 4u) & 0xFFFFu, 0x1234u); }

/* Stops the program, with a second interrupt up and above its own level. */
static void handler_stops(void)
{
    pc_ctx.stopped = 1;
    pc_vdp_vint_raise(pc_clock_now());
}

/* The video chip on, V-int on, the program's mask as given, a block about to start just before V-int rises. */
static void order_scene(uint32_t mask)
{
    reset_all();
    handler_work = handler_writes_the_chip;
    pc_ctx.imask = (u8)mask;
    pc_ctx.a[7] = SCENE_STACK;
    reg(1, 0x64);
    reg(12, 0x81);
    reg(15, 2);
    go_to(line_t(0, 224, PC_VDP_VINT_HCLOCK - 100u));
    CHECK(pt_irq_level == 0);
    /* The block's time is charged and the deadline's tick runs, as at any block's start: V-int rises, 100 master clocks in. */
    pt_clock += 40u;
    (void)pt_port_tick();
    CHECK(pt_irq_level == 6);
    CHECK(handler_entries == 0);
}

/* The next block is a safe point: what is due and has not run goes now. So with the rule broken a scene fails as the program would. */
static void next_safe_point(void)
{
    while (pt_irq_level > pc_ctx.imask)
        (void)pt_take_irq(&pc_ctx, 0u);
}

static void test_order(void)
{
    const PcVdpState *s = pc_vdp_state();
    uint64_t t, stamp;
    uint32_t n;

    /* The block ends in a two-word command. The handler runs first, so the command is the chip's last and the data words land at it. */
    order_scene(3);
    pt_port_write(4, CTRL, SCENE_COMMAND);
    CHECK(handler_entries == 1);
    next_safe_point();
    CHECK(handler_entries == 1);
    w16(DATA, 0x1357);
    w16(DATA, 0x2468);
    CHECK(s->vram[SCENE_VRAM] == 0x13 && s->vram[SCENE_VRAM + 1u] == 0x57 && s->vram[SCENE_VRAM + 2u] == 0x24 && s->vram[SCENE_VRAM + 3u] == 0x68);
    /* The handler's own writes did land, and all it changed in the context is back. */
    CHECK(s->cram[2] == 0x0246 && s->regs[10] == 0x20);
    CHECK(pt_irq_level == 0 && pc_ctx.imask == 3 && pc_ctx.a[7] == SCENE_STACK);
    CHECK(pc_handler_level() == 0);
    CHECK(pc_counts.vints == 1 && pc_counts.vints_merged == 0);

    /* The same with the command as two word writes. */
    order_scene(3);
    cmd(1, SCENE_VRAM);
    CHECK(handler_entries == 1);
    next_safe_point();
    w16(DATA, 0x3579);
    CHECK(s->vram[SCENE_VRAM] == 0x35 && s->vram[SCENE_VRAM + 1u] == 0x79);

    /* A word to the data port goes through the same door. */
    order_scene(3);
    w16(DATA, 0x0000);
    CHECK(handler_entries == 1);

    /* The program's mask holds the interrupt off: the command goes first and the handler does not run, until the mask comes down and another write comes. */
    order_scene(6);
    pt_port_write(4, CTRL, SCENE_COMMAND);
    CHECK(handler_entries == 0);
    w16(DATA, 0x4680);
    CHECK(s->vram[SCENE_VRAM] == 0x46 && s->vram[SCENE_VRAM + 1u] == 0x80);
    CHECK(pt_irq_level == 6);
    pc_ctx.imask = 3;
    w16(DATA, 0x5791);
    CHECK(handler_entries == 1);

    /* A read does not take it. */
    order_scene(3);
    (void)pt_port_read(2, DATA);
    CHECK(handler_entries == 0);

    /* The handler has the cart hold the console: the write it came in front of goes nowhere. */
    order_scene(3);
    handler_work = handler_holds_the_console;
    n = (uint32_t)pc_counts.ctrl_writes;
    pt_port_write(4, CTRL, SCENE_COMMAND);
    CHECK(handler_entries == 1);
    CHECK((uint32_t)pc_counts.ctrl_writes == n);
    cart_hold = 0;

    /* The handler changes where it returns to. This cannot follow that, so it shows in the run's report. */
    order_scene(3);
    handler_work = handler_changes_its_return;
    pt_port_write(4, CTRL, SCENE_COMMAND);
    CHECK(handler_entries == 1);
    CHECK(pc_counts.handler_redirects == 1);

    /* An ordinary handler does not count there. */
    order_scene(3);
    pt_port_write(4, CTRL, SCENE_COMMAND);
    CHECK(pc_counts.handler_redirects == 0);

    /* The program stops inside the handler, with a second interrupt due: no more handlers run. */
    reset_all();
    pc_ctx.a[7] = SCENE_STACK;
    pc_ctx.imask = 3;
    reg(1, 0x64);
    reg(12, 0x81);
    reg(0, 0x14);
    reg(10, 0);
    go_to(line_t(0, 230, 0));
    pt_port_irq_ack(6);
    pt_port_irq_return(6);
    pc_ctx.imask = 3;
    go_to(line_t(1, 5, PC_VDP_HINT_HCLOCK));
    CHECK(pt_irq_level == 4);
    handler_work = handler_stops;
    w16(DATA, 0x0000);
    CHECK(handler_entries == 1);
    CHECK(pc_ctx.stopped == 1);

    /* Inside a handler that has lowered the mask, a second H-int goes in front of a write. The write keeps the time its own block gave it, later only by the second handler's entry. */
    reset_all();
    pc_ctx.a[7] = SCENE_STACK;
    pc_ctx.imask = 3;
    display_on();
    reg(10, 0);
    reg(0, 0x14);
    go_to(line_t(0, 230, 0));
    go_to(line_t(1, 5, PC_VDP_HINT_HCLOCK));
    CHECK(pt_irq_level == 4);
    pc_ctx.imask = 4;
    pt_port_irq_ack(4);
    pc_ctx.imask = 3;
    cmd(3, 0x0008);
    pt_clock += PC_MCLK_LINE / 7u + 1u;
    (void)pt_port_tick();
    CHECK(pt_irq_level == 4);
    pt_at = 40u;
    t = pc_access_time();
    pt_at = 40u;
    n = rec->n_writes;
    w16(DATA, 0x0111);
    CHECK(handler_entries == 1);
    CHECK(rec->n_writes == n + 1u);
    stamp = line_t(1, rec->writes[n].vline, rec->writes[n].hclock);
    CHECK(stamp >= t + 300u && stamp <= t + 330u);      /* 44 clocks of entry, at 7 and a bit master clocks each */
    pc_ctx.imask = 3;
    pt_port_irq_return(4);
}

/* ---- pads: the game's reader sequence, three and six button */
#define IO_DATA1 0xA10003u
#define IO_CTRL1 0xA10009u
static uint32_t rb(uint32_t a) { return pt_port_read(1, a); }
static void wb(uint32_t a, uint32_t v) { pt_port_write(1, a, v); }

static void pad_cycle(uint32_t out[5])
{
    wb(IO_CTRL1, 0x40);
    wb(IO_DATA1, 0x00);
    out[0] = rb(IO_DATA1);      /* TH low */
    wb(IO_DATA1, 0x40);
    out[1] = rb(IO_DATA1);      /* TH high */
    wb(IO_CTRL1, 0x43);
    wb(IO_DATA1, 0x00);
    wb(IO_DATA1, 0x02);
    wb(IO_CTRL1, 0x40);
    out[2] = rb(IO_DATA1);      /* TH low */
    wb(IO_DATA1, 0x40);
    wb(IO_DATA1, 0x00);
    out[3] = rb(IO_DATA1);      /* TH low: a six button pad shows itself here */
    wb(IO_DATA1, 0x40);
    out[4] = rb(IO_DATA1);      /* TH high: X, Y, Z, Mode */
    wb(IO_CTRL1, 0x40);
    wb(IO_DATA1, 0x00);
}

static void test_pads(void)
{
    uint32_t v[5];
    reset_all();
    /* the version register and its word read */
    CHECK(rb(0xA10001u) == PC_CONSOLE_VERSION);
    CHECK(r16(0xA10000u) == (PC_CONSOLE_VERSION | PC_CONSOLE_VERSION << 8));
    /* at power-on the control registers read 0, and every I/O word read has its byte on both halves */
    CHECK(r16(0xA10008u) == 0 && r16(0xA1000Cu) == 0);
    /* six button, nothing held */
    go_to(line_t(0, 100, 0));
    pad_cycle(v);
    CHECK(v[0] == 0x33 && v[1] == 0x7F && v[2] == 0x33 && v[3] == 0x30 && v[4] == 0x7F);
    /* the count forgets after its timeout, so the same again a frame later */
    go_to(line_t(1, 100, 0));
    pad_cycle(v);
    CHECK(v[0] == 0x33 && v[1] == 0x7F && v[2] == 0x33 && v[3] == 0x30 && v[4] == 0x7F);
    /* each button on its line */
    go_to(line_t(2, 100, 0));
    pc_io_set_pad(0, PC_PAD_A | PC_PAD_DOWN | PC_PAD_X | PC_PAD_C);
    pad_cycle(v);
    CHECK(v[0] == (0x33u & ~0x10u & ~0x02u));   /* A and Down on TH low */
    CHECK(v[1] == (0x7Fu & ~0x20u & ~0x02u));   /* C and Down on TH high */
    CHECK(v[3] == (0x30u & ~0x10u));            /* A only on the sign read */
    CHECK(v[4] == (0x7Fu & ~0x20u & ~0x04u));   /* C and X */
    go_to(line_t(3, 100, 0));
    pc_io_set_pad(0, PC_PAD_START | PC_PAD_UP | PC_PAD_B | PC_PAD_LEFT | PC_PAD_RIGHT | PC_PAD_Y | PC_PAD_Z | PC_PAD_MODE);
    pad_cycle(v);
    CHECK(v[0] == (0x33u & ~0x20u & ~0x01u));
    CHECK(v[1] == (0x7Fu & ~0x10u & ~0x01u & ~0x04u & ~0x08u));
    CHECK(v[4] == (0x7Fu & ~0x10u & ~0x02u & ~0x01u & ~0x08u));
    /* three button: no sign, and the extra read is the plain one */
    go_to(line_t(4, 100, 0));
    pc_io_set_pad(0, 0);
    pc_io_set_three_button(0, 1);
    pad_cycle(v);
    CHECK(v[0] == 0x33 && v[1] == 0x7F && v[2] == 0x33 && v[3] == 0x33 && v[4] == 0x7F);
    pc_io_set_three_button(0, 0);
    /* port 2 answers on its own registers */
    go_to(line_t(5, 100, 0));
    pc_io_set_pad(1, PC_PAD_B);
    wb(0xA1000Bu, 0x40);
    wb(0xA10005u, 0x40);
    CHECK(rb(0xA10005u) == (0x7Fu & ~0x10u));
    pc_io_set_pad(1, 0);
    /* an even address answers the same as the odd one above it */
    CHECK(rb(0xA10004u) == rb(0xA10005u));
    /* a write to an even address lands too, as the game's set-up does */
    wb(0xA1000Au, 0x00);
    CHECK(rb(0xA1000Bu) == 0);
}

/* ---- the Z80 seam */
static void test_z80(void)
{
    reset_all();
    /* the bus request's grant: 0 in bit 0 of the even byte when requested */
    CHECK((rb(0xA11100u) & 1u) == 1u);
    wb(0xA11100u, 0x01);
    CHECK((rb(0xA11100u) & 1u) == 0u);
    w16(0xA11100u, 0x0000);
    CHECK((rb(0xA11100u) & 1u) == 1u);
    w16(0xA11100u, 0x0100);
    CHECK((r16(0xA11100u) & 0x100u) == 0u);
    /* its RAM takes writes */
    wb(0xA00040u, 0x5A);
    CHECK(pc_z80_ram()[0x40] == 0x5A);
    pt_port_write(2, 0xA01234u, 0x7788u);
    CHECK(pc_z80_ram()[0x1234] == 0x77);
}

#include "pc_z80_test.c"

/* ---- the bus */
static void test_bus(void)
{
    reset_all();
    /* the cart's addresses go to the cart, all sizes */
    pt_port_write(2, 0x1000u, 0x1234u);
    CHECK(cart[0x800] == 0x1234);
    CHECK(pt_port_read(2, 0x1000u) == 0x1234 && pt_port_read(1, 0x1000u) == 0x12 && pt_port_read(1, 0x1001u) == 0x34);
    pt_port_write(4, 0x2000u, 0xAABBCCDDu);
    CHECK(pt_port_read(4, 0x2000u) == 0xAABBCCDDu);
    /* a probe for an add-on that is not there reads all ones */
    CHECK(pt_port_read(2, 0xA130ECu) == 0xFFFF);
    CHECK(pt_port_read(1, 0xA130F3u) == 0xFF);
    CHECK(pc_counts.other_reads == 2);
    /* the console held: reads 0, writes go nowhere */
    cart_hold = 1;
    pt_port_write(2, 0x1000u, 0x9999u);
    CHECK(cart[0x800] == 0x1234);
    CHECK(pt_port_read(2, 0x1000u) == 0);
    cmd(1, 0);
    CHECK(pc_counts.ctrl_writes == 0);
    cart_hold = 0;
}

/* ---- the save: the Pocket's layout, a round trip, a kill mid-write */
static void test_save(void)
{
    static uint8_t a[PC_SAVE_BYTES], b[PC_SAVE_BYTES];
    char path[512], temp[512];
    FILE *f;
    uint32_t i;
    snprintf(path, sizeof(path), "%s/test.sav", scratch);
    snprintf(temp, sizeof(temp), "%s/test.sav.tmp", scratch);
    for (i = 0; i < PC_SAVE_BYTES; i++)
        a[i] = (uint8_t)(i * 37u + 11u);
    remove(path);
    CHECK(pc_save_load(path, b) == -1);
    CHECK(pc_save_store(path, a) == 0);
    CHECK(pc_save_load(path, b) == (long)PC_SAVE_BYTES);
    CHECK(memcmp(a, b, PC_SAVE_BYTES) == 0);
    /* on disk, each pair is swapped */
    f = fopen(path, "rb");
    CHECK(f != NULL);
    if (f) {
        CHECK(fgetc(f) == a[1] && fgetc(f) == a[0]);
        fclose(f);
    }
    /* a file in the older layout: the rest of the 8 KB reads as 0 */
    f = fopen(path, "wb");
    if (f) {
        for (i = 0; i < PC_SAVE_BYTES_OLD; i++)
            fputc(0x11, f);
        fclose(f);
    }
    memset(b, 0x55, PC_SAVE_BYTES);
    CHECK(pc_save_load(path, b) == (long)PC_SAVE_BYTES_OLD);
    CHECK(b[0] == 0x11 && b[PC_SAVE_BYTES_OLD - 1u] == 0x11 && b[PC_SAVE_BYTES_OLD] == 0x00 && b[PC_SAVE_BYTES - 1u] == 0x00);
    /* any other size is not a save, and the firmware's bytes are left alone */
    f = fopen(path, "wb");
    if (f) {
        fputc(0x11, f);
        fputc(0x22, f);
        fclose(f);
    }
    memset(b, 0x55, PC_SAVE_BYTES);
    CHECK(pc_save_load(path, b) == PC_SAVE_WRONG_SIZE);
    CHECK(b[0] == 0x55 && b[PC_SAVE_BYTES - 1u] == 0x55);
    /* a killed write leaves a temporary file, and the old save stays whole */
    CHECK(pc_save_store(path, a) == 0);
    f = fopen(temp, "wb");
    if (f) {
        fputc(0x55, f);
        fclose(f);
    }
    CHECK(pc_save_load(path, b) == (long)PC_SAVE_BYTES && memcmp(a, b, PC_SAVE_BYTES) == 0);
    remove(temp);
    remove(path);
}

/* ---- the 68000's divide times. Made-up operands, each with the clocks the FX68K core took in simulation, with the time of the two instructions that loaded the registers taken off. */
typedef struct DivideCase { uint16_t high, low, divisor, clocks; } DivideCase;

static const DivideCase divu_cases[] = {
    {0x0B9F, 0x5741, 0x5ECC, 120}, {0x0006, 0x4F18, 0x003D, 120}, {0x0002, 0xE452, 0x0462, 130}, {0x0000, 0x005F, 0x0004, 130}, {0x0007, 0x07AC, 0xB2DF, 132},
    {0x0000, 0xFB55, 0x001B, 124}, {0x01C8, 0x3A4A, 0x41C0, 124}, {0x0003, 0xA49D, 0x0306, 128}, {0x0006, 0x0C2B, 0x0EC0, 130}, {0x0000, 0x012C, 0x0010, 132},
    {0x0000, 0x7E2B, 0x03BA, 134}, {0x0001, 0x0964, 0x001E, 126}, {0x0000, 0x0002, 0x0002, 136}, {0x0000, 0x3238, 0x0002, 124}, {0x0000, 0x0BB3, 0x000C, 126},
    {0x0012, 0x9028, 0x09B1, 124}, {0x0000, 0x0B56, 0x0001, 122}, {0x0000, 0x014C, 0x0729, 136}, {0xA0ED, 0xD076, 0xDEC6, 98}, {0x0000, 0x0242, 0x00ED, 134},
    {0x0008, 0xDDDE, 0x085E, 128}, {0x0000, 0x4E42, 0x0038, 128}, {0x0000, 0x271B, 0x0001, 122}, {0x0000, 0xF996, 0x34C9, 134},
    {0x0014, 0x8B75, 0x0014, 10}, {0x0173, 0x834E, 0x0173, 10}, {0xBABC, 0x4B9C, 0x09AD, 10},
};
static const DivideCase divs_cases[] = {
    {0x0001, 0xFFFE, 0x0004, 122}, {0x0000, 0x7FFF, 0x0001, 122}, {0x0001, 0x2AC9, 0x0018, 138}, {0x0000, 0x9AE3, 0x01EE, 146}, {0x0000, 0x111C, 0x0341, 148},
    {0x0000, 0x000A, 0x0003, 148}, {0x1C16, 0xF1B3, 0x1C17, 120}, {0x0016, 0x0054, 0x002C, 148}, {0x0004, 0x0006, 0x0008, 148}, {0x000B, 0x4DEF, 0xF522, 146},
    {0x0000, 0x0611, 0xFFFF, 146}, {0x0000, 0x6C76, 0xFFFC, 138}, {0x0000, 0x0000, 0xFFFF, 152}, {0x0017, 0x6336, 0xE6D3, 142}, {0x0000, 0x7FFF, 0x8001, 152},
    {0x002E, 0x8088, 0xFFA3, 150}, {0x5555, 0x5555, 0xAAAA, 122}, {0x0000, 0x8F9A, 0xFFFF, 134}, {0xFFF5, 0x000C, 0x0016, 128}, {0xFF4F, 0xB2AD, 0x02DC, 140},
    {0xFF8E, 0x493D, 0x3AB2, 142}, {0xFFFF, 0xFFDA, 0x0003, 152}, {0xFFFF, 0xFF2D, 0x0011, 152}, {0xFFFF, 0xFFEA, 0x0003, 152}, {0xFF8C, 0xFF26, 0x00E6, 154},
    {0xDC06, 0xB057, 0x3B21, 134}, {0xFFF7, 0x0001, 0x0009, 126}, {0xFE06, 0xA1FB, 0xB422, 144}, {0xFFD4, 0xAC2B, 0xFFA2, 130}, {0xFFE7, 0x5F13, 0xFAD1, 148},
    {0xFFFF, 0xFE60, 0xFF14, 154}, {0xFFFF, 0xFBE3, 0xFF0C, 152}, {0xFFFF, 0xFA8F, 0xFFD7, 152}, {0xAAAA, 0xAAAA, 0xAAAA, 124}, {0xFB2D, 0x1C04, 0xFB2D, 126},
    {0xBDC0, 0x568E, 0xBDC0, 124}, {0x0002, 0x39AB, 0xFFFF, 16}, {0x0001, 0xAEC2, 0x0001, 16}, {0xA61B, 0x415B, 0xA61D, 18}, {0xFB70, 0x6C43, 0xFB72, 18},
};

static void test_divide(void)
{
    size_t i;
    for (i = 0; i < sizeof(divu_cases) / sizeof(divu_cases[0]); i++) {
        const DivideCase *k = &divu_cases[i];
        CHECK(pt_clocks_divu((uint32_t)k->high << 16 | k->low, k->divisor) == k->clocks);
    }
    for (i = 0; i < sizeof(divs_cases) / sizeof(divs_cases[0]); i++) {
        const DivideCase *k = &divs_cases[i];
        CHECK(pt_clocks_divs((uint32_t)k->high << 16 | k->low, k->divisor) == k->clocks);
    }
}

/* ---- the frame file, the two parts' shared format, through this machine's own record */
static void test_frame_file(void)
{
    static PcFrame back;
    char path[512];
    FILE *f;
    reset_all();
    display_on();
    go_to(line_t(0, 10, 0));
    cmd(3, 2);
    w16(DATA, 0x0ABC);
    snprintf(path, sizeof(path), "%s/test.frames", scratch);
    f = fopen(path, "wb");
    CHECK(f != NULL);
    CHECK(pc_frame_write(f, rec) == 0);
    fclose(f);
    f = fopen(path, "rb");
    CHECK(f != NULL);
    CHECK(pc_frame_read(f, &back) == 0);
    fclose(f);
    remove(path);
    CHECK(back.n_writes == rec->n_writes && back.writes[0].value == rec->writes[0].value);
    /* the renderer's rule applied to the frame's start gives the machine's state */
    {
        uint32_t i;
        for (i = 0; i < back.n_writes; i++)
            pc_state_apply(&back.start, &back.writes[i]);
        CHECK(memcmp(back.start.cram, pc_vdp_state()->cram, sizeof(back.start.cram)) == 0);
        CHECK(memcmp(back.start.regs, pc_vdp_state()->regs, sizeof(back.start.regs)) == 0);
    }
}

typedef struct Group { const char *name; void (*run)(void); } Group;
static const Group groups[] = {
    {"clock", test_clock}, {"ports", test_ports}, {"status", test_status}, {"log", test_log}, {"queue", test_queue}, {"dma", test_dma},
    {"interrupts", test_interrupts}, {"tick", test_tick}, {"order", test_order}, {"pads", test_pads}, {"z80", test_z80}, {"z80bus", test_z80_bus}, {"z80chips", test_z80_chips}, {"z80order", test_z80_order}, {"bus", test_bus}, {"save", test_save}, {"divide", test_divide}, {"frame", test_frame_file},
};

int main(int argc, char **argv)
{
    uint32_t i;
    const char *only = argc > 1 ? argv[1] : NULL;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 2)
        scratch = argv[2];
    rec = (PcFrame *)malloc(sizeof(PcFrame));
    pt_rom = (u8 *)calloc(1, 0x400000u);
    pt_rom_size = 0x400000u;
    pt_rom_bus_end = 0x400000u;
    pt_low_device_end = 0x10000u;
    for (i = 0; i < sizeof(groups) / sizeof(groups[0]); i++)
        if (!only || !strcmp(only, "all") || !strcmp(only, groups[i].name))
            groups[i].run();
    printf("%d checks, %d failed\n", checks, fails);
    printf(fails ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    return fails ? 1 : 0;
}
