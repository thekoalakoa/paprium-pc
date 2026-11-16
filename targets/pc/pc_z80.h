/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Z80 and its bus. The game's sound driver runs on it, and drives the FM chip and the PSG. It runs in step behind the machine's clock: pc_z80_run_to in pc_sound.h brings it up to a moment. The
 * 68000 sees the bus request, the reset line and the Z80's RAM through the calls here. pc_z80.c says how the two share the bus and time.
 */
#ifndef PC_Z80_H
#define PC_Z80_H

#include <stdint.h>

#include "pc_sound.h"

void pc_z80_reset(void);                /* power-on, or the console's reset line: the Z80 is in reset and its RAM is clear */
uint32_t pc_z80_read(int size, uint32_t address);
void pc_z80_write(int size, uint32_t address, uint32_t value);
uint32_t pc_z80_busreq_read(int size);
void pc_z80_busreq_write(int size, uint32_t value);
void pc_z80_reset_write(int size, uint32_t value);
void pc_z80_console_reset(void);            /* the console's reset button: the Z80's reset register cleared, as a write of 0 to it does */
void pc_z80_psg_write_from_68000(uint32_t value);
void pc_z80_frame_mark(uint32_t frame, uint64_t t);     /* the sound log's frame record */
void pc_z80_note_cart_write(uint32_t address, uint64_t t);  /* a 68000 write to cart RAM, for the count below */
const uint8_t *pc_z80_ram(void);

/* What it did. A test or the end of the run reads these. */
typedef struct PcZ80Counts {
    uint64_t clocks;            /* Z80 clocks run, without waits */
    uint64_t wait_clocks;       /* Z80 clocks spent waiting for the 68000's bus */
    uint64_t window_reads;      /* reads through the bank window */
    uint64_t other_pages;       /* of those, reads that went to a bank other than the cart's first page */
    uint64_t window_writes;     /* writes through the bank window */
    uint64_t ym_writes, ym_reads, psg_writes, bank_writes, chip_writes, chip_reads;     /* chip_ = the video chip's ports */
    uint64_t io_cycles;         /* port instructions, which reach nothing on this console */
    uint64_t dma_waits;         /* window accesses that waited for a DMA's end */
    uint64_t resets, bus_requests;
    uint64_t no_grant_writes;   /* 68000 writes to the Z80's space with the bus not requested: they go nowhere */
    uint64_t near_cart_writes;  /* Z80 reads of a cart RAM word the 68000 wrote within a line of it: their order is not exact, so a run must show none */
} PcZ80Counts;
const PcZ80Counts *pc_z80_counts(void);

#endif /* PC_Z80_H */
