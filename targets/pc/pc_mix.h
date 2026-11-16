/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The mix: FM, PSG and the cart's DT output, through the Pocket core's own output chain, to 48 kHz stereo. Nothing here gives the machine anything back. See pc_sound.h for the two calls the
 * FM and PSG side makes, and the frame's end.
 *
 * With no output asked for, every call returns at once, and the machine runs just the same.
 */
#ifndef PC_MIX_H
#define PC_MIX_H

#include <stdint.h>

/* Whether anything listens. The synth asks before it works out the DT output, which only the mix reads. */
int pc_mix_on(void);

/* The cart's DT output changed at time t. */
void pc_mix_dt(uint64_t t, int32_t left, int32_t right);

/* Send the output to the audio device's ring too, by this call, which must never wait. Asked for before the machine starts. */
void pc_mix_to_device(void (*push)(const int16_t *pairs, uint32_t n));

/* A test switch: the Pocket's output block has a one-bit way to sample at 48 kHz in place of 24 kHz. This takes it, for a comparison by ear. Asked for before the machine starts. */
void pc_mix_no_hold(void);

/* Finishes the WAV file. Also runs by itself when the program ends. */
void pc_mix_close(void);

void pc_mix_report(void);

#endif /* PC_MIX_H */
