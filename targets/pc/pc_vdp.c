/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The video chip's model. pc_vdp.h says what it is and what it leaves out. Written from the public hardware documents (the Sega Genesis Software Manual and the
 * public notes on the VDP). The timing points named "measured" were measured by scripted bus traffic on the Nuked-MD VDP in simulation, see THIRD_PARTY.md. No code of it is here.
 *
 * Time. Every access comes with the time the machine gives it (pc_access_time): the clock in the main program, the effective time in an interrupt handler. A write
 * goes through the write queue against the chip's access slots, which decides when the 68000 may go on. In the frame's log it gets the time it entered the queue, as pc_frame.h
 * says: the renderer works out for itself which slot that reaches. It goes in the log if that time falls while the V counter is 0-223 of the frame being recorded, and in the live
 * state in any case.
 */
#include <stdio.h>
#include <string.h>

#include "pt_rt.h"
#include "pc_clock.h"
#include "pc_cart.h"
#include "pc_vdp.h"

PcCounts pc_counts;
PcVdpSeen pc_vdp_seen;
PcFrame *pc_frame_rec;
int pc_vread_first = -1, pc_vread_last = -1;

static PcVdpState S;
static int ctrl_pending;        /* the first of a two-word command has been written */
static uint32_t code;           /* CD5-CD0 */
static uint32_t address;        /* A15-A0 */
static uint32_t address_a16;    /* the second word's A16 bit: only matters with the 128 KB mode bit */

/* THE TERADRIVE'S ANSWER. In the 128 KB mode (register 1 bit 7) a TeraDrive's video chip keeps each word across two 64 KB memories, A16 included.
   The game uses that mode once, in its boot test of the console: it writes $5A02 at $10000 and $A501 at $0000 and reads $10000 back, and only a
   TeraDrive gives $5A02. Like the Pocket core's stub (its vram_td_stub.v, a 64-byte store for the second memory's byte), this keeps the words
   written in that mode for the 64 places the address bits A16 and A5-A1 pick, and gives them back to reads in that mode. Everything else stays as
   on the other consoles: the 64 KB memory takes a write with A16 clear in its usual place, and nothing else uses the mode. A word read in that mode
   that was not written in it reads 0 here: the game never does that. */
static int teradrive;
static uint16_t td_words[64];
#define TD_PLACE() (address_a16 << 5 | (address >> 1 & 31u))

void pc_vdp_set_teradrive(int on)
{
    teradrive = on != 0;
    memset(td_words, 0, sizeof(td_words));
}
static uint32_t ctrl_first;

static uint64_t fifo_land[4];   /* when each of the last four writes lands */
static uint32_t fifo_head;
static uint64_t fifo_last;

static int flag_v, flag_h;
static uint32_t hint_count;
static uint64_t dma_start_time, dma_end_time;   /* the last DMA: a Z80 access to the 68000's bus waits for its end */

extern PtCtx pc_ctx;

const PcVdpState *pc_vdp_state(void) { return &S; }

static inline int h40(void) { return (S.regs[12] & 0x81u) != 0u; }
static inline int display_on(void) { return (S.regs[1] & 0x40u) != 0u; }

/* ---- where the status word's VBlank bit changes, relative to the V counter's step. Measured: it sets at the step to line 224 and clears at the step to line 261, each within about 15 clocks. */
#define VB_SET_LINE     224u
#define VB_SET_HCLOCK   0u
#define VB_CLEAR_LINE   261u
#define VB_CLEAR_HCLOCK 0u

static int in_vblank(uint64_t t)
{
    uint32_t v = pc_time_vline(t), h = pc_time_hclock(t);
    if (!display_on())
        return 1;       /* the chip's flag is also up with the display off */
    if (v < VB_SET_LINE || (v == VB_SET_LINE && h < VB_SET_HCLOCK))
        return 0;
    if (v > VB_CLEAR_LINE || (v == VB_CLEAR_LINE && h >= VB_CLEAR_HCLOCK))
        return 0;
    return 1;
}

/* ---- the chip's access slots: where a write from the queue, or a DMA word, can land.
 * With the picture being drawn there are 18 a line in the 320 pixel mode, at the places in this table, measured, and on the line before the picture too, from just after its start.
 * In the blanking or with the display off nearly every slot is free: measured, 16.66 clocks a byte by DMA in the 320 pixel mode, which is the public figure of 205 bytes a line. The 256
 * pixel mode's 16 and 167 are the public figures, not measured. A VRAM word takes two slots; CRAM and VSRAM one. */
static const uint16_t slots_h40[18] = {
    362u, 829u, 957u, 1085u, 1341u, 1469u, 1597u, 1853u, 1981u, 2109u, 2365u, 2493u, 2621u, 2877u, 3005u, 3133u, 3373u, 3389u
};
#define DRAWN_FROM_HCLOCK 34u   /* on the line before the picture */
#define DMA_START_MCLK 100u     /* from the command's last word to the first word of a DMA: measured, about */

static int line_is_drawn(uint32_t vline, uint32_t hclock)
{
    return display_on() && (vline < 224u || (vline == 261u && hclock >= DRAWN_FROM_HCLOCK));
}

uint64_t pc_vdp_next_slot(uint64_t t)
{
    uint32_t v = pc_time_vline(t), h = pc_time_hclock(t), i;
    uint64_t line_start = t - h;
    if (!line_is_drawn(v, h)) {
        uint32_t per_line = h40() ? 205u : 167u;
        uint32_t k = (h * per_line + PC_MCLK_LINE - 1u) / PC_MCLK_LINE;
        return line_start + k * PC_MCLK_LINE / per_line;
    }
    if (h40()) {
        for (i = 0; i < 18u; i++)
            if (slots_h40[i] >= h)
                return line_start + slots_h40[i];
    } else {
        uint32_t k = (h + 213u) / 214u;
        if (k < 16u)
            return line_start + k * 214u;
    }
    /* The next line's first slot. A line can turn from drawn to blank here, so ask again. */
    return pc_vdp_next_slot(line_start + PC_MCLK_LINE);
}

/* ---- the log */
static void log_write(uint64_t t, int kind, uint32_t addr, uint32_t value)
{
    PcWrite w;
    uint32_t vline = pc_time_vline(t);
    w.vline = (uint16_t)vline;
    w.hclock = (uint16_t)pc_time_hclock(t);
    w.kind = (uint8_t)kind;
    w.flags = pc_handler_level() == 4 ? PC_WF_IN_HINT : 0;
    w.addr = (uint16_t)addr;
    w.value = (uint16_t)value;
    pc_state_apply(&S, &w);
    if (vline < 224u) {
        PcFrame *f = pc_frame_rec;
        if (!f)
            return;
        if (f->number + 1u == pc_time_frame(t)) {
            /* A write that lands just past the next frame's start, with the tick not there yet: it is in the live state now, so that frame's start will hold it. */
        } else if (f->number != pc_time_frame(t)) {
            if (pc_counts.misfiled++ < 8u)
                printf("note: a write for frame %u, line %u, position %u, with frame %u on record; kind %d, address %X, in a handler at level %d\n", (unsigned)pc_time_frame(t), (unsigned)vline,
                       (unsigned)w.hclock, (unsigned)f->number, kind, (unsigned)addr, pc_handler_level());
        } else if (f->n_writes < PC_FRAME_WRITES_MAX) {
            f->writes[f->n_writes++] = w;
        } else {
            f->dropped++;
            pc_counts.frame_full++;
        }
    }
}

/* ---- interrupts: each flag rises whatever its enable says; the request to the 68000 is flag and enable, with V-int first. */
static int request_level(void)
{
    if (flag_v && (S.regs[1] & 0x20u))
        return 6;
    if (flag_h && (S.regs[0] & 0x10u))
        return 4;
    return 0;
}

int pc_vdp_pending_level(void) { return request_level(); }

static void update_request(uint64_t t, int old_level)
{
    int level = request_level();
    pt_irq_level = (u32)level;
    if (level != old_level && level > old_level) {
        /* An enable written while its flag was up: the interrupt rises at that write. */
        pc_tick_note_raise(level, t, pc_handler_level() == 0 && level <= pc_ctx.imask);
    }
    if (level)
        pc_tick_force();
}

void pc_vdp_irq_ack(int level)
{
    if (level == 6)
        flag_v = 0;
    else if (level == 4)
        flag_h = 0;
    pt_irq_level = (u32)request_level();
}

/* The H-int counter runs on lines 0-224 and is loaded with register 10 on the others. At zero it raises its flag and takes register 10 as it stands then, so
 * register 10 written ahead of time sets the gap after the next. */
void pc_vdp_hint_counter(uint64_t t, uint32_t vline)
{
    if (vline <= 224u) {
        if (hint_count == 0u) {
            hint_count = S.regs[10];
            if (flag_h)
                pc_counts.hints_merged++;
            pc_counts.hints++;
            flag_h = 1;
            pc_tick_note_raise(4, t, pc_handler_level() == 0 && 4 <= pc_ctx.imask);
            pt_irq_level = (u32)request_level();
        } else {
            hint_count--;
        }
    } else {
        hint_count = S.regs[10];
    }
}

void pc_vdp_vint_raise(uint64_t t)
{
    if (flag_v)
        pc_counts.vints_merged++;
    pc_counts.vints++;
    flag_v = 1;
    pc_tick_note_raise(6, t, pc_handler_level() == 0 && 6 <= pc_ctx.imask);
    pt_irq_level = (u32)request_level();
}

void pc_vdp_frame_start(uint32_t frame)
{
    PcFrame *f = pc_frame_rec;
    if (!f)
        return;
    f->number = frame;
    f->n_writes = 0;
    f->dropped = 0;
    memcpy(&f->start, &S, sizeof(S));
}

void pc_vdp_reset(void)
{
    memset(&S, 0, sizeof(S));
    /* The chip keeps this register inverted, and its reset clears the latch: so it reads as all ones until the first write, and the H-int counter stays quiet. */
    S.regs[10] = 0xFFu;
    ctrl_pending = 0;
    code = address = address_a16 = ctrl_first = 0;
    memset(fifo_land, 0, sizeof(fifo_land));
    fifo_head = 0;
    fifo_last = 0;
    flag_v = flag_h = 0;
    hint_count = S.regs[10];
    dma_start_time = dma_end_time = 0;
    pt_irq_level = 0;
}

/* ---- the write queue. A write waits for a slot behind the ones before it; a fifth write holds the 68000 until the oldest has landed. */
static uint64_t queue_write(uint64_t t, int slots)
{
    uint64_t oldest = fifo_land[fifo_head], land;
    if (oldest > t) {
        pc_counts.stall_mclk += oldest - t;
        pc_clock_stall(oldest - t);
        t = oldest;
    }
    land = pc_vdp_next_slot(t > fifo_last ? t : fifo_last + 1u);
    if (slots == 2)
        land = pc_vdp_next_slot(land + 1u);
    fifo_land[fifo_head] = land;
    fifo_head = (fifo_head + 1u) & 3u;
    fifo_last = land;
    return land;
}

/* One word to the memory the command names. when is its time in the log. */
static void store_word(uint64_t when, uint32_t v)
{
    switch (code & 0xFu) {
    case 1u:
        if (S.regs[1] & 0x80u) {
            if (teradrive)
                td_words[TD_PLACE()] = (uint16_t)v;
            if (address_a16)
                break;  /* the 128 KB mode bit, with a second bank this console does not have: the write goes nowhere */
        }
        log_write(when, PC_WRITE_VRAM, address & 0xFFFEu, (address & 1u) ? ((v >> 8) | (v << 8)) & 0xFFFFu : v);
        break;
    case 3u:
        log_write(when, PC_WRITE_CRAM, (address >> 1) & 63u, v);
        break;
    case 5u:
        log_write(when, PC_WRITE_VSRAM, (address >> 1) & 63u, v);
        break;
    default:
        break;
    }
    address = (address + S.regs[15]) & 0xFFFFu;
}

/* ---- DMA from the 68000's bus. The 68000 is off the bus until it ends. The firmware runs up to the start only, as nothing it does can reach the chip during it.
 * A source in the cart's window steps the firmware's stream pointer, word by word, through the cart path. */
static uint32_t dma_source_word(uint32_t a, uint64_t start)
{
    a &= PT_BUS_MASK;
    if (a >= PT_RAM_LOW)
        return pt_mem_r16(pt_ram, a & 0xFFFEu);
    if (a < pt_low_device_end)
        return pc_cart_r16(a, start);
    if (a < pt_rom_bus_end)
        return pt_mem_r16(pt_rom, a);
    return 0xFFFFu;
}

static void dma_run(uint64_t t)
{
    uint32_t len = (uint32_t)S.regs[20] << 8 | S.regs[19];
    uint32_t src = (uint32_t)S.regs[22] << 8 | S.regs[21];     /* in words, within a 128 KB window */
    uint32_t high = (uint32_t)(S.regs[23] & 0x7Fu) << 17;
    uint32_t n = len ? len : 0x10000u, i, slots = (code & 0xFu) == 1u ? 2u : 1u;
    uint64_t start = t > fifo_last ? t : fifo_last, cur = start + DMA_START_MCLK;
    for (i = 0; i < n; i++) {
        uint32_t v = dma_source_word(high | (src << 1), start);
        uint32_t j;
        for (j = 0; j < slots; j++)
            cur = pc_vdp_next_slot(cur + 1u);
        store_word(cur, v);
        if (pc_time_vline(cur) < 224u)
            pc_counts.dma_picture_words++;
        src = (src + 1u) & 0xFFFFu;
    }
    /* What the chip's registers read after a transfer. */
    S.regs[19] = S.regs[20] = 0;
    S.regs[21] = (uint8_t)src;
    S.regs[22] = (uint8_t)(src >> 8);
    code &= 0x1Fu;
    pc_counts.dma_runs++;
    pc_counts.dma_words += n;
    pc_counts.dma_mclk += cur - t;
    fifo_last = cur;
    dma_start_time = t;
    dma_end_time = cur;
    pc_clock_stall(cur - t);
}

uint64_t pc_vdp_dma_start(void) { return dma_start_time; }
uint64_t pc_vdp_dma_end(void) { return dma_end_time; }

/* ---- the control port */
static void ctrl_write(uint32_t v, uint64_t t)
{
    pc_counts.ctrl_writes++;
    if (ctrl_pending) {
        ctrl_pending = 0;
        address = (address & 0x3FFFu) | ((v & 3u) << 14);
        address_a16 = (v >> 2) & 1u;
        code = (code & 3u) | ((v >> 2) & 0x3Cu);
        if ((code & 0x20u) && (S.regs[1] & 0x10u)) {
            if (S.regs[23] & 0x80u) {
                /* Fill and copy: no use of them in the game, so only a count, which the run's report shows. */
                if (S.regs[23] & 0x40u)
                    pc_vdp_seen.copy++;
                else
                    pc_vdp_seen.fill++;
                code &= 0x1Fu;
            } else {
                dma_run(t);
            }
        }
        return;
    }
    if ((v & 0xC000u) == 0x8000u) {
        uint32_t r = (v >> 8) & 0x1Fu;
        /* A register write also sets the command's low bits and address, on the chip. */
        code = (code & 0x3Cu) | ((v >> 14) & 3u);
        address = (address & 0xC000u) | (v & 0x3FFFu);
        if (r < PC_VDP_REGS) {
            int old_level = request_level();
            log_write(t, PC_WRITE_REG, r, v & 0xFFu);
            if (r == 12u && (v & 6u))
                pc_vdp_seen.interlace++;
            if (r == 0u || r == 1u)
                update_request(t, old_level);
        }
        return;
    }
    ctrl_first = v;
    ctrl_pending = 1;
    code = (code & 0x3Cu) | ((v >> 14) & 3u);
    address = (address & 0xC000u) | (v & 0x3FFFu);
}

/* ---- the data port */
static void data_write(uint32_t v, uint64_t t)
{
    /* The time for the log: when the 68000 made the write, or if the queue was full, when a place came free. The renderer looks for the slot from there. Stamping the slot here would make it look again and land the write one slot late. */
    uint64_t entered = fifo_land[fifo_head] > t ? fifo_land[fifo_head] : t;
    pc_counts.data_writes++;
    ctrl_pending = 0;
    queue_write(t, (code & 0xFu) == 1u ? 2 : 1);
    store_word(entered, v);
}

static uint32_t data_read(uint64_t t)
{
    uint32_t v = 0;
    uint64_t wait = fifo_last > t ? fifo_last : t;
    pc_counts.data_reads++;
    ctrl_pending = 0;
    wait = pc_vdp_next_slot(wait + 1u);
    pc_counts.stall_mclk += wait - t;
    pc_clock_stall(wait - t);
    switch (code & 0xFu) {
    case 0u:
        if ((S.regs[1] & 0x80u) && teradrive)
            v = td_words[TD_PLACE()];
        else
            v = (uint32_t)S.vram[address & 0xFFFEu] << 8 | S.vram[(address & 0xFFFEu) + 1u];
        break;
    case 4u:
        v = S.vsram[((address >> 1) & 63u) < PC_VSRAM_WORDS ? (address >> 1) & 63u : 0u];
        break;
    case 8u:
        v = S.cram[(address >> 1) & 63u];
        break;
    case 12u:
        v = S.vram[address & 0xFFFFu ^ 1u];
        break;
    default:
        break;
    }
    address = (address + S.regs[15]) & 0xFFFFu;
    return v;
}

/* ---- status and the counters */
uint32_t pc_vdp_status_at(uint64_t t)
{
    uint32_t s = 0, h = pc_time_hclock(t);
    if (fifo_last <= t)
        s |= 0x200u;
    if (fifo_land[fifo_head] > t)
        s |= 0x100u;
    if (flag_v)
        s |= 0x80u;
    if (in_vblank(t))
        s |= 8u;
    if (h >= 232u && h < 864u)
        s |= 4u;        /* in the horizontal blank: measured, within about 30 clocks; no reader in the game */
    return s;
}

/* The V counter's value on each line: 0-234, then back to 229 and up to 255, on the NTSC 224 line picture. */
static uint32_t vcounter_of(uint32_t vline)
{
    return vline <= 234u ? vline : vline - 6u;
}

/* The H counter from the position. Its top eight bits count pairs of pixels, 8 master clocks a pixel, with the V counter's step at 0xA5 and a jump over part of the range in the blank. In
 * the blank some pixels are longer, which costs it three counts. The game never reads this byte. */
static uint32_t hcounter_of(uint32_t h)
{
    uint32_t n = 0xA5u + h / 16u;
    if (n > 0xB6u)
        n += 0xE4u - 0xB7u;
    if (h >= 640u)
        n -= 3u;
    return n & 0xFFu;
}

uint32_t pc_vdp_hv_at(uint64_t t)
{
    return vcounter_of(pc_time_vline(t)) << 8 | hcounter_of(pc_time_hclock(t));
}

/* ---- the ports */
static uint32_t word_read(uint32_t a)
{
    uint64_t t = pc_access_time();
    switch (a & 0x1Cu) {
    case 0u:
        return data_read(t);
    case 4u:
        pc_counts.status_reads++;
        ctrl_pending = 0;
        return pc_vdp_status_at(t);
    case 8u: case 12u: {
        uint32_t hv = pc_vdp_hv_at(t);
        pc_counts.hv_reads++;
        if (pc_vread_first < 0)
            pc_vread_first = (int)(hv >> 8);
        pc_vread_last = (int)(hv >> 8);
        return hv;
    }
    default:
        return 0xFFFFu;
    }
}

uint32_t pc_vdp_read(int size, uint32_t a)
{
    if (size == 1) {
        uint32_t v = word_read(a);
        return (a & 1u) ? (v & 0xFFu) : (v >> 8);
    }
    if (size == 2)
        return word_read(a);
    {
        uint32_t saved = pt_at, hi, lo;
        hi = word_read(a);
        pt_at = saved > 4u ? saved - 4u : 0u;
        lo = word_read(a + 2u);
        return hi << 16 | lo;
    }
}

static void word_write(uint32_t a, uint32_t v)
{
    uint64_t t = pc_access_time();
    if ((a & 0x1Cu) == 0u)
        data_write(v, t);
    else if ((a & 0x1Cu) == 4u)
        ctrl_write(v, t);
}

void pc_vdp_write(int size, uint32_t a, uint32_t v)
{
    if (size == 1) {
        if ((a & 0x18u) == 0x10u) {
            pc_counts.psg_writes++;     /* the sound chip inside the video chip: pc_bus.c has sent the byte on to pc_psg.c */
            return;
        }
        word_write(a, (v & 0xFFu) * 0x101u);
    } else if (size == 2) {
        word_write(a, v & 0xFFFFu);
    } else {
        uint32_t saved = pt_at;
        word_write(a, v >> 16);
        pt_at = saved > 4u ? saved - 4u : 0u;
        word_write(a + 2u, v & 0xFFFFu);
    }
}

/* A byte from the Z80, at the Z80's own time, so the 68000's block remainder stays for the 68000's next access. Its 8 bit bus puts the byte on both halves, as a 68000 byte write does. */
void pc_vdp_z80_write(uint32_t a, uint32_t v, uint64_t t)
{
    if ((a & 0x1Cu) == 0u)
        data_write((v & 0xFFu) * 0x101u, t);
    else if ((a & 0x1Cu) == 4u)
        ctrl_write((v & 0xFFu) * 0x101u, t);
}
