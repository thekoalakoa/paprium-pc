/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Unit test of the lockstep core with a scripted reference and hand-written "translated" steps.
 * A clean case must pass. Each planted difference must be reported with the right kind, and the
 * flags compare only what is live.
 */
#include <stdio.h>
#include <string.h>

#include "pt_rt.h"

const PtEntry pt_map[] = {{0, 0, 0}};
const u32 pt_map_n = 0;
const PtOverlay pt_overlays[] = {{0, 0, 0, 0}};

static LsCpu state;
static int script;

/* The scripted reference: block 0x100 does one device read and one write, then moves to 0x110. */
static void ref_step(int n) {
    (void)n;
    if (script == 1) ls_ref_event(LS_READ, 2, 0xC00004, 0x3400);
    ls_ref_event(LS_WRITE, 2, 0xFF0010, 0x1234);
    ls_ref_event(LS_READ, 2, 0xFF0010, 0x1234);    /* a RAM read: left out of the queue unless strict */
    state.pc = 0x110;
    state.d[0] = 0x3400;
    state.fz = 1;
}
static void ref_get(LsCpu *s) { *s = state; }
static uint8_t ref_ram[0x10000];
static const uint8_t *ref_work_ram(void) { return ref_ram; }

static void ref_poke(uint32_t address, int size, uint32_t value) {
    (void)size;
    ref_ram[(address & 0xFFFF) ^ 1] = (uint8_t)value;
}

static const LsRef ref = {ref_step, ref_get, 0, 0, ref_work_ram, 0, ref_poke};

static void start(const char *dir, int with_read) {
    LsOptions o;
    memset(&o, 0, sizeof(o));
    o.report_dir = dir;
    o.low_device_end = 0x10000;
    o.rom_end = 0x400000;
    ls_init(&ref, &o);
    memset(&state, 0, sizeof(state));
    state.pc = 0x100;
    script = with_read;
}

static int failed;
static void expect(const char *name, const char *kind) {
    const LsStats *s = ls_stats();
    int ok = kind ? (s->differences > 0 && strcmp(s->first_kind, kind) == 0) : s->differences == 0;
    printf("case %s %s: differences %llu first %s\n", ok ? "ok" : "FAIL", name, (unsigned long long)s->differences,
           s->first_kind);
    if (!ok) failed++;
}

int main(int argc, char **argv) {
    static PtCtx ctx;
    PtCtx *c = &ctx;
    const char *dir = argc > 1 ? argv[1] : NULL;
    u32 v;

    /* clean */
    memset(c, 0, sizeof(*c));
    start(dir, 1);
    ls_block(c, 0x100, 2, 0, 0);
    v = ls_read(2, 0xC00004);
    D(0) = v;
    ls_write(2, 0xFF0010, 0x1234);
    c->fz = 1;
    ls_block(c, 0x110, 1, 4, 0);
    expect("clean", NULL);

    /* a wrong written value */
    memset(c, 0, sizeof(*c));
    start(dir, 0);
    ls_block(c, 0x100, 2, 0, 0);
    ls_write(2, 0xFF0010, 0x9999);
    expect("wrong write value", "write_value");

    /* a write the reference did not make */
    memset(c, 0, sizeof(*c));
    start(dir, 0);
    ls_block(c, 0x100, 2, 0, 0);
    ls_write(2, 0xFF0020, 0x1234);
    expect("wrong write address", "access_order");

    /* an access the reference made and the translated side did not */
    memset(c, 0, sizeof(*c));
    start(dir, 1);
    ls_block(c, 0x100, 2, 0, 0);
    ls_block(c, 0x110, 1, 0, 0);
    expect("missing access", "missing_access");

    /* a different next block */
    memset(c, 0, sizeof(*c));
    start(dir, 0);
    ls_block(c, 0x100, 2, 0, 0);
    ls_write(2, 0xFF0010, 0x1234);
    D(0) = 0x3400;
    c->fz = 1;
    ls_block(c, 0x120, 1, 0, 0);
    expect("next pc", "next_pc");

    /* a register difference */
    memset(c, 0, sizeof(*c));
    start(dir, 0);
    ls_block(c, 0x100, 2, 0, 0);
    ls_write(2, 0xFF0010, 0x1234);
    D(0) = 0x3401;
    c->fz = 1;
    ls_block(c, 0x110, 1, 0, 0);
    expect("register", "register");

    /* a flag difference where the flag is live */
    memset(c, 0, sizeof(*c));
    start(dir, 0);
    ls_block(c, 0x100, 2, 0, 0);
    ls_write(2, 0xFF0010, 0x1234);
    D(0) = 0x3400;
    c->fz = 0;
    ls_block(c, 0x110, 1, 4, 0);
    expect("live flag", "flags");

    /* the same flag difference where it is dead is not a difference */
    memset(c, 0, sizeof(*c));
    start(dir, 0);
    ls_block(c, 0x100, 2, 0, 0);
    ls_write(2, 0xFF0010, 0x1234);
    D(0) = 0x3400;
    c->fz = 0;
    ls_block(c, 0x110, 1, 3, 0);
    expect("dead flag", NULL);

    /* the status word in an interrupt frame: a dead flag may differ, and the reference's word is kept */
    memset(c, 0, sizeof(*c));
    start(dir, 0);
    ls_block(c, 0x100, 2, 4, 0);
    ls_ref_event(LS_WRITE, 2, 0xFF8000, 0x271D);    /* X, N, Z and C set; only Z is live */
    /* the block's own events come first in the queue */
    ls_write(2, 0xFF0010, 0x1234);
    v = ls_srframe(0xFF8000, 0x2704);
    if (v != 0x271D) { printf("case FAIL status word kept\n"); failed++; }
    expect("status word, dead flags differ", NULL);

    memset(c, 0, sizeof(*c));
    start(dir, 0);
    ls_block(c, 0x100, 2, 4, 0);
    ls_ref_event(LS_WRITE, 2, 0xFF8000, 0x2700);    /* Z clear, and Z is live */
    ls_write(2, 0xFF0010, 0x1234);
    ls_srframe(0xFF8000, 0x2704);
    expect("status word, a live flag differs", "sr_frame");

    /* the whole work RAM at a frame end */
    memset(c, 0, sizeof(*c));
    start(dir, 0);
    memset(ref_ram, 0, sizeof(ref_ram));
    memset(pt_ram, 0, sizeof(pt_ram));
    ls_request_ram_check();
    ls_block(c, 0x100, 2, 0, 0);
    expect("work RAM equal", NULL);

    memset(c, 0, sizeof(*c));
    start(dir, 0);
    ref_ram[0x1234] = 1;
    ls_request_ram_check();
    ls_block(c, 0x100, 2, 0, 0);
    expect("work RAM differs", "ram_image");

    /* natural timing: the reference took an interrupt after the block; its accesses wait in the queue */
    memset(c, 0, sizeof(*c));
    start(dir, 0);
    ls_block(c, 0x100, 2, 0, 1);
    ls_write(2, 0xFF0010, 0x1234);
    ls_note_irq(6);
    ls_ref_event(LS_READ, 4, 0x78, 0x300);
    ls_ref_event(LS_WRITE, 4, 0xFF7FFC, 0x110);
    ls_ref_event(LS_WRITE, 2, 0xFF7FFA, 0x2004);
    state.pc = 0x300;
    state.a[7] = 0xFF7FFA;
    D(0) = 0x3400;
    c->fz = 1;
    A(7) = 0xFF8000;
    ls_block(c, 0x110, 1, 4, 1);
    if (pt_irq_level != 6) { printf("case FAIL natural interrupt level %u\n", (unsigned)pt_irq_level); failed++; }
    pt_irq_level = 0;
    if (ls_read(4, 0x78) != 0x300) { printf("case FAIL natural interrupt vector\n"); failed++; }
    ls_write(4, 0xFF7FFC, 0x110);
    (void)ls_srframe(0xFF7FFA, 0x2004);
    expect("natural interrupt followed", NULL);

    /* the same, with an access of the block left undone */
    memset(c, 0, sizeof(*c));
    start(dir, 0);
    ls_block(c, 0x100, 2, 0, 1);
    ls_note_irq(6);
    ls_block(c, 0x110, 1, 4, 1);
    pt_irq_level = 0;
    expect("natural interrupt, access missing before it", "missing_access");

    /* strict reads: a wrong value read from plain memory */
    {
        LsOptions o;
        memset(&o, 0, sizeof(o));
        o.report_dir = dir;
        o.low_device_end = 0x10000;
        o.rom_end = 0x400000;
        o.strict_reads = 1;
        ls_init(&ref, &o);
        memset(&state, 0, sizeof(state));
        state.pc = 0x100;
        script = 0;
        memset(c, 0, sizeof(*c));
        ls_block(c, 0x100, 2, 0, 0);
        ls_write(2, 0xFF0010, 0x1234);
        (void)ls_read_plain(2, 0xFF0010, 0x1235);
        expect("strict reads, wrong value", "read_value");
    }

    /* a route's RAM change reaches both sides at the next safe point, not before */
    memset(c, 0, sizeof(*c));
    start(dir, 0);
    memset(ref_ram, 0, sizeof(ref_ram));
    memset(pt_ram, 0, sizeof(pt_ram));
    ls_poke(0xFF1234, 1, 0x5A);
    ls_block(c, 0x100, 2, 0, 0);
    if (pt_ram[0x1234 ^ 1] != 0) { printf("case FAIL a poke waits for a safe point\n"); failed++; }
    ls_write(2, 0xFF0010, 0x1234);
    pt_mem_w16(pt_ram, 0x10, 0x1234);
    ref_ram[0x10] = 0x34;
    ref_ram[0x11] = 0x12;
    D(0) = 0x3400;
    c->fz = 1;
    ls_block(c, 0x110, 1, 0, 1);
    if (pt_ram[0x1234 ^ 1] != 0x5A || ref_ram[0x1234 ^ 1] != 0x5A) { printf("case FAIL a poke reaches both sides\n"); failed++; }
    ls_write(2, 0xFF0010, 0x1234);
    ls_request_ram_check();
    state.pc = 0x110;
    ls_block(c, 0x110, 1, 0, 0);
    expect("a route's poke on both sides", NULL);

    /* the same RAM change on one side only is a difference */
    memset(c, 0, sizeof(*c));
    start(dir, 0);
    memset(ref_ram, 0, sizeof(ref_ram));
    memset(pt_ram, 0, sizeof(pt_ram));
    pt_ram[0x1234 ^ 1] = 0x5A;
    ls_request_ram_check();
    ls_block(c, 0x100, 2, 0, 0);
    expect("a poke on one side only", "ram_image");

    /* a block of RAM code whose bytes changed */
    memset(c, 0, sizeof(*c));
    start(dir, 0);
    ls_block(c, 0x100, 2, 0, 0);
    ls_ram_code_check(0xFF5000, 0);
    expect("RAM code changed", "code_changed");

    printf("%s\n", failed ? "RESULT: FAIL" : "RESULT: PASS");
    return failed ? 1 : 0;
}
