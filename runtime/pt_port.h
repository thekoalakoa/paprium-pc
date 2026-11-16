/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Layer 2 of the formula: the small port layer at the game's choke points.
 *
 * A port implements these functions once. The generated code reaches them through the device branch
 * of the memory functions, or, once a hook site names them, directly. In the desk probe nothing is
 * hooked: every device access stays a plain read or write, and the lockstep rig answers it.
 *
 * Time, in the speed build only. Each block adds its 68000 clocks to pt_clock; when pt_clock reaches pt_deadline, the block calls pt_port_tick, which steps
 * the machine. So the machine runs from counted time and no wall clock, and a wait loop spins in counted time as it did on the console. The lockstep and
 * replay builds count nothing: the rig's reference machine has its own clock.
 */
#ifndef PT_PORT_H
#define PT_PORT_H

#include <stdint.h>

/* The PC program can take the translated game from a DLL that the player's PC builds from their own ROM: see targets/pc/pc_game_dll.h. The
 * generated code is then compiled with PT_GAME_DLL, and the runtime's data it uses is imported from the program, which defines it. Every other
 * build sees a plain extern. */
#ifndef PT_DATA
#if defined(PT_GAME_DLL)
#define PT_DATA extern __declspec(dllimport)
#else
#define PT_DATA extern
#endif
#endif

/* ---- the clock */
PT_DATA uint64_t pt_clock;       /* 68000 clocks since power-on, from the published instruction times */
PT_DATA uint64_t pt_deadline;    /* the clock at which the machine wants pt_port_tick called */
PT_DATA uint32_t pt_at;          /* set before a device access in an interrupt handler: how many clocks of its block are still to come after that access */

/* The deadline passed. Returns 0 to go on, or not 0 to stop the program: the running code then returns up through every caller to pt_run. */
int pt_port_tick(void);

/* An interrupt handler has run its rte. */
void pt_port_irq_return(int level);

/* Any access outside ROM and work RAM. size is 1, 2 or 4 bytes. */
uint32_t pt_port_read(int size, uint32_t address);
void pt_port_write(int size, uint32_t address, uint32_t value);

/* The program waited on purpose for this many 68000 cycles. A port may let due interrupts fire here. */
void pt_port_delay(int cycles);

/* An interrupt is being taken: the machine drops or moves its request line. */
void pt_port_irq_ack(int level);

/* Choke points a port can take over later, once a hook site in the patch source names them.
 * None of them is called in the desk probe.
 *   frame wait   the game waits for the next vertical blank
 *   line event   the game's per-line register writes, table driven
 *   DMA list     the game flushes its queued transfers to the video chip
 *   pads, save, cart call: the rest of what the game asks of the machine */
void pt_port_frame_wait(void);
void pt_port_line_event(int line, int event);
void pt_port_dma_list(uint32_t list_address, int count);
uint32_t pt_port_pad_read(int port);
void pt_port_save(int write, uint32_t address, int length);
void pt_port_cart_call(uint32_t command);

#endif /* PT_PORT_H */
