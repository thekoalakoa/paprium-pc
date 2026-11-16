/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The console the game finds (pc_console.h), and the two add-ons it can find on it: a 32X and a Mega CD. Neither add-on is here, only the answers to
 * the game's own checks, as the Pocket core's detection stubs give them (its rtl/PAPRIUM/paprium_addons.sv), and the Mega CD's link that carries the
 * CPU MATE partner's pad word (its paprium_mcd_link.sv and paprium_snoop.sv, and the cart firmware's buddy/s1_glue.c).
 *
 *   Console   the version register (pc_io.h) for the console and unit in effect; with TMSS (version 1), writes to $A14000-$A14003 ('SEGA') and
 *             $A14101 are taken, and while $A14101 bit 0 is clear the bytes at $000120 read 0x01 instead of the cart's (pc_addons_low_read): no
 *             TMSS ROM and no licence screen, only what the game's check looks at. The TeraDrive's 128 KB video memory answer is the video
 *             chip's (pc_vdp_set_teradrive). The cart's $1FE4 model bits are the firmware's option word (pc_main.c sets them as the cart starts).
 *   32X       word reads of $A130EC / $A130EE give "MARS".
 *   Mega CD   the unit of the Mega CD list (pc_console.h). The words of its BIOS's header (the BIOS at $400000, its offsets 0x120-0x14F) that the
 *             game compares give that unit's identification letters and model digits, and no copy of any Sega program: the first Mega-CD has
 *             'BO' at offset 0x128 and '1' at 0x14B, every other compared word 0; pc_addons.c has every unit's. The gate array's registers $A12000-$A1203F keep
 *             what the game writes and give it back as the Pocket's stub does: $A12003 reads with bit 0 set and bit 2 clear, $A12020 holds the busy
 *             flag and echoes $A12011, $A12022 is 'W' (the sub-CPU runs), $A12024 is the partner's word. The program memory's probe word $420000
 *             keeps what is written, and its offset 0x290 reads "MB". The version register says a Mega CD is attached. Any other read of the Mega
 *             CD's 4 MB from $400000 reads as nothing there, as without the Mega CD.
 *   The link  each update the game sends (a byte $A12011 with low bits 2 and bit 7 changed, while the sub-CPU runs) raises the busy flag; the brain
 *             (pc_brain.h) computes the word at once, and the link puts it in $A12024 and drops the busy flag at the moments the real sub-CPU would
 *             have: the brain counts them in the sub-CPU's cycles at 12.5 MHz from the update. The game skips an update while the sub-CPU is busy,
 *             and the partner's play follows from that, so the word is held back until then. The sub-CPU's reset ($A12001 bit 0 from 1 to 0) and
 *             its bus request ($A12001 bit 1 from 0 to 1, the game's "AI off") stop the update in flight; the release after a reset restarts the
 *             brain. The game's writes to the Mega CD's word memory (128 KB from $600000), and to the first 8 KB of its program memory from $420000
 *             while the sub-CPU is held and bank 0 is open, go into the brain's mirror (pc_brain_sram), each word as written: the brain reads the
 *             stage from there.
 *             Without a brain (cpumate=off, no brain file, or a call that failed) the link gives the stub's own answers, busy 0 and word 0, as a
 *             cart firmware built without the brain does: the game still offers CPU MATE and the partner stands still.
 *
 * With everything off the machine is the one this program always was: pc_addons_on is 0 and the bus never asks here.
 */
#ifndef PC_ADDONS_H
#define PC_ADDONS_H

#include <stdint.h>

#include "pc_console.h"

extern int pc_addons_on;            /* an add-on is in place: pc_bus.c asks here before it calls an address nothing */

/* The console from the machine's next reset on. */
void pc_addons_configure(const PcConsole *console);

/* The cart powers on: its link and its glue for the brain start afresh, then as pc_addons_reset. The brain keeps its own memory, as the cart's work RAM
   keeps it across a Soft Reset; the restart flag the glue raises makes it start its partner afresh. A Soft Reset that restarts the cart, as the Pocket's
   does, calls this (pc_main.c's console_start, which also reads the brain when CPU MATE has just been switched on). */
void pc_addons_power_on(void);

/* The console resets (the cart's hold, a Soft Reset): the configured console takes effect, its version register goes to the I/O chip (pc_io.h), and
   the Mega CD's own registers clear. The link and the brain are the cart's, and keep their state: the game's next boot resets the sub-CPU itself. */
void pc_addons_reset(void);

/* The console in effect since the last reset, and the machine it makes after the hardware rules. */
const PcConsole *pc_addons_console(void);
const PcMachine *pc_addons_machine(void);

/* A read or a write of the 68000 at master clock t: 1 when an add-on answered or took it. */
int pc_addons_read(int size, uint32_t address, uint64_t t, uint32_t *value);
int pc_addons_write(int size, uint32_t address, uint32_t value, uint64_t t);
/* A read of the cart's first 64 KB that the cart answered with value: the TMSS answer at $000120, else value unchanged. */
uint32_t pc_addons_low_read(int size, uint32_t address, uint32_t value);

/* The brain: whether it is ready, and a line saying why not (for the menu and the console). */
int pc_addons_brain_ready(void);
const char *pc_addons_brain_status(void);
void pc_addons_set_brain_status(const char *text);

/* A test: every word written into the brain's mirror ("w" with its place in the mirror and the word), each release ("r"), and each call, with the
   game's terrain cursor ("c") and the answer ("a" with the word and the two times), as lines of text in file, so that the calls can be made again
   on another RV32 machine and compared. Off with NULL. */
void pc_addons_trace(void *file);

/* The run's totals, for the report. */
void pc_addons_report(void);

#endif /* PC_ADDONS_H */
