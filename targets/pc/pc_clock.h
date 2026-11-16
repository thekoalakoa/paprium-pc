/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The master clock, and the machine's lines and frames stepped from it.
 *
 * TIME
 *   The translated program adds the published 68000 clocks of each block to pt_clock. The real console is slower than those figures: its refresh takes the bus
 *   now and then, and so does the Z80. So master clocks = 68000 clocks x 7 x a scale for the refresh, plus the time devices held the 68000 (DMA, a full write queue, each Z80 access to its bus).
 *
 *   A moment in master clocks since power-on maps to a frame, a line and a position by plain arithmetic: T = 0 is the moment the video chip's V counter becomes 0.
 *   So the V counter steps at position 0 of every line, and that moment comes in the horizontal blank, before the line's picture.
 *
 * THE TICK
 *   The machine's own events (lines, the H-int counter, V-int, the frame) run from pt_port_tick, up to the clock, with two exceptions that keep late interrupts
 *   harmless, because the translated program takes interrupts only at safe points and before a write to the video chip:
 *   - The tick does not pass an event that raised an interrupt the program has not masked until the program has taken it. The clock runs on meanwhile. So no H-int
 *     merges with the next one unless the program masks it, and then the chip would merge it too.
 *   - While a handler runs, the tick follows that handler's effective time and not the clock. The handler's effective start is the latest of its raise (plus half
 *     an average instruction), the end of any time the 68000 was stopped for a device, and the effective end of the handler before it that could hold it off. A handler writes what it writes at that time: the writes get
 *     that time in the frame's log, and the chip's H-int counter sees a register 10 written in time for the next underflow.
 *   When the handler ends the tick runs free again up to the clock.
 */
#ifndef PC_CLOCK_H
#define PC_CLOCK_H

#include <stdint.h>

#define PC_MCLK_LINE    3420u
#define PC_FRAME_LINES  262u
#define PC_MCLK_FRAME   (PC_MCLK_LINE * PC_FRAME_LINES)
#define PC_LAST_PICTURE_LINE 223u

/* Above the published times. Refresh: whole-core simulation of the exact chips, five boot loops with no Z80 running, 1.4 to 2.3 % with 1.6 % on three of them, in parts per 65,536.
   The Z80 takes the 68000's bus for each access through its window, and this is what one access costs the 68000. It is set against the whole-core simulation's idle loop on an attract run from the same
   save: at 88 the loop's total is 0.29 % under the simulation's and its quiet frames 0.20 % over, where 84 gives 0.13 % under and 0.34 % over, and 94 gives 0.52 % and 0.03 % under. So it serves both.
   It also holds whatever else that simulation's time has and this machine's has not, such as the cart chip's reply times, which cost nothing here. */
#define PC_SCALE_REFRESH_Q16    1049u   /* 1.6 % */
#ifndef PC_Z80_BUS_SHARE_MCLK
#define PC_Z80_BUS_SHARE_MCLK   88u
#endif

/* ---- the clock */
void     pc_clock_reset(void);
uint64_t pc_clock_now(void);                /* master clocks since power-on, at pt_clock */
void     pc_clock_stall(uint64_t mclk);     /* a device held the 68000 this long */
void     pc_clock_share(uint64_t mclk);     /* the Z80 took the 68000's bus this long. This happens behind the 68000's clock, so unlike a stall it does not hold back an interrupt's handler */
uint64_t pc_sound_time(void);               /* the moment the 68000 has reached, for the Z80 and the sound chips: pc_access_time's moment, without using up the block's remainder, and never past the raise of an interrupt that is due and not taken */

/* ---- time as frame, line and position */
static inline uint32_t pc_time_frame(uint64_t t) { return (uint32_t)(t / PC_MCLK_FRAME); }
static inline uint32_t pc_time_vline(uint64_t t) { return (uint32_t)((t % PC_MCLK_FRAME) / PC_MCLK_LINE); }
static inline uint32_t pc_time_hclock(uint64_t t) { return (uint32_t)(t % PC_MCLK_LINE); }

/* ---- access times */
uint64_t pc_access_time(void);              /* the time of the device access now running: effective time in a handler, the clock elsewhere */
int      pc_handler_level(void);            /* the level of the innermost open handler, 0 in the main program */

/* ---- the tick */
void pc_tick_init(void);
void pc_tick_force(void);                   /* something moved the clock or changed what the tick waits for: look again at the next block */
void pc_tick_note_raise(int level, uint64_t t, int masked_by_main);
void pc_tick_stop_request(void);            /* the program must stop: the next block's deadline check unwinds it */
int  pc_tick_stop_requested(void);
void pc_tick_stop_clear(void);
void pc_tick_set_stack_floor(uintptr_t floor);  /* stop the 68000 once the C stack goes below floor (0: no check); clears pc_tick_stack_deep */
int  pc_tick_stack_deep(void);              /* 1 when that stop came */
void pc_tick_run_events(void);              /* with the 68000 held: run the machine's events up to the clock */
uint64_t pc_tick_position(void);            /* how far the tick has come */
uint64_t pc_tick_max_lag(int clear);        /* the most a handler ran behind its effective time, in master clocks */
void pc_tick_reset_handlers(void);          /* the 68000 was reset: no handler is open any more */

/* ---- what the tick calls, in pc_main.c: the frame's two ends */
void pc_machine_frame_start(uint32_t frame);    /* the V counter became 0 */
void pc_machine_frame_end(uint32_t frame);      /* the V counter became 224: the picture is complete */

#endif /* PC_CLOCK_H */
