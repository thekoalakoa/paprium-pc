/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * pt_port_read and pt_port_write: which device answers which address. ROM and work RAM never come here: the runtime serves them.
 *
 * A census of the game's whole run lists every address it touches outside those. Anything else counts in pc_counts and reads as a floating bus, so an unseen path shows up
 * in the run's report. That includes the game's probes for add-ons this console does not have: all ones is no add-on. When paprium_pc.cfg puts a 32X or a
 * Mega CD on the console, or picks a console with TMSS, pc_addons.c answers its addresses first (pc_addons.h); with none, nothing here changes.
 *
 * With the console held by the cart nothing is on the bus: the rest of a running block reads 0 and its writes go nowhere.
 *
 * A write to the video chip takes any interrupt that is due before the write reaches the chip: see take_due_interrupts.
 */
#include <stdio.h>

#include "pt_rt.h"
#include "pc_addons.h"
#include "pc_cart.h"
#include "pc_clock.h"
#include "pc_io.h"
#include "pc_vdp.h"
#include "pc_z80.h"

extern PtCtx pc_ctx;

#define OTHER_LOG_MAX 32
static int other_logged;

static void other_note(const char *what, uint32_t a)
{
    if (other_logged < OTHER_LOG_MAX) {
        other_logged++;
        printf("note: %s at %06X, which the census did not list\n", what, (unsigned)a);
    }
}

static int is_vdp(uint32_t a) { return a >= 0xC00000u && a < 0xC00020u; }
static int is_io(uint32_t a) { return a >= 0xA10000u && a < 0xA10020u; }
static int is_z80(uint32_t a) { return a >= 0xA00000u && a < 0xA10000u; }
static int is_busreq(uint32_t a) { return (a & ~1u) == 0xA11100u; }
static int is_z80reset(uint32_t a) { return (a & ~1u) == 0xA11200u; }

/* The add-on probes the census lists: they read as nothing there, and writes are ignored. */
static int is_known_quiet(uint32_t a)
{
    return (a >= 0xA13000u && a < 0xA13100u) || (a >= 0xA14000u && a < 0xA14200u) || (a >= 0x400000u && a < 0x800000u);
}

uint32_t pt_port_read(int size, uint32_t a)
{
    if (pc_cart_holding())
        return 0u;
    if (a < 0x10000u) {
        uint64_t t = pc_clock_now();
        uint32_t v;
        if (size == 1) {
            v = pc_cart_r8(a, t);
        } else if (size == 2) {
            v = pc_cart_r16(a, t);
        } else {
            uint32_t hi = pc_cart_r16(a, t);
            v = hi << 16 | pc_cart_r16(a + 2u, t);
        }
        return pc_addons_on ? pc_addons_low_read(size, a, v) : v;     /* a console with TMSS: its check at $000120 */
    }
    if (is_vdp(a)) {
        pc_z80_run_to(pc_sound_time());     /* the Z80 can write the same ports: it goes first if it is earlier */
        return pc_vdp_read(size, a);
    }
    if (is_io(a))
        return pc_io_read(size, a, pc_access_time());
    if (is_busreq(a))
        return pc_z80_busreq_read(size);
    if (is_z80(a))
        return pc_z80_read(size, a);
    if (pc_addons_on) {
        uint32_t v;
        if (pc_addons_read(size, a, pc_access_time(), &v))
            return v;
    }
    pc_counts.other_reads++;
    if (!is_known_quiet(a))
        other_note("a read", a);
    return size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu;
}

/* A 68000 takes a raised interrupt before its next instruction. The translated program looks only at the start of a safe block, and it charges a block's whole time at the block's start,
 * so an interrupt can rise inside a block that still has a chip write to make. That write must not go first. The chip's command is two words held in one latch, and a handler writes
 * commands of its own: a command written just before the handler runs is replaced by the handler's, and the data words the program writes next land somewhere else. The game draws its first
 * logo that way, one row of the map a frame, at the start of the vertical blank, and rows went missing whenever the program got there just ahead of the raise.
 *
 * So the handler runs here, inside the write. A block keeps only its temporaries and its flags in C variables, and a handler leaves those alone. All else is in the context, where the
 * handler's own saves and its rte put everything back, the context's flags and the mask included. The block's address is not known here, so the address pushed is only stack contents: the rte
 * comes back to this call. A handler that changes it would mean to go somewhere else, which this cannot follow. That counts in pc_counts, so it shows in the run's report.
 *
 * One order this gets wrong, by name. A write early in a block, with the raise later in the same block, comes first on a 68000 and second here, because an access in main code has no time of
 * its own inside its block. It matters only for a command the handler would then replace. Nearly all the writes measured in that position are register words, which no command depends on. One
 * is a two-word command, by about 5 master clocks, on one route. There a 68000 would lose the row of the map that follows and this order lands it. No picture we compared is wrong
 * from it, and the count covers only the routes that were run, not the game's whole run. */
static void take_due_interrupts(void)
{
    while (pt_irq_level > pc_ctx.imask && !pc_ctx.stopped) {
        uint32_t at = pt_at;
        if (pt_take_irq(&pc_ctx, 0u) != 0u && !pc_ctx.stopped)
            pc_counts.handler_redirects++;
        pt_at = at;
    }
}

/* The same rule for what the Z80 sees: a handler that releases the bus or writes the Z80's RAM must not land inside what the main program is doing to the same things. Says whether the console is still there. */
static int due_first(void)
{
    take_due_interrupts();
    return !pc_cart_holding();
}

void pt_port_write(int size, uint32_t a, uint32_t v)
{
    if (pc_cart_holding())
        return;
    if (a < 0x10000u) {
        uint64_t t = pc_clock_now();
        pc_z80_note_cart_write(a, pc_sound_time());
        if (size == 1) {
            pc_cart_w8(a, v, t);
        } else if (size == 2) {
            pc_cart_w16(a, v, t);
        } else {
            pc_cart_w16(a, v >> 16, t);
            pc_cart_w16(a + 2u, v & 0xFFFFu, t);
        }
        return;
    }
    if (is_vdp(a)) {
        take_due_interrupts();
        if (pc_cart_holding())
            return;
        pc_z80_run_to(pc_sound_time());     /* the Z80 can write the same ports: it goes first if it is earlier */
        if (size <= 2 && (a & 0x18u) == 0x10u)
            pc_z80_psg_write_from_68000(v & 0xFFu);
        pc_vdp_write(size, a, v);
    } else if (is_io(a)) {
        pc_io_write(size, a, v, pc_access_time());
    } else if (is_busreq(a)) {
        if (due_first())
            pc_z80_busreq_write(size, v);
    } else if (is_z80reset(a)) {
        if (due_first())
            pc_z80_reset_write(size, v);
    } else if (is_z80(a)) {
        if (due_first())
            pc_z80_write(size, a, v);
    } else if (pc_addons_on && pc_addons_write(size, a, v, pc_access_time())) {
        return;
    } else {
        pc_counts.other_writes++;
        if (!is_known_quiet(a))
            other_note("a write", a);
    }
}

/* The block after a run of NOPs is a safe point, so an interrupt already due is taken there. One that rises inside that block waits for its next chip write or safe point. The NOPs' time is in their block already. */
void pt_port_delay(int cycles)
{
    (void)cycles;
}
