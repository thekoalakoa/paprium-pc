/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The small interface between the two halves of the sound work, so each can be built and tested without the other.
 *
 * Part Z: the Z80, the FM chip, the PSG and their bus. Part C: the cart synth, the mix and the audio output. Sound flows one way, from the machine to the output. Nothing here gives the
 * machine anything back, so no choice about output can change a frame or a byte of RAM.
 *
 * Times are master clocks since power-on, as in pc_clock.h. For each source they only move forward.
 */
#ifndef PC_SOUND_H
#define PC_SOUND_H

#include <stdint.h>

/* ---- Part Z provides */

/* The Z80, the FM chip and the PSG run up to time t, and their sound goes to the mix. A time already passed does nothing. The cart calls this before the firmware runs up to the same time,
   so the Z80's reads of the cart and the firmware's writes land in time order. */
void pc_z80_run_to(uint64_t t);

/* Every YM2612 and PSG write and every Z80 read of the cart goes to this file, in the 16-byte record of the whole-core simulation's event log, so one analysis reads both. For tests. */
void pc_sound_log_open(const char *path);

/* The 68000 posted this word to the cart's mailbox at this address, at time t. It goes in the same file, so a song's time can be counted from the post that starts it. */
void pc_sound_log_post(uint64_t t, uint32_t address, uint32_t word);

/* ---- Part C provides */

/* The FM chip's next sample came out at time t. left and right are the sum of the chip's 24 time slots, each slot's YM2612 output times 3, as the Pocket core's audio_cond adds them up before it
   shifts the sum and adds the PSG. */
void pc_sound_fm(uint64_t t, int32_t left, int32_t right);

/* The PSG's output changed at time t. level is the sum of its four channels on the chip's own level table. */
void pc_sound_psg(uint64_t t, int32_t level);

/* The picture's frame ended at time t. All sources have run up to t: the mix takes them, and what it makes goes to the output. Never waits. */
void pc_sound_frame_end(uint64_t t);

/* The output as 48 kHz stereo in a WAV file, for tests and listening packs. No audio device is opened for this. */
void pc_sound_file_open(const char *path);

#endif /* PC_SOUND_H */
