/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The CPU MATE brain: the partner's AI that the game hands to a Mega CD. On the Pocket it is a reverse-engineered emulation of the game's CPU
 * partner, built as RV32IM code for the cart's MCU and shipped inside Little-Man Firmware (little_man_firmware.bin). Its files are not part of
 * this repository, and this program carries none of it: the players' download carries Little-Man Firmware 1.0's file beside Paprium.exe, or a
 * cpumate_brain line in paprium_pc.cfg names another copy (pc_console.h); this program reads the brain's three parts out of it at start, checks
 * each against its SHA-256, and runs them in its own RV32IM interpreter (pc_rv32.h). Without it the Mega CD's link gives the stub's own answers,
 * as a firmware built without the brain does on the Pocket: the game still offers CPU MATE, and the partner stands still.
 *
 * The brain's parts, their places and its entry table are the firmware's (its buddy/zz_buddy_brain.c and docs/FAFRIUM.md): the code and its
 * read-only data at the MCU's instruction memory 0x5360-0xD2EB, the counting helpers at work RAM 0x80002C00 and its image tables at 0x80004110,
 * its data in work RAM below 0x80002C00. It reads the game's own tables from the game file through the MCU's flash window (0x04000000 plus the
 * 68000 address, each 68000 word as a 16-bit value) and the game's view of the stage from the Mega CD mirror (pc_brain_sram) that pc_addons.c
 * fills from what the game writes for the Mega CD.
 */
#ifndef PC_BRAIN_H
#define PC_BRAIN_H

#include <stddef.h>
#include <stdint.h>

/* The Mega CD mirror: 256 KB as the Pocket's SRAM holds it, every 68000 word as a 16-bit value in this computer's order. */
#define PC_BRAIN_SRAM_BYTES     0x40000u    /* 256 KB; not a game address */
#define PC_BRAIN_PRG_OFF        0x10000u    /* the Mega CD program memory's first 8 KB, from $420000 */
#define PC_BRAIN_CURSOR_OFF     0x12000u    /* the game's terrain cursor (pc_addons.c); a place in the mirror, not a game address */
#define PC_BRAIN_WORD_RAM_OFF   0x20000u    /* the Mega CD word memory, 128 KB from $600000; a place in the mirror, not a game address */

/* Reads the brain from path: the player's little_man_firmware.bin, or a folder holding brain.bin, cf.bin and rodata.bin. Returns 0; -1 when there
   is nothing at path to open; -2 when something is there but is not a brain this program takes, with the reason in why (a file of the wrong length,
   however long, is refused by its length without being read); -3 when a file is there but cannot be read (another program holds it, or Windows
   denies it), with the reason in why. */
int pc_brain_load(const char *path, char *why, size_t why_size);

/* A test: the three files of a folder, with no check of their hashes, for the tests' made-up brains. Returns as pc_brain_load. */
int pc_brain_load_test(const char *folder, char *why, size_t why_size);

/* The same from the three parts in memory, with no check of their hashes: for the tests' made-up brains. */
void pc_brain_load_parts(const uint8_t *code, size_t code_bytes, const uint8_t *helpers, size_t helper_bytes, const uint8_t *tables, size_t table_bytes);

int pc_brain_ready(void);               /* loaded, and no call of it has failed */
uint8_t *pc_brain_sram(void);           /* the Mega CD mirror */

/* The Mega CD's sub-CPU was released after a reset: the sub-CPU's start-up pattern into the word memory's mirror, the program's captured tables to
   the brain's own copy, the counter's table key cleared, and the brain's restart flag raised before its next call. */
void pc_brain_release(void);

/* One update: the brain's two entries, H2 (the AI's turn and its cycle counter) and H1 (the pad word). Returns 0 with the word and the two times, in
   the sub-CPU's cycles at 12.5 MHz from the update: when the real sub-CPU would have put the word out (d_word) and ended its busy flag (d_busy, never
   before d_word). Anything else is a PC_RV32_* stop: the brain is then off for the rest of the run, as an exception turns it off on the Pocket. */
int pc_brain_call(uint32_t *word, uint32_t *d_word, uint32_t *d_busy);

/* The run's totals, for the report. */
void pc_brain_report(void);

#endif /* PC_BRAIN_H */
