/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The audio output. The machine pushes finished samples into a one-way ring and never waits: a full ring drops what does not fit and counts it. One thread on the device's side pulls from the
 * ring. Nothing flows back, so nothing here can move a frame or a byte of the machine's memory.
 *
 * This file's other half, pc_audio.c, is the only file that sees SDL's audio. A headless run never starts SDL. A hidden window's run forces SDL's dummy driver, so an automated run never makes
 * a sound on anyone's desktop. Only a player's own run opens a real device.
 *
 * A DEVICE IS ONE OF:
 *   sdl     the real thing, SDL's default device. A player's run
 *   dummy   SDL's path with its dummy driver, which plays nothing
 *   null    no SDL: a thread that pulls in real time and drops what it pulls
 *   stall   null with the pull stopping for 0.1 to 2 seconds at random times, as a stuck device would
 *   slow    null pulling at 90 % of real time, as a device far behind would
 *   dead    a device that never pulls, as one lost for good would: the ring fills and stays full
 *   fail    an open that fails, so the game goes on with no output
 */
#ifndef PC_AUDIO_H
#define PC_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#define PC_AUDIO_RATE 48000
#define PC_ACTION_MUTE 32     /* beside pc_window.h's other PC_ACTION_ bits: the mute key */

/* Opens the device by the names above. Returns 0, or -1 with the reason in error, after which every call here does nothing. volume is 0 to 100. */
int pc_audio_open(const char *device, int volume, char *error, size_t error_size);
void pc_audio_close(void);

/* The machine's side. Never waits. pairs is left, right, left, right. */
void pc_audio_push(const int16_t *pairs, uint32_t n);

/* The device's side: n pairs of output, whatever the ring holds. Never waits. */
void pc_audio_pull(int16_t *pairs, uint32_t n);

/* Mutes or unmutes. Applied on the device's side only. */
void pc_audio_toggle_mute(void);
int pc_audio_muted(void);
/* The volume, 0 to 100, from now on. Applied on the device's side only, as mute is: the game's sound, and its file, are the same at any volume. */
void pc_audio_set_volume(int volume);
int pc_audio_volume(void);

void pc_audio_report(void);

/* For the freeze log. pc_audio_describe: the device opened and its rate, after a good pc_audio_open. pc_audio_totals: the gaps so far, and the lines about the sound that its table had no room
   for, or nothing when no device is open. pc_audio_end_of_run, after pc_audio_close and before the totals: a gap or a pause still going on when the run ended gets its line, and the gap is
   counted. Text only: nothing here reaches the machine. */
void pc_audio_describe(char *text, size_t size);
void pc_audio_totals(char *text, size_t size);
void pc_audio_end_of_run(void);

/* The machine's side, once a frame: the frame the game is making now. The device's side puts it on what it notes. A plain store: nothing comes back. pc_audio_paused: the player has paused
   the game, or let it go on, so a ring that runs dry meanwhile is noted as a pause and not as a gap. */
void pc_audio_frame(uint32_t frame);
void pc_audio_paused(int paused);

/* The machine's side, each frame: how far behind its pace the game is (pc_pace.h), in samples. Two frames or more is a stall, and so is a frame that ends half a frame or more further
   behind than the one before it. The device's side lets go of the ring's extra delay only once there has been no stall, no gap and no skip for 3 seconds, longer while stalls come soon after
   a let-go, and counts what the game is still behind as sound to come (pc_audio.c, DELAY). Plain stores: nothing comes back. */
void pc_audio_pace(uint32_t behind_pairs);

/* The freeze log's side, from its own thread: one line of text about the sound's start, a gap, a pause, a skip or the sound's delay, and the reading of the PC's timer it is about. Returns 1 with
   a line, 2 with a line about the delay (a let-go, a delay that stays high, and its end), which the freeze log counts on an allowance of its own, and 0 when there is none. The device's side
   notes each one as it happens, and never waits for this. */
int pc_audio_take_line(uint64_t *when, char *text, size_t size);

#endif /* PC_AUDIO_H */
