/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The cart's synth, as the Pocket core does it in logic: the block schedule, the wave voices' envelopes and mix, the SFX channels, the echo rings, the output stage and the DT playback.
 * It works on the firmware's own views of the same fields: the window at hb.sy_win and the sound memory at hb.cram, laid out as the firmware's docs/MCU_INTERFACE.md sections 3.8 and 3.10
 * say. No firmware line changes for it. A block takes no time here, as in the reference runner.
 */
#ifndef PC_SYNTH_H
#define PC_SYNTH_H

#include <stdint.h>

/* Back to power-on, after the firmware's hb_reset: every register at its reset value, the schedule at its start, the output curve in the sound memory. */
void pc_synth_reset(void);

/* The synth runs up to the firmware's time now, at every board call. */
void pc_synth_step(uint32_t now);

/* A 68000 write just made an arrival at time t: the DT position is noted for it, so a CONFIG naming that arrival can be dated there. */
void pc_synth_arrival(uint64_t t);

/* The next time the synth has something to do that the firmware can see, for the firmware's idle jump. */
uint32_t pc_synth_next_wake(void);

/* One block, with nothing of the schedule: the chip's order, envelope to output stage, at dac_buf entry idx, on the YM path or not. Also what the tests call. */
void pc_synth_block(uint32_t idx, int ym);

/* The scheduler's decision at one TIM6 instant, on the words as the firmware's section 3.10.2 names them. The input words go in through the pointers and come out changed.
   Returns 1 when a block is to be mixed, with its dac_buf entry in idx. incomplete is set when a DT half is left incomplete. Also what the tests call. */
int pc_synth_decide(int path, uint32_t half, uint32_t nblk, uint32_t *blk, uint32_t *fp, uint32_t *cur, uint32_t *fill, uint32_t *cnt, uint32_t pos, uint32_t *idx, int *incomplete);

/* For a test: called after every block with its dac_buf entry. */
extern void (*pc_synth_block_seen)(uint32_t idx, int ym);

void pc_synth_report(void);

#endif /* PC_SYNTH_H */
