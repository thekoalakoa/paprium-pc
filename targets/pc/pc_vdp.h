/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The video chip as the 68000 sees it, for this one game: ports, registers, memories, the write queue, 68000-to-chip DMA, status, the V counter, the H-int counter and
 * the two interrupts. It draws nothing: it fills a PcFrame (pc_frame.h) and the renderer draws from that.
 *
 * Left out by name, because a census of the game's whole run found no use of them: fill and copy DMA, interlace, the H counter's latch, CRAM dots, the test
 * registers, the sprite collision and overflow flags (the renderer runs later, so they read 0). A fill, a copy or an interlace bit counts in pc_vdp_seen, so an
 * unseen path shows up in the run's report.
 */
#ifndef PC_VDP_H
#define PC_VDP_H

#include <stdint.h>

#include "pc_frame.h"

/* Where the chip's own events fall in a line, in master clocks after the V counter's step. Measured on the Nuked-MD VDP in simulation: the H-int request rises on the same clock as the
   step, and the V-int request 788 clocks after the step to line 224. */
#define PC_VDP_HINT_HCLOCK  0u
#define PC_VDP_VINT_HCLOCK  788u

typedef struct PcCounts {
    uint64_t status_reads, hv_reads, data_reads, data_writes, ctrl_writes, dma_runs, dma_words, dma_picture_words, dma_mclk, stall_mclk;
    uint64_t hints, vints, hints_merged, vints_merged;
    uint64_t cart_reads, cart_writes, io_reads, io_writes, z80_writes, z80_reads, psg_writes, other_reads, other_writes;
    uint64_t misfiled, frame_full, handler_redirects;
} PcCounts;

typedef struct PcVdpSeen {
    uint32_t fill, copy, interlace, reg_first_fill_frame;
} PcVdpSeen;

extern PcCounts pc_counts;
extern PcVdpSeen pc_vdp_seen;
extern PcFrame *pc_frame_rec;           /* the frame being recorded */
extern int pc_vread_first, pc_vread_last;   /* the V counter values the program read since these were last set to -1 */

void pc_vdp_reset(void);                /* power-on, or the console's reset line */
/* A TeraDrive's second 64 KB of video memory, for the one thing the game asks of it: its boot test writes two words in the 128 KB mode and reads one
   back (pc_vdp.c). Off on every other console; set at the console's reset (pc_addons.c). */
void pc_vdp_set_teradrive(int on);
const PcVdpState *pc_vdp_state(void);   /* the live memories and registers */

uint32_t pc_vdp_read(int size, uint32_t address);
void pc_vdp_write(int size, uint32_t address, uint32_t value);
void pc_vdp_z80_write(uint32_t address, uint32_t value, uint64_t t);    /* a byte from the Z80, at the Z80's own time */
uint64_t pc_vdp_dma_start(void);        /* when the last DMA began */
uint64_t pc_vdp_dma_end(void);          /* and ended: until then the 68000's bus is taken */

/* The tick's calls */
void pc_vdp_hint_counter(uint64_t t, uint32_t vline);
void pc_vdp_vint_raise(uint64_t t);
void pc_vdp_frame_start(uint32_t frame);
void pc_vdp_irq_ack(int level);

/* What a test reads back */
uint32_t pc_vdp_status_at(uint64_t t);
uint32_t pc_vdp_hv_at(uint64_t t);
uint64_t pc_vdp_next_slot(uint64_t t);
int pc_vdp_pending_level(void);

#endif /* PC_VDP_H */
