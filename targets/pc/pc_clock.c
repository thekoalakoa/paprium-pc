/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The master clock and the tick. pc_clock.h explains both.
 */
#include <string.h>

#include "pt_rt.h"
#include "pc_clock.h"
#include "pc_vdp.h"
#include "pc_z80.h"

/* ---- the clock: master clocks = base + (68000 clocks since the base) x scale. */
static uint64_t base_mclk, base_cpu;
static uint32_t frac;           /* what the last conversion left over, in 65,536ths of a master clock */
static uint32_t scale_q16;      /* master clocks per 68000 clock, in 65,536ths */
static uint64_t last_stall_end;  /* the 68000 was off the bus, or waiting on it, until this time */

/* A word form branch charges less when taken than its block charged, so a difference can be briefly negative. */
static inline void rebase(void)
{
    int64_t acc = (int64_t)(pt_clock - base_cpu) * (int64_t)scale_q16 + frac;
    base_mclk = (uint64_t)((int64_t)base_mclk + (acc >> 16));
    frac = (uint32_t)(acc & 0xFFFF);
    base_cpu = pt_clock;
}

uint64_t pc_clock_now(void)
{
    int64_t acc = (int64_t)(pt_clock - base_cpu) * (int64_t)scale_q16 + frac;
    return (uint64_t)((int64_t)base_mclk + (acc >> 16));
}

void pc_clock_stall(uint64_t mclk)
{
    base_mclk += mclk;
    last_stall_end = pc_clock_now();
    pc_tick_force();
}

void pc_clock_share(uint64_t mclk)
{
    base_mclk += mclk;
    pc_tick_force();
}

void pc_clock_reset(void)
{
    pt_clock = 0;
    base_mclk = base_cpu = 0;
    last_stall_end = 0;
    frac = 0;
    scale_q16 = 7u * (65536u + PC_SCALE_REFRESH_Q16);
}

/* The value of pt_clock at which the clock reaches t. */
static uint64_t cpu_time_of(uint64_t t)
{
    uint64_t now;
    rebase();
    now = base_mclk;
    if (t <= now)
        return pt_clock;
    return pt_clock + (((t - now) << 16) - frac + scale_q16 - 1u) / scale_q16;
}

/* ---- handlers */
#define MAX_HANDLERS 8
#define HALF_INSTRUCTION_MCLK 35u   /* half an average 68000 instruction: 5 clocks */
#define HOLD_RECHECK_CPU 64u        /* while an interrupt holds the tick: look again this often, in case the program has masked it since */

typedef struct Handler {
    int level;
    uint64_t lag;               /* real time minus effective time */
} Handler;

static Handler handlers[MAX_HANDLERS];
static int depth;
static uint64_t raise_time[8];
static uint8_t raised_masked[8];
static uint64_t last_end[8];    /* the effective end of the last handler at each level */
static uint64_t max_lag;
static int stop_request;
static uintptr_t stack_floor;   /* see pc_tick_set_stack_floor below */
static int stack_deep;

extern PtCtx pc_ctx;

int pc_handler_level(void)
{
    return depth ? handlers[depth - 1].level : 0;
}

uint64_t pc_access_time(void)
{
    uint64_t t = pc_clock_now();
    if (depth) {
        t -= handlers[depth - 1].lag;
        if (pt_at) {
            t -= ((uint64_t)pt_at * scale_q16) >> 16;
            pt_at = 0;
        }
    }
    return t;
}

uint64_t pc_sound_time(void)
{
    uint64_t t = pc_clock_now();
    if (depth) {
        t -= handlers[depth - 1].lag;
        if (pt_at)
            t -= ((uint64_t)pt_at * scale_q16) >> 16;
    }
    /* The tick's own rule: a handler not yet taken will run at an effective time from its raise on, so nothing that handler can reach may go past the raise. */
    if (pt_irq_level > pc_ctx.imask && raise_time[pt_irq_level] < t)
        t = raise_time[pt_irq_level];
    return t;
}

void pc_tick_note_raise(int level, uint64_t t, int masked_by_main)
{
    raise_time[level] = t;
    raised_masked[level] = (uint8_t)masked_by_main;
}

void pt_port_irq_ack(int level)
{
    uint64_t now = pc_clock_now(), start;
    int i;
    pc_vdp_irq_ack(level);
    start = raised_masked[level] ? now : raise_time[level] + HALF_INSTRUCTION_MCLK;
    /* A 68000 stopped for a DMA or a full write queue takes nothing until it is back. */
    if (last_stall_end > start)
        start = last_stall_end;
    /* A handler at this level or above could hold this one off, so it starts no earlier than that one's end. A handler below would not: this one nests in it. */
    for (i = level; i < 8; i++)
        if (last_end[i] > start)
            start = last_end[i];
    if (start > now)
        start = now;
    if (depth < MAX_HANDLERS) {
        handlers[depth].level = level;
        handlers[depth].lag = now - start;
        if (now - start > max_lag)
            max_lag = now - start;
        depth++;
    }
    pt_at = 0;
    pc_tick_force();
}

void pt_port_irq_return(int level)
{
    if (depth) {
        depth--;
        last_end[level] = pc_clock_now() - handlers[depth].lag;
    }
    pc_tick_force();
}

void pc_tick_reset_handlers(void)
{
    depth = 0;
    memset(last_end, 0, sizeof(last_end));
    memset(raised_masked, 0, sizeof(raised_masked));
}

/* ---- the tick's events, in time order within a line: the V counter's step at position 0, then the H-int counter, then V-int on its line. */
enum { EV_LINE, EV_HINT, EV_VINT };
static uint64_t ev_time;
static int ev_kind;

static void next_event(void)
{
    /* Called with ev_time and ev_kind on the event just done. */
    uint64_t line_start = ev_time - pc_time_hclock(ev_time);
    if (ev_kind == EV_LINE) {
        ev_kind = EV_HINT;
        ev_time = line_start + PC_VDP_HINT_HCLOCK;
    } else if (ev_kind == EV_HINT && pc_time_vline(ev_time) == 224u) {
        ev_kind = EV_VINT;
        ev_time = line_start + PC_VDP_VINT_HCLOCK;
    } else {
        ev_kind = EV_LINE;
        ev_time = line_start + PC_MCLK_LINE;
    }
}

static void run_event(void)
{
    uint32_t vline = pc_time_vline(ev_time);
    switch (ev_kind) {
    case EV_LINE:
        pc_z80_run_to(ev_time);     /* so the Z80 is never more than a line behind, and its bus share lands in about the right place */
        if (vline == 0u) {
            pc_machine_frame_start(pc_time_frame(ev_time));
        } else if (vline == 224u) {
            pc_z80_frame_mark(pc_time_frame(ev_time), ev_time);
            pc_machine_frame_end(pc_time_frame(ev_time));
        }
        break;
    case EV_HINT:
        pc_vdp_hint_counter(ev_time, vline);
        break;
    default:
        pc_vdp_vint_raise(ev_time);
        break;
    }
    next_event();
}

void pc_tick_init(void)
{
    ev_time = 0;
    ev_kind = EV_LINE;
    depth = 0;
    max_lag = 0;
    stop_request = 0;
    memset(raise_time, 0, sizeof(raise_time));
    memset(last_end, 0, sizeof(last_end));
    memset(raised_masked, 0, sizeof(raised_masked));
    pt_deadline = 0;
}

void pc_tick_force(void)
{
    pt_deadline = 0;
}

void pc_tick_stop_request(void)
{
    stop_request = 1;
    pt_deadline = 0;
}

int pc_tick_stop_requested(void)
{
    return stop_request;
}

void pc_tick_stop_clear(void)
{
    stop_request = 0;
    stack_deep = 0;
    pt_deadline = 0;
}

uint64_t pc_tick_position(void)
{
    return ev_time;
}

uint64_t pc_tick_max_lag(int clear)
{
    uint64_t v = max_lag;
    if (clear)
        max_lag = 0;
    return v;
}

/* The translated 68000's calls are C calls (runtime/pt_rt.h), so a 68000 call that never returns keeps its C frame. After a Console Reset the game can be
   left in a loop of such calls that changes nothing but its own stack in work RAM (pc_main.c, console_locked); stack_floor, once set, stops the 68000
   before that loop runs the program's stack out, and stack_deep says so until the 68000 starts again. 0, as before any Console Reset: no check. */
void pc_tick_set_stack_floor(uintptr_t floor)
{
    stack_floor = floor;
    stack_deep = 0;
}

int pc_tick_stack_deep(void)
{
    return stack_deep;
}

int pt_port_tick(void)
{
    uint64_t lag;
    if (stop_request)
        return 1;
    if (stack_floor && (uintptr_t)__builtin_frame_address(0) < stack_floor) {
        stack_deep = 1;
        pc_tick_stop_request();
        return 1;
    }
    rebase();
    for (;;) {
        uint64_t target;
        if (pt_irq_level > pc_ctx.imask) {
            pt_deadline = pt_clock + HOLD_RECHECK_CPU;
            return 0;
        }
        lag = depth ? handlers[depth - 1].lag : 0;
        target = base_mclk - lag;
        if (ev_time > target)
            break;
        run_event();
        if (stop_request)
            return 1;
    }
    pt_deadline = cpu_time_of(ev_time + lag);
    return 0;
}

void pc_tick_run_events(void)
{
    uint64_t now;
    rebase();
    now = base_mclk;
    while (ev_time <= now)
        run_event();
}
