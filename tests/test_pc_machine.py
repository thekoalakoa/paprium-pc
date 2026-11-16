# Copyright (C) 2026 Marsupial Solutions LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""The PC program's machine part: the published instruction times, the emission, the machine alone with each rule broken in turn, the firmware as the cart, and the whole program on a
made-up ROM. tests/test_all.py calls run(); no game byte is used anywhere here.

A planted bug is one line of a source file changed on a copy in the scratch folder. The test program built from that copy must fail, in the group of checks named beside the plant.
"""
import ctypes
import hashlib
import json
import os
import re
import shutil
import stat
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, REPO)
sys.path.insert(0, HERE)

import asm68  # noqa: E402
import pc_console_scenes  # noqa: E402
import pc_arcade_scenes  # noqa: E402
import harness  # noqa: E402
import pc_brain_places  # noqa: E402
from tool import cycles68k, dec68k, discover, emit_c, flagsopt, image, regions  # noqa: E402
from targets.pc import pc_build  # noqa: E402

PC = os.path.join(REPO, 'targets', 'pc')
JOBS = int(os.environ.get('PC_TEST_JOBS', '2'))
STACK = (0xFF << 16) + (1 << 15)    # the made-up programs' stack, in work RAM
RT = os.path.join(REPO, 'runtime')
# SDL's drivers for real pads, all off, for every run of the program or a test that opens SDL: a pad plugged into the test PC is then neither read nor
# counted, so it cannot change a result. SDL reads these hints from the environment. The tests make their own pads with SDL's virtual joysticks.
NO_REAL_PADS = {'SDL_JOYSTICK_RAWINPUT': '0', 'SDL_JOYSTICK_HIDAPI': '0', 'SDL_XINPUT_ENABLED': '0', 'SDL_JOYSTICK_WGI': '0', 'SDL_DIRECTINPUT_ENABLED': '0'}


# ---- the published times: a row of each kind, by hand from the published tables
_FORMS = [
    # words, clocks when the block starts
    ((0x4E71,), 4),                     # nop
    ((0x7001,), 4),                     # moveq
    ((0x3001,), 4),                     # move.w d1,d0
    ((0x2200,), 4),                     # move.l d0,d1
    ((0x3010,), 8),                     # move.w (a0),d0
    ((0x2010,), 12),                    # move.l (a0),d0
    ((0x3080,), 8),                     # move.w d0,(a0)
    ((0x2080,), 12),                    # move.l d0,(a0)
    ((0x3100,), 8),                     # move.w d0,-(a0)
    ((0x32D8,), 12),                    # move.w (a0)+,(a1)+
    ((0x22D8,), 20),                    # move.l (a0)+,(a1)+
    ((0x33FC, 0x1234, 0x00C0, 0x0000), 20),  # move.w #imm,abs.l
    ((0x3039, 0x00C0, 0x0000), 16),     # move.w abs.l,d0
    ((0x40C0,), 6),                     # move sr,d0
    ((0x46FC, 0x2700), 16),             # move #imm,sr
    ((0xD041,), 4),                     # add.w d1,d0
    ((0xD081,), 8),                     # add.l d1,d0
    ((0xD090,), 14),                    # add.l (a0),d0
    ((0xD150,), 12),                    # add.w d0,(a0)
    ((0xD1D0,), 14),                    # adda.l (a0),a0
    ((0xD0C1,), 8),                     # adda.w d1,a0
    ((0x0640, 0x0001), 8),              # addi.w #1,d0
    ((0x0680, 0, 1), 16),               # addi.l #1,d0
    ((0x0280, 0, 1), 16),               # andi.l #1,d0
    ((0x2100,), 12),                    # move.l d0,-(a0)
    ((0x0C80, 0, 1), 14),               # cmpi.l #1,d0
    ((0x5240,), 4),                     # addq.w #1,d0
    ((0x5280,), 8),                     # addq.l #1,d0
    ((0x5248,), 8),                     # addq.w #1,a0
    ((0x5250,), 12),                    # addq.w #1,(a0)
    ((0x4240,), 4),                     # clr.w d0
    ((0x4280,), 6),                     # clr.l d0
    ((0x4250,), 12),                    # clr.w (a0)
    ((0x4A50,), 8),                     # tst.w (a0)
    ((0xB041,), 4),                     # cmp.w d1,d0
    ((0xB081,), 6),                     # cmp.l d1,d0
    ((0xB3C8,), 6),                     # cmpa.l a0,a1
    ((0xB308,), 12),                    # cmpm.b (a0)+,(a1)+
    ((0xE348,), 8),                     # lsl.w #1,d0
    ((0xE588,), 12),                    # lsl.l #2,d0
    ((0xE368,), 6),                     # lsl.w d1,d0, before its count
    ((0xE3A8,), 8),                     # lsl.l d1,d0, before its count
    ((0xE3D0,), 12),                    # lsl.w (a0)
    ((0x0800, 3), 10),                  # btst #3,d0
    ((0x0300,), 6),                     # btst d1,d0
    ((0x08D0, 1), 16),                  # bset #1,(a0)
    ((0x57C0,), 4),                     # seq d0, before the run decides
    ((0x57D0,), 12),                    # seq (a0)
    ((0xC0C1,), 38),                    # mulu d1,d0, before its bits
    ((0xC1D0,), 42),                    # muls (a0),d0, before its bits
    ((0x80C1,), 0),                     # divu d1,d0: all of it by the values
    ((0x48C0,), 4),                     # ext.l d0
    ((0x4840,), 4),                     # swap d0
    ((0xC141,), 6),                     # exg d0,d1
    ((0x41E8, 0x0010), 8),              # lea 16(a0),a0
    ((0x4850,), 12),                    # pea (a0)
    ((0x4E56, 0xFFFC), 16),             # link a6,#-4
    ((0x4E5E,), 12),                    # unlk a6
    ((0x48E7, 0xC000), 24),             # movem.l d0/d1,-(a7)
    ((0x4CDF, 0x0003), 28),             # movem.l (a7)+,d0/d1
    ((0x4C97, 0x0003), 20),             # movem.w (a7),d0/d1
    ((0x4ED0,), 8),                     # jmp (a0)
    ((0x4E90,), 16),                    # jsr (a0)
    ((0x4EB9, 0, 0x1000), 20),          # jsr abs.l
    ((0x6100, 0x0010), 18),             # bsr
    ((0x6000, 0x0010), 10),             # bra
    ((0x6610,), 8),                     # bne.s, not taken
    ((0x6600, 0x0010), 12),             # bne.w, not taken
    ((0x51C8, 0x0010), 10),             # dbf, taken
    ((0x4E75,), 16),                    # rts
    ((0x4E73,), 20),                    # rte
]


def decode_words(words):
    mem = {i * 2: w for i, w in enumerate(words)}
    return dec68k.decode(lambda a: mem.get(a, 0x4E71), 0)


def table_problems(table=None):
    problems = []
    for words, want in _FORMS:
        I = decode_words(words)
        got = cycles68k.clocks(I)
        if got != want:
            problems.append('%04X: %d, the table says %d' % (words[0], got, want))
    return problems


def test_times(check):
    p = table_problems()
    check('the published instruction times, row by row', not p, '; '.join(p[:8]))
    # A planted bug in the table: the same comparison must see it.
    keep = cycles68k._MOVE_BW[0][2]
    cycles68k._MOVE_BW[0][2] = 10
    p = table_problems()
    cycles68k._MOVE_BW[0][2] = keep
    check('a wrong row in the time table is seen', bool(p))
    I = decode_words((0x6610,))
    J = decode_words((0x6600, 0x0010))
    check('a short branch adds 2 when taken, a word branch takes 2 less', cycles68k.branch_taken_extra(I) == 2 and cycles68k.branch_taken_extra(J) == -2)


# ---- the emission, on a made-up program
def _emission_rom(extra=b''):
    """Vectors, a main program with a long straight run, an interrupt handler that writes the chip, and one reached only through a table."""
    rom = bytearray(0x10000)
    for v in range(1, 64):
        rom[4 * v:4 * v + 4] = struct.pack('>I', 0x1F00)
    rom[0:4] = struct.pack('>I', STACK)
    rom[4:8] = struct.pack('>I', 0x400)
    rom[4 * 28:4 * 29] = struct.pack('>I', 0x600)       # level 4
    rom[4 * 30:4 * 31] = struct.pack('>I', 0x700)       # level 6
    code = []
    code += [0x7000] * 100                              # moveq, 100 of them: a straight run
    code += [0xE368]                                    # lsl.w d1,d0
    code += [0xC0C1]                                    # mulu d1,d0
    code += [0x80C1]                                    # divu d1,d0
    code += [0x57C0]                                    # seq d0
    code += [0x6610]                                    # bne.s
    code += [0x51C8, 0xFFFC]                            # dbf d0,-4
    code += [0x60FE]                                    # bra self
    rom[0x400:0x400 + 2 * len(code)] = b''.join(struct.pack('>H', w) for w in code)
    handler = [0x33FC, 0x8A00, 0x00C0, 0x0004, 0x33FC, 0x1234, 0x00C0, 0x0000, 0x4E71, 0x4E71, 0x4E73]      # two writes, then rte
    rom[0x600:0x600 + 2 * len(handler)] = b''.join(struct.pack('>H', w) for w in handler)
    rom[0x700:0x702] = struct.pack('>H', 0x4E73)
    rom[0x1F00:0x1F02] = struct.pack('>H', 0x4E73)
    return bytes(rom)


def test_emission(tmp, check):
    rom = _emission_rom()
    img = image.Image(rom)
    d = discover.Discovery(img).run()
    blocks, regs = regions.build(d)
    flagsopt.analyse(blocks, regs)
    prog = emit_c.Program(d, blocks, regs)
    gen = os.path.join(tmp, 'gen_emission')
    prog.write(gen)
    with open(os.path.join(gen, 'gen_000.c'), encoding='ascii') as f:
        text = f.read()
    run_blocks = [b for pc, b in blocks.items() if 0x400 <= pc < 0x400 + 200 and b.safe]
    check('a straight run gets a safe point every 64 instructions', len(run_blocks) >= 2 and all(len(b.insns) <= 64 for b in blocks.values()),
          str([(hex(b.pc), len(b.insns)) for b in run_blocks]))
    check('each block charges its time', 'PT_BLOCK(0x000400u, 64, ' in text and ', 256);' in text.split('PT_BLOCK(0x000400u')[1].split('\n')[0] + ';', text[:0])
    check('what the run decides is charged where it decides', all(x in text for x in ('PT_CHARGE_SHIFT(', 'PT_CHARGE_MULU(', 'PT_CHARGE_DIVU(', 'if (fz) PT_CHARGE(2);', 'PT_CHARGE(2); ', 'PT_CHARGE(4);')))
    handler_part = text.split('PT_BLOCK(0x000600u')[1].split('PT_BLOCK')[0]
    # After the first write: the second write (20), two NOPs (8), the rte (20), plus the last 4 of its own.
    check('a device access in a handler says what is left of its block', 'PT_AT(52);' in handler_part and 'PT_AT(32);' in handler_part, handler_part[:300])
    main_part = text.split('PT_BLOCK(0x000400u')[1].split('PT_BLOCK')[0]
    check('no such store outside a handler', 'PT_AT' not in main_part)
    # A planted bug: with no vector seeds and no rte, the rule finds nothing, so the check above would fail.
    check('a handler reached only through a table is found from its rte', 0x600 in emit_c.interrupt_blocks(d, blocks))
    d2 = discover.Discovery(image.Image(rom)).run()
    d2.vector_tables = []
    blocks2, regs2 = regions.build(d2)
    flagsopt.analyse(blocks2, regs2)
    found = emit_c.interrupt_blocks(d2, {pc: b for pc, b in blocks2.items() if b.kind != 'rte' and pc < 0x700})
    check('that rule is what finds it: without the rte and the vectors, nothing is marked', 0x600 not in found)


# ---- the fallback's times against the table, form by form
_FB_TEST = r'''
#include <stdio.h>
#include "pt_rt.h"
int pt_fallback_static_clocks(u32 op, u32 next);
u32 pt_fallback(PtCtx *c, u32 pc);
const PtEntry pt_map[1];
const u32 pt_map_n = 0;
const PtOverlay pt_overlays[1];
int main(void) {
    unsigned op, next;
    while (scanf("%x %x", &op, &next) == 2)
        printf("%d\n", pt_fallback_static_clocks(op, next));
    return 0;
}
'''


def test_fallback_times(tmp, check):
    folder = os.path.join(tmp, 'fb_times')
    os.makedirs(folder, exist_ok=True)
    src = os.path.join(folder, 'fb_times.c')
    with open(src, 'w', newline='\n') as f:
        f.write(_FB_TEST)
    exe = os.path.join(folder, 'fb_times.exe')
    cmd = [harness.GCC, '-O1', '-w', '-std=gnu11', '-I', RT, src, os.path.join(RT, 'pt_rt.c'), os.path.join(RT, 'pt_fallback.c'), os.path.join(RT, 'pt_port_null.c'), '-o', exe]
    r = subprocess.run(cmd, capture_output=True, text=True, env=harness.gcc_env())
    if r.returncode:
        check('the fallback time test builds', False, r.stderr[:500])
        return
    # Every form the table knows, with the operands the decoder gives. Dynamic parts are checked apart.
    rows = []
    for op in range(0x10000):
        try:
            I = decode_words((op, 0x0003, 0x0000))
            if I.op in cycles68k.REFUSED:
                continue
            want = cycles68k.clocks(I)
        except (dec68k.DecodeError, KeyError, ValueError, TypeError, IndexError):
            continue
        rows.append((op, 3, want))
    text = ''.join('%X %X\n' % (op, nx) for op, nx, _ in rows)
    r = subprocess.run([exe], input=text, capture_output=True, text=True, env=harness.gcc_env())
    got = [int(x) for x in r.stdout.split()]
    bad = [(hex(op), want, have) for (op, nx, want), have in zip(rows, got) if want != have]
    check('the fallback charges what the table charges, on all %d forms' % len(rows), len(got) == len(rows) and not bad, '%d differ: %s' % (len(bad), bad[:12]))


# ---- the machine alone, and each plant
PLANTS = [
    # file, the text, what to put, the group that must fail
    ('pc_clock.c', 'scale_q16 = 7u * (65536u + PC_SCALE_REFRESH_Q16);', 'scale_q16 = 7u * 65536u;', 'clock'),
    ('pc_clock.c', 'base_mclk += mclk;\n    last_stall_end', 'base_mclk += mclk / 2u;\n    last_stall_end', 'clock'),
    ('pc_clock.c', 'base_mclk += mclk;\n    pc_tick_force();', 'base_mclk += mclk / 2u;\n    pc_tick_force();', 'clock'),
    ('pc_vdp.c', '(address & 1u) ? ((v >> 8) | (v << 8)) & 0xFFFFu : v', 'v', 'ports'),
    ('pc_vdp.c', 'address = (address & 0x3FFFu) | ((v & 3u) << 14);', 'address = (address & 0x3FFFu);', 'ports'),
    ('pc_vdp.c', '        ctrl_pending = 0;\n        return pc_vdp_status_at(t);', '        return pc_vdp_status_at(t);', 'ports'),
    ('pc_vdp.c', 'if (!display_on())\n        return 1;', 'if (0)\n        return 1;', 'status'),
    ('pc_vdp.c', 'return vline <= 234u ? vline : vline - 6u;', 'return vline;', 'status'),
    ('pc_vdp.c', 'S.regs[10] = 0xFFu;', '', 'ports'),
    ('pc_vdp.c', 'if (vline < 224u) {\n        PcFrame *f', 'if (vline < 262u) {\n        PcFrame *f', 'log'),
    ('pc_vdp.c', 'memcpy(&f->start, &S, sizeof(S));', '', 'log'),
    ('pc_vdp.c', 'if (oldest > t) {', 'if (0) {', 'queue'),
    ('pc_vdp.c', 'if (slots == 2)\n        land =', 'if (0)\n        land =', 'queue'),
    ('pc_vdp.c', 'S.regs[19] = S.regs[20] = 0;', '', 'dma'),
    ('pc_vdp.c', 'src = (src + 1u) & 0xFFFFu;', 'src = src + 1u;', 'dma'),
    ('pc_vdp.c', 'pc_clock_stall(cur - t);', '', 'dma'),
    ('pc_vdp.c', 'if ((code & 0x20u) && (S.regs[1] & 0x10u)) {', 'if (code & 0x20u) {', 'dma'),
    ('pc_vdp.c', 'if (r == 0u || r == 1u)\n                update_request(t, old_level);', '', 'interrupts'),
    ('pc_vdp.c', 'hint_count = S.regs[10];\n            if (flag_h)', 'hint_count = 0;\n            if (flag_h)', 'interrupts'),
    ('pc_vdp.c', 'if (level == 6)\n        flag_v = 0;', 'if (level == 6)\n        { }', 'interrupts'),
    ('pc_clock.c', 'if (pt_irq_level > pc_ctx.imask) {', 'if (0) {', 'tick'),
    ('pc_clock.c', 'handlers[depth].lag = now - start;', 'handlers[depth].lag = 0;', 'tick'),
    ('pc_clock.c', 'if (last_end[i] > start)', 'if (0)', 'tick'),
    ('pc_clock.c', 'if (last_stall_end > start)', 'if (0)', 'tick'),
    ('pc_clock.c', 't -= ((uint64_t)pt_at * scale_q16) >> 16;\n            pt_at = 0;', '', 'tick'),
    ('pc_clock.c', 't -= ((uint64_t)pt_at * scale_q16) >> 16;\n    }\n    /* The tick', ';\n    }\n    /* The tick', 'z80order'),
    ('pc_clock.c', 'start = raised_masked[level] ? now : raise_time[level] + HALF_INSTRUCTION_MCLK;', 'start = raise_time[level] + HALF_INSTRUCTION_MCLK;', 'tick'),
    ('pc_io.c', 'if (!p->three && p->rises == 2) {', 'if (0) {', 'pads'),
    ('pc_io.c', 'if (t - p->last_edge > PAD_TIMEOUT_MCLK)\n        p->rises = 0;\n    if (p->th) {', 'if (p->th) {', 'pads'),
    ('pc_io.c', 'v |= v << 8;', '', 'pads'),
    ('pc_io.c', 'if (!p->three && p->rises == 3) {\n            HELD(PC_PAD_Z, 0);', 'if (!p->three && p->rises == 9) {\n            HELD(PC_PAD_Z, 0);', 'pads'),
    ('pc_z80.c', 'uint32_t bit = bus_requested ? 0u : 1u;', 'uint32_t bit = bus_requested ? 1u : 0u;', 'z80'),
    # The Z80's bus, hand-counted on made-up programs.
    ('pc_z80.c', 'wait_acc += Z80_WAIT_Q16;', 'wait_acc += 0u;', 'z80bus'),
    ('pc_z80.c', 'pc_clock_share(PC_Z80_BUS_SHARE_MCLK);', '', 'z80bus'),
    ('pc_z80.c', 'bank = (bank >> 1 | (uint32_t)(v & 1u) << 8) & 0x1FFu;', 'bank = (bank << 1 | (uint32_t)(v & 1u)) & 0x1FFu;', 'z80bus'),
    ('pc_z80.c', 'pc_ym_reset(t);\n', 'pc_ym_reset(t);\n            bank = 0;\n', 'z80bus'),
    ('pc_z80.c', 'if (!bus_requested) {\n        counts.no_grant_writes++;\n        return;\n    }', '', 'z80bus'),
    ('pc_z80.c', 'pins = z80_reset(&cpu);', '', 'z80bus'),
    ('pc_z80.c', 'if (bus_requested || in_reset || pc_cart_holding()) {', 'if (in_reset || pc_cart_holding()) {', 'z80bus'),
    ('pc_z80.c', 'if (bus_requested || in_reset || pc_cart_holding()) {', 'if (bus_requested || pc_cart_holding()) {', 'z80bus'),
    ('pc_z80.c', 'z_time += (dma_end - z_time + Z_MCLK - 1u) / Z_MCLK * Z_MCLK;', '', 'z80bus'),
    ('pc_z80.c', 'pc_counts.cart_reads = keep;', '', 'z80bus'),
    ('pc_z80.c', 'if (gap < PC_MCLK_LINE)', 'if (0)', 'z80bus'),
    ('pc_z80.c', 'pc_vdp_z80_write(a, v, z_time);', 'pc_vdp_write(1, 0xC00000u | (a & 0xFFu), v);', 'z80order'),
    ('pc_vdp.c', 'data_write((v & 0xFFu) * 0x101u, t);\n    else if ((a & 0x1Cu) == 4u)\n        ctrl_write((v & 0xFFu) * 0x101u, t);', 'data_write((v & 0xFFu) * 0x101u, t);', 'z80bus'),
    ('pc_bus.c', 'pc_z80_run_to(pc_sound_time());     /* the Z80 can write the same ports: it goes first if it is earlier */\n        if (size <= 2', 'if (size <= 2', 'z80bus'),
    ('pc_bus.c', 'pc_z80_note_cart_write(a, pc_sound_time());', '', 'z80bus'),
    # The chips on the machine's clock.
    ('pc_z80.c', 'pc_ym_reset(t);\n', '', 'z80chips'),
    ('pc_ym.c', '#define YM_MCLK 42u', '#define YM_MCLK 84u', 'z80chips'),
    ('pc_ym.c', 'if (ym.cycles == SUM_ENDS_AT_CYCLE) {', 'if (ym.cycles == SUM_ENDS_AT_CYCLE || ym.cycles == 15u) {', 'z80chips'),
    ('pc_psg.c', '#define PSG_MCLK 15u', '#define PSG_MCLK 30u', 'z80chips'),
    ('pc_psg.c', '{1200, 926, 746,', '{1200, 926, 745,', 'z80chips'),
    ('pc_psg.c', '{1200, 926, 746, 582, 458, 348, 274, 208, 158, 115, 86, 61, 40, 22, 10, 0}', '{1200, 926, 746, 582, 458, 348, 274, 208, 158, 115, 86, 61, 40, 22, 0, 0}', 'z80chips'),
    ('pc_bus.c', 'pc_z80_psg_write_from_68000(v & 0xFFu);', ';', 'z80chips'),
    ('pc_z80.c', 'pc_psg_write(v, z_time);', '', 'z80chips'),
    ('pc_psg.c', 'while (psg.write_flag || psg.write_flag_l)\n        one_clock();', '', 'z80chips'),
    ('pc_bus.c', 'if (pc_cart_holding())\n        return 0u;', '', 'bus'),
    ('pc_bus.c', 'if (pc_cart_holding())\n        return;', '', 'bus'),
    ('pc_bus.c', 'return size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu;', 'return 0u;', 'bus'),
    ('pc_bus.c', 'while (pt_irq_level > pc_ctx.imask && !pc_ctx.stopped) {', 'while (0) {', 'order'),
    ('pc_bus.c', 'while (pt_irq_level > pc_ctx.imask && !pc_ctx.stopped) {', 'while (pt_irq_level && !pc_ctx.stopped) {', 'order'),
    ('pc_bus.c', 'while (pt_irq_level > pc_ctx.imask && !pc_ctx.stopped) {', 'while (pt_irq_level > pc_ctx.imask) {', 'order'),
    ('pc_bus.c', '        pt_at = at;\n', '', 'order'),
    ('pc_bus.c', 'take_due_interrupts();\n        if (pc_cart_holding())\n            return;', 'take_due_interrupts();', 'order'),
    ('pc_bus.c', 'pc_counts.handler_redirects++;', ';', 'order'),
    ('pc_bus.c', 'if (pt_take_irq(&pc_ctx, 0u) != 0u && !pc_ctx.stopped)', 'pt_take_irq(&pc_ctx, 0u);', 'order'),
    ('pc_save.c', 'fw[i ^ 1u] = file[i];', 'fw[i] = file[i];', 'save'),
    ('pc_save.c', 'memset(fw, 0x00, PC_SAVE_BYTES);\n    for (i = 0; i < (uint32_t)n; i++)', 'memset(fw, 0x55, PC_SAVE_BYTES);\n    for (i = 0; i < (uint32_t)n; i++)', 'save'),
    ('pc_save.c', 'if (n != PC_SAVE_BYTES && n != PC_SAVE_BYTES_OLD)\n        return PC_SAVE_WRONG_SIZE;', '', 'save'),
    ('pc_vdp.c', 'queue_write(t, (code & 0xFu) == 1u ? 2 : 1);\n    store_word(entered, v);', 'store_word(queue_write(t, (code & 0xFu) == 1u ? 2 : 1), v);', 'log'),
]

# The same, in the runtime's divide times.
RUNTIME_PLANTS = [
    ('if (remainder > 0xFFFFu) {', 'if (0) {'),
    ('clocks += DIVU_BIT_ZERO;', 'clocks += DIVU_BIT_ONE;'),
    ('for (bit = 15; bit >= 1; bit--) {', 'for (bit = 15; bit >= 0; bit--) {'),
    ('{ { 120, 122 }, { 126, 124 } }', '{ { 120, 122 }, { 124, 122 } }'),
    ('(quotient >> 1)', '(quotient)'),
    ('return negative ? DIVS_TOO_LARGE_NEGATIVE : DIVS_TOO_LARGE;', 'return DIVS_TOO_LARGE;'),
]


# The two rules about a late handler. Each plant must lose the order on more than half of the test's seeds.
ORDER_PLANTS = [
    ('pc_clock.c', 'if (pt_irq_level > pc_ctx.imask && raise_time[pt_irq_level] < t)', 'if (0)', "the late handler's cue"),
    ('pc_bus.c', 'take_due_interrupts();\n    return !pc_cart_holding();', 'return !pc_cart_holding();', "the main program's hold"),
]


def _machine_exe(folder, name, overrides, cores):
    """The machine test, built from this folder's files with some replaced."""
    os.makedirs(folder, exist_ok=True)
    flags = ['-w', '-std=gnu11', '-O1', '-DPT_PC_BUILD', '-I', folder, '-I', PC, '-I', RT] + pc_build.sound_core_flags(cores)
    sources = []
    for n in pc_build.MACHINE:
        sources.append(overrides.get(n + '.c', os.path.join(PC, n + '.c')))
    objects = []
    jobs = []
    for s in sources:
        jobs.append((s, os.path.join(folder, os.path.basename(s)[:-2] + '.o'), flags, [os.path.join(PC, 'pc_vdp.h'), os.path.join(PC, 'pc_clock.h'), os.path.join(PC, 'pc_frame.h')]))
    objects += pc_build.compile_jobs(jobs, 2)
    objects += pc_build.compile_jobs(pc_build.sound_core_jobs(cores, folder, '-O1'), 2)
    exe = os.path.join(folder, name)
    return flags, objects, exe


def _runtime_objects(folder):
    os.makedirs(folder, exist_ok=True)
    flags = ['-w', '-std=gnu11', '-O1', '-DPT_PC_BUILD', '-I', RT]
    return pc_build.compile_jobs([(os.path.join(RT, 'pt_rt.c'), os.path.join(folder, 'pt_rt.o'), flags, [os.path.join(RT, 'pt_rt.h')])], 2)


_TEST_MAP = r'''
#include "pt_rt.h"
const PtEntry pt_map[1];
const u32 pt_map_n = 0;
const PtOverlay pt_overlays[1];
'''


def _sound_cores(config):
    if not config:
        return None
    with open(config, encoding='utf-8') as f:
        return json.load(f).get('sound_cores')


def test_machine_alone(tmp, config, check):
    cores = _sound_cores(config)
    if not cores:
        print('skip the machine alone: the local configuration names no sound_cores')
        return
    folder = os.path.join(tmp, 'machine')
    os.makedirs(folder, exist_ok=True)
    mapsrc = os.path.join(folder, 'test_map.c')
    with open(mapsrc, 'w', newline='\n') as f:
        f.write(_TEST_MAP)
    flags, objects, _ = _machine_exe(folder, 'x', {}, cores)
    rt_objs = _runtime_objects(os.path.join(folder, 'rt'))
    test_src = os.path.join(HERE, 'pc_machine_test.c')
    test_obj = pc_build.compile_jobs([(test_src, os.path.join(folder, 'pc_machine_test.o'), flags, [os.path.join(PC, 'pc_vdp.h'), os.path.join(PC, 'pc_clock.h'), os.path.join(HERE, 'pc_z80_test.c')]),
                                      (mapsrc, os.path.join(folder, 'test_map.o'), flags, [])], 2)

    def link_and_run(objs, exe, group, cwd, runtime=None):
        r = subprocess.run([harness.GCC, '-o', exe] + objs + (runtime or rt_objs) + test_obj + ['-lm'], capture_output=True, text=True, env=harness.gcc_env())
        if r.returncode:
            return None, r.stderr[:1000]
        r = subprocess.run([exe, group, cwd], capture_output=True, text=True, env=harness.gcc_env())
        return r.returncode, r.stdout

    exe = os.path.join(folder, 'pc_machine_test.exe')
    rc, out = link_and_run(objects, exe, 'all', folder)
    check('the machine alone, made-up data', rc == 0, (out or '')[-600:])

    # Each plant, on a copy of the one file.
    names = {os.path.basename(o)[:-2]: o for o in objects}
    for i, (name, old, new, group) in enumerate(PLANTS):
        with open(os.path.join(PC, name), newline='') as f:
            text = f.read()
        if text.count(old) != 1:
            check('plant %d in %s applies' % (i, name), False, 'found %d times: %r' % (text.count(old), old[:60]))
            continue
        pf = os.path.join(folder, 'plant%d' % i)
        os.makedirs(pf, exist_ok=True)
        src = os.path.join(pf, name)
        with open(src, 'w', newline='') as f:
            f.write(text.replace(old, new))
        obj = pc_build.compile_jobs([(src, os.path.join(pf, name[:-2] + '.o'), flags, [])], 1)[0]
        objs = [obj if os.path.basename(o) == name[:-2] + '.o' else o for o in objects]
        rc, out = link_and_run(objs, os.path.join(pf, 'test.exe'), group, pf)
        check('plant in %s, %s: the %s checks catch it' % (name, old.split('\n')[0][:40], group), rc == 1, (out or '')[-200:])
        shutil.rmtree(pf, ignore_errors=True)

    # The late handler: the plant shows up on most seeds, so the test is not catching it by luck.
    for i, (name, old, new, what) in enumerate(ORDER_PLANTS):
        with open(os.path.join(PC, name), newline='') as f:
            text = f.read()
        if text.count(old) != 1:
            check('order plant %d in %s applies' % (i, name), False, 'found %d times: %r' % (text.count(old), old[:60]))
            continue
        pf = os.path.join(folder, 'orderplant%d' % i)
        os.makedirs(pf, exist_ok=True)
        src = os.path.join(pf, name)
        with open(src, 'w', newline='') as f:
            f.write(text.replace(old, new))
        obj = pc_build.compile_jobs([(src, os.path.join(pf, name[:-2] + '.o'), flags, [])], 1)[0]
        objs = [obj if os.path.basename(o) == name[:-2] + '.o' else o for o in objects]
        rc, out = link_and_run(objs, os.path.join(pf, 'test.exe'), 'z80order', pf)
        found = re.search(what + r'[^\n]*order lost on (\d+) of (\d+) seeds', out or '')
        check('order plant, %s: the order is lost on more than half the seeds' % what, rc == 1 and found is not None and int(found.group(1)) * 2 > int(found.group(2)), (out or '')[-400:])
        shutil.rmtree(pf, ignore_errors=True)

    with open(os.path.join(RT, 'pt_rt.c'), newline='') as f:
        text = f.read()
    rt_flags = ['-w', '-std=gnu11', '-O1', '-DPT_PC_BUILD', '-I', RT]
    for i, (old, new) in enumerate(RUNTIME_PLANTS):
        if text.count(old) != 1:
            check('plant %d in pt_rt.c applies' % i, False, 'found %d times: %r' % (text.count(old), old[:60]))
            continue
        pf = os.path.join(folder, 'rtplant%d' % i)
        os.makedirs(pf, exist_ok=True)
        src = os.path.join(pf, 'pt_rt.c')
        with open(src, 'w', newline='') as f:
            f.write(text.replace(old, new))
        obj = pc_build.compile_jobs([(src, os.path.join(pf, 'pt_rt.o'), rt_flags, [])], 1)
        rc, out = link_and_run(objects, os.path.join(pf, 'test.exe'), 'divide', pf, runtime=obj)
        check('plant in pt_rt.c, %s: the divide checks catch it' % old[:40], rc == 1, (out or '')[-200:])
        shutil.rmtree(pf, ignore_errors=True)


# ---- the console the game finds, its add-ons and the CPU MATE brain's seam, on made-up data: tests/pc_console_test.c
# Each plant must make the named group of that test fail.
CONSOLE_PLANTS = [
    ('pc_addons.c', '    pc_io_set_version(pc_machine_version(&mach));', '', 'version'),
    ('pc_addons.c', '*v = a == 0xA130ECu ? 0x4D41u : 0x5253u;', '*v = a == 0xA130ECu ? 0x5253u : 0x4D41u;', 'x32'),
    ('pc_addons.c', '    { 0x0000u, 0x0000u, 0x0000u, 0x424Fu, 0x0000u, 0x0031u, 0x0000u, 0x0000u },',
     '    { 0x0000u, 0x0000u, 0x0000u, 0x0000u, 0x0000u, 0x0031u, 0x0000u, 0x0000u },', 'megacd'),
    ('pc_addons.c', '| (g3 & 0xF8u) | (g3 & 2u) | 1u;', '| g3;', 'megacd'),
    ('pc_addons.c', 'case 16: return (busy ? 0x8000u : 0u) | c1;', 'case 16: return busy ? 0x8000u : 0u;', 'megacd'),
    ('pc_addons.c', 'int post = (lower & 0x7F) == 2 && ((lower >> 7) & 1) != last7 && sres_run;', 'int post = (lower & 0x7F) == 2 && sres_run;', 'link'),
    ('pc_addons.c', '(uint64_t)cycles * CLK_MD_PER_SUB_NUM + CLK_MD_PER_SUB_DEN - 1u) / CLK_MD_PER_SUB_DEN;', '(uint64_t)cycles * 8u);', 'link'),
    ('pc_addons.c', 'busy_at = release_time(post_t, db);', 'busy_at = release_time(post_t, dw);', 'link'),
    ('pc_addons.c', '                comstat2 = 0;                                            /* the sub-CPU\'s start clears its word before the game can read it */', '', 'link'),
    ('pc_addons.c', 'if (upper >= 0) w = (uint16_t)((w & 0x00FFu) | (uint32_t)upper << 8);', 'if (upper >= 0) w = (uint16_t)((w & 0xFF00u) | (uint32_t)upper);', 'link'),
    ('pc_addons.c', '(g3 >> 6) == 0u && !sres_run', '!sres_run', 'link'),
    ('pc_addons.c', 'uint16_t cursor = (uint16_t)pt_mem_r16(pt_ram, GAME_CURSOR);', 'uint16_t cursor = 0u;', 'link'),
    ('pc_addons.c', '        state = cpumate && pc_brain_ready() ? ST_READY : ST_DISABLED;', '        state = pc_brain_ready() ? ST_READY : ST_DISABLED;', 'nobrain'),
    ('pc_brain.c', '    flag_pending = 1;\n    n_releases++;', '    n_releases++;', 'link'),
    ('pc_brain.c', '    { SRAM + 0x28000u, 0x1080u, 0x00FFu },', '    { SRAM + 0x28000u, 0x1080u, 0x0000u },', 'link'),         # the brain's MCU places, not a game address
    ('pc_brain.c', '    { SRAM + 0x105F4u, 0x80001820u, 0x130u },', '    { SRAM + 0x105F6u, 0x80001820u, 0x130u },', 'link'),     # MCU places, not a game address
    ('pc_brain.c', '        failed = 1;\n', '', 'nobrain'),
    ('pc_rv32.c', 'else if ((w >> 25) == 0x20u) v = (uint32_t)((int32_t)s >> sh);', 'else if ((w >> 25) == 0x20u) v = s >> sh;', 'rv32'),
    ('pc_rv32.c', 'if (b == 0u) return 0xFFFFFFFFu;\n    if (a == 0x80000000u', 'if (b == 0u) return 0u;\n    if (a == 0x80000000u', 'rv32'),
    ('pc_rv32.c', 'else if (f3 == 1u) v = sext(v, 16);', ';', 'rv32'),
    ('pc_rv32.c', '            if (!r || !r->writable)\n                STOP(PC_RV32_STORE, a);', '            if (!r)\n                STOP(PC_RV32_STORE, a);', 'rv32'),
    ('pc_rv32.c', 'next = (x[rs1] + imm_i(w)) & ~1u;', 'next = x[rs1] + imm_i(w);', 'rv32'),
] + pc_console_scenes.PLANTS

# The random programs for the comparison with unicorn: only RV32IM, a fixed data base in sp, aligned accesses inside the data, forward branches.
RV_DATA_BASE = 0x2800
RV_RETURN = 0xFFFFFFF0


def _rv_random_program(rng, length):
    def r_(f7, rs2, rs1, f3, rd, op): return f7 << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 | rd << 7 | op
    def i_(imm, rs1, f3, rd, op): return (imm & 0xFFF) << 20 | rs1 << 15 | f3 << 12 | rd << 7 | op
    def s_(imm, rs2, rs1, f3): return ((imm >> 5) & 0x7F) << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 | (imm & 0x1F) << 7 | 0x23
    def b_(off, rs2, rs1, f3): return ((off >> 12) & 1) << 31 | ((off >> 5) & 0x3F) << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 | ((off >> 1) & 0xF) << 8 | ((off >> 11) & 1) << 7 | 0x63
    def j_(off, rd): return ((off >> 20) & 1) << 31 | ((off >> 1) & 0x3FF) << 21 | ((off >> 11) & 1) << 20 | ((off >> 12) & 0xFF) << 12 | rd << 7 | 0x6F
    dst = [n for n in range(32) if n not in (1, 2)]
    src = list(range(32))
    words = []
    for k in range(length):
        kind = rng.randrange(10)
        rd, rs1, rs2 = rng.choice(dst), rng.choice(src), rng.choice(src)
        left = length - k
        if kind < 3:
            f7, f3 = rng.choice([(0, 0), (0x20, 0), (0, 1), (0, 2), (0, 3), (0, 4), (0, 5), (0x20, 5), (0, 6), (0, 7)] + [(1, f) for f in range(8)] * 2)
            words.append(r_(f7, rs2, rs1, f3, rd, 0x33))
        elif kind < 5:
            f3 = rng.choice([0, 2, 3, 4, 6, 7, 1, 5])
            if f3 == 1:
                imm = rng.randrange(32)
            elif f3 == 5:
                imm = rng.randrange(32) | rng.choice([0, 0x400])
            else:
                imm = rng.randrange(-2048, 2048)
            words.append(i_(imm, rs1, f3, rd, 0x13))
        elif kind == 5:
            words.append((rng.getrandbits(20) << 12) | rd << 7 | rng.choice([0x37, 0x17]))
        elif kind == 6:
            f3 = rng.choice([0, 1, 2, 4, 5])
            size = {0: 1, 4: 1, 1: 2, 5: 2, 2: 4}[f3]
            off = rng.randrange(-2048, 2048 - 4) & ~(size - 1)
            words.append(i_(off, 2, f3, rd, 0x03))
        elif kind == 7:
            f3 = rng.choice([0, 1, 2])
            size = 1 << f3
            off = rng.randrange(-2048, 2048 - 4) & ~(size - 1)
            words.append(s_(off & 0xFFF, rs2, 2, f3))
        elif kind == 8 and left > 2:
            hop = rng.randrange(1, min(8, left - 1) + 1)
            words.append(b_(hop * 4, rs2, rs1, rng.choice([0, 1, 4, 5, 6, 7])))
        elif left > 2:
            hop = rng.randrange(1, min(8, left - 1) + 1)
            words.append(j_(hop * 4, rng.choice([0] + [n for n in dst if n != 0])))
        else:
            words.append(i_(rng.randrange(-2048, 2048), rs1, 0, rd, 0x13))
    words.append(0x00008067)                    # jalr x0, 0(ra)
    regs = [0] + [RV_RETURN, RV_DATA_BASE] + [rng.getrandbits(32) if rng.random() < 0.8 else rng.choice([0, 1, 0x80000000, 0xFFFFFFFF, 0x7FFFFFFF]) for _ in range(29)]
    data = bytes(rng.getrandbits(8) for _ in range(4096))
    return words, regs, data


def _rv_unicorn(words, regs, data):
    from unicorn import Uc, UC_ARCH_RISCV, UC_MODE_RISCV32
    from unicorn import riscv_const as RC
    uc = Uc(UC_ARCH_RISCV, UC_MODE_RISCV32)
    uc.mem_map(0x1000, 0x1000)
    uc.mem_map(0x2000, 0x1000)
    uc.mem_map(RV_RETURN & ~0xFFF, 0x1000)     # unicorn reads the return address's page before it sees it has arrived there
    uc.mem_write(0x1000, struct.pack('<%dI' % len(words), *words))
    uc.mem_write(0x2000, data)
    ids = [getattr(RC, 'UC_RISCV_REG_X%d' % i) for i in range(32)]
    for i in range(1, 32):
        uc.reg_write(ids[i], regs[i])
    uc.emu_start(0x1000, RV_RETURN, count=100000)      # a step limit, not a game address
    return [uc.reg_read(ids[i]) & 0xFFFFFFFF for i in range(32)], bytes(uc.mem_read(0x2000, 0x1000))


def test_console(tmp, config, check):
    cores = _sound_cores(config)
    if not cores:
        print('skip the console test: the local configuration names no sound_cores')
        return
    folder = os.path.join(tmp, 'console')
    os.makedirs(folder, exist_ok=True)
    mapsrc = os.path.join(folder, 'test_map.c')
    with open(mapsrc, 'w', newline='\n') as f:
        f.write(_TEST_MAP)
    flags, objects, _ = _machine_exe(folder, 'x', {}, cores)
    rt_objs = _runtime_objects(os.path.join(folder, 'rt'))
    test_src = os.path.join(HERE, 'pc_console_test.c')
    deps = [os.path.join(PC, n) for n in ('pc_console.h', 'pc_addons.h', 'pc_brain.h', 'pc_rv32.h', 'pc_config.h')]
    test_obj = pc_build.compile_jobs([(test_src, os.path.join(folder, 'pc_console_test.o'), flags, deps), (mapsrc, os.path.join(folder, 'test_map.o'), flags, [])], 2)

    def link_and_run(objs, exe, args, cwd):
        r = subprocess.run([harness.GCC, '-o', exe] + objs + rt_objs + test_obj + ['-lm'], capture_output=True, text=True, env=harness.gcc_env())
        if r.returncode:
            return None, r.stderr[:1000]
        r = subprocess.run([exe] + args, capture_output=True, text=True, env=harness.gcc_env(), cwd=cwd)
        return r.returncode, r.stdout

    exe = os.path.join(folder, 'pc_console_test.exe')
    rc, out = link_and_run(objects, exe, ['all'], folder)
    check('the console, the 32X and the Mega CD, its link with a made-up brain, the RV32IM interpreter: made-up data', rc == 0, (out or '')[-800:])
    if rc is None:
        return
    for i, (name, old, new, group) in enumerate(CONSOLE_PLANTS):
        with open(os.path.join(PC, name), newline='') as f:
            text = f.read()
        if text.count(old) != 1:
            check('console plant %d in %s applies' % (i, name), False, 'found %d times: %r' % (text.count(old), old[:60]))
            continue
        pf = os.path.join(folder, 'plant%d' % i)
        os.makedirs(pf, exist_ok=True)
        src = os.path.join(pf, name)
        with open(src, 'w', newline='') as f:
            f.write(text.replace(old, new))
        obj = pc_build.compile_jobs([(src, os.path.join(pf, name[:-2] + '.o'), flags, [])], 1)[0]
        objs = [obj if os.path.basename(o) == name[:-2] + '.o' else o for o in objects]
        rc, out = link_and_run(objs, os.path.join(pf, 'test.exe'), [group], pf)
        check('console plant in %s, %s: the %s checks catch it' % (name, old.strip().split('\n')[0][:48], group), rc == 1, (out or '')[-200:])
        shutil.rmtree(pf, ignore_errors=True)
    # The interpreter against unicorn's RISC-V (QEMU's), instruction for instruction in effect: random programs of our own, the registers and the data after.
    try:
        import unicorn  # noqa: F401
    except ImportError:
        print('skip the interpreter against unicorn: no unicorn in this Python')
        return
    import random
    rng = random.Random(20261113)
    same, runs, first = 0, int(os.environ.get('PC_RV_FUZZ', '400')), None
    for n in range(runs):
        words, regs, data = _rv_random_program(rng, rng.randrange(8, 120))
        path = os.path.join(folder, 'rv.bin')
        with open(path, 'wb') as f:
            f.write(struct.pack('<I', len(words)) + struct.pack('<%dI' % len(words), *words) + struct.pack('<32I', *regs) + data)
        r = subprocess.run([exe, 'rvfile', path], capture_output=True, text=True, env=harness.gcc_env(), cwd=folder)
        got = {}
        for line in r.stdout.splitlines():
            parts = line.split()
            if len(parts) == 2 and (parts[0][0] in 'xm' or parts[0] == 'stop'):
                got[parts[0]] = int(parts[1], 16) if parts[0] != 'stop' else int(parts[1])
        want_regs, want_data = _rv_unicorn(words, regs, data)
        ok = got.get('stop') == 0 and all(got.get('x%d' % i) == want_regs[i] for i in range(32)) and \
            all(got.get('m%03X' % k) == struct.unpack_from('<I', want_data, k)[0] for k in range(0, 4096, 4))
        same += ok
        if not ok and first is None:
            first = 'program %d: stop %s; first register that differs: %s' % (n, got.get('stop'), next(('x%d %08X against unicorn %08X' % (i, got.get('x%d' % i, -1), want_regs[i])
                                                                                                    for i in range(32) if got.get('x%d' % i) != want_regs[i]), 'none (data)'))
    check('the RV32IM interpreter against unicorn on %d random programs: the same registers and data after each' % runs, same == runs, first or '')


# ---- the firmware as the cart, on a made-up ROM
CART_PLANTS = [
    ('pc_cart.c', '#define FM_XOR          0xA8A2u', '#define FM_XOR          0x0000u'),
    ('pc_cart.c', 'hb.sptr = (hb.sptr + 2u) & BRD_STREAM_MAX_WORDS;', ''),
    ('pc_cart.c', 'pc_cart_w16(a, v | v << 8, t);', 'pc_cart_w16(a, v, t);'),
    ('pc_cart.c', 'if (holding)\n            pc_tick_stop_request();', ''),
    ('pc_cart.c', 'pc_synth_step(at_limit ? limit : hb.now);', ''),
    ('pc_synth.c', '*blk = (*blk + 1u) & 0xFFFu;\n        }\n    } else if (*fp', '}\n    } else if (*fp'),
    ('pc_synth.c', '        hb.cause |= BRD_CAUSE_BLK;', ''),
    ('pc_synth.c', 'x = x < -0x1FFF ? -0x1FFE : x > 0x1FFF ? 0x1FFE : x;', ''),
    ('pc_synth.c', 'if (!done_ok && now - sy.q[i].t < timeout && sy.q_n < QUEUE_N)\n            break;', ''),
    ('pc_synth.c', 'if (!done_ok) {\n            diag_add(8u);', 'if (0) {\n            diag_add(8u);'),
    ('pc_synth.c', 'half = (lev + (lev < 0 ? -63 : -64)) >> 1;', 'half = (lev + 64) >> 1;'),
    ('pc_synth.c', '(uint16_t)k->declick > 160u ? 160u : (uint32_t)(uint16_t)k->declick', '(uint32_t)(uint16_t)k->declick & 0xFFu'),
    ('pc_synth.c', 'boost_slot = SLOT_RING;', ''),
    ('pc_synth.c', 'c->delay--;', ''),
    ('pc_synth.c', '    uint32_t index = ((word >> 24) + 1u) & 0xFFu;', '    uint32_t index = (word >> 24) + 1u;'),
    ('pc_synth.c', 'chip_level_T[level / 6]', 'chip_level_T[level / 6 + 1]'),
    ('pc_synth.c', 'if (!window)\n        x = x < -8192 ? -8192 : x > 8191 ? 8191 : x;', ''),
]


def test_sound_core_pins(tmp, config, check):
    """The build takes the sound chips' cores only with exactly the text it pins, and says so plainly otherwise."""
    cores = _sound_cores(config)
    if not cores:
        print('skip the sound core pins: the local configuration names no sound_cores')
        return
    check('the pinned cores pass', bool(pc_build.sound_core_flags(cores)))
    folder = os.path.join(tmp, 'pins')
    for name, files in pc_build.SOUND_CORE_PINS.items():
        for rel in files:
            copy = {k: os.path.join(folder, k) for k in cores}
            for other, ofiles in pc_build.SOUND_CORE_PINS.items():
                for orel in ofiles:
                    target = os.path.join(copy[other], orel)
                    os.makedirs(os.path.dirname(target), exist_ok=True)
                    shutil.copyfile(os.path.join(cores[other], orel), target)
            check('a copy of the cores passes', bool(pc_build.sound_core_flags(copy)))
            path = os.path.join(copy[name], rel)
            with open(path, 'r+b') as f:
                first = f.read(1)
                f.seek(0)
                f.write(bytes([first[0] ^ 1]))
            try:
                pc_build.sound_core_flags(copy)
                check('a byte changed in %s: refused' % rel, False)
            except RuntimeError as e:
                check('a byte changed in %s: refused' % rel, 'pinned' in str(e), str(e))
            shutil.rmtree(folder, ignore_errors=True)
    for broken, words in ((None, 'names no sound_cores'), ({'fm': cores['fm'], 'psg': cores['psg']}, 'no folder'), (dict(cores, psg=folder), 'no file')):
        try:
            pc_build.sound_core_flags(broken)
            check('sound_cores %s: refused' % words, False)
        except RuntimeError as e:
            check('sound_cores %s: refused' % words, words in str(e), str(e))


def test_z80_against_firmware(tmp, config, check):
    """The Z80's reads of the cart and the firmware's turns land in time order: the real firmware, the real cart seam, a made-up ROM and a made-up Z80 program. tests/pc_z80_cart_test.c."""
    with open(config, encoding='utf-8') as f:
        cfg = json.load(f)
    tree = cfg.get('fw_source')
    cores = cfg.get('sound_cores')
    if not tree or not os.path.isdir(tree) or not cores:
        print('skip the Z80 against the firmware: the local configuration names no firmware tree or no sound_cores')
        return
    folder = os.path.join(tmp, 'pc_cart')
    jobs = pc_build.firmware_jobs(tree, folder, '-O1')
    objects = pc_build.compile_jobs(jobs, JOBS)
    fw_flags = jobs[0][2]
    objects += pc_build.compile_jobs(pc_build.machine_jobs(os.path.join(folder, 'pc'), '-O1', pc_build.MACHINE, pc_build.sound_core_flags(cores)), JOBS)
    objects += pc_build.compile_jobs(pc_build.sound_core_jobs(cores, os.path.join(folder, 'pc'), '-O1'), JOBS)
    objects += _runtime_objects(os.path.join(folder, 'rt'))
    mapsrc = os.path.join(folder, 'test_map.c')
    with open(mapsrc, 'w', newline='\n') as f:
        f.write(_TEST_MAP)
    test_flags = fw_flags + ['-DPT_PC_BUILD']
    objects += pc_build.compile_jobs([(os.path.join(HERE, 'pc_z80_cart_test.c'), os.path.join(folder, 'pc_z80_cart_test.o'), test_flags, jobs[0][3]),
                                      (mapsrc, os.path.join(folder, 'test_map.o'), ['-w', '-std=gnu11', '-O1', '-DPT_PC_BUILD', '-I', RT], [])], 1)

    def run(objs, exe_path):
        r = subprocess.run([harness.GCC, '-o', exe_path] + objs + ['-lm'], capture_output=True, text=True, env=harness.gcc_env())
        if r.returncode:
            return None, r.stderr[:1000]
        r = subprocess.run([exe_path], capture_output=True, text=True, env=harness.gcc_env())
        return r.returncode, r.stdout

    rc, out = run(objects, os.path.join(folder, 'pc_z80_cart_test.exe'))
    check('the Z80 sees the firmware\'s store at its own time, with a 68000 access past it', rc == 0, (out or '')[-500:])
    with open(os.path.join(PC, 'pc_cart.c'), newline='') as f:
        text = f.read()
    old = 'pc_z80_run_to(t);   /* so its reads of the cart and this turn\'s writes land in time order */'
    check('plant in pc_cart.c applies', text.count(old) == 1)
    pf = os.path.join(folder, 'z80plant')
    os.makedirs(pf, exist_ok=True)
    src = os.path.join(pf, 'pc_cart.c')
    with open(src, 'w', newline='') as f:
        f.write(text.replace(old, ''))
    obj = pc_build.compile_jobs([(src, os.path.join(pf, 'pc_cart.o'), fw_flags, [])], 1)[0]
    rc, out = run([obj if os.path.basename(o) == 'pc_cart.o' else o for o in objects], os.path.join(pf, 'test.exe'))
    check('plant in pc_cart.c, the firmware goes first: caught', rc not in (0, None), (out or '')[-300:])
    old = 'pc_sound_log_post(t, a, v);'
    check('plant of the post in the sound log applies', text.count(old) == 1)
    with open(src, 'w', newline='') as f:
        f.write(text.replace(old, ''))
    obj = pc_build.compile_jobs([(src, os.path.join(pf, 'pc_cart.o'), fw_flags, [])], 1)[0]
    rc, out = run([obj if os.path.basename(o) == 'pc_cart.o' else o for o in objects], os.path.join(pf, 'test2.exe'))
    check('plant in pc_cart.c, no post in the sound log: caught', rc not in (0, None), (out or '')[-300:])
    shutil.rmtree(pf, ignore_errors=True)

    # The sound's two halves as one: each source reaches the sound file, and with its one connection broken it does not.
    join_flags = test_flags
    join_obj = pc_build.compile_jobs([(os.path.join(HERE, 'pc_sound_join_test.c'), os.path.join(folder, 'pc_sound_join_test.o'), join_flags, jobs[0][3])], 1)[0]
    join_objects = [join_obj if os.path.basename(o) == 'pc_z80_cart_test.o' else o for o in objects]

    def run_join(objs, exe_path):
        r = subprocess.run([harness.GCC, '-o', exe_path] + objs + ['-lm'], capture_output=True, text=True, env=harness.gcc_env())
        if r.returncode:
            return None, r.stderr[:1000]
        r = subprocess.run([exe_path, exe_path + '.wav'], capture_output=True, text=True, env=harness.gcc_env())
        return r.returncode, r.stdout

    rc, out = run_join(join_objects, os.path.join(folder, 'pc_sound_join_test.exe'))
    check('the sound file holds each source in its own stretch: PSG, FM, the cart\'s DT output', rc == 0, (out or '')[-400:])
    machine_flags = ['-w', '-std=gnu11', '-O1', '-foptimize-sibling-calls', '-DPT_PC_BUILD', '-I', pc_build.RUNTIME, '-I', PC] + pc_build.sound_core_flags(cores)
    for name, source, old, flags in (('FM', 'pc_ym.c', 'pc_sound_fm(ym_time, sum_left, sum_right);', machine_flags),
                                     ('PSG', 'pc_psg.c', 'pc_sound_psg(psg_time, level);', machine_flags),
                                     ('DT', 'pc_synth.c', 'pc_mix_dt(t, l, r);', fw_flags)):
        with open(os.path.join(PC, source), newline='') as f:
            text = f.read()
        check('plant of the %s connection applies' % name, text.count(old) == 1)
        pf = os.path.join(folder, 'joinplant_' + name)
        os.makedirs(pf, exist_ok=True)
        src = os.path.join(pf, source)
        with open(src, 'w', newline='') as f:
            f.write(text.replace(old, ''))
        obj = pc_build.compile_jobs([(src, os.path.join(pf, source[:-2] + '.o'), flags, [])], 1)[0]
        rc, out = run_join([obj if os.path.basename(o) == source[:-2] + '.o' else o for o in join_objects], os.path.join(pf, 'test.exe'))
        check('plant, the %s does not reach the mix: caught' % name, rc not in (0, None), (out or '')[-300:])
        shutil.rmtree(pf, ignore_errors=True)


def test_cart(tmp, config, check):
    with open(config, encoding='utf-8') as f:
        cfg = json.load(f)
    tree = cfg.get('fw_source')
    if not tree or not os.path.isdir(tree):
        print('skip the PC cart: the local configuration names no firmware tree')
        return
    folder = os.path.join(tmp, 'pc_cart')
    jobs = pc_build.firmware_jobs(tree, folder, '-O1')
    objects = pc_build.compile_jobs(jobs, JOBS)
    fw_flags = jobs[0][2]
    test_obj = pc_build.compile_jobs([(os.path.join(HERE, 'pc_cart_test.c'), os.path.join(folder, 'pc_cart_test.o'), fw_flags, jobs[0][3])], 1)
    exe = os.path.join(folder, 'pc_cart_test.exe')

    def run(objs, exe_path):
        r = subprocess.run([harness.GCC, '-o', exe_path] + objs + test_obj + ['-lm'], capture_output=True, text=True, env=harness.gcc_env())
        if r.returncode:
            return None, r.stderr[:1000]
        r = subprocess.run([exe_path], capture_output=True, text=True, env=harness.gcc_env())
        return r.returncode, r.stdout

    rc, out = run(objects, exe)
    check('the firmware answers as the cart on a made-up ROM, in the PC program', rc == 0, (out or '')[-500:])
    for i, (name, old, new) in enumerate(CART_PLANTS):
        with open(os.path.join(PC, name), newline='') as f:
            text = f.read()
        if text.count(old) != 1:
            check('cart plant %d applies' % i, False, 'found %d times: %r' % (text.count(old), old[:60]))
            continue
        pf = os.path.join(folder, 'plant%d' % i)
        os.makedirs(pf, exist_ok=True)
        src = os.path.join(pf, name)
        with open(src, 'w', newline='') as f:
            f.write(text.replace(old, new))
        obj_name = name[:-2] + '.o'
        obj = pc_build.compile_jobs([(src, os.path.join(pf, obj_name), fw_flags, [])], 1)[0]
        objs = [obj if os.path.basename(o) == obj_name else o for o in objects]
        rc, out = run(objs, os.path.join(pf, 'test.exe'))
        check('cart plant, %s: caught' % old.split('\n')[0][:40], rc not in (0, None), (out or '')[-200:])
        shutil.rmtree(pf, ignore_errors=True)


# ---- the mix against the Pocket's chain, and the audio output with made-up clocks
MIX_PLANTS = [
    ('#define STEP_HZ         6144000u', '#define STEP_HZ         7056000u', 'the filter at the rate its text says, not the rate its clock gives'),  # rates; not a game address
    ('uint32_t psg_amp = (psg + (psg >> 1)) & 0xFFFFu;', 'uint32_t psg_amp = psg;', 'the PSG with no half added'),
    ('#define STEPS_A_VALUE   256u', '#define STEPS_A_VALUE   128u', 'sampling at 48 kHz with the switch off'),
    ('mix = mix > 32767 ? 32767 : mix < -32768 ? -32768 : mix;', '', 'no clip after the DT sum'),
    ('    if (mx.values < mx.mute_values)\n        o[0] = o[1] = 0;\n', '', 'no hold at the start'),
    ('    if (!mx.no_hold)\n        out_pair(o[0], o[1]);\n', '', 'each value sent once'),
    ('int64_t y1 = mx.dc_y[ch] - (mx.dc_y[ch] >> mx.dc_shift_y);', 'int64_t y1 = mx.dc_y[ch] - (mx.dc_y[ch] >> 10);', 'the DC blocker with the wrong pole'),
]
AUDIO_PLANTS = [
    ('dev.rate = (dev.mean_fill - dev.target) * RATE_GAIN;', 'dev.rate = (dev.target - dev.mean_fill) * RATE_GAIN;', 'the rate control the wrong way'),
    ('level = 1.0f - (float)dev.fade / FADE_PAIRS;', 'level = 1.0f;', 'no fade out'),
    ('level = (float)dev.fade / FADE_PAIRS;', 'level = 1.0f;', 'no fade in'),
    ('    if (n > room) {\n        dropped += n - room;\n        n = room;\n    }\n', '', 'a push past the ring'),
    ('if (fill > dev.over)\n            skip_to(fill, dev.target, NOTE_SKIP);', '', 'no skip when far over'),
    ('if (aim < FADE_PAIRS || fill < aim + FADE_PAIRS ||', 'if (fill < aim + FADE_PAIRS ||', 'a skip aims under the crossfade and reads past the ring\'s head'),
    ('#define OVER_FRAMES     8u', '#define OVER_FRAMES     2u', 'a skip at two frames over, which takes a run of stalls\' cushion'),
    ('                skip_to(fill, fill - (uint32_t)excess, NOTE_SETTLE);\n', '                ;\n', 'a stall\'s make-up stays in the ring as extra delay'),
    ('#define SETTLE_CALM     (3u * 48000u)', '#define SETTLE_CALM     (0u * 48000u)', 'the delay let go while stalls still come'),
    ('            stall_said = 0;\n            calm_broken();\n', '            stall_said = 0;\n', 'a stall does not start the calm again'),
    (' || behind_pairs >= behind_now + FRAME_PAIRS / 2u)', ')', 'a slow frame among frames on time is no stall, so the let-go comes among them'),
    ('behind_pairs >= behind_now + FRAME_PAIRS / 2u)', 'behind_pairs >= behind_now + FRAME_PAIRS / 4u)', 'a small step in the lateness counts as a stall'),
    ('    if (note == NOTE_SKIP) {\n        calm_broken();\n', '    if (note == NOTE_SKIP) {\n', 'a skip does not start the calm again'),
    ('        stall_said = 0;\n        calm_broken();\n        dev.settle_armed = 1;\n', '        stall_said = 0;\n', 'a gap leaves its catch-up in the ring'),
    ('            dev.calm_need *= 2u;', '            dev.calm_need *= 1u;', 'stalls just after each let-go never lengthen the wait'),
    ('&& dev.calm_need < SETTLE_CALM_MOST)', '&& dev.calm_need < 16u * SETTLE_CALM_MOST)', 'the wait grows past 48 seconds'),
    ('            dev.settle_watch = 0;\n            dev.calm_need = SETTLE_CALM;\n', '            dev.settle_watch = 0;\n', 'the wait never comes back to 3 seconds'),
    ('    if (!dev.settle_armed && dev.settle_mean > (double)(dev.target + FRAME_PAIRS)) {', '    if (0) {', 'a ring a device holdup left high is never let go'),
    ('if (excess > (double)SETTLE_OVER && fill > (uint32_t)excess)', 'if (excess > 0 && fill > (uint32_t)excess)', 'the delay let go however little it is over'),
    ('double excess = dev.settle_mean - (double)dev.target + (double)owed;', 'double excess = dev.settle_mean - (double)dev.target;', 'what the game still owes left out of the delay'),
    ('uint32_t owed = behind_now < FRAME_PAIRS ? behind_now : FRAME_PAIRS;', 'uint32_t owed = behind_now;', 'what the game owes taken past a frame'),
    ('            note_put(NOTE_HIGH, clock_now(), frame_now, (uint64_t)(dev.mean_fill - dev.target));\n', '', 'a delay that stays high goes unsaid'),
    ('beta = 3.5;', 'beta = 14.0;', 'a resampler that is not flat to 18 kHz'),
    ('#define TARGET_FRAMES   3u', '#define TARGET_FRAMES   2u', 'a queue of two frames, as before'),
    ('                dev.gap_when = clock_now();\n', '', 'a gap noted with no time'),
    ('                dev.gap_paused = paused_now;\n', '', 'a pause noted as a gap'),
    ('    note_put(note, clock_now(), frame_now, skip);\n', '', 'a skip goes unnoted'),
    ('                if (dev.started)\n                    dev.gap_pairs++;\n                else\n                    dev.start_pairs++;\n', '                dev.gap_pairs++;\n', 'the start counted as a gap'),
    ('    note_put(NOTE_GAP_AT_END, dev.gap_when, dev.gap_frame, dev.gap_pairs);\n', '', 'a gap still going on at the end gets no line'),
    ('    dev.gaps_all_pairs += dev.gap_pairs;\n    if (dev.gap_pairs > dev.gap_longest) {\n        dev.gap_longest = dev.gap_pairs;\n        dev.gap_longest_frame = dev.gap_frame;\n    }\n}\n', '}\n',
     'a gap still going on at the end is not in the totals'),
    ('(unsigned long long)dropped, (unsigned long long)dev.pulled, (unsigned)notes_lost);', '(unsigned long long)dropped, (unsigned long long)dev.pulled, 0u);', 'lines lost from the full table go unsaid'),
]


def test_mix_and_audio(tmp, check):
    folder = os.path.join(tmp, 'pc_mix')
    os.makedirs(folder, exist_ok=True)
    env = harness.gcc_env()

    def build_and_run(test, plant_name, plant_text):
        exe = os.path.join(folder, test + plant_name + '.exe')
        args = [pc_build.GCC, '-std=gnu11', '-O2', '-Wall', '-Wno-unused-function', '-o', exe, os.path.join(HERE, test + '.c'), '-lm']
        if plant_text is None:
            args += ['-Werror', '-I', PC]
        else:
            with open(os.path.join(folder, test[:-5] + '.c'), 'w', newline='', encoding='utf-8') as f:
                f.write(plant_text)
            args += ['-I', folder, '-I', PC, '-iquote', folder]
        r = subprocess.run(args, capture_output=True, text=True, env=env)
        if r.returncode:
            return None, r.stderr[:800]
        x = subprocess.run([exe], capture_output=True, text=True, env=env, cwd=folder, timeout=900)
        return x.returncode, x.stdout

    for test, source_name, plants in (('pc_mix_test', 'pc_mix.c', MIX_PLANTS), ('pc_audio_test', 'pc_audio.c', AUDIO_PLANTS)):
        rc, out = build_and_run(test, '', None)
        check(test + ': the output follows its rules', rc == 0, (out or '')[-600:])
        print(out)
        with open(os.path.join(PC, source_name), newline='', encoding='utf-8') as f:
            source = f.read()
        for n, (a, b, what) in enumerate(plants):
            check('plant in the source: ' + what, source.count(a) == 1)
            rc, out = build_and_run(test, '_plant%d' % n, source.replace(a, b))
            check('%s plant: %s: caught' % (test, what), rc not in (0, None), (out or '')[-300:])


# ---- the whole program on a made-up ROM, built the way the real one is
CODE = 2 << 16              # the made-up program's place in its ROM
HINT = CODE + 0x100
VINT = CODE + 0x200
OTHER = CODE + 0xF00


PAD_RAM = (0xFF << 16) + 0x14     # where the made-up program with pads=True keeps port 1's two reads of each frame
PORT1_DATA = 0xA10003             # the console's I/O chip: port 1's data and control registers
PORT1_CTRL = 0xA10009


def made_up_rom(page=False, pads=False):
    """A small program of ours: sets up the video chip, takes an H-int every 16 lines and a V-int each frame, counts them in work RAM, and writes a colour in the H-int. With page=True
    it first writes a second small program into the cart RAM, with reset vectors, for a restart to run from there: that one reads the cart RAM and counts in work RAM.
    With pads=True its V-int also reads the pad in port 1, with TH low (Start, A, Up, Down) and with TH high (C, B, the four directions), into work RAM at
    PAD_RAM: a key or a button that reaches the game then changes the machine, so a test can see whether one does."""
    size = 0x800000
    rom = bytearray(size)
    for v in range(1, 64):
        rom[4 * v:4 * v + 4] = struct.pack('>I', OTHER)
    rom[0:4] = struct.pack('>I', STACK)
    rom[4:8] = struct.pack('>I', CODE)
    rom[4 * 28:4 * 29] = struct.pack('>I', HINT)
    rom[4 * 30:4 * 31] = struct.pack('>I', VINT)
    ctrl_hi, ctrl_lo = 0x00C0, 0x0004
    data_hi, data_lo = 0x00C0, 0x0000
    code = [0x46FC, 0x2700]
    if page:
        words = {0x0000: STACK >> 16, 0x0002: STACK & 0xFFFF, 0x0004: 0x0000, 0x0006: 0x0200,
                 0x0200: 0x46FC, 0x0202: 0x2700,                    # move #$2700,sr
                 0x0204: 0x3039, 0x0206: 0x0000, 0x0208: 0x0300,    # move.w $300,d0
                 0x020A: 0x5279, 0x020C: 0x00FF, 0x020E: 0x0020,    # addq.w #1, a counter in work RAM
                 0x0210: 0x60F2}                                    # bra back
        for a, w in sorted(words.items()):
            code += [0x33FC, w, 0x0000, a]
    for value in (0x8004, 0x8174, 0x8C81, 0x8F02, 0x8A0F, 0x8014):
        code += [0x33FC, value, ctrl_hi, ctrl_lo]
    code += [0x46FC, 0x2300]
    spin = CODE + 2 * len(code)
    code += [0x3039, 0x00FF, 0x0010]                    # move.w a counter in work RAM,d0
    disp = spin - (CODE + 2 * len(code) + 2)
    code += [0x6000 | (disp & 0xFF)]                    # bra.s spin
    rom[CODE:CODE + 2 * len(code)] = b''.join(struct.pack('>H', w) for w in code)
    hint = [0x5279, 0x00FF, 0x0012,                     # addq.w #1, a counter in work RAM
            0x33FC, 0xC002, ctrl_hi, ctrl_lo, 0x33FC, 0x0000, ctrl_hi, ctrl_lo,    # CRAM 1
            0x33FC, 0x0123, data_hi, data_lo,
            0x4E73]
    rom[HINT:HINT + 2 * len(hint)] = b''.join(struct.pack('>H', w) for w in hint)
    vint = [0x5279, 0x00FF, 0x0010]                     # addq.w #1, a counter in work RAM
    if pads:
        def move_b_imm(value, address):                 # move.b #value, an absolute long address
            return [0x13FC, value, address >> 16, address & 0xFFFF]

        def move_b_abs(source, dest):                   # move.b from one absolute long address to another
            return [0x13F9, source >> 16, source & 0xFFFF, dest >> 16, dest & 0xFFFF]
        vint += (move_b_imm(0x40, PORT1_CTRL)            # TH an output
                 + move_b_imm(0x00, PORT1_DATA)          # TH low: Start, A, Up, Down
                 + move_b_abs(PORT1_DATA, PAD_RAM)
                 + move_b_imm(0x40, PORT1_DATA)          # TH high: C, B and the four directions
                 + move_b_abs(PORT1_DATA, PAD_RAM + 1))
    vint += [0x4E73]                                    # rte
    rom[VINT:VINT + 2 * len(vint)] = b''.join(struct.pack('>H', w) for w in vint)
    rom[OTHER:OTHER + 2] = struct.pack('>H', 0x4E73)
    return bytes(rom)


def _frames(run_dir):
    with open(os.path.join(run_dir, 'frames.txt'), encoding='ascii') as f:
        rows = []
        for line in f:
            w = line.split()
            row = {'frame': int(w[0])}
            i = 1
            while i + 1 < len(w):
                if w[i] in ('dma', 'io', 'oth'):
                    row[w[i]] = (int(w[i + 1]), int(w[i + 2]))
                    i += 3
                elif w[i] == 'vr':
                    row['vr'] = (int(w[i + 1]), int(w[i + 2]), int(w[i + 3]))
                    i += 4
                elif w[i].startswith('w') and w[i][1:].isdigit():
                    row[w[i]] = (int(w[i + 1]), int(w[i + 2]))
                    i += 3
                else:
                    row[w[i]] = w[i + 1]
                    i += 2
            rows.append(row)
    return rows


def _hold_with_no_sharing(path):
    """Opens the file as a backup or a cloud client can: no other program can read it until the handle closes."""
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateFileW.restype = ctypes.c_void_p
    kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    handle = kernel.CreateFileW(path, 0xC0000000, 0, None, 3, 0x80, None)
    if handle == ctypes.c_void_p(-1).value:
        raise OSError('could not hold ' + path)
    return lambda: kernel.CloseHandle(handle)


def _test_renderer_lines(exe, first, wd, cfg, keys, env, check):
    """What the console and the freeze log say about the renderer and the sound, on the window's path on SDL's dummy driver, which has only the software renderer."""
    def words(folder):
        with open(os.path.join(folder, 'stalls.txt'), encoding='ascii', errors='replace') as f:
            return f.read().splitlines()
    lines = words(wd)
    check('the console says each renderer that would not start, in order', all(('renderer %s could not start' % n) in first.stdout for n in ('opengl', 'direct3d', 'direct3d11', 'direct3d12')) and first.stdout.index('renderer opengl could') < first.stdout.index('renderer direct3d could'), first.stdout[:600])
    check('the first line of stalls.txt names the renderer in use', lines[0].startswith('==== run started') and ', renderer software, a gap over 100 ms' in lines[0], lines[0])
    check('stalls.txt has a note for each one that would not start', sum(1 for x in lines if 'could not start' in x) == 4, str(lines[:10]))
    check('stalls.txt names the audio device and its rate', any(x.startswith('audio: SDL driver dummy, opened at 48000 Hz, 2 channels') for x in lines), str(lines[:10]))
    check('stalls.txt counts the sound\'s gaps, just before the closing line', lines[-2].startswith('sound: ') and ' gaps, ' in lines[-2] and ' run ended at frame' in lines[-1], str(lines[-3:]))
    # A renderer named in the configuration goes first, and a name that is not one stops at its line. Nothing is shown on a hidden window.
    with open(cfg, encoding='ascii') as f:
        text = f.read()
    other = os.path.join(wd, 'renderer')
    os.makedirs(other, exist_ok=True)
    named = os.path.join(other, 'named.cfg')
    with open(named, 'w', encoding='ascii') as f:
        f.write(text + 'renderer=direct3d12\n')
    r = subprocess.run([exe, '--hidden-window', '--config', named, '--run-dir', other, '--key-script', keys, '--render'], capture_output=True, text=True, env=env, timeout=120)
    check('a renderer the configuration names goes first', r.returncode == 0 and r.stdout.index('renderer direct3d12 could') < r.stdout.index('renderer opengl could'), r.stdout[:600])
    with open(named, 'w', encoding='ascii') as f:
        f.write(text + 'renderer=vulkan\n')
    r = subprocess.run([exe, '--hidden-window', '--config', named, '--run-dir', other, '--key-script', keys, '--render'], capture_output=True, text=True, env=env, timeout=120)
    check('a renderer that is not one stops at its line of the configuration', r.returncode == 2 and 'renderer=vulkan' in r.stdout, r.stdout[-300:])
    with open(named, 'w', encoding='ascii') as f:
        f.write(text)
    # A real audio device has no way to fail here on purpose, so these say what any failed open does: one console line, a line in the log, and the game goes on.
    quiet = os.path.join(wd, 'no_sound')
    os.makedirs(quiet, exist_ok=True)
    r = subprocess.run([exe, '--hidden-window', '--config', named, '--run-dir', quiet, '--key-script', keys, '--render', '--sound-device', 'fail'], capture_output=True, text=True, env=env, timeout=120)
    lines = words(quiet)
    check('a device that will not open: the run passes', r.returncode == 0 and 'RESULT: PASS' in r.stdout and r.stdout.count('no sound:') == 1, r.stdout[-300:])
    check('and stalls.txt says so, with no gaps counted', any(x.startswith('audio: no device: ') for x in lines) and not any(x.startswith('sound: ') for x in lines), str(lines[:8]))
    # SDL's own audio failing to start, on the path a player's run takes: not a hidden window, on the dummy video driver, where nothing can be shown. Only a run that fails makes a message, and that is held back on this driver.
    nosdl = os.path.join(wd, 'no_sdl_audio')
    os.makedirs(nosdl, exist_ok=True)
    env2 = dict(env, SDL_AUDIODRIVER='no_such_driver')
    r = subprocess.run([exe, '--config', named, '--key-script', keys, '--stall-log', os.path.join(nosdl, 'stalls.txt')], capture_output=True, text=True, env=env2, timeout=120)
    lines = words(nosdl)
    check('SDL\'s audio will not start on a player\'s path: the game goes on, and says so once', r.returncode == 0 and 'RESULT: PASS' in r.stdout and r.stdout.count('no sound:') == 1 and any(x.startswith('audio: no device: ') for x in lines), r.stdout[-300:] + str(lines[:6]))


def made_up_four_copies(seed=1, flip=None):
    """A made-up save with a copy in each of the cart's four places, in the file's byte order; flip: a byte of the firmware's order to change one bit of."""
    fw = bytearray(8192)
    for c, (number, at, n) in enumerate(((4, 0x000, 0x200), (1, 0x400, 0x100), (2, 0x600, 0x100), (3, 0x800, 0x100))):
        for i in range(n - 8):
            fw[at + i] = (i * 13 + (seed + c) * 7 + 1) & 0xFF
        fw[at + n - 8:at + n - 4] = (2 + seed if c == 0 else 2).to_bytes(4, 'little')
        fw[at + n - 4:at + n] = bytes((number, 0, 0xC2, 0x3F))
        x = 0
        for b in fw[at:at + n]:
            x ^= b
        fw[at + n - 3] = x
    if flip is not None:
        fw[flip] ^= 0x01
    return bytes(fw[i ^ 1] for i in range(8192))


def made_up_cart_save(seed=1):
    """A made-up save in the cart's own format, in the file's byte order: two copies of made-up bytes, each ending with a sequence number, the save's number, the byte that makes the copy's XOR 0,
    and 0xC2 0x3F. pc_save.c checks a file for such a copy before it may become the backup."""
    fw = bytearray(8192)

    def put(at, n, number, sequence, s):
        for i in range(n - 8):
            fw[at + i] = (i * 13 + s * 7 + 1) & 0xFF
        fw[at + n - 8:at + n - 4] = sequence.to_bytes(4, 'little')
        fw[at + n - 4:at + n] = bytes((number, 0, 0xC2, 0x3F))
        x = 0
        for b in fw[at:at + n]:
            x ^= b
        fw[at + n - 3] = x
    put(0, 0x200, 4, 2 + seed, seed)
    put(0x400, 0x100, 1, 2, seed + 1)
    return bytes(fw[i ^ 1] for i in range(8192))


def _save_scenes(exe, rom_path, folder, env):
    """A player's save during play, on a hidden window on SDL's dummy driver, with a byte of the cart's save changed by the test's switch. Returns what each scene found, by name."""
    good = made_up_cart_save()
    other = bytes((i * 11 + 5) & 0xFF for i in range(8192))     # made-up bytes with no copy in the cart's format in them
    found = {}

    def scene(name, bytes_at_start, prepare=None, during=None, change=(70,), quit_at=80, again=False):
        """One run. again: the next run in the same folder, on the files the last one left, with bytes_at_start, if given, as the save the game wrote in that last run."""
        d = os.path.join(folder, name)
        if not again:
            shutil.rmtree(d, ignore_errors=True)
            os.makedirs(d)
        save = os.path.join(d, 'play.sav')
        cfg = os.path.join(d, 'play.cfg')
        keys = os.path.join(d, 'keys.txt')
        with open(cfg, 'w', encoding='ascii') as f:
            f.write('rom=%s\nsave=%s\nvsync=off\n' % (rom_path.replace('\\', '/'), save.replace('\\', '/')))
        with open(keys, 'w', encoding='ascii') as f:
            f.write('%d down Escape\n' % quit_at)
        if bytes_at_start is not None:
            with open(save, 'wb') as f:
                f.write(bytes_at_start)
        if prepare:
            prepare(save)
        args = [exe, '--hidden-window', '--config', cfg, '--key-script', keys, '--stall-log', os.path.join(d, 'stalls.txt')]
        for frame in change or ():
            args += ['--test-save-change', str(frame)]
        with open(os.path.join(d, 'out.txt'), 'w') as out:
            t0 = time.monotonic()
            p = subprocess.Popen(args, stdout=out, stderr=subprocess.STDOUT, env=env)
            if during:
                during(save)
            rc = p.wait(timeout=120)
            took = time.monotonic() - t0
        with open(os.path.join(d, 'out.txt'), errors='replace') as f:
            words = f.read()

        def contents(suffix=''):
            try:
                with open(save + suffix, 'rb') as f:
                    return f.read()
            except OSError:
                return None
        for leaving in ('', '.bak'):
            if os.path.isfile(save + leaving):
                os.chmod(save + leaving, stat.S_IWRITE | stat.S_IREAD)
        return {'rc': rc, 'words': words, 'save': contents(), 'bak': contents('.bak'), 'kept': contents('.bak.kept'), 'tmp': contents('.tmp'), 'apart': contents('.failed_check'), 'took': took,
                'name': save.replace('\\', '/')}

    def read_only(save):
        os.chmod(save, stat.S_IREAD)

    def lets_go_when_the_temp_appears(save):
        for _ in range(4000):
            if os.path.exists(save + '.tmp'):
                break
            time.sleep(0.005)
        time.sleep(0.8)     # the temporary file shows before the first try has failed: the program's tries at quit are 0.5 s apart
        os.chmod(save, stat.S_IWRITE | stat.S_IREAD)

    def folder_for_the_copy(save):
        os.makedirs(save + '.bak')

    def folder_for_the_temp(save):
        os.makedirs(save + '.tmp')

    def an_earlier_backup(save):
        with open(save + '.bak', 'wb') as f:
            f.write(made_up_cart_save(7))

    r = scene('first', good)
    found['copy'] = r['rc'] == 0 and r['bak'] == good and r['tmp'] is None and r['apart'] is None and r['save'] != good and sum(a != b for a, b in zip(r['save'], good)) == 1 and 'could not write' not in r['words']
    found['the console says it checks good'] = 'it checks good' in r['words']
    r = scene('not_a_save', other, prepare=an_earlier_backup)
    found['not a save: the backup stays and is kept, the file is kept apart'] = (r['rc'] == 0 and r['bak'] == made_up_cart_save(7) and r['kept'] == made_up_cart_save(7) and r['apart'] == other and r['save'] != other
                                                                               and 'does not check as a save' in r['words'] and 'keeps .bak as it is now as .bak.kept' in r['words'])
    # The made-up ROM's firmware has no list of saves, so the game here writes no copy of its own: the next run's file stands in for the good save the game would have written.
    r = scene('not_a_save', made_up_cart_save(9), again=True)
    found['not a save, the next run: its save is the backup now, and the one before is still kept'] = (r['rc'] == 0 and r['bak'] == made_up_cart_save(9) and r['kept'] == made_up_cart_save(7)
                                                                                                       and 'it checks good' in r['words'])
    r = scene('blank', bytes(8192), prepare=an_earlier_backup)
    found['blank: nothing kept apart, the backup stays and is kept'] = r['rc'] == 0 and r['bak'] == made_up_cart_save(7) and r['kept'] == made_up_cart_save(7) and r['apart'] is None and r['save'] != bytes(8192)
    r = scene('missing', None, prepare=an_earlier_backup)
    found['no save but a backup: the backup stays and is kept'] = r['rc'] == 0 and r['bak'] == made_up_cart_save(7) and r['kept'] == made_up_cart_save(7) and 'no save ' in r['words']
    r = scene('missing', made_up_cart_save(9), again=True)
    found['no save, the next run: its save is the backup now, and the one before is still kept'] = r['rc'] == 0 and r['bak'] == made_up_cart_save(9) and r['kept'] == made_up_cart_save(7)
    r = scene('missing', made_up_cart_save(10), again=True)
    found['no save, a run later still: the backup moves on, the kept one stays'] = r['rc'] == 0 and r['bak'] == made_up_cart_save(10) and r['kept'] == made_up_cart_save(7)
    damaged = made_up_four_copies(3, flip=0x11)
    r = scene('one_bit', damaged, prepare=an_earlier_backup)
    found['one bit flipped in the first of four copies: refused, kept apart, the backup stays and is kept'] = (r['rc'] == 0 and 'does not check as a save' in r['words'] and r['apart'] == damaged
                                                                                                              and r['bak'] == made_up_cart_save(7) and r['kept'] == made_up_cart_save(7))
    r = scene('four', made_up_four_copies(3))
    found['four whole copies check good and become the backup'] = r['rc'] == 0 and 'it checks good' in r['words'] and r['bak'] == made_up_four_copies(3) and r['kept'] is None
    cut = made_up_four_copies(3)[:4096]
    r = scene('cut', cut, prepare=an_earlier_backup)
    found['a save cut to 4 KB: refused and said why, kept apart at its length, the backup stays and is kept'] = (r['rc'] == 0 and 'does not check as a save' in r['words']
                                                                                                              and 'an 8 KB save cut short' in r['words'] and r['apart'] == cut
                                                                                                              and r['bak'] == made_up_cart_save(7) and r['kept'] == made_up_cart_save(7))
    wiped = bytearray(made_up_four_copies(3))
    wiped[0x600:0x700] = bytes(0x100)
    wiped = bytes(wiped)

    def whole_backup(save):
        with open(save + '.bak', 'wb') as f:
            f.write(made_up_four_copies(3))
    r = scene('wiped', wiped, prepare=whole_backup)
    found['a save whose place is wiped but whole in .bak: refused and said why, kept apart, the backup stays and is kept'] = (
        r['rc'] == 0 and "save 2's place is blank in it but whole in .bak" in r['words'] and r['apart'] == wiped and r['bak'] == made_up_four_copies(3) and r['kept'] == made_up_four_copies(3))
    r = scene('goes_back', good, prepare=read_only, change=(70, 140), quit_at=200)
    found['goes back: a failed write that the save then undoes leaves no failure and no temporary file'] = (r['rc'] == 0 and r['words'].count('could not write the save') == 1 and 'The save could not be written' not in r['words']
                                                                                                        and r['tmp'] is None and r['save'] == good and r['bak'] == good and r['took'] < 6.0)
    r = scene('nochange', good, change=False)
    found['no change, no copy'] = r['rc'] == 0 and r['bak'] is None and r['save'] == good
    r = scene('nofile', None)
    found['no file, no copy'] = r['rc'] == 0 and r['bak'] is None and r['save'] is not None and r['tmp'] is None
    r = scene('stuck', good, prepare=read_only)
    found['stuck: named at quit'] = r['rc'] == 0 and ('The save could not be written to ' + r['name']) in r['words'] and (r['name'] + '.tmp') in r['words'] and r['save'] == good and r['tmp'] is not None and r['tmp'] != good and r['bak'] == good
    found['stuck: it kept trying'] = r['took'] >= 2.0 and r['words'].count('could not write the save') == 1
    r = scene('freed', good, prepare=read_only, during=lets_go_when_the_temp_appears)
    found['freed: a retry gets through'] = r['rc'] == 0 and 'The save could not be written' not in r['words'] and r['tmp'] is None and r['save'] != good and r['bak'] == good
    r = scene('nocopy', good, prepare=folder_for_the_copy)
    found['no copy: not replaced, and named'] = (r['rc'] == 0 and ('a copy of %s could not be written to %s.bak first' % (r['name'], r['name'])) in r['words'] and r['save'] == good and r['tmp'] is not None
                                                 and r['tmp'] != good)
    r = scene('notemp', good, prepare=folder_for_the_temp)
    found['no temp: says the newest did not reach the disk'] = r['rc'] == 0 and 'did not reach the disk' in r['words'] and r['save'] == good
    return found


def _test_save_in_play(exe, rom_path, folder, env, sdl, check):
    found = _save_scenes(exe, rom_path, folder, env)
    for name, ok in found.items():
        check('the save in play, %s' % name, ok)
    # Planted bugs in the run loop's half: each must lose its scene.
    with open(exe + '.rsp', encoding='utf-8') as f:
        objects = [line.strip().strip('"') for line in f if line.strip()]
    with open(os.path.join(PC, 'pc_main.c'), newline='') as f:
        text = f.read()
    plants = (
        ('pc_save_replace(save_store_path, now)', 'pc_save_store(save_store_path, now)', 'copy', 'no copy of the save as loaded'),
        ('    save_at_quit();\n', '    save_check();\n', 'freed: a retry gets through', 'one try at quit'),
        ('    if (save_failed)\n        save_failed_message();\n', '', 'stuck: named at quit', 'a failed save at quit says nothing'),
        ('        if (save_failed) {\n            save_failed = 0;\n            pc_save_forget_temp(save_store_path);\n        }\n', '',
         'goes back: a failed write that the save then undoes leaves no failure and no temporary file', 'a failure stays set when the save goes back to what the file holds'),
    )
    for n, (a, b, scene, what) in enumerate(plants):
        check('plant in pc_main.c: ' + what, text.count(a) == 1)
        where = os.path.join(folder, 'plant%d' % n)
        os.makedirs(where, exist_ok=True)
        src = os.path.join(where, 'pc_main.c')
        with open(src, 'w', newline='') as f:
            f.write(text.replace(a, b))
        job = pc_build.machine_jobs(where, '-O1', ('pc_main',))[0]
        obj = pc_build.compile_jobs([(src, os.path.join(where, 'pc_main.o'), job[2], [])], 1)[0]
        planted = os.path.join(where, 'pc_port.exe')
        pc_build.link([obj if os.path.basename(o) == 'pc_main.o' else o for o in objects], planted, pc_build.sdl_link_args(sdl) + ['-lwinmm'])
        shutil.copy2(pc_build.sdl_dll(sdl), os.path.join(where, 'SDL2.dll'))
        check('plant in pc_main.c: %s: caught' % what, not _save_scenes(planted, rom_path, os.path.join(where, 'runs'), env)[scene])


def _test_controls_in_program(exe, rom_path, wc, rows, env, cfg, check, pause_too=False):
    """The controls screen in a player's run, on a hidden window on SDL's dummy driver: F4 opens it over the paused game, nine presses of Down reach
    player 1's A, Enter and Space set it, Escape closes it and saves, and the game goes on to its own Escape. The machine runs as with no screen, and
    the configuration keeps the player's own lines.

    With pause_too, P goes down in the same pass as F4, as two keys pressed within a frame of each other do, and everything must come out the same. An
    earlier build let that P end the pause the screen starts: the game ran on behind a screen that was never shown, which still took the keys and saved
    them in the player's file."""
    os.makedirs(wc, exist_ok=True)
    tag = ', with P in the same pass as F4' if pause_too else ''
    play_cfg = os.path.join(wc, 'play.cfg')
    own = 'rom=%s\nsave=%s\nvsync=off\n' % (rom_path.replace('\\', '/'), os.path.join(wc, 'play.sav').replace('\\', '/'))
    with open(play_cfg, 'w', encoding='ascii', newline='') as f:
        f.write(own)
    keys = os.path.join(wc, 'keys.txt')
    # The key script's clock counts each pass of a pause too: F4 at frame 3 opens the screen, and the game goes on at frame 4 once Escape at 27 closes it.
    script = ['3 down P', '3 down F4', '4 up P', '4 up F4'] if pause_too else ['3 down F4', '4 up F4']
    for i in range(9):
        script += ['%d down Down' % (5 + 2 * i), '%d up Down' % (6 + 2 * i)]
    script += ['23 down Return', '24 up Return', '25 down Space', '26 up Space', '27 down Escape', '28 up Escape', '40 down Escape']
    with open(keys, 'w', encoding='ascii') as f:
        f.write('\n'.join(script) + '\n')
    r = subprocess.run([exe, '--hidden-window', '--config', play_cfg, '--run-dir', wc, '--key-script', keys, '--render', '--pause-shot', '26'], capture_output=True,
                       text=True, env=env, timeout=120)
    check('a run with the controls screen in it ends by its Escape key' + tag, r.returncode == 0 and 'RESULT: PASS' in r.stdout, r.stdout[-600:])
    check('the console names the controls key and the controller database' + tag, 'F4 opens the controls screen' in r.stdout and
          ('controller database ' in r.stdout if cfg.get('controller_db') else 'no controller database' in r.stdout), r.stdout[-900:])
    screen_rows = _frames(wc)
    check('the screen pauses the game: 24 passes with no frame, and the last Escape comes at frame 16' + tag, len(screen_rows) == 17, str(len(screen_rows)))
    check('the controls screen changes nothing in the machine' + tag, [x['ram'] for x in screen_rows] == [x['ram'] for x in rows[:17]])
    with open(play_cfg, encoding='ascii', newline='') as f:
        text = f.read()
    check('the screen saved Space as player 1\'s A, after the player\'s own lines, kept as they were' + tag,
          text.startswith(own + '\n# ---- Controls.') and '\nkey_p1_a=Space\n' in text and text.count('key_p1_a=') == 1, text[:300])
    shot = os.path.join(wc, 'pause_000026.ppm')
    ok = os.path.exists(shot)
    if ok:
        with open(shot, 'rb') as f:
            head = f.read(20).split(b'\n')
        ok = head[0] == b'P6' and head[1] == b'896 700'
    check('a picture of the window with the screen open, at the configuration\'s size and the menu bar\'s strip' + tag, ok)


# The menu bar's items that do what a key does: the key, the presses after the menu key that reach the item, the action's name on the console. The menus
# are Game, Video, Sound, Controls, Scum!, Console, Help, in that order; Video holds the six filters, Next filter, Full screen, the two shapes, the sprite limits.
MENU_KEYS = (
    ('P', ['Return'], 'pause'),
    ('F1', ['Right'] + ['Down'] * 13 + ['Return'], 'limits'),
    ('F2', ['Right'] + ['Down'] * 12 + ['Return'], 'shape'),
    ('F3', ['Right', 'Right', 'Return'], 'mute'),
    ('F4', ['Right'] * 3 + ['Return'], 'controls'),
    ('F6', ['Right'] + ['Down'] * 6 + ['Return'], 'filter'),
    ('F11', ['Right'] + ['Down'] * 7 + ['Return'], 'fullscreen'),
    ('Escape', ['Up', 'Return'], 'quit'),
)


def _menu_script(path, presses, start=3, end=40):
    """A key script: F10 at start, then each press two script ticks apart (a pause's passes count), then Escape at end."""
    lines, t = ['%d down F10' % start, '%d up F10' % (start + 1)], start + 2
    for k in presses:
        lines += ['%d down %s' % (t, k), '%d up %s' % (t + 1, k)]
        t += 2
    if end:
        lines += ['%d down Escape' % end, '%d up Escape' % (end + 1)]
    with open(path, 'w', encoding='ascii') as f:
        f.write('\n'.join(lines) + '\n')


def _actions(stdout):
    return [line[len('actions: '):] for line in stdout.splitlines() if line.startswith('actions: ')]


def _menu_items(stdout):
    """--menu-dump's lines: {(menu, label): (key, command, arg, look)}."""
    items = {}
    for line in stdout.splitlines():
        if line.startswith('menu '):
            w = [x.strip() for x in line[5:].split('|')]
            if len(w) == 5:
                items[(w[0], w[1])] = (w[2], w[3], w[4])
    return items


def _test_menu_in_program(exe, rom_path, wm, rows, env, sdl, check):
    """The menu bar in a player's run, on a hidden window on SDL's dummy driver. The made-up program reads player 1's pad every frame, so a key that
    reached the game would change its machine."""
    os.makedirs(wm, exist_ok=True)
    own = 'rom=%s\nsave=%s\nvsync=off\n' % (rom_path.replace('\\', '/'), os.path.join(wm, 'play.sav').replace('\\', '/'))

    def play(name, presses, cfg_extra='', end=40, extra=()):
        d = os.path.join(wm, name)
        os.makedirs(d, exist_ok=True)
        cfg = os.path.join(d, 'play.cfg')
        with open(cfg, 'w', encoding='ascii', newline='') as f:
            f.write(own + cfg_extra)
        keys = os.path.join(d, 'keys.txt')
        if presses is None:
            with open(keys, 'w', encoding='ascii') as f:
                f.write('%d down Escape\n%d up Escape\n' % (end, end + 1))
        else:
            _menu_script(keys, presses, end=end)
        r = subprocess.run([exe, '--hidden-window', '--config', cfg, '--run-dir', d, '--key-script', keys, '--render', '--menu-dump'] + list(extra),
                           capture_output=True, text=True, env=env, timeout=180)
        with open(cfg, encoding='ascii', newline='') as f:
            return d, r, f.read()

    # The menu names each key, as the controls have it; a key moved in the file is the one it names.
    d, r, text = play('names', None, end=12)
    items = _menu_items(r.stdout)
    want = {('Game', 'Pause'): 'P', ('Game', 'Quit'): 'Escape', ('Video', 'Next filter'): 'F6', ('Video', 'Full screen'): 'F11',
            ('Video', 'Next aspect ratio'): 'F2', ('Video', 'The cart\'s sprite limits'): 'F1', ('Sound', 'Mute'): 'F3', ('Controls', 'Controls screen'): 'F4',
            ('Controls', 'Menu bar'): 'F10'}
    check('the menu bar names the key of each item that has one', r.returncode == 0 and all(items.get(k, ('',))[0] == v for k, v in want.items()),
          str({k: items.get(k) for k in want}))
    check('the menu bar holds the Scum! cheats, the 25 arcade stages and Off, the console lines, and Soft Reset',
          ('Scum!', 'Health: no damage') in items and ('Scum!', 'Pills: a Blu pill is not spent') in items and ('Scum!', 'Lives: a death is not spent') in items
          and sum(1 for k in items if k[0] == 'Arcade stage (list)') == 26 and ('Arcade stage (list)', 'Arena') in items
          and ('Console', 'Console: Mega Drive') in items and ('Console', 'Mega CD: None') in items and ('Console', '32X') in items
          and ('Game', 'Soft Reset') in items, str(sorted(items))[:600])
    check('the Console menu\'s two lists are the Pocket core\'s, its names in its order, Mega Drive and None ticked, and no Region item and no Europe',
          [k[1] for k in items if k[0] == 'Consoles (list)'] == ['Mega Drive', 'Genesis', 'Mega Drive 2', 'Genesis Model 2', 'Genesis 3', 'Nomad', 'Mega Jet',
                                                                  'TeraDrive', 'Unknown']
          and [k[1] for k in items if k[0] == 'Mega CD units (list)'] == ['None', 'Mega-CD / Sega CD', 'Mega-CD 2 / Sega CD 2', 'Wondermega',
                                                                          'Wondermega M2 / X\'Eye', 'LaserActive', 'Aiwa CSD-GM1', 'CDX', 'Unknown CD']
          and 'tick' in items.get(('Consoles (list)', 'Mega Drive'), ('', '', ''))[2] and 'tick' in items.get(('Mega CD units (list)', 'None'), ('', '', ''))[2]
          and not any(k[1].startswith('Region') or 'Europe' in k[1] for k in items), str([k for k in items if k[0].startswith(('Console', 'Mega CD'))]))
    check('CPU MATE is grey while the Mega CD is off', 'grey' in items.get(('Console', 'CPU MATE, with the Mega CD'), ('', '', ''))[2])
    check('a run that only opens no menu changes nothing in the machine', [x['ram'] for x in _frames(d)] == [x['ram'] for x in rows[:len(_frames(d))]])
    d, r, text = play('names_moved', None, cfg_extra='key_mute=M\nkey_menu=F9\nmegacd=on\n', end=12)
    items = _menu_items(r.stdout)
    check('a key moved in the file is the key the menu names', items.get(('Sound', 'Mute'), ('',))[0] == 'M' and items.get(('Controls', 'Menu bar'), ('',))[0] == 'F9',
          str(items.get(('Sound', 'Mute'))))
    check('CPU MATE can be chosen once the Mega CD is on', 'grey' not in items.get(('Console', 'CPU MATE, with the Mega CD'), ('', '', 'grey'))[2])
    pc_console_scenes.menu_rules(play, _menu_items, check)
    check('a file with the menu bar\'s lines starts', r.returncode == 0 and 'RESULT: PASS' in r.stdout, r.stdout[-300:])

    # Each item that has a key does what its key does: the same action, and the machine as without either.
    for key, presses, name in MENU_KEYS:
        kd = os.path.join(wm, 'key_%s' % name)
        os.makedirs(kd, exist_ok=True)
        with open(os.path.join(kd, 'play.cfg'), 'w', encoding='ascii', newline='') as f:
            f.write(own)
        with open(os.path.join(kd, 'keys.txt'), 'w', encoding='ascii') as f:
            f.write('3 down %s\n4 up %s\n%s' % (key, key, '' if key == 'Escape' else '30 down Escape\n31 up Escape\n'))
        # --frames bounds the runs whose Escape only closes something (the controls screen) or comes while paused.
        r1 = subprocess.run([exe, '--hidden-window', '--config', os.path.join(kd, 'play.cfg'), '--run-dir', kd, '--key-script', os.path.join(kd, 'keys.txt'),
                             '--render', '--frames', '40'], capture_output=True, text=True, env=env, timeout=180)
        d2, r2, _ = play('menu_%s' % name, presses, end=0 if key == 'Escape' else 60, extra=('--frames', '40'))
        a1, a2 = _actions(r1.stdout), _actions(r2.stdout)
        check('the menu item for %s gives the action its key gives (%s)' % (key, name), r1.returncode == 0 and r2.returncode == 0 and name in a1 and a2[:1] == ['menu']
              and name in a2[1:2], '%s / %s' % (a1, a2))
        m1, m2, plain = [x['ram'] for x in _frames(kd)], [x['ram'] for x in _frames(d2)], [x['ram'] for x in rows]
        n = min(len(m1), len(m2), len(plain))
        check('the menu item for %s changes nothing in the machine but what its key changes' % key, n >= min(4, len(plain)) and m1[:n] == m2[:n] == plain[:n],
              '%d / %d / %d rows' % (len(m1), len(m2), len(plain)))

    # The mouse: a click on the title Sound opens it (the bar at scale 2: Game, Video, then Sound from x = 164), a click on its first row chooses Mute.
    d = os.path.join(wm, 'mouse')
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, 'play.cfg'), 'w', encoding='ascii', newline='') as f:
        f.write(own)
    with open(os.path.join(d, 'keys.txt'), 'w', encoding='ascii') as f:
        f.write('3 move 300,300\n4 move 200,10\n5 click 200,10\n7 move 200,48\n9 click 200,48\n30 down Escape\n31 up Escape\n')
    r = subprocess.run([exe, '--hidden-window', '--config', os.path.join(d, 'play.cfg'), '--run-dir', d, '--key-script', os.path.join(d, 'keys.txt'), '--render',
                        '--frames', '40'], capture_output=True, text=True, env=env, timeout=180)
    check('the mouse: a click on Sound and one on its first row give the mute action, and the game goes on', r.returncode == 0 and _actions(r.stdout)[:1] == ['mute']
          and 'muted' in r.stdout, '%s %s' % (_actions(r.stdout), r.stdout[-300:]))

    # Settings: each goes into the player's file in its own place, the player's lines kept.
    d, r, text = play('cheat', ['Right'] * 4 + ['Return'], cfg_extra='# my cheats\ncheats=pills\nscale=3\n')
    check('Scum! Health, chosen: the file\'s cheats line holds health and pills, in its place, the player\'s lines kept',
          r.returncode == 0 and text == own + '# my cheats\ncheats=health,pills\nscale=3\n', text[-200:])
    check('the menu shows the cheats ticked', 'tick' in _menu_items(r.stdout).get(('Scum!', 'Health: no damage'), ('', '', ''))[2])
    d, r, text = play('stage', ['Right'] * 4 + ['Down'] * 6 + ['Right'] + ['Down'] * 6 + ['Return'])
    check('Scum! Arcade stage, the sixth stage chosen from its list: arcade_stage=6 is added', text == own + 'arcade_stage=6\narcade_mode=off\n', text[-200:])
    check('the menu names the stage chosen and ticks it in the list', ('Scum!', 'Arcade stage: Police HQ') in _menu_items(r.stdout)
          and 'tick' in _menu_items(r.stdout).get(('Arcade stage (list)', 'Police HQ'), ('', '', ''))[2])
    pc_console_scenes.menu_lists(play, own, check)
    pc_arcade_scenes.menu(play, own, _menu_items, check)
    pc_arcade_scenes.menu_plants(exe, sdl, wm, own, env, _menu_script, _menu_items, pc_build, check)
    d, r, text = play('sixbutton', ['Right'] * 4 + ['Down'] * 9 + ['Return'])
    check('Scum! 6-Button pad, player 2: pad2=three is added, as the controls screen writes it', text == own + 'pad2=three\n', text[-200:])
    d, r, text = play('volume', ['Right', 'Right', 'Down', 'Down', 'Down', 'Return'])
    check('Sound, Volume 50 %: volume=50 is added, and the output takes it', text == own + 'volume=50\n' and 'volume 50' in r.stdout, text[-200:])

    # A settings file the program cannot write. A Soft Reset, like the next start, reads the file again, so the window must never call a change kept
    # that is not in it. A read-only file (another program holding it takes the same path): Console, Region: USA, which the game takes only when it
    # starts, is told as not saved; Sound, Volume 50 %, which acts at once, as lasting only until a Soft Reset or the end of the program; then Game,
    # Soft Reset says the restart has the file's settings, not the changes. The file stays as it was.
    d = os.path.join(wm, 'read_only')
    os.makedirs(d, exist_ok=True)
    cfg = os.path.join(d, 'play.cfg')
    with open(cfg, 'w', encoding='ascii', newline='') as f:
        f.write(own)
    os.chmod(cfg, stat.S_IREAD)
    keys = os.path.join(d, 'keys.txt')
    _menu_script(keys, ['Right'] * 6 + ['Down', 'Return'] + ['F10', 'Right', 'Right', 'Down', 'Down', 'Down', 'Return'] + ['F10', 'Down', 'Return'], end=0)
    try:
        r = subprocess.run([exe, '--hidden-window', '--config', cfg, '--run-dir', d, '--key-script', keys, '--render', '--frames', '60'], capture_output=True,
                           text=True, env=env, timeout=240)
        with open(cfg, encoding='ascii', newline='') as f:
            text = f.read()
    finally:
        os.chmod(cfg, stat.S_IREAD | stat.S_IWRITE)
    told = [x for x in r.stdout.splitlines() if x.startswith('menu: ')]
    check('a read-only settings file: Console Genesis is told as not saved, so a Soft Reset goes without it', 'menu: Console: Genesis was not saved: play.cfg could '
          'not be replaced, so a Soft Reset or the next start goes without it.' in told and not any(x.startswith('menu: Console: Genesis, from') for x in told),
          str(told))
    check('a read-only settings file: Volume 50 % acts, and is told as lasting only until a Soft Reset or the end of the program',
          'menu: Volume 50 %. Not saved: play.cfg could not be replaced, so this lasts only until a Soft Reset or the end of the program.' in told
          and 'volume 50' in r.stdout, str(told))
    check('a read-only settings file: Soft Reset says the restart has the file\'s settings, without the changes it could not take', 'menu: Soft Reset: the '
          'game starts again from power-on, with your save and the settings in play.cfg; the changes that could not be saved there are not among '
          'them.' in told and r.returncode == 0 and r.stdout.count('RESULT: PASS') == 2, str(told) + r.stdout[-300:])
    check('a read-only settings file: the file is as it was, and no temporary file is left beside it', text == own and not os.path.exists(cfg + '.tmp'),
          text[-200:])
    # A file of 32 KB or more is more than the program rewrites: the words say that, not that it could not be read. The game reads it and plays.
    big = ''.join('# a line of a long comment, %05d %s\n' % (i, 'x' * 60) for i in range(400))
    d, r, text = play('too_large', ['Right'] * 6 + ['Down', 'Return'], cfg_extra=big)
    check('a settings file of 32 KB or more: Console Genesis is told as not saved, because the file is that large', r.returncode == 0 and text == own + big
          and 'menu: Console: Genesis was not saved: play.cfg is 32 KB or larger, more than the program rewrites, so a Soft Reset or the next start goes '
          'without it.' in r.stdout, r.stdout[-400:])

    # A player's key that chooses an item, or closes a page, is the menu's alone: it reaches the game only once it has been let go and pressed again.
    # Enter chooses an item and is player 1's Start; any key closes a page. Each scene holds its key for 10 script ticks after the menu or page has
    # closed, and its machine must be the plain run's, frame for frame.
    plain = [x['ram'] for x in rows]

    def keys_run(name, lines, program=exe):
        kd = os.path.join(wm, name)
        os.makedirs(kd, exist_ok=True)
        kcfg = os.path.join(kd, 'play.cfg')
        with open(kcfg, 'w', encoding='ascii', newline='') as f:
            f.write(own)
        kkeys = os.path.join(kd, 'keys.txt')
        with open(kkeys, 'w', encoding='ascii') as f:
            f.write('\n'.join(lines) + '\n')
        kr = subprocess.run([program, '--hidden-window', '--config', kcfg, '--run-dir', kd, '--key-script', kkeys, '--render', '--frames', '20'],
                            capture_output=True, text=True, env=env, timeout=180)
        with open(kcfg, encoding='ascii', newline='') as f:
            return [x['ram'] for x in _frames(kd)], kr, f.read()

    def first_difference(ram):
        return next((i for i in range(min(len(ram), len(plain))) if ram[i] != plain[i]), None)

    ram, r, text = keys_run('enter_in_play', ['5 down Return', '15 up Return'])
    check('the control: Enter pressed in play reaches this program, whose machine then differs from the plain run', r.returncode == 0 and len(ram) == 20
          and (first_difference(ram) or 0) >= 5, '%s rows, first difference %s' % (len(ram), first_difference(ram)))
    # Sound, Volume 75 % (F10, Right twice, Down twice), chosen with Enter held from tick 13 to 23.
    held_enter = ['3 down F10', '4 up F10', '5 down Right', '6 up Right', '7 down Right', '8 up Right', '9 down Down', '10 up Down', '11 down Down', '12 up Down',
                  '13 down Return', '23 up Return']
    # Help (F10, Left), Keys chosen with Enter; the page closed by player 1's Up, held from tick 9 to 19.
    held_up = ['3 down F10', '4 up F10', '5 down Left', '6 up Left', '7 down Return', '8 up Return', '9 down Up', '19 up Up']
    ram, r, text = keys_run('held_enter', held_enter)
    check('an item chosen with Enter (player 1\'s Start) held for 10 ticks: it is chosen, and the game never sees the key', r.returncode == 0
          and text == own + 'volume=75\n' and len(ram) == 20 and first_difference(ram) is None, '%s rows, first difference %s, %s' % (len(ram), first_difference(ram), text[-80:]))
    ram, r, text = keys_run('held_up', held_up)
    check('the Keys page closed with player 1\'s Up held for 10 ticks: the game never sees the key', r.returncode == 0 and 'menu: the Keys page' in r.stdout
          and len(ram) == 20 and first_difference(ram) is None, '%s rows, first difference %s, %s' % (len(ram), first_difference(ram), r.stdout[-300:]))
    # A planted bug: pc_input.c with nothing holding a player's keys back when a menu or a page closes. Both scenes must then see the key reach the game.
    with open(os.path.join(PC, 'pc_input.c'), newline='') as f:
        source = f.read()
    hold = '    if ((menu || menu_open_last_poll) && !pc_menu_is_open())\n        for (p = 0; p < PC_PLAYERS; p++)\n            held_back[p] = 1;\n'
    check('plant in pc_input.c: nothing holds a player\'s keys back when a menu or a page closes', source.count(hold) == 1)
    with open(exe + '.rsp', encoding='utf-8') as f:
        objects = [line.strip().strip('"') for line in f if line.strip()]
    if sdl and source.count(hold) == 1 and any(os.path.basename(o) == 'pc_input.o' for o in objects):
        where = os.path.join(wm, 'plant_input')
        os.makedirs(where, exist_ok=True)
        src = os.path.join(where, 'pc_input.c')
        with open(src, 'w', newline='') as f:
            f.write(source.replace(hold, ''))
        job = [j for j in pc_build.window_jobs(where, '-O1', sdl) if os.path.basename(j[0]) == 'pc_input.c'][0]
        obj = pc_build.compile_jobs([(src, os.path.join(where, 'pc_input.o'), job[2], [])], 1)[0]
        planted = os.path.join(where, 'pc_port.exe')
        pc_build.link([obj if os.path.basename(o) == 'pc_input.o' else o for o in objects], planted, pc_build.sdl_link_args(sdl) + ['-lwinmm'])
        shutil.copy2(pc_build.sdl_dll(sdl), os.path.join(where, 'SDL2.dll'))
        ram1, r1, _ = keys_run('plant_held_enter', held_enter, program=planted)
        ram2, r2, _ = keys_run('plant_held_up', held_up, program=planted)
        check('plant in pc_input.c: caught by both scenes', r1.returncode == 0 and r2.returncode == 0 and first_difference(ram1) is not None
              and first_difference(ram2) is not None, 'first differences %s / %s' % (first_difference(ram1), first_difference(ram2)))
    else:
        check('plant in pc_input.c: the program\'s link names pc_input.o, and SDL is there to link a planted one', False, str(objects[:5]))

    # Soft Reset: the run ends with the save, and the program starts again on its own, with the same command line but the test's keys. The run folder
    # is given with a backslash at its end, which the new command line must keep as the argument it is.
    d = os.path.join(wm, 'soft')
    os.makedirs(d, exist_ok=True)
    cfg = os.path.join(d, 'play.cfg')
    with open(cfg, 'w', encoding='ascii', newline='') as f:
        f.write(own)
    keys = os.path.join(d, 'keys.txt')
    _menu_script(keys, ['Down', 'Return'], end=0)
    r = subprocess.run([exe, '--hidden-window', '--config', cfg, '--run-dir', d + '\\', '--key-script', keys, '--render', '--frames', '30'], capture_output=True,
                       text=True, env=env, timeout=240)
    check('Game, Soft Reset: the run ends and the program starts again by itself, which plays its frames and ends too',
          r.returncode == 0 and 'Soft Reset: the program started again as process' in r.stdout and r.stdout.count('RESULT: PASS') == 2
          and r.stdout.count('Player 1:') == 2, r.stdout[-900:])
    again = [x['ram'] for x in _frames(d)]
    n = min(len(again), len(rows))
    check('after the Soft Reset the program ran from power-on: its frames are the plain run\'s', len(again) == 30 and again[:n] == [x['ram'] for x in rows[:n]],
          str(len(again)))


def _test_save_at_start(exe, rom_path, wd, keys, env, check):
    """A player's run, with a save the program cannot take. Only a file that does not exist may start a blank cart: anything else stops with a message, and the file is as it was."""
    good = bytes((i * 11 + 5) & 0xFF for i in range(8192))
    save = os.path.join(wd, 'start.sav')
    cfg = os.path.join(wd, 'start.cfg')
    with open(cfg, 'w', encoding='ascii') as f:
        f.write('rom=%s\nsave=%s\nvsync=off\n' % (rom_path.replace('\\', '/'), save.replace('\\', '/')))

    def play():
        return subprocess.run([exe, '--hidden-window', '--config', cfg, '--key-script', keys], capture_output=True, text=True, env=env, timeout=120)

    def contents():
        with open(save, 'rb') as f:
            return f.read()

    def refused(what, data, words, hold=False):
        with open(save, 'wb') as f:
            f.write(data)
        release = _hold_with_no_sharing(save) if hold else None
        try:
            r = play()
        finally:
            if release:
                release()
        check('a save %s stops the program with a message' % what, r.returncode == 2 and words in r.stdout and 'Nothing was changed' in r.stdout, r.stdout[-300:])
        check('and the file is as it was, with no temporary file beside it', contents() == data and not os.path.exists(save + '.tmp'))

    refused('held by another program', good, 'Cannot read the save file', hold=True)
    refused('that is empty', b'', 'not the size of a save')
    refused('cut short', good[:1000], 'not the size of a save')
    refused('that is too long', good + b'\0', 'not the size of a save')
    # Once the other program lets go, the same file starts the game and stays whole.
    with open(save, 'wb') as f:
        f.write(good)
    r = play()
    check('the good file starts the game', r.returncode == 0 and 'RESULT: PASS' in r.stdout, r.stdout[-300:])
    # The older layout's size loads too.
    with open(save, 'wb') as f:
        f.write(good[:4096])
    r = play()
    check('a save in the older layout starts the game', r.returncode == 0 and 'RESULT: PASS' in r.stdout, r.stdout[-300:])
    # No file at all: a new cart.
    os.remove(save)
    r = play()
    check('no file: a blank cart, and the game starts', r.returncode == 0 and 'RESULT: PASS' in r.stdout, r.stdout[-300:])
    if os.path.exists(save):
        os.remove(save)
    # A folder that is not there is not a new cart.
    with open(cfg, 'w', encoding='ascii') as f:
        f.write('rom=%s\nsave=%s\nvsync=off\n' % (rom_path.replace('\\', '/'), os.path.join(wd, 'no_such_folder', 'start.sav').replace('\\', '/')))
    r = play()
    check('a save in a folder that is gone stops the program', r.returncode == 2 and 'Cannot read the save file' in r.stdout, r.stdout[-300:])


def test_whole_program(tmp, config, check):
    with open(config, encoding='utf-8') as f:
        cfg = json.load(f)
    tree = cfg.get('fw_source')
    if not tree or not os.path.isdir(tree):
        print('skip the whole program test: the local configuration names no firmware tree')
        return
    folder = os.path.join(tmp, 'pc_whole')
    os.makedirs(folder, exist_ok=True)
    # This program reads the pads, so the runs through the window below show whether a key that a screen or a menu took also reached the game.
    rom = made_up_rom(pads=True)
    rom_path = os.path.join(folder, 'made_up.bin')
    with open(rom_path, 'wb') as f:
        f.write(rom)
    local = {'sound_cores': cfg.get('sound_cores'), 'sdl_dir': cfg.get('sdl_dir', ''), 'rom_path': rom_path, 'rom_sha256': hashlib.sha256(rom).hexdigest(), 'out_dir': folder, 'out_root': folder, 'fw_source': tree, 'bus_low_device_end': 65536, 'hints': {},
             'controller_db': cfg.get('controller_db', '')}
    local_path = os.path.join(folder, 'local.json')
    with open(local_path, 'w', encoding='utf-8') as f:
        json.dump(local, f)
    r = subprocess.run([sys.executable, '-I', '-B', os.path.join(REPO, 'ptool.py'), 'pc-build', '--config', local_path, '--jobs', str(JOBS), '--opt=-O1'], capture_output=True, text=True, env=harness.gcc_env())
    check('ptool pc-build, on a made-up ROM', r.returncode == 0, (r.stdout + r.stderr)[-800:])
    if r.returncode == 0 and cfg.get('sdl_dir') and cfg.get('controller_db'):
        db, licence = pc_build.controller_db_files(cfg['controller_db'])
        same = True
        for there, here in ((db, 'gamecontrollerdb.txt'), (licence, pc_build.CONTROLLER_DB_LICENCE)):
            with open(there, 'rb') as f1, open(os.path.join(folder, here), 'rb') as f2:
                same = same and f1.read() == f2.read()
        check('the build puts the controller database and its licence beside the program, unchanged', same)
    if r.returncode:
        return
    with open(os.path.join(folder, 'image_check.txt'), encoding='ascii') as f:
        image = f.read().strip()
    exe = os.path.join(folder, 'pc_port.exe')
    r = subprocess.run([sys.executable, '-I', '-B', os.path.join(HERE, 'check_pc_link.py'), '--exe', exe, '--rsp', exe + '.rsp', '--firmware', tree], capture_output=True, text=True, env=harness.gcc_env())
    check('what the finished program links: no reference machine, not the old module of the firmware', r.returncode == 0, r.stdout[-500:])

    def run(name, *args):
        rd = os.path.join(folder, name)
        os.makedirs(rd, exist_ok=True)
        return rd, subprocess.run([exe, '--rom', rom_path, '--run-dir', rd, '--image-check', image] + list(args), capture_output=True, text=True, env=harness.gcc_env())

    a, ra = run('a', '--frames', '20')
    check('the program runs on the machine, with no reference machine behind it', ra.returncode == 0 and 'RESULT: PASS' in ra.stdout, ra.stdout[-600:])
    rows = _frames(a)
    # Frame numbers run from 0. A full frame after the handlers are on has 14 H-ints (lines 0-224 by 16) and one V-int.
    check('one line a frame', [x['frame'] for x in rows] == list(range(20)))
    late = rows[5:]
    check('one V-int a frame, an H-int every 16 lines', all(int(x['vi']) == 1 and int(x['hi']) == 14 for x in late), str(late[0]))
    check('nothing reads the V counter in this program', all(x['vr'] == (-1, -1, 0) for x in late))
    check('its handler writes are logged in the frame', all(int(x['wr']) >= 14 for x in late[1:]), str(late[1]))
    b, rb = run('b', '--frames', '20')
    same = True
    for name in ('frames.txt', 'cart.txt'):
        with open(os.path.join(a, name), 'rb') as f1, open(os.path.join(b, name), 'rb') as f2:
            same = same and f1.read() == f2.read()
    check('the same run twice: the logs are byte-equal', same)
    _test_stall_log_in_program(folder, exe, rom_path, image, cfg.get('sdl_dir'), rows, check)
    # The sound's way out cannot move the game: every way gives these same logs, a device that will not open only means no sound, and the file comes out the same twice.
    wavs = (os.path.join(folder, 's_file.wav'), os.path.join(folder, 's_file2.wav'))
    sound_runs = [('s_file', '--sound-file', wavs[0]), ('s_file2', '--sound-file', wavs[1]), ('s_fail', '--sound-device', 'fail')]
    if cfg.get('sdl_dir'):
        sound_runs += [('s_null', '--sound-device', 'null'), ('s_stall', '--sound-device', 'stall'), ('s_slow', '--sound-device', 'slow'), ('s_dead', '--sound-device', 'dead')]
    for name, *args in sound_runs:
        d, rs = run(name, '--frames', '20', *args)
        same = rs.returncode == 0
        for log in ('frames.txt', 'cart.txt'):
            with open(os.path.join(a, log), 'rb') as f1, open(os.path.join(d, log), 'rb') as f2:
                same = same and f1.read() == f2.read()
        check('sound by way of %s: the logs are byte-equal with no sound at all' % name, same, rs.stdout[-300:])
    with open(wavs[0], 'rb') as f1, open(wavs[1], 'rb') as f2:
        one, two = f1.read(), f2.read()
    pairs = (len(one) - 44) // 4
    check('the sound file twice: byte-equal', one == two)
    check('the sound file: 48 kHz stereo, its sizes in its header, 801 samples a frame', one[:4] == b'RIFF' and struct.unpack('<I', one[24:28])[0] == 48000 and struct.unpack('<I', one[40:44])[0] == len(one) - 44 and abs(pairs - 20 * 48000 * 3420 * 262 // 53693175) < 900, str(pairs))
    for name, licence in pc_build.SOUND_CORE_LICENCES.items():
        with open(os.path.join(os.path.dirname(exe), licence), 'rb') as f1, open(os.path.join(cfg['sound_cores'][name], 'LICENSE'), 'rb') as f2:
            check('the build puts %s beside the program' % licence, f1.read() == f2.read())
    w, rw = subprocess.run([exe, '--rom', rom_path, '--run-dir', a, '--image-check', '0123456789ABCDEF', '--frames', '1'], capture_output=True, text=True, env=harness.gcc_env()), None
    check('a program built from another image refuses to run', w.returncode == 3 and 'RESULT: FAIL' in w.stdout, w.stdout[-300:])
    if cfg.get('sdl_dir'):
        # A player's run, through the window's path on SDL's dummy driver: the same frames, Escape ends it, and any other game file is refused with nothing written.
        env = harness.gcc_env()
        env['SDL_VIDEODRIVER'] = 'dummy'
        env['SDL_RENDER_DRIVER'] = 'software'
        env.update(NO_REAL_PADS)
        wd = os.path.join(folder, 'w')
        os.makedirs(wd, exist_ok=True)
        play_cfg = os.path.join(wd, 'play.cfg')
        with open(play_cfg, 'w', encoding='ascii') as f:
            f.write('rom=%s\nsave=%s\nvsync=off\n' % (rom_path.replace('\\', '/'), os.path.join(wd, 'play.sav').replace('\\', '/')))
        keys = os.path.join(wd, 'keys.txt')
        with open(keys, 'w', encoding='ascii') as f:
            f.write('11 down Escape\n')
        rw_ = subprocess.run([exe, '--hidden-window', '--config', play_cfg, '--run-dir', wd, '--key-script', keys, '--render'], capture_output=True, text=True, env=env)
        check('a run through the window\'s path, ended by its Escape key', rw_.returncode == 0 and 'RESULT: PASS' in rw_.stdout and 'Player 1:' in rw_.stdout and 'hidden' in rw_.stdout, rw_.stdout[-600:])
        _test_renderer_lines(exe, rw_, wd, play_cfg, keys, env, check)
        window_rows = _frames(wd)
        check('Escape at frame 11 ends the run there', len(window_rows) == 12, str(len(window_rows)))
        check('the window\'s path runs the same machine as a headless run', [x['ram'] for x in window_rows] == [x['ram'] for x in rows[:12]])
        # The sound's settings and its mute key, by the configuration's own name for it: the machine does not move for them.
        wm = os.path.join(folder, 'wm')
        os.makedirs(wm, exist_ok=True)
        mute_cfg = os.path.join(wm, 'mute.cfg')
        with open(mute_cfg, 'w', encoding='ascii') as f:
            f.write('rom=%s\nsave=%s\nvsync=off\nvolume=40\nkey_mute=M\n' % (rom_path.replace('\\', '/'), os.path.join(wm, 'play.sav').replace('\\', '/')))
        keys3 = os.path.join(wm, 'keys.txt')
        with open(keys3, 'w', encoding='ascii') as f:
            f.write('4 down M\n4 up M\n11 down Escape\n')
        rm_ = subprocess.run([exe, '--hidden-window', '--config', mute_cfg, '--run-dir', wm, '--key-script', keys3, '--render'], capture_output=True, text=True, env=env, timeout=120)
        check('the mute key and the volume reach the output', rm_.returncode == 0 and 'volume 40, muted' in rm_.stdout, rm_.stdout[-600:])
        check('the mute key and the volume change nothing in the machine', [x['ram'] for x in _frames(wm)] == [x['ram'] for x in window_rows])
        # Pause, the sprite limit key while paused, and the end of the pause: the machine does not move for any of it.
        wp = os.path.join(folder, 'wp')
        os.makedirs(wp, exist_ok=True)
        keys2 = os.path.join(wp, 'keys.txt')
        with open(keys2, 'w', encoding='ascii') as f:
            f.write('3 down P\n3 up P\n8 down F1\n8 up F1\n9 down F1\n9 up F1\n12 down P\n12 up P\n15 down Escape\n')
        rp = subprocess.run([exe, '--hidden-window', '--config', play_cfg, '--run-dir', wp, '--key-script', keys2, '--render'], capture_output=True, text=True, env=env, timeout=120)
        check('a run with a pause in it ends by its Escape key', rp.returncode == 0 and 'RESULT: PASS' in rp.stdout, rp.stdout[-400:])
        paused_rows = _frames(wp)
        check('the pause stops the game: its script ticks pass with no frame, and Escape comes at frame 6', len(paused_rows) == 7, str(len(paused_rows)))
        check('the pause and the sprite limit key change nothing in the machine or the picture', [(x['ram'], x['pic']) for x in paused_rows] == [(x['ram'], x['pic']) for x in window_rows[:7]])
        _test_filters_in_program(exe, rom_path, os.path.join(folder, 'filters'), window_rows, check)
        other = os.path.join(wd, 'other.bin')
        with open(other, 'wb') as f:
            f.write(rom[:-1] + bytes([rom[-1] ^ 1]))
        other_cfg = os.path.join(wd, 'other.cfg')
        other_save = os.path.join(wd, 'other.sav')
        with open(other_cfg, 'w', encoding='ascii') as f:
            f.write('rom=%s\nsave=%s\n' % (other.replace('\\', '/'), other_save.replace('\\', '/')))
        ro = subprocess.run([exe, '--hidden-window', '--config', other_cfg], capture_output=True, text=True, env=env)
        check('any other game file is refused', ro.returncode == 3 and 'not the game file' in ro.stdout, ro.stdout[-300:])
        check('and nothing is written', not os.path.exists(other_save))
        _test_controls_in_program(exe, rom_path, os.path.join(folder, 'wc'), rows, env, cfg, check)
        _test_controls_in_program(exe, rom_path, os.path.join(folder, 'wc2'), rows, env, cfg, check, pause_too=True)
        _test_menu_in_program(exe, rom_path, os.path.join(folder, 'wmenu'), rows, env, cfg.get('sdl_dir'), check)
        pc_brain_places.test_brain_words_in_program(exe, rom_path, os.path.join(folder, 'wbrain'), env, cfg.get('sdl_dir'), check, _menu_script,
                                                    _hold_with_no_sharing)
        _test_save_at_start(exe, rom_path, wd, keys, env, check)
        _test_save_in_play(exe, rom_path, os.path.join(folder, 'sp'), env, cfg.get('sdl_dir'), check)
    # The frame file, read back with the shared header's layout: the head, the state, the writes.
    dump = os.path.join(folder, 'frames.bin')
    c, rc = run('c', '--frames', '12', '--dump-frames', dump, '--dump-range', '10-10')
    with open(dump, 'rb') as f:
        head = struct.unpack('<4I', f.read(16))
    check('a frame file holds the frame asked for', head[1] == 10 and head[2] >= 14 and head[3] == 0, str(head))
    # A restart with the low page as translated goes back to translated code.
    d, rd = run('d', '--frames', '12', '--test-restart', '5')
    check('a restart with the low page unchanged goes back to translated code', rd.returncode == 0 and 'fallback: 0 entries' in rd.stdout, rd.stdout[-400:])
    # A restart on a page in the cart RAM: its vectors, its code, in the fallback, never back to the translated low page.
    folder2 = os.path.join(tmp, 'pc_whole2')
    os.makedirs(folder2, exist_ok=True)
    rom = made_up_rom(page=True)
    rom_path2 = os.path.join(folder2, 'made_up.bin')
    with open(rom_path2, 'wb') as f:
        f.write(rom)
    local = dict(local, rom_path=rom_path2, rom_sha256=hashlib.sha256(rom).hexdigest(), out_dir=folder2, out_root=folder2)
    local_path = os.path.join(folder2, 'local.json')
    with open(local_path, 'w', encoding='utf-8') as f:
        json.dump(local, f)
    r = subprocess.run([sys.executable, '-I', '-B', os.path.join(REPO, 'ptool.py'), 'pc-build', '--config', local_path, '--jobs', str(JOBS), '--opt=-O1'], capture_output=True, text=True, env=harness.gcc_env())
    check('ptool pc-build, on the second made-up ROM', r.returncode == 0, (r.stdout + r.stderr)[-800:])
    if r.returncode:
        return
    with open(os.path.join(folder2, 'image_check.txt'), encoding='ascii') as f:
        image2 = f.read().strip()
    e = os.path.join(folder2, 'e')
    os.makedirs(e, exist_ok=True)
    re_ = subprocess.run([os.path.join(folder2, 'pc_port.exe'), '--rom', rom_path2, '--run-dir', e, '--image-check', image2, '--frames', '20', '--test-restart', '9'], capture_output=True, text=True, env=harness.gcc_env())
    words = re_.stdout
    check('a restart on a page in the cart RAM runs it in the fallback', re_.returncode == 0 and 'low page from cart RAM 1' in words and 'stopped 0' in words and 'fallback: 1 entries' in words, words[-500:])
    rows = _frames(e)
    after = rows[11:]
    check('that page reads the cart RAM and counts in work RAM on every frame', all(int(x['cr']) > 1000 for x in after) and len({x['ram'] for x in after}) == len(after), str(after[0]))
    check('no interrupt runs after the restart: the reset\'s mask is on', all(int(x['hi']) == 0 for x in after) or all(x['vi'] == '1' for x in after))


# ---- the console's lines in the whole program, on a made-up ROM: a small program of ours reads what the game reads to know its console, starts a Mega
# CD as the game does and sends one update, and the program's logs show what it found. With a made-up brain of ours given by --test-brain.
# The made-up program's own work RAM: what it stores (the version register, the video chip's PAL bit, the 32X's words, two Mega CD words), the latest
# word from $A12024, and $A12020 with its busy flag in bit 15.
CONSOLE_RAM = 0xFF0040              # the made-up program's own, not a game address
CONSOLE_WORD = 0xFF0050             # not a game address
CONSOLE_BUSY = 0xFF0052             # not a game address
MADE_UP_D_WORD, MADE_UP_D_BUSY = 400000, 900000     # the made-up brain's times in the sub-CPU's cycles, about 1.9 and 4.3 frames; not a game address
MCD_BIOS, MCD_PRG = 0x400000, 0x420000              # the Mega CD's windows
GAME_CURSOR = 0x9920                # the work RAM word the CPU MATE link copies for the brain as the game's terrain cursor (targets/pc/pc_addons.c)


def console_rom():
    a = asm68.Asm(CODE)
    a.move_to_sr(asm68.imm(0x2700))
    for value in (0x8004, 0x8174, 0x8F02):
        a.move(2, asm68.imm(value), asm68.absl(0xC00004))
    a.move(2, asm68.absl(0xA10000), asm68.absl(CONSOLE_RAM))            # the version register, both bytes
    a.move(2, asm68.absl(0xC00004), asm68.d(0))
    a.immop('andi', 2, 1, asm68.d(0))
    a.move(2, asm68.d(0), asm68.absl(CONSOLE_RAM + 2))                  # the video chip's PAL bit
    a.move(4, asm68.absl(0xA130EC), asm68.absl(CONSOLE_RAM + 4))        # the 32X's "MARS", as one long
    a.move(2, asm68.absl(MCD_BIOS + 0x128), asm68.absl(CONSOLE_RAM + 8))    # the Mega CD's "BO"
    a.move(2, asm68.absl(MCD_PRG + 0x290), asm68.absl(CONSOLE_RAM + 10))    # its program memory's "MB"
    # TMSS's check as the game makes it: the word at $000120, again with $A14101 bit 0 clear, and again with it set
    a.move(2, asm68.absl(0x000120), asm68.absl(CONSOLE_RAM + 12))
    a.move(1, asm68.imm(0), asm68.absl(0xA14101))
    a.move(2, asm68.absl(0x000120), asm68.absl(CONSOLE_RAM + 14))
    a.move(1, asm68.imm(1), asm68.absl(0xA14101))
    a.move(2, asm68.absl(0x000120), asm68.absl(CONSOLE_RAM + 0x20))
    # the TeraDrive's video memory test as the game makes it: the 128 KB mode, $5A02 at $10000, $A501 at $0000, $10000 read back, the mode off
    a.move(2, asm68.imm(0x8184), asm68.absl(0xC00004))
    a.move(4, asm68.imm(0x40000004), asm68.absl(0xC00004))
    a.move(2, asm68.imm(0x5A02), asm68.absl(0xC00000))
    a.move(4, asm68.imm(0x40000000), asm68.absl(0xC00004))
    a.move(2, asm68.imm(0xA501), asm68.absl(0xC00000))
    a.move(4, asm68.imm(0x00000004), asm68.absl(0xC00004))
    a.move(2, asm68.absl(0xC00000), asm68.absl(CONSOLE_RAM + 0x22))
    a.move(2, asm68.imm(0x8174), asm68.absl(0xC00004))
    # the Mega CD's start as the game makes it: bus request, reset, a program word while held, the release; a word memory word and the cursor
    for v in (3, 2, 0):
        a.move(1, asm68.imm(v), asm68.absl(0xA12001))
    a.move(1, asm68.imm(0), asm68.absl(0xA12003))
    a.move(2, asm68.imm(0x0700), asm68.absl(MCD_PRG + 0x5F4))
    a.move(1, asm68.imm(1), asm68.absl(0xA12001))
    a.move(2, asm68.imm(0x0011), asm68.absl(0x600000))
    a.move(2, asm68.imm(0x0002), asm68.absl(0xFF0000 + GAME_CURSOR))
    # one update: bit 7 of the echo turned over, low bits 2
    a.move(1, asm68.absl(0xA12021), asm68.d(0))
    a.immop('eori', 1, 0x80, asm68.d(0))
    a.immop('andi', 1, 0x80, asm68.d(0))
    a.addq(1, 2, asm68.d(0))
    a.move(1, asm68.d(0), asm68.absl(0xA12011))
    a.move_to_sr(asm68.imm(0x2300))
    a.label('loop')
    a.move(2, asm68.absl(0xA12024), asm68.absl(CONSOLE_WORD))
    a.move(2, asm68.absl(0xA12020), asm68.absl(CONSOLE_BUSY))
    a.branch('bra', 'loop')
    a.label('vint')
    a.addq(2, 1, asm68.absl(0xFF0010))                     # the made-up program's frame count, not a game address
    a.rte()
    return asm68.image(a, stack=STACK, vectors={30: a.labels['vint']}, size=0x800000)


def _rv_word(kind, *f):
    """RV32 words for the made-up brain, encoded as the RISC-V unprivileged specification gives them."""
    if kind == 'lui':
        rd, v = f
        return (v & 0xFFFFF000) | rd << 7 | 0x37
    if kind == 'addi':
        rd, rs1, imm = f
        return (imm & 0xFFF) << 20 | rs1 << 15 | rd << 7 | 0x13
    if kind == 'lhu':
        rd, rs1, imm = f
        return (imm & 0xFFF) << 20 | rs1 << 15 | 5 << 12 | rd << 7 | 0x03
    if kind == 'sw':
        rs2, rs1, imm = f
        return ((imm >> 5) & 0x7F) << 25 | rs2 << 20 | rs1 << 15 | 2 << 12 | (imm & 0x1F) << 7 | 0x23
    if kind == 'add':
        rd, rs1, rs2 = f
        return rs2 << 20 | rs1 << 15 | rd << 7 | 0x33
    raise ValueError(kind)


def made_up_brain(folder):
    """A brain of ours in the firmware's three files: H2 puts the two times into the counter's output; H1 gives the word memory's first word plus the
    cursor. No byte of any real brain."""
    def li(rd, v):
        return [_rv_word('lui', rd, v + 0x800), _rv_word('addi', rd, rd, ((v & 0xFFF) ^ 0x800) - 0x800)]
    h2 = [_rv_word('lui', 7, 0x80002000)] + li(28, MADE_UP_D_WORD) + [_rv_word('sw', 28, 7, 0x234)] + li(28, MADE_UP_D_BUSY) + \
        [_rv_word('sw', 28, 7, 0x238), 0x00008067]
    h1 = [_rv_word('lui', 5, 0x08020000), _rv_word('lhu', 10, 5, 0), _rv_word('lui', 6, 0x08012000), _rv_word('lhu', 7, 6, 0), _rv_word('add', 10, 10, 7), 0x00008067]
    code = bytearray(0xD2EC - 0x5360)
    for at, words in ((0x859C, h2), (0x8504, h1)):
        code[at - 0x5360:at - 0x5360 + 4 * len(words)] = struct.pack('<%dI' % len(words), *words)
    os.makedirs(folder, exist_ok=True)
    for name, data in (('brain.bin', bytes(code)), ('cf.bin', bytes(5328)), ('rodata.bin', bytes(1336))):
        with open(os.path.join(folder, name), 'wb') as f:
            f.write(data)


def test_console_program(tmp, config, check):
    with open(config, encoding='utf-8') as f:
        cfg = json.load(f)
    tree = cfg.get('fw_source')
    if not tree or not os.path.isdir(tree):
        print('skip the console program test: the local configuration names no firmware tree')
        return
    folder = os.path.join(tmp, 'pc_console')
    os.makedirs(folder, exist_ok=True)
    rom = console_rom()
    rom_path = os.path.join(folder, 'made_up.bin')
    with open(rom_path, 'wb') as f:
        f.write(rom)
    local = {'sound_cores': cfg.get('sound_cores'), 'sdl_dir': '', 'rom_path': rom_path, 'rom_sha256': hashlib.sha256(rom).hexdigest(), 'out_dir': folder, 'out_root': folder,
             'fw_source': tree, 'bus_low_device_end': 65536, 'hints': {}}
    local_path = os.path.join(folder, 'local.json')
    with open(local_path, 'w', encoding='utf-8') as f:
        json.dump(local, f)
    r = subprocess.run([sys.executable, '-I', '-B', os.path.join(REPO, 'ptool.py'), 'pc-build', '--config', local_path, '--jobs', str(JOBS), '--opt=-O1'], capture_output=True, text=True, env=harness.gcc_env())
    check('ptool pc-build, on the console\'s made-up ROM', r.returncode == 0, (r.stdout + r.stderr)[-800:])
    if r.returncode:
        return
    with open(os.path.join(folder, 'image_check.txt'), encoding='ascii') as f:
        image = f.read().strip()
    exe = os.path.join(folder, 'pc_port.exe')
    brain = os.path.join(folder, 'made_up_brain')
    made_up_brain(brain)
    words = ['--log-word', '%06X' % CONSOLE_RAM, '--log-word', '%06X' % (CONSOLE_RAM + 2), '--log-word', '%06X' % (CONSOLE_RAM + 4), '--log-word', '%06X' % (CONSOLE_RAM + 6),
             '--log-word', '%06X' % (CONSOLE_RAM + 8), '--log-word', '%06X' % (CONSOLE_RAM + 10), '--log-word', '%06X' % CONSOLE_WORD, '--log-word', '%06X' % CONSOLE_BUSY]

    def run(name, *args, logged=None):
        rd = os.path.join(folder, name)
        os.makedirs(rd, exist_ok=True)
        rr = subprocess.run([exe, '--rom', rom_path, '--run-dir', rd, '--image-check', image, '--frames', '12'] + (logged or words) + list(args), capture_output=True, text=True,
                            env=harness.gcc_env())
        rows = _frames(rd) if rr.returncode == 0 else []
        return rd, rr, rows

    def lw(row, k):
        v = row.get('lw%d' % k)
        return int(v) if v is not None else None

    cases = [
        ('default', (), 0x2020, 0xFFFF, 0xFFFF),
        ('us', ('--console', 'region=us'), 0xA0A0, 0xFFFF, 0xFFFF),
        ('genesis', ('--console', 'console=genesis'), 0xA0A0, 0xFFFF, 0xFFFF),
        ('genesis_jp', ('--console', 'console=genesis', '--console', 'region=jp'), 0x2020, 0xFFFF, 0xFFFF),
        ('x32', ('--console', 'x32=on'), 0x2020, 0x4D41, 0xFFFF),
        ('megacd', ('--console', 'megacd=on'), 0x0000, 0xFFFF, 0x424F),
        ('megacd_us', ('--console', 'megacd=on', '--console', 'region=us'), 0x8080, 0xFFFF, 0x424F),
        ('nomad', ('--console', 'console=nomad'), 0xA1A1, 0xFFFF, 0xFFFF),
        ('megadrive2_megacd2', ('--console', 'console=megadrive2', '--console', 'megacd=megacd2'), 0x0101, 0xFFFF, 0x424F),
        ('genesis_laseractive', ('--console', 'console=genesis', '--console', 'megacd=laseractive'), 0x8181, 0xFFFF, 0x424F),
        ('aiwa', ('--console', 'megacd=aiwa'), 0x0101, 0xFFFF, 0x0000),
        ('genesis3_megacd', ('--console', 'console=genesis3', '--console', 'megacd=megacd'), 0xA1A1, 0xFFFF, 0xFFFF),     # no expansion port: no Mega CD
        ('unknown_unknowncd', ('--console', 'console=unknown', '--console', 'megacd=unknown'), 0x0000, 0xFFFF, 0x0000),
    ]
    base = None
    for name, args, version, x32, cd in cases:
        rd, rr, rows = run(name, *args)
        ok = rr.returncode == 0 and len(rows) == 12
        last = rows[-1] if ok else {}
        check('the console\'s made-up program, %s: version register %04X, 32X %04X, Mega CD %04X, PAL bit 0' % (name, version, x32, cd),
              ok and lw(last, 0) == version and lw(last, 1) == 0 and lw(last, 2) == x32 and lw(last, 4) == cd, rr.stdout[-300:] if not ok else str(last))
        if name == 'default':
            base = rd
    # every line at its default value: the same logs as no line at all
    rd, rr, rows = run('defaults', '--console', 'region=auto', '--console', 'console=auto', '--console', 'x32=off', '--console', 'megacd=off', '--console', 'cpumate=off')
    same = rr.returncode == 0
    for log in ('frames.txt', 'cart.txt'):
        with open(os.path.join(base, log), 'rb') as f1, open(os.path.join(rd, log), 'rb') as f2:
            same = same and f1.read() == f2.read()
    check('every console line at its default: the logs are byte-equal with no line at all', same)
    rd, rr, rows = run('defaults_named', '--console', 'console=megadrive', '--console', 'megacd=none', '--console', 'x32=off', '--console', 'cpumate=off')
    same = rr.returncode == 0
    for log in ('frames.txt', 'cart.txt'):
        with open(os.path.join(base, log), 'rb') as f1, open(os.path.join(rd, log), 'rb') as f2:
            same = same and f1.read() == f2.read()
    check('the defaults by their names in the lists (console=megadrive, megacd=none): the logs are byte-equal with no line at all', same)
    pc_console_scenes.program_checks(run, lw, CONSOLE_RAM, check)
    rr = subprocess.run([exe, '--rom', rom_path, '--run-dir', rd, '--image-check', image, '--frames', '1', '--console', 'region=mars'], capture_output=True, text=True, env=harness.gcc_env())
    check('a console line with a value it does not know is refused', rr.returncode == 2, rr.stderr[-200:])
    # No Europe (the team's decision): region=eu is a value the program does not know, refused the same way.
    rr2 = subprocess.run([exe, '--rom', rom_path, '--run-dir', rd, '--image-check', image, '--frames', '1', '--console', 'region=eu'], capture_output=True, text=True, env=harness.gcc_env())
    check('region=eu is refused as region=mars is, in the same words', rr2.returncode == 2 and rr2.stderr.replace('region=eu', 'region=mars') == rr.stderr,
          rr2.stderr[-200:])
    # the made-up brain through the link: busy from the update, the word about 1.9 frames later, busy about 4.3 frames after the update
    rd, rr, rows = run('mate', '--console', 'megacd=on', '--console', 'cpumate=on', '--test-brain', brain)
    ok = rr.returncode == 0 and len(rows) == 12
    word_at = next((r['frame'] for r in rows if lw(r, 6) == 0x0013), None) if ok else None
    busy = [r['frame'] for r in rows if lw(r, 7) is not None and lw(r, 7) & 0x8000] if ok else []
    check('the made-up brain\'s word reaches $A12024 two or three frames after the update, and busy holds about four frames',
          ok and word_at is not None and busy and 1 <= word_at - busy[0] <= 3 and 3 <= len(busy) <= 5 and all(lw(r, 6) == 0x0013 for r in rows if r['frame'] >= word_at),
          'word at %s, busy on frames %s; %s' % (word_at, busy, rr.stdout[-300:]))
    check('the run says where its brain came from, and the link\'s totals', ok and 'brain calls 1, words 1' in rr.stdout, rr.stdout[-400:])
    rd, rr, rows = run('nomate', '--console', 'megacd=on', '--console', 'cpumate=off', '--test-brain', brain)
    ok = rr.returncode == 0 and len(rows) == 12
    check('CPU MATE off: the link gives the stub\'s answers, no busy and no word', ok and all(lw(r, 6) == 0 and not (lw(r, 7) & 0x8000) for r in rows), str(rows[-1] if rows else rr.stdout[-300:]))

ORDER_RAM = (0xFF << 16) + 0x20     # its counters in work RAM: rows written, rows lost, registers or flags changed, V-ints
ORDER_NOPS = 25


def order_rom():
    """A small program of ours, with the shape that lost rows of a picture. Each frame it waits for the vertical blank, waits a little more, then writes a row: a long block of its own, ending in
    the chip's two-word command, then the data words in a loop, whose head is a safe point. V-int rises inside that long block. Its handler writes a command and a register of its own, so if the
    handler ran between the program's command and its data, the row would land nowhere. The program reads the row back and counts. It also counts if a register or a flag moved across the write."""
    rom = bytearray(0x800000)
    for v in range(1, 64):
        rom[4 * v:4 * v + 4] = struct.pack('>I', OTHER)
    rom[0:4] = struct.pack('>I', STACK)
    rom[4:8] = struct.pack('>I', CODE)
    rom[4 * 30:4 * 31] = struct.pack('>I', VINT)
    rom[OTHER:OTHER + 2] = struct.pack('>H', 0x4E73)
    ctrl, data, status = asm68.absl(0xC00004), asm68.absl(0xC00000), asm68.absl(0xC00005)
    d, a, imm, ind, disp = asm68.d, asm68.a, asm68.imm, asm68.ind, asm68.disp
    vram = 0x2400
    command, read_back, kept = 0x40000000 | vram << 16, vram << 16, 0x12345678
    rows, lost, bad = 0, 2, 4
    p = asm68.Asm(CODE)
    p.words(0x46FC, 0x2700)                 # move #$2700,sr
    for value in (0x8004, 0x8174, 0x8C81, 0x8F02, 0x8AFF):
        p.move(2, imm(value), ctrl)
    p.lea(ctrl, 0)
    p.lea(data, 1)
    p.lea(status, 2)
    p.lea(asm68.absl(ORDER_RAM), 3)
    p.moveq(0, 7)
    p.move(4, imm(command), d(6))
    p.move(4, imm(read_back), d(5))
    p.move(4, imm(kept), d(4))
    p.words(0x46FC, 0x2300)                 # move #$2300,sr
    p.label('frame')
    p.addq(2, 1, d(7))
    p.label('leave_blank')
    p.bit('btst', imm(3), ind(2))
    p.branch('ne', 'leave_blank')
    # A wait that changes from frame to frame, so the next loop meets the blank's start at each phase of its own time.
    p.move(2, d(7), d(0))
    p.immop('andi', 2, 7, d(0))
    p.label('vary')
    p.dbra(0, 'vary')
    p.label('enter_blank')
    p.bit('btst', imm(3), ind(2))
    p.branch('eq', 'enter_blank')
    for _ in range(ORDER_NOPS):
        p.nop()
    # A safe point, just ahead of the raise on some frames and just past it on others. The flag set here is read two blocks on, with the write between: only the context holds it there.
    p.move(2, d(7), d(1))
    p.arith('cmp', 2, d(7), 1)
    p.branch('bra', 'long_block')
    p.label('long_block')
    for _ in range(13):
        p.lea(disp(0, 0), 4)                # time, with no flag touched
    p.movem_to_mem(4, 1 << 6, ind(0))       # the command, d6: two words on the chip, no flag touched
    p.branch('bra', 'after_write')
    p.label('after_write')
    p.branch('eq', 'flag_kept')
    p.addq(2, 1, disp(3, bad))
    p.label('flag_kept')
    p.moveq(5, 3)
    p.label('row')
    p.move(2, d(7), ind(1))
    p.dbra(3, 'row')
    p.move(4, d(5), ind(0))
    p.move(2, ind(1), d(0))
    p.arith('cmp', 2, d(7), 0)
    p.branch('eq', 'landed')
    p.addq(2, 1, disp(3, lost))
    p.label('landed')
    p.arith('cmp', 4, imm(kept), 4)
    p.branch('eq', 'register_kept')
    p.addq(2, 1, disp(3, bad))
    p.label('register_kept')
    p.addq(2, 1, disp(3, rows))
    p.branch('bra', 'frame')
    code = p.bytes()
    rom[CODE:CODE + len(code)] = code
    h = asm68.Asm(VINT)
    h.movem_to_mem(4, 0xFFFE, asm68.pre(7))  # d0-d7/a0-a6
    h.move(2, imm(0xC002), ctrl)             # a colour, by a command of its own
    h.move(2, imm(0x0000), ctrl)
    h.move(2, imm(0x0123), data)
    for n in range(8):
        h.moveq(-1, n)                       # every register changed, and the flags: not zero
    for n in range(7):
        h.move(4, imm(0), a(n))
    h.moveq(-1, 0)
    h.move(2, imm(0x8A10), asm68.absl(0xC00004))     # register 10, last
    h.movem_to_regs(4, 0x7FFF, asm68.post(7))
    h.addq(2, 1, asm68.absl(ORDER_RAM + 6))
    h.rte()
    code = h.bytes()
    rom[VINT:VINT + len(code)] = code
    return bytes(rom)


def _order_counts(exe, rom_path, image, run_dir, frames='120'):
    os.makedirs(run_dir, exist_ok=True)
    words = []
    for n in range(4):
        words += ['--log-word', '%X' % (ORDER_RAM + 2 * n)]
    r = subprocess.run([exe, '--rom', rom_path, '--run-dir', run_dir, '--image-check', image, '--frames', frames] + words, capture_output=True, text=True, env=harness.gcc_env())
    rows = _frames(run_dir) if r.returncode == 0 else []
    last = rows[-1] if rows else {}
    return r, [int(last.get('lw%d' % n, -1)) for n in range(4)]


def test_interrupt_order(tmp, config, check):
    """The whole program, translated from a made-up ROM: a raised interrupt goes in front of a chip write inside a translated block, and the block goes on as if nothing had run."""
    with open(config, encoding='utf-8') as f:
        cfg = json.load(f)
    tree = cfg.get('fw_source')
    if not tree or not os.path.isdir(tree):
        print('skip the interrupt order test: the local configuration names no firmware tree')
        return
    folder = os.path.join(tmp, 'pc_order')
    os.makedirs(folder, exist_ok=True)
    rom = order_rom()
    rom_path = os.path.join(folder, 'made_up.bin')
    with open(rom_path, 'wb') as f:
        f.write(rom)
    local = {'sound_cores': cfg.get('sound_cores'), 'sdl_dir': '', 'rom_path': rom_path, 'rom_sha256': hashlib.sha256(rom).hexdigest(), 'out_dir': folder, 'out_root': folder, 'fw_source': tree, 'bus_low_device_end': 65536, 'hints': {}}
    local_path = os.path.join(folder, 'local.json')
    with open(local_path, 'w', encoding='utf-8') as f:
        json.dump(local, f)
    r = subprocess.run([sys.executable, '-I', '-B', os.path.join(REPO, 'ptool.py'), 'pc-build', '--config', local_path, '--jobs', str(JOBS), '--opt=-O1'], capture_output=True, text=True, env=harness.gcc_env())
    check('ptool pc-build, on the interrupt order ROM', r.returncode == 0, (r.stdout + r.stderr)[-800:])
    if r.returncode:
        return
    with open(os.path.join(folder, 'image_check.txt'), encoding='ascii') as f:
        image = f.read().strip()
    exe = os.path.join(folder, 'pc_port.exe')
    r, (rows, lost, bad, vints) = _order_counts(exe, rom_path, image, os.path.join(folder, 'run'))
    check('the row is written on every frame, and V-int runs on every frame', r.returncode == 0 and rows >= 110 and vints >= 110, '%s %s' % ((rows, lost, bad, vints), r.stdout[-300:]))
    check('no row is lost to a V-int that rose inside the block before its data', lost == 0, str((rows, lost, bad, vints)))
    check('no register or flag moved across the handler', bad == 0, str((rows, lost, bad, vints)))
    check('no handler returned somewhere else', 'handlers returning elsewhere 0' in r.stdout, r.stdout[-400:])

    # The rule broken, in the same program: rows are lost on some frames and land on others, so the scene really does span the raise.
    with open(os.path.join(PC, 'pc_bus.c'), newline='') as f:
        text = f.read()
    old = 'while (pt_irq_level > pc_ctx.imask && !pc_ctx.stopped) {'
    check('plant in pc_bus.c applies', text.count(old) == 1)
    pf = os.path.join(folder, 'plant')
    os.makedirs(pf, exist_ok=True)
    src = os.path.join(pf, 'pc_bus.c')
    with open(src, 'w', newline='') as f:
        f.write(text.replace(old, 'while (0) {'))
    job = pc_build.machine_jobs(pf, '-O1', ('pc_bus',))[0]
    obj = pc_build.compile_jobs([(src, os.path.join(pf, 'pc_bus.o'), job[2], [])], 1)[0]
    with open(exe + '.rsp', encoding='utf-8') as f:
        objects = [line.strip().strip('"') for line in f if line.strip()]
    objects = [obj if os.path.basename(o) == 'pc_bus.o' else o for o in objects]
    planted = os.path.join(pf, 'pc_port.exe')
    pc_build.link(objects, planted, ['-lwinmm'])
    r, (rows, lost, bad, vints) = _order_counts(planted, rom_path, image, os.path.join(pf, 'run'))
    check('plant in pc_bus.c, the write no longer takes what is due: rows are lost, on some frames only', r.returncode == 0 and 0 < lost < rows, '%s %s' % ((rows, lost, bad, vints), r.stdout[-300:]))
    shutil.rmtree(pf, ignore_errors=True)


def test_renderer(tmp, check):
    """The renderer alone on made-up scenes, with every planted fault in its own test."""
    folder = os.path.join(tmp, 'pr_render')
    os.makedirs(folder, exist_ok=True)
    exe = os.path.join(folder, 'pr_render_test.exe')
    sources = [os.path.join(HERE, 'pr_render_test.c')] + [os.path.join(PC, n + '.c') for n in pc_build.RENDERER]
    r = subprocess.run([pc_build.GCC, '-std=gnu11', '-O1', '-DPR_TEST_PLANTS', '-I', PC, '-o', exe] + sources, capture_output=True, text=True, env=harness.gcc_env())
    check('the renderer test builds', r.returncode == 0, r.stderr[:800])
    if r.returncode:
        return
    r = subprocess.run([exe], capture_output=True, text=True, env=harness.gcc_env(), cwd=folder)
    check('the renderer on made-up scenes, planted faults caught', r.returncode == 0, r.stdout[-800:])
    # The program's own renderer holds no planted fault.
    obj = pc_build.compile_jobs(pc_build.machine_jobs(os.path.join(folder, 'plain'), '-O1', pc_build.RENDERER), 2)
    nm = subprocess.run([os.path.join(os.path.dirname(pc_build.GCC), 'nm')] + obj, capture_output=True, text=True, env=harness.gcc_env())
    check("no planted fault in the program's objects", 'pr_plant' not in nm.stdout, nm.stdout[:300])


def test_config(tmp, check):
    """The player's configuration, the game file's hash, the save file and the vsync rule."""
    folder = os.path.join(tmp, 'pc_config')
    os.makedirs(folder, exist_ok=True)

    def build(name, save_source, header_folder, config_source=os.path.join(PC, 'pc_config.c')):
        exe = os.path.join(folder, name + '.exe')
        # A plant can leave a helper with no caller, so that one warning stays off.
        r = subprocess.run([pc_build.GCC, '-std=gnu11', '-O1', '-Wall', '-Werror', '-Wno-unused-function', '-Dfclose=test_fclose', '-Dfflush=test_fflush', '-I', header_folder, '-I', PC, '-o', exe,
                            os.path.join(HERE, 'pc_config_test.c'), config_source, save_source], capture_output=True, text=True, env=harness.gcc_env())
        return exe, r

    exe, r = build('pc_config_test', os.path.join(PC, 'pc_save.c'), PC)
    check('the configuration test builds, with no warnings', r.returncode == 0, r.stderr[:800])
    if r.returncode:
        return
    r = subprocess.run([exe], capture_output=True, text=True, env=harness.gcc_env(), cwd=folder)
    check('the configuration, the hash, the save file and the vsync rule', r.returncode == 0, r.stdout[-800:])
    # Planted bugs in the save file and in the vsync rule: each must make the test fail.
    plants = (
        ('pc_save.c', 'return no_such_file(path) ? PC_SAVE_NO_FILE : PC_SAVE_UNREADABLE;', 'return PC_SAVE_NO_FILE;', 'any failed open reads as no file'),
        ('pc_save.c', 'GetLastError() == ERROR_FILE_NOT_FOUND;', '(GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND);', 'a folder that is gone reads as no file'),
        ('pc_save.c', 'if (n != PC_SAVE_BYTES && n != PC_SAVE_BYTES_OLD)\n        return PC_SAVE_WRONG_SIZE;', 'if (n > PC_SAVE_BYTES)\n        return PC_SAVE_WRONG_SIZE;', 'a short file reads as a save'),
        ('pc_save.c', 'static uint8_t file[PC_SAVE_BYTES + 1u];', 'static uint8_t file[PC_SAVE_BYTES];', 'a file too long reads as a save'),
        ('pc_save.c', 'if (fclose(f) != 0) {', 'fclose(f);\n    if (0) {', 'a failed close still replaces the save'),
        ('pc_save.c', ' || flush_to_disk(f) != 0', '', 'a failed flush still replaces the save'),
        ('pc_save.c', '    if (!copy_kept) {\n', '    if (0) {\n', 'the file as loaded is not kept'),
        ('pc_save.c', 'if (file_check == PC_SAVE_GOOD && file_bytes) {', 'if (file_bytes) {', 'a file that did not check good becomes the backup'),
        ('pc_save.c', 'if (!failed && file_check == PC_SAVE_NOT_A_SAVE && file_bytes) {', 'if (!failed && file_bytes) {', 'a blank file is kept'),
        ('pc_save.c', '            failed = keep_the_backup(path) != 0;\n', '            failed = 0;\n', 'a start without a good save leaves the backup for the next good run to push out'),
        ('pc_save.c', '    return keep_copy(path, PC_SAVE_BACKUP_KEPT, backup, n);\n', '    return n > sizeof(backup) ? keep_copy(path, PC_SAVE_BACKUP_KEPT, backup, n) : 0;\n', 'the backup is not kept before a start without a good save'),
        ('pc_save.c', 'found = k >= TRAILER && !(k & 3u) && !x;', 'found = k >= TRAILER && !(k & 3u) && (x | 1u);', 'the check takes a copy whose XOR is not 0'),
        ('pc_save.c', 'if (fw[end - 2] != 0xC2 || fw[end - 1] != 0x3F)', 'if (fw[end - 2] != 0x3F || fw[end - 1] != 0xC2)', 'the check reads the closing bytes in the other order'),
        ('pc_save.c', 'return sequence != 0 && sequence != 0xFFFFFFFFu;', 'return (int)(sequence | 1u);', 'a copy with no sequence number passes'),
        ('pc_save.c', '#define COPY_MAX 1024u', '#define COPY_MAX 4096u', 'a copy longer than the cart allows passes'),
        ('pc_save.c', ' || xor_of(place, length) != 0)', ')', 'a damaged copy in one of the cart\'s places passes'),
        ('pc_save.c', ' || place[length - 4u] != cart_places[i].number', '', 'a copy of the wrong save in one of the cart\'s places passes'),
        ('pc_save.c', '        if (all_one_byte(place, length))\n            continue;\n', '        if (1)\n            continue;\n', 'the cart\'s places are not looked at'),
        ('pc_save.c', '                return PC_SAVE_NOT_A_SAVE;\n        return PC_SAVE_GOOD;\n    }\n', '                return PC_SAVE_GOOD;\n        return PC_SAVE_GOOD;\n    }\n',
         'a save cut to 4 KB passes as the older layout'),
        ('pc_save.c', '            return (int)cart_places[i].number;\n', '            return 0;\n', 'a place wiped blank passes while .bak holds its save'),
        ('pc_save.c', '    memset(fw, 0x00, PC_SAVE_BYTES);\n    for (i = 0; i < n; i++)\n        fw[i ^ 1u] = raw[i];\n', '    memset(fw, 0x00, PC_SAVE_BYTES);\n    for (i = 0; i < n; i++)\n        fw[i] = raw[i];\n',
         'the backup is read in the wrong byte order'),
        ('pc_save.c', '    newest_in_temp = 0;\n    if (snprintf(temp', '    if (snprintf(temp', 'a temporary file is forgotten twice'),
        ('pc_save.c', '        copy_kept = 1;\n', '', 'the copy is made again at every replacement'),
        ('pc_save.c', 'keep_copy(path, PC_SAVE_BACKUP, file, file_bytes)', 'keep_copy(path, PC_SAVE_BACKUP, file + 1, file_bytes)', 'the copy is not the file as loaded'),
        ('pc_save.c', '    file_bytes = n;\n', '    file_bytes = PC_SAVE_BYTES;\n', 'an older save is copied at the wrong length'),
        ('pc_save.c', '    file_bytes = 0;\n    file_check = PC_SAVE_BLANK;\n', '', 'a run with no file copies an earlier one'),
        ('pc_save.c', '            copy_failed = 1;\n            return -1;\n', '            copy_failed = 1;\n', 'a failed copy still replaces the save'),
        ('pc_save.c', '        newest_in_temp = 1;\n', '', 'the newest bytes in the temporary file go unnoticed'),
        ('pc_pace.h', 'const int hitch = took >= PC_VSYNC_HITCH_FRAMES * frame;', 'const int hitch = 0;', 'a hitch counts against the monitor'),
        ('pc_pace.h', 'w->loss += late > 0 && late > wait ? wait : late;', 'w->loss += late > 0 && late > wait ? wait : late > 0 ? late : 0;', 'time made up does not count back'),
        ('pc_pace.h', 'if (w->loss * 1000 > (int64_t)PC_VSYNC_QUARTER_FRAMES * frame) {\n', 'if (w->loss * 20 > (int64_t)PC_VSYNC_QUARTER_FRAMES * frame) {\n', 'a small steady loss is never seen'),
        ('pc_pace.h', '#define PC_VSYNC_SLOW_QUARTERS  4 ', '#define PC_VSYNC_SLOW_QUARTERS  1 ', 'one slow quarter second turns vsync off'),
        ('pc_pace.h', '        if (least * 4 * PC_VSYNC_SLOW_QUARTERS < sum)\n            return PC_VSYNC_KEEP;\n', '', 'a loss that is not steady turns vsync off'),
        ('pc_pace.h', '    else if (w->calm_needed < PC_VSYNC_CALM_MOST)\n        w->calm_needed *= 2;\n', '', 'the wait does not grow when vsync goes off again soon'),
        ('pc_pace.h', '        w->calm_needed = PC_VSYNC_CALM_FRAMES;\n    else if', '        ;\n    else if', 'the wait never comes back to 10 seconds'),
        ('pc_pace.h', '    if (w->at_once && w->slow_monitor)\n        w->no_returns = 1;\n', '', 'a return that fails on its first second on a slow monitor is tried again'),
        ('pc_pace.h', '    if (w->at_once && w->slow_monitor)\n', '    if (w->at_once)\n', 'a return that fails on its first second ends the tries on any monitor'),
        ('pc_pace.h', 'w->slow_monitor = w->refresh > 0 && w->refresh * 59 > frame * 60;', 'w->slow_monitor = w->refresh > frame;', 'a monitor the system calls 59 Hz, which may run at 59.94, counts as slower than the game'),
        ('pc_pace.h', '        if (w->no_returns)\n            return PC_VSYNC_KEEP;\n', '', 'vsync comes back on after the tries have ended'),
        ('pc_pace.h', '    if (!hitch && last_shown && began_behind) {', '    if (!hitch && began_behind) {', 'a frame after one left unshown counts against the monitor'),
        ('pc_pace.h', '    if (!hitch && last_shown && began_behind) {', '    if (!hitch && last_shown) {', 'a frame that began on time counts against the monitor'),
        ('pc_pace.h', '    p->behind = point - p->next;\n', '    p->behind = 0;\n', 'the pace never says the game is behind'),
        ('pc_pace.h', 'wait = w->refresh > 0 && present > w->refresh ? w->refresh : present;', 'wait = present;', 'a present longer than a refresh counts in full'),
        ('pc_pace.h', '        if (present > w->present_most)\n            w->present_most = present;\n', '', 'the switch line gives no longest present'),
        ('pc_pace.h', '    if (now >= p->next) {\n        p->unshown++;\n        return 0;\n    }\n', '', 'every frame is shown, however far behind'),
        ('pc_pace.h', '        p->next = now + p->period - (uint64_t)PC_PACE_MAKE_UP * p->period;\n', '        p->next = now + p->period;\n', 'a dropped stall makes up nothing'),
        ('pc_pace.h', 'if (hitch || present > frame || again) {\n            w->calm = 0;', 'if (hitch || again) {\n            w->calm = 0;', 'vsync comes back on during a slow stretch'),
        ('pc_pace.h', 'if (hitch || present > frame || again) {', 'if (hitch || present > frame || (again && 0)) {', 'vsync comes back on among slow frames'),
        ('pc_pace.h', 'const int again = took >= 2 * frame && w->since_slow >= 0', 'const int again = took >= 2 * frame && w->since_slow >= -1', 'a single slow frame of a busy PC holds vsync off'),
        ('pc_config.c', '        return cannot_read(path, (DWORD)_doserrno, 1, error, error_size);', '        return -1;', 'a settings file that cannot be read passes as no file'),
        ('pc_config.c', '        text += 3;\n    while (*text) {', '        text += 0;\n    while (*text) {', 'a byte order mark is read as part of the first line'),
        ('pc_config.c', '        return cannot_read(path, code, 0, error, error_size);', '        return cannot_read(path, code, 1, error, error_size);',
         'a settings file that cannot even be reached is said to be there'),
        ('pc_config.c', '        PUT(at, 3);\n        at += 3;\n        kept_end = bom = 3;\n', '', 'the controls screen reads a byte order mark as part of the first line'),
        ('pc_config.c', '        PUT(at, 3);\n        at += 3;\n        kept_end = 3;\n', '', 'the menu\'s writer reads a byte order mark as part of the first line'),
        ('pc_config.c', '    if (memchr(text, 0, (size_t)n)) {\n        free(text);\n        return not_plain_text(path, error, error_size);\n    }\n', '',
         'a settings file saved as UTF-16 is read as text'),
        ('pc_config.c', '        if (a != INVALID_FILE_ATTRIBUTES || (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND))\n            return PC_CONFIG_NOT_READ;\n',
         '        (void)a;\n        (void)code;\n', 'a settings file that cannot be opened is replaced by a change alone'),
        ('pc_config.c', '        if (memchr(old, 0, (size_t)n)) {\n            free(old);\n            return PC_CONFIG_NOT_READ;\n        }\n', '',
         'a settings file saved as UTF-16 is merged into'),
    )
    for n, (name, a, b, what) in enumerate(plants):
        with open(os.path.join(PC, name), newline='') as f:
            source = f.read()
        check('plant in %s: %s' % (name, what), source.count(a) == 1)
        pf = os.path.join(folder, 'plant%d' % n)
        os.makedirs(pf, exist_ok=True)
        with open(os.path.join(pf, name), 'w', newline='') as f:
            f.write(source.replace(a, b))
        planted, r = build('plant%d' % n, os.path.join(pf, 'pc_save.c') if name == 'pc_save.c' else os.path.join(PC, 'pc_save.c'), pf,
                           os.path.join(pf, 'pc_config.c') if name == 'pc_config.c' else os.path.join(PC, 'pc_config.c'))
        run = subprocess.run([planted], capture_output=True, text=True, env=harness.gcc_env(), cwd=folder) if r.returncode == 0 else None
        check('plant: %s: caught' % what, r.returncode == 0 and run.returncode != 0, r.stderr[:400])
        for leftover in ('pc_config_test.sav', 'pc_config_test.sav.tmp', 'pc_config_test_short.sav', 'pc_config_test_refused.sav', 'pc_config_test_refused.sav.tmp', 'no_such_save.sav', os.path.join('no_such_folder', 'no_such_save.sav')):
            if os.path.exists(os.path.join(folder, leftover)):
                os.remove(os.path.join(folder, leftover))
        if os.path.isdir(os.path.join(folder, 'pc_config_test_folder.sav')):
            os.rmdir(os.path.join(folder, 'pc_config_test_folder.sav'))


def test_window(tmp, config, check):
    """The window and the keys, through SDL, on its dummy video driver. Nothing is ever shown."""
    with open(config, encoding='utf-8') as f:
        cfg = json.load(f)
    sdl = cfg.get('sdl_dir')
    if not sdl:
        print('skip the window test: the local configuration names no SDL')
        return
    folder = os.path.join(tmp, 'pc_window')
    os.makedirs(folder, exist_ok=True)
    env = harness.gcc_env()
    env['SDL_VIDEODRIVER'] = 'dummy'
    env['SDL_RENDER_DRIVER'] = 'software'
    env.update(NO_REAL_PADS)

    shader_text = pc_build.shader_text_file(folder)

    def build(name, window_source, input_source=os.path.join(PC, 'pc_input.c'), controls_source=os.path.join(PC, 'pc_controls.c')):
        exe = os.path.join(folder, name + '.exe')
        args = [pc_build.GCC, '-std=gnu11', '-O1', '-Wall', '-Werror', '-Wno-unused-function', '-I', PC, '-I', os.path.join(pc_build._sdl_folder(sdl), 'include', 'SDL2'), '-o', exe,
                os.path.join(HERE, 'pc_window_test.c'), window_source, input_source, controls_source, os.path.join(PC, 'pc_font.c'), os.path.join(PC, 'pc_config.c'),
                os.path.join(PC, 'pc_filter.c'), os.path.join(PC, 'pc_osd.c'), os.path.join(PC, 'pc_menu.c'), shader_text] + pc_build.sdl_link_args(sdl)
        r = subprocess.run(args, capture_output=True, text=True, env=env)
        shutil.copy2(pc_build.sdl_dll(sdl), os.path.join(folder, 'SDL2.dll'))
        return exe, r
    exe, r = build('pc_window_test', os.path.join(PC, 'pc_window.c'))
    check('the window test builds, with no warnings', r.returncode == 0, r.stderr[:800])
    if r.returncode:
        return
    r = subprocess.run([exe], capture_output=True, text=True, env=env, cwd=folder)
    check("the window and the keys, on SDL's dummy driver", r.returncode == 0, r.stdout[-800:])
    # Planted bugs in the window: each must make the test fail.
    with open(os.path.join(PC, 'pc_window.c'), encoding='utf-8') as f:
        source = f.read()
    plants = (
        ('r->w = (LINES * k * 4 + 1) / 3;', 'r->w = picture_w * k;', 'the 4:3 shape loses its width'),
        ('SDL_SetTextureScaleMode(small_texture, SDL_ScaleModeNearest);', 'SDL_SetTextureScaleMode(small_texture, SDL_ScaleModeLinear);', 'the first enlargement is filtered'),
        ('int kw = (place_rect.w + p->width - 1) / p->width;', 'int kw = place_rect.w / p->width;', 'the enlargement is too small'),
        ('{"opengl", "direct3d", "direct3d11", "direct3d12", "software"};\n#define TRY_MAX', '{"opengl", "direct3d11", "direct3d", "direct3d12", "software"};\n#define TRY_MAX', 'the renderers in another order'),
        ('o->renderer && o->renderer[0] ? o->renderer : try_order[0]', 'try_order[0]', 'the configuration\'s renderer is not asked for'),
        ('    if (k && !strcmp(name, first))\n            continue;\n', '', 'the one asked for is tried twice'),
        ('        snprintf(fallback_lines[fallback_n++],', '        if (0) snprintf(fallback_lines[fallback_n++],', 'a renderer that will not start leaves no line'),
        ('if (o->test_w > 0 && o->test_h > 0) {', 'if (0) {', 'the test size for the window is not used'),
        ('int kw = (out_w * 3 + 2) / (LINES * 4);', 'int kw = out_w * 3 / (LINES * 4);', 'the 4:3 width without the window\'s rounding (scale 2 shows the picture 1 x)'),
    )
    for n, (a, b, what) in enumerate(plants):
        check('plant in the source: ' + what, source.count(a) == 1)
        path = os.path.join(folder, 'plant%d.c' % n)
        with open(path, 'w', encoding='utf-8') as f:
            f.write(source.replace(a, b))
        exe, r = build('plant%d' % n, path)
        run = subprocess.run([exe], capture_output=True, text=True, env=env, cwd=folder) if r.returncode == 0 else None
        check('window plant: %s: caught' % what, r.returncode == 0 and run.returncode != 0, r.stderr[:400])
    # Planted bugs in the controllers: each must make the test fail. The first is the fault of a pad on both ports.
    plants = (
        ('pc_input.c', '    int slot = id < 0 ? -1 : find_device(id), i;', '    int slot = -1, i;', 'one pad opened twice'),
        ('pc_controls.c', 'GCB(X), GCB(A), GCB(B),', 'GCB(A), GCB(A), GCB(B),', 'a button on the wrong pad bit'),
        ('pc_input.c', '            if ((i = find_device(e.jdevice.which)) >= 0) {', '            if (0) {', 'a pad pulled out stays on its port'),
    )
    for n, (name, a, b, what) in enumerate(plants):
        with open(os.path.join(PC, name), encoding='utf-8') as f:
            source = f.read()
        check('plant in the source: ' + what, source.count(a) == 1)
        path = os.path.join(folder, 'input_plant%d_%s' % (n, name))
        with open(path, 'w', encoding='utf-8') as f:
            f.write(source.replace(a, b))
        sources = {'pc_input.c': os.path.join(PC, 'pc_input.c'), 'pc_controls.c': os.path.join(PC, 'pc_controls.c')}
        sources[name] = path
        exe, r = build('input_plant%d' % n, os.path.join(PC, 'pc_window.c'), sources['pc_input.c'], sources['pc_controls.c'])
        run = subprocess.run([exe], capture_output=True, text=True, env=env, cwd=folder, timeout=120) if r.returncode == 0 else None
        check('controller plant: %s: caught' % what, r.returncode == 0 and run.returncode != 0, r.stderr[:400])


def test_filters(tmp, config, check):
    """The picture filters (targets/pc/pc_filter.h) through pc_window.c: tests/pc_filter_test.c on SDL's dummy driver, and in a hidden window with the OpenGL renderer
    on this PC's own GPU when one will start there. Nothing is ever shown. Then faults planted in the sources: each must make a run fail."""
    with open(config, encoding='utf-8') as f:
        cfg = json.load(f)
    sdl = cfg.get('sdl_dir')
    if not sdl:
        print('skip the filter test: the local configuration names no SDL')
        return
    folder = os.path.join(tmp, 'pc_filter')
    os.makedirs(folder, exist_ok=True)
    shader_text = pc_build.shader_text_file(folder)
    sources = {name: os.path.join(PC, name) for name in ('pc_window.c', 'pc_filter.c', 'pc_osd.c', 'pc_input.c', 'pc_controls.c', 'pc_font.c', 'pc_config.c', 'pc_menu.c')}
    dummy = harness.gcc_env()
    dummy['SDL_VIDEODRIVER'] = 'dummy'
    dummy['SDL_RENDER_DRIVER'] = 'software'
    dummy['SDL_AUDIODRIVER'] = 'dummy'
    dummy.update(NO_REAL_PADS)
    gpu = harness.gcc_env()
    for name in ('SDL_VIDEODRIVER', 'SDL_RENDER_DRIVER'):
        gpu.pop(name, None)
    gpu['SDL_AUDIODRIVER'] = 'dummy'
    gpu.update(NO_REAL_PADS)

    def build(name, replace=None):
        exe = os.path.join(folder, name + '.exe')
        files = dict(sources)
        if replace:
            files[replace[0]] = replace[1]
        r = subprocess.run([pc_build.GCC, '-std=gnu11', '-O1', '-Wall', '-Werror', '-Wno-unused-function', '-I', PC, '-I', os.path.join(pc_build._sdl_folder(sdl), 'include', 'SDL2'),
                            '-o', exe, os.path.join(HERE, 'pc_filter_test.c')] + list(files.values()) + [shader_text] + pc_build.sdl_link_args(sdl),
                           capture_output=True, text=True, env=harness.gcc_env())
        shutil.copy2(pc_build.sdl_dll(sdl), os.path.join(folder, 'SDL2.dll'))
        return exe, r

    def run(exe, mode, env):
        return subprocess.run([exe, mode], capture_output=True, text=True, env=env, cwd=folder, timeout=300)

    exe, r = build('pc_filter_test')
    check('the filter test builds, with no warnings', r.returncode == 0, r.stderr[:800])
    if r.returncode:
        return
    r = run(exe, 'logic', dummy)
    check('the filters on SDL\'s dummy driver: the filter line, the words\' font, a preset\'s file check, sharp, the shader looks refused, the filter key', r.returncode == 0, r.stdout[-800:])
    r = run(exe, 'gl', gpu)
    on_gpu = 'skip:' not in r.stdout
    if on_gpu:
        check('the filters on this PC\'s GPU, in a hidden window: every look builds, draws upright in its place, and leaves OpenGL as SDL had it', r.returncode == 0, r.stdout[-800:])
    else:
        print('skip the filters on the GPU: the OpenGL renderer will not start here')
    plants = (
        ('pc_config.c', '        if (!pc_config_is_preset(value) || strlen(value) >= sizeof(c->filter))\n            return 0;\n', '', 'logic', 'any filter line is taken'),
        ('pc_font.c', '/* \'R\' */ "####. #...# #...# ####. #.#.. #..#. #...#"', '/* \'R\' */ "####. #...# #...# ####. #.#.. #..#. #...."', 'logic', 'a letter of the font loses a dot'),
        ('pc_osd.c', "    if (u >= 'a' && u <= 'z')\n        u = (unsigned char)(u - 'a' + 'A');\n    return pc_font_glyph(u);", '    return pc_font_glyph(u);', 'logic',
         'small letters over the window are not drawn as capitals'),
        ('pc_window.c', '    r->h = out_h;\n    if (frame_w > out_w) {', '    r->h = out_h / LINES * LINES;\n    if (frame_w > out_w) {', 'logic', 'the looks do not fill the height'),
        ('pc_window.c', '        if (next == PC_LOOK_PRESET && !preset_ready)\n            continue;\n', '', 'logic', 'the filter key stops at a preset that is not there'),
        ('pc_window.c', '    SDL_RenderCopy(renderer, text_texture, NULL, &r);\n}', '}', 'logic', 'the look\'s name is never drawn'),
        ('pc_window.c', 'key && key[0] ? ": the next filter" : ""', '""', 'logic', 'the look\'s name on the window does not say which key changes it'),
        ('pc_window.c', '    if (pc_filter_needs_gl(k)) {\n        if (!looks_ready())', '    if (0) {\n        if (!looks_ready())', 'logic', 'a shader look is taken on a renderer without OpenGL'),
        ('pc_filter.c', '            if (!file_there(folder, value, full, sizeof(full))) {\n                snprintf(why, size, "the preset %s names %s = %s', '            if (0) {\n                snprintf(why, size, "the preset %s names %s = %s', 'logic', 'a preset\'s missing shader file passes'),
        ('pc_filter.c', '    gl.UseProgram((GLuint)s->program);\n', '', 'gl', 'the shader program SDL had is not put back'),
        ('pc_filter.c', 'top_down ? -2.0f : 2.0f, 0, 0, 0, 0, -1, 0, -1, top_down ? 1.0f : -1.0f, 0, 1}', '2.0f, 0, 0, 0, 0, -1, 0, -1, -1.0f, 0, 1}', 'gl', 'an offscreen look is drawn upside down'),
        ('pc_filter.c', '                gl.Uniform1f(where, (GLfloat)atof(equals + 1));', '                (void)where;', 'gl', 'the looks\' settings never reach their shaders'),
        ('pc_config.c', '        if (!strcmp(value, "bvm"))\n            value = "pvm";\n', '', 'logic', 'filter=bvm is refused'),
        ('pc_filter.c', '            if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) {', '            if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY) || 1) {', 'gl',
         'a librashader.dll that is there but will not load is called missing'),
    )
    for n, (name, a, b, mode, what) in enumerate(plants):
        with open(sources[name], encoding='utf-8', newline='') as f:
            text = f.read()
        check('plant in the source: ' + what, text.count(a) == 1)
        if mode == 'gl' and not on_gpu:
            continue
        path = os.path.join(folder, 'plant%d_%s' % (n, name))
        with open(path, 'w', encoding='utf-8', newline='') as f:
            f.write(text.replace(a, b))
        exe, r = build('filter_plant%d' % n, (name, path))
        run_ = run(exe, mode, dummy if mode == 'logic' else gpu) if r.returncode == 0 else None
        check('filter plant: %s: caught' % what, r.returncode == 0 and run_.returncode != 0, r.stderr[:400])


def _test_filters_in_program(exe, rom_path, folder, base_rows, check):
    """The whole program with each look, by its filter line and by its key, in a hidden window: on SDL's dummy driver, and on this PC's GPU when OpenGL starts
    there. The machine and the game's picture never change for a look; only the window's drawing does."""
    os.makedirs(folder, exist_ok=True)
    dummy = harness.gcc_env()
    dummy['SDL_VIDEODRIVER'] = 'dummy'
    dummy['SDL_RENDER_DRIVER'] = 'software'
    dummy.update(NO_REAL_PADS)
    gpu = harness.gcc_env()
    for name in ('SDL_VIDEODRIVER', 'SDL_RENDER_DRIVER'):
        gpu.pop(name, None)
    gpu['SDL_AUDIODRIVER'] = 'dummy'
    gpu.update(NO_REAL_PADS)
    base = [(x['ram'], x['pic']) for x in base_rows[:12]]

    def go(name, env, extra, keys_text):
        rd = os.path.join(folder, name)
        os.makedirs(rd, exist_ok=True)
        cfg = os.path.join(rd, 'play.cfg')
        with open(cfg, 'w', encoding='ascii') as f:
            f.write('rom=%s\nsave=%s\nvsync=off\n%s' % (rom_path.replace('\\', '/'), os.path.join(rd, 'play.sav').replace('\\', '/'), extra))
        keys = os.path.join(rd, 'keys.txt')
        with open(keys, 'w', encoding='ascii') as f:
            f.write(keys_text)
        r = subprocess.run([exe, '--hidden-window', '--config', cfg, '--run-dir', rd, '--key-script', keys, '--render', '--no-stall-log'], capture_output=True, text=True, env=env, timeout=120)
        return [(x['ram'], x['pic']) for x in _frames(rd)] if os.path.exists(os.path.join(rd, 'frames.txt')) else [], r

    escape = '11 down Escape\n'
    rows, r = go('d_crt', dummy, 'filter=crt\n', escape)
    check('filter=crt on a renderer without OpenGL: refused in plain words, the game runs as with off', r.returncode == 0 and 'RESULT: PASS' in r.stdout and 'The filter crt could not be used' in r.stdout and 'drawn as with off' in r.stdout and rows == base, r.stdout[-500:])
    rows, r = go('d_sharp', dummy, 'filter=sharp\n', escape)
    check('filter=sharp there: taken, and the game the same', r.returncode == 0 and 'filter: sharp,' in r.stdout and rows == base, r.stdout[-400:])
    rows, r = go('d_key', dummy, '', '3 down F6\n3 up F6\n5 down F6\n5 up F6\n7 down F6\n7 up F6\n11 down Escape\n')
    check('the filter key there: sharp, off and sharp again, saying the others need OpenGL, and the game the same', r.returncode == 0 and r.stdout.count('filter: sharp,') == 2 and 'filter: off,' in r.stdout and 'need the OpenGL renderer' in r.stdout and rows == base, r.stdout[-600:])
    check('the key list names F6 for the filter and F4 for the controls screen', re.search(r'^F6 +filter$', r.stdout, re.M) is not None and re.search(r'^F4 +controls$', r.stdout, re.M) is not None, r.stdout[:900])
    rows, r = go('d_bad', dummy, 'filter=lcd\n', escape)
    check('a filter line naming no look stops at its line', r.returncode == 2 and 'filter=lcd' in r.stdout, r.stdout[-300:])
    rows, r = go('d_bvm', dummy, 'filter=bvm\n', escape)
    check('filter=bvm is taken as the pvm look: on a renderer without OpenGL, refused in plain words as pvm, and the game runs as with off',
          r.returncode == 0 and 'RESULT: PASS' in r.stdout and 'The filter pvm could not be used' in r.stdout and rows == base, r.stdout[-500:])
    rows, r = go('g_off', gpu, '', escape)
    if r.returncode != 0 or 'renderer opengl' not in r.stdout:
        print('skip the filters in the program on the GPU: the OpenGL renderer will not start here')
        return
    check('on this PC\'s GPU, the program with no filter line runs the game as on the dummy driver', rows == base, r.stdout[-300:])
    for look in ('sharp', 'scanlines', 'crt', 'pvm', 'curved'):
        rows, r = go('g_' + look, gpu, 'filter=%s\n' % look, escape)
        check('on the GPU, filter=%s: taken, and the machine and the game\'s picture the same as with no filter' % look, r.returncode == 0 and ('filter: %s,' % look) in r.stdout and rows == base, r.stdout[-400:])
    keys = ''.join('%d down F6\n%d up F6\n' % (f, f) for f in (2, 3, 4, 5, 6, 7, 8)) + '11 down Escape\n'
    rows, r = go('g_key', gpu, '', keys)
    order = [line.split(',')[0] for line in r.stdout.splitlines() if line.startswith('filter: ')]
    check('on the GPU, the filter key goes through every look in order and back to off, and the game is the same', r.returncode == 0 and order == ['filter: sharp', 'filter: scanlines', 'filter: crt', 'filter: pvm', 'filter: curved', 'filter: off', 'filter: sharp'] and rows == base, str(order) + r.stdout[-300:])
    rows, r = go('g_preset', gpu, 'filter=no_such_folder/my.slangp\n', escape)
    check('on the GPU, a preset that is not there: refused in plain words, the game runs as with off', r.returncode == 0 and 'there is no preset file' in r.stdout and 'drawn as with off' in r.stdout and rows == base, r.stdout[-400:])


# The controls' planted bugs: file, the text, what replaces it, what it breaks. tests/pc_controls_test.c must fail on each.
CONTROLS_PLANTS = (
    ('pc_input.c', '            if (known) {', '            if (0 && known) {', 'the database replaces SDL\'s own mappings'),
    ('pc_input.c', '        if (wants_kind(p) && (assigned[p] < 0', '        if (0 && wants_kind(p) && (assigned[p] < 0', 'a pad asked for by kind is not given first'),
    ('pc_input.c', '            if (q != p && assigned[q] == slot)\n                assigned[q] = old;\n', '            (void)old;\n', 'a pad one player chooses stays the other player\'s too'),
    ('pc_input.c', 'static int threshold(int deadzone) { return deadzone * 32767 / 100; }', 'static int threshold(int deadzone) { (void)deadzone; return 16000; }',
     'the dead zone is not used'),
    ('pc_input.c', '        int first = right ? 2 : 0;', '        int first = 0;', 'the right stick reads the left one'),
    ('pc_input.c', '                stop = (actions & (PC_ACTION_CONTROLS | PC_ACTION_MENU)) != 0;', '                stop = 0;', 'the keys after the one that opens the screen go to the game'),
    ('pc_input.c', '    if (strncmp(want + 8, "0000", 4))', '    if (0)', 'the same pad by another way of connecting is another kind'),
    ('pc_controls.c', '            if ((q != p || b != a) && *k == key) {', '            if (0) {', 'one key does two actions'),
    ('pc_controls.c', '    if (s.nav_wait_release) {', '    if (0) {', 'the press that is taken also chooses'),
    ('pc_controls.c', '        if (key == SDL_SCANCODE_ESCAPE) {\n            say(C_DIM, "Not changed");', '        if (0) {\n            say(C_DIM, "Not changed");',
     'Escape cannot leave a press-it-now as it was'),
    ('pc_controls.c', '    if (p >= 0 && slot != pc_input_player_device(p))\n        return;', '', 'another player\'s pad answers for this one'),
    ('pc_controls.c', '    pl->deadzone = DEFAULT_DEADZONE;', '    pl->deadzone = 10;', 'the defaults move'),
    ('pc_controls.c', 'SDL_SCANCODE_F2, SDL_SCANCODE_F6,', 'SDL_SCANCODE_F2, SDL_SCANCODE_F4,', 'two program keys are F4 by default'),
    ('pc_controls.c', '    if (!s.dirty)\n        return 0;', '    if (0)\n        return 0;', 'nothing changed, and the file is written anyway'),
    ('pc_controls.c', '    s.unsaved = why;\n', '', 'a failed save of the controls is forgotten, so a Soft Reset says the changes are in the file'),
    ('pc_controls.c', '            snprintf(error, error_size, "Not saved: %.30s %s", file_name(), why);',
     '            snprintf(error, error_size, "Not saved: %s %s", s.file, why);', 'the screen\'s Not saved line starts with the whole path and is cut at the edge'),
    ('pc_config.c', '                keep = !pc_config_is_control(name);', '                keep = 1;', 'the old control lines stay in the file'),
    ('pc_config.c', '            keep = !block_has_comment(block, line);', '            keep = 1;', 'the screen\'s comments pile up in the file'),
    ('pc_input.c', '    if (actions & PC_ACTION_CONTROLS)\n        actions &= PC_ACTION_CONTROLS | PC_ACTION_QUIT;\n    else if', '    if',
     'a pause key in the poll that opens the screen pauses too'),
    ('pc_input.c', '        if (held_back[p] && !now)\n            held_back[p] = 0;\n        pad[p] = held_back[p] ? 0 : now;', '        pad[p] = now;',
     'the press that closes the screen reaches the game'),
    ('pc_input.c', '            else if (i == PC_PORT_CONTROLS && !waiting)', '            else if (i == PC_PORT_CONTROLS && (waiting || 1))',
     'taking a pad input for Controls closes the screen'),
    ('pc_input.c', '            else if (!screen && i == PC_PORT_MENU)\n                actions |= pc_controls_port_bit(i);\n', '',
     'a pad\'s Menu bar input cannot close the menu it opened'),
    ('pc_input.c', '        this_gc = !only_kind && devices[i].gc != NULL;', '        this_gc = 0;', '"any" takes a plain joystick before a game controller'),
    ('pc_input.c', 'devices[i].number == n && !strcmp(devices[i].base, d->base)', 'devices[i].number == n && !strncmp(devices[i].base, d->base, strlen(d->base))',
     'a name that begins like another pad\'s gets a number'),
    ('pc_input.c', 'devices[i].number == n && !strcmp', 'devices[i].number >= n && !strcmp', 'a pad plugged in again gets a new number'),
    ('pc_controls.c', '    if (!(p < 0 && a == PC_PORT_CONTROLS) && last_way_in(key)) {', '    if (0) {', 'the only key that opens the screen can be given away'),
    ('pc_controls.c', '    if (p < 0 && a == PC_PORT_CONTROLS && (s.column ?', '    if (0 && (s.column ?', 'the last way to open the screen can be cleared'),
    ('pc_controls.c', '                    *other = 0;\n                    moved = 1;', '                    moved = 1;', 'a page\'s reset puts one key on two actions'),
    ('pc_controls.c', '            memcpy(c->player[p].controller, s.browse_controller, sizeof(c->player[p].controller));\n            pc_input_set_assignment(s.browse_slots);\n',
     '', 'going along the pad row and back takes the other player\'s pad'),
    ('pc_input.c', '            int mine = had[p].used ? had_free(p) : -1;',
     '            int mine = -1;', 'a pad that comes back is not given back to the player who had it'),
    ('pc_input.c', '    if (!player3_heard && assigned[last] >= 0) {\n        kept = assigned[last];',
     '    if (assigned[last] >= 0) {\n        kept = assigned[last];', "players 1 and 2 take player 3's pad while the game hears player 3"),
    ('pc_input.c', 'served = player3_heard ? PC_PLAYERS : last',
     'served = PC_PLAYERS', "player 3's kind of pad comes before player 2 while the game does not hear player 3"),
    ('pc_input.c', '    if (kept >= 0 && taken_by_other(last, kept))\n        remember(last, kept, 0);\n',
     '', 'player 3 does not get back the pad player 1 or 2 took'),
    ('pc_input.c', '    for (p = 0; p < PC_PLAYERS; p++)\n        if (assigned[p] == slot)\n            remember(p, slot, 1);\n',
     '', "a pad pulled out is not remembered as its player's"),
    ('pc_input.c', '!devices[slot].used || devices[slot].order <= had[p].since || strcmp(', '!devices[slot].used || strcmp(',
     'a pad already plugged in at the pull is taken for the pad that comes back'),
    ('pc_input.c', '            remember(p, slot, 1);', '            remember(p, slot, 0);', 'a pull is remembered as a pad still plugged in'),
)


def test_controls(tmp, config, check):
    """The controls and the controls screen, on SDL's virtual joysticks with the real pads' drivers off, and SDL's dummy video driver. The scenes are in
    pc_controls_test.c. With controller_db in the local configuration, the real database is loaded too."""
    with open(config, encoding='utf-8') as f:
        cfg = json.load(f)
    sdl = cfg.get('sdl_dir')
    if not sdl:
        print('skip the controls test: the local configuration names no SDL')
        return
    folder = os.path.join(tmp, 'pc_controls')
    os.makedirs(folder, exist_ok=True)
    shader_text = pc_build.shader_text_file(folder)
    env = harness.gcc_env()
    env['SDL_VIDEODRIVER'] = 'dummy'
    env['SDL_RENDER_DRIVER'] = 'software'
    env.update(NO_REAL_PADS)
    db = []
    if cfg.get('controller_db'):
        db = [pc_build.controller_db_files(cfg['controller_db'])[0]]
    else:
        print('the controls test without the real controller database: the local configuration names no controller_db')

    def build(name, sources):
        exe = os.path.join(folder, name + '.exe')
        args = [pc_build.GCC, '-std=gnu11', '-O1', '-Wall', '-Werror', '-Wno-unused-function', '-I', PC, '-I', os.path.join(pc_build._sdl_folder(sdl), 'include', 'SDL2'), '-o', exe,
                os.path.join(HERE, 'pc_controls_test.c'), os.path.join(PC, 'pc_window.c'), sources['pc_input.c'], sources['pc_controls.c'], os.path.join(PC, 'pc_font.c'),
                os.path.join(PC, 'pc_menu.c'),
                sources['pc_config.c'], os.path.join(PC, 'pc_filter.c'), os.path.join(PC, 'pc_osd.c'), shader_text] + pc_build.sdl_link_args(sdl)
        r = subprocess.run(args, capture_output=True, text=True, env=env)
        shutil.copy2(pc_build.sdl_dll(sdl), os.path.join(folder, 'SDL2.dll'))
        return exe, r

    own = {name: os.path.join(PC, name) for name in ('pc_input.c', 'pc_controls.c', 'pc_config.c')}
    exe, r = build('pc_controls_test', own)
    check('the controls test builds, with no warnings', r.returncode == 0, r.stderr[:800])
    if r.returncode:
        return
    r = subprocess.run([exe] + db, capture_output=True, text=True, env=env, cwd=folder, timeout=300)
    check('the controls: names, lines, made-up pads plugged in and out, the database, and the screen by made-up keys and pads', r.returncode == 0, r.stdout[-1200:])
    for line in r.stdout.splitlines():
        if line.startswith(('the real database', 'keys with a name', 'controller database', 'the screen through')):
            print('  ' + line)
    for n, (name, a, b, what) in enumerate(CONTROLS_PLANTS):
        with open(own[name], encoding='utf-8') as f:
            source = f.read()
        check('plant in the source: ' + what, source.count(a) == 1)
        path = os.path.join(folder, 'plant%d_%s' % (n, name))
        with open(path, 'w', encoding='utf-8') as f:
            f.write(source.replace(a, b))
        exe, r = build('plant%d' % n, dict(own, **{name: path}))
        run = subprocess.run([exe] + db, capture_output=True, text=True, env=env, cwd=folder, timeout=300) if r.returncode == 0 else None
        check('controls plant: %s: caught' % what, r.returncode == 0 and run.returncode != 0, r.stderr[:400])


def test_stall_log(tmp, check):
    """The freeze log alone: no machine, no SDL. The scenes are in pc_stall_test.c."""
    folder = os.path.join(tmp, 'pc_stall')
    os.makedirs(folder, exist_ok=True)

    def build(where, source):
        os.makedirs(where, exist_ok=True)
        exe = os.path.join(where, 'pc_stall_test.exe')
        r = subprocess.run([pc_build.GCC, '-std=gnu11', '-O1', '-Wall', '-Werror', '-Wno-unused-function', '-I', PC, '-o', exe, os.path.join(HERE, 'pc_stall_test.c'), source], capture_output=True, text=True, env=harness.gcc_env())
        return exe, r

    def scene(exe, name, path, timeout=120):
        return subprocess.run([exe, name, path], capture_output=True, text=True, env=harness.gcc_env(), cwd=folder, timeout=timeout)

    source = os.path.join(PC, 'pc_stall.c')
    exe, r = build(folder, source)
    check('the freeze log test builds, with no warnings', r.returncode == 0, r.stderr[:800])
    if r.returncode:
        return
    # Its numbers are the time of day on a real PC, so a test machine that is busy just then can spoil one try. A real fault fails every try, and the plants below get one try each.
    log = os.path.join(folder, 'stalls_parts.txt')
    for attempt in range(3):
        r = scene(exe, 'parts', log)
        if r.returncode == 0:
            break
    check('each part named, with its frame, time and stack; nothing for a normal pass; a stall on file while it lasts', r.returncode == 0, r.stdout[-800:])
    for name in ('cap', 'noisy', 'stop'):
        r = scene(exe, name, os.path.join(folder, 'stalls_%s.txt' % name))
        check('freeze log scene %s' % name, r.returncode == 0, r.stdout[-400:])
    no_folder = os.path.join(folder, 'no_such_folder', 'stalls.txt')
    a_folder = os.path.join(folder, 'a_folder')
    os.makedirs(a_folder, exist_ok=True)
    for name, where, what in (('nowrite', no_folder, 'a folder that is not there'), ('nowrite', a_folder, 'a folder in the file\'s place'), ('locked', os.path.join(folder, 'stalls_locked.txt'), 'a file another program holds')):
        r = scene(exe, name, where)
        check('a log it cannot write, %s: it runs on and says nothing' % what, r.returncode == 0 and r.stdout == '' and r.stderr == '', (r.stdout + r.stderr)[-400:])
    check('and makes no folder', not os.path.exists(os.path.dirname(no_folder)))
    with open(source, newline='') as f:
        text = f.read()
    plants = (
        ('if (now - pass_start > limit_ticks) {', 'if (1) {', 'parts', 'every pass gets a line'),
        ('part_ticks[current_part] += now - part_start;\n    part_start = now;\n    current_part = part;', 'part_ticks[0] += now - part_start;\n    part_start = now;\n    current_part = part;', 'parts', 'the time goes to the wrong part'),
        ('    current_part = part;\n', '', 'parts', 'the part a stack is in is not kept'),
        ('e->ticks[PART_FIRMWARE] = firmware;', 'e->ticks[PART_FIRMWARE] = 0;', 'parts', 'the firmware\'s time is lost'),
        ('if (firmware_flag && *firmware_flag)', 'if (0)', 'parts', 'a stuck firmware turn is called work'),
        ('e->vsync = state_vsync;', 'e->vsync = 1;', 'parts', 'vsync always reads on'),
        ('            look_at_main(id, since);\n', '', 'parts', 'no look at a stuck thread'),
        ('next_look *= 3u;', 'next_look *= 1000u;', 'parts', 'one look, however long it sticks'),
        ('plants[i].fired != frame', '1', 'parts', 'a plant goes off each time its part comes, not once a frame'),
        ('frame >= plants[i].first && frame <= plants[i].last', 'frame == plants[i].first', 'parts', 'a plant over a run of frames goes off on its first only'),
        ('            next_look *= 3u;\n        }\n        out_flush();\n', '            next_look *= 3u;\n        }\n', 'parts', 'nothing reaches the file until the run ends, so a run that never comes back leaves nothing'),
        ('FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_ALWAYS', 'GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS', 'parts', 'each write replaces the file'),
        ('        put_line(header_lines[i], PC_STALL_MAX_LINES - 1);\n', '        ;\n', 'parts', 'the monitor, renderer and GPU lines are lost'),
        ('built, first_line_words, ms_of(limit_ticks));', 'built, "", ms_of(limit_ticks));', 'parts', 'the first line does not name the renderer'),
        ('        put_line(footer_line, PC_STALL_MAX_LINES - 1);\n', '        ;\n', 'parts', 'the sound\'s gaps are not counted'),
        ('footer_line[PC_STALL_FOOTER_CHARS];', 'footer_line[400];', 'parts', 'the sound\'s totals line is cut at 399 characters'),
        ('    closing = closing_sound;\n', '    closing = 0;\n', 'parts', 'a look during the sound device\'s close at the run\'s end calls it stuck'),
        ('    if (now - closing_start > limit_ticks) {', '    if (0) {', 'parts', 'a slow close of the sound device gets no line'),
        ('    if (now - closing_start > limit_ticks) {', '    if (1) {', 'parts', 'a quick close of the sound device gets a line too'),
        ('    if (pass_id != id || stop_asked)\n', '    if (pass_id != id)\n', 'stop', 'a look at the run\'s end, while the main thread only waits for the log\'s last lines, calls that wait stuck'),
        ('        snprintf(line, sizeof(line), "%s %s", clock, text);', '        snprintf(line, sizeof(line), "%s", text);', 'parts', 'a line from the sound has no time of day'),
        ('    char text[PC_STALL_NOTE_CHARS];\n', '    char text[200];\n', 'parts', 'a long note, such as a vsync switch\'s reason, is cut short'),
        ('if (lines_written >= room) {', 'if (0) {', 'cap', 'no cap on the lines'),
        ('        put_sound_line(line, kind == 2);\n', '        put_line(line, PC_STALL_MAX_LINES - RESERVED_LINES);\n', 'cap', 'the sound\'s lines use up the freeze lines\' allowance'),
        ('        put_sound_line(line, kind == 2);\n', '        put_sound_line(line, 0);\n', 'cap', 'the lines about the sound\'s delay use up the room of its gaps'),
        ('            cap_line("the sound\'s delay", PC_STALL_DELAY_LINES, "later ones");\n', '', 'cap', 'the delay\'s allowance runs out unsaid'),
        ('            cap_line("the sound", PC_STALL_SOUND_LINES, "later ones");\n', '', 'cap', 'the sound\'s allowance runs out unsaid'),
        ('            cap_line("its first lines, gaps, stacks and notes", PC_STALL_MAX_LINES - RESERVED_LINES, "later gaps and stacks");\n', '', 'cap',
         'the freeze lines\' allowance runs out unsaid'),
        ('    h = CreateFileA(log_path,', '    if (GetFileAttributesA(log_path) == INVALID_FILE_ATTRIBUTES) printf("cannot write the log\\n");\n    h = CreateFileA(log_path,', 'nowrite', 'a failed write makes a noise'),
        ('    part = current_part;\n    frame = current_frame;\n', '    HeapFree(GetProcessHeap(), 0, HeapAlloc(GetProcessHeap(), 0, 100));\n    part = current_part;\n    frame = current_frame;\n', 'noisy', 'the look takes a lock while the main thread is held, and hangs'),
    )
    for n, (a, b, name, what) in enumerate(plants):
        check('plant in pc_stall.c: ' + what, text.count(a) == 1)
        where = os.path.join(folder, 'plant%d' % n)
        os.makedirs(where, exist_ok=True)
        path = os.path.join(where, 'pc_stall.c')
        with open(path, 'w', newline='') as f:
            f.write(text.replace(a, b))
        planted, r = build(where, path)
        target = no_folder if name == 'nowrite' else os.path.join(where, 'stalls.txt')
        try:
            run_it = scene(planted, name, target, 30) if r.returncode == 0 else None
            caught = r.returncode == 0 and (run_it.returncode != 0 or (name == 'nowrite' and run_it.stdout != ''))
        except subprocess.TimeoutExpired:
            caught = name == 'noisy'
        check('freeze log plant: %s: caught' % what, caught, r.stderr[:400])


def _test_stall_log_in_program(folder, exe, rom_path, image, sdl, base_rows, check):
    """The freeze log in the finished program, on a made-up ROM: it changes nothing in the logs, and a stall made at a known frame and part is written down as that."""
    root = os.path.join(folder, 'stall')
    os.makedirs(root, exist_ok=True)
    env = harness.gcc_env()

    def run(name, *args, program=exe, run_env=env):
        rd = os.path.join(root, name)
        os.makedirs(rd, exist_ok=True)
        return rd, subprocess.run([program, '--rom', rom_path, '--run-dir', rd, '--image-check', image, '--frames', '70'] + list(args), capture_output=True, text=True, env=run_env, timeout=120)

    def text_of(rd, name='stalls.txt'):
        path = os.path.join(rd, name)
        if not os.path.exists(path):
            return ''
        with open(path, encoding='ascii', errors='replace') as f:
            return f.read()

    def logs(rd):
        out = []
        for name in ('frames.txt', 'cart.txt'):
            with open(os.path.join(rd, name), 'rb') as f:
                out.append(f.read())
        return out

    on, r = run('on')
    check('the log on: the run passes', r.returncode == 0 and 'RESULT: PASS' in r.stdout, r.stdout[-400:])
    off, r = run('off', '--no-stall-log')
    check('the log off: the run passes and writes no log', r.returncode == 0 and text_of(off) == '', r.stdout[-400:])
    check('the logs are byte-equal with the log on and off', logs(on) == logs(off))
    words = text_of(on)
    check('a block with its top line, and its totals at the end', words.startswith('==== run started') and 'monitor: no window' in words and 'run ended at frame 70:' in words, words[:400])
    on_render, r = run('on_render', '--render')
    for frame, part, args, plain in ((7, 'work', (), on), (9, 'render', ('--render',), on_render), (63, 'save', (), on), (11, 'pacing', ('--paced',), on)):
        rd, r = run('plant_' + part, '--test-stall', '%d:%s:160' % (frame, part), *args)
        words = text_of(rd)
        check('a stall made in %s at frame %d is written down as that' % (part, frame), r.returncode == 0 and ('frame %d, no window, in %s:' % (frame, part)) in words and 'gap of' in words, words[-600:])
        check('and the logs are byte-equal to the run without it', logs(rd) == logs(plain))
    rd, r = run('no_gap', '--test-stall', '7:work:160', '--stall-limit', '1000')
    check('a stall under the limit gets no line', r.returncode == 0 and 'gap of' not in text_of(rd), text_of(rd)[-300:])
    rd, r = run('short_limit', '--test-stall', '7:work:30', '--stall-limit', '20')
    check('a test can ask for a shorter limit', 'frame 7, no window, in work:' in text_of(rd), text_of(rd)[-300:])
    missing = os.path.join(root, 'no_such_folder', 'stalls.txt')
    rd, r = run('nowhere', '--stall-log', missing, '--test-stall', '7:work:160')
    check('a log it cannot write: the run passes, says nothing about it, and its logs are byte-equal', r.returncode == 0 and 'stalls.txt' not in r.stdout and 'no_such_folder' not in r.stdout and logs(rd) == logs(on), r.stdout[-300:])
    check('a part with no such name stops at the command line', run('bad_part', '--test-stall', '7:nowhere:160')[1].returncode == 2)
    if not sdl:
        return
    # Through the window's path on SDL's dummy driver: the marks around the present and the event poll, the top lines from the window, and vsync as it is.
    wenv = harness.gcc_env()
    wenv['SDL_VIDEODRIVER'] = 'dummy'
    wenv['SDL_RENDER_DRIVER'] = 'software'
    wenv.update(NO_REAL_PADS)
    cfg = os.path.join(root, 'play.cfg')
    with open(cfg, 'w', encoding='ascii') as f:
        f.write('rom=%s\nsave=%s\nvsync=off\n' % (rom_path.replace('\\', '/'), os.path.join(root, 'play.sav').replace('\\', '/')))
    keys = os.path.join(root, 'keys.txt')
    with open(keys, 'w', encoding='ascii') as f:
        f.write('11 down Escape\n')

    def window_run(name, part, program=exe, wanted=None):
        rd = os.path.join(root, name)
        os.makedirs(rd, exist_ok=True)
        r = subprocess.run([program, '--hidden-window', '--config', cfg, '--run-dir', rd, '--key-script', keys, '--render', '--test-stall', '5:%s:160' % part], capture_output=True, text=True, env=wenv, timeout=120)
        return rd, r

    plain = os.path.join(root, 'w_plain')
    os.makedirs(plain, exist_ok=True)
    r = subprocess.run([exe, '--hidden-window', '--config', cfg, '--run-dir', plain, '--key-script', keys, '--render', '--no-stall-log'], capture_output=True, text=True, env=wenv, timeout=120)
    check('the window\'s path with the log off', r.returncode == 0 and text_of(plain) == '', r.stdout[-300:])
    for part in ('present', 'events'):
        rd, r = window_run('w_' + part, part)
        words = text_of(rd)
        check('through the window, a stall made in %s is written down as that' % part, r.returncode == 0 and 'frame 5, vsync off, in %s:' % part in words and 'gap of' in words, words[-600:])
        check('and the logs through the window are byte-equal with the log off', logs(rd) == logs(plain))
    words = text_of(os.path.join(root, 'w_present'))
    check('the top of the block names the monitor\'s refresh rate, the renderer and the GPU', ' Hz' in words.split('renderer:')[0] and 'renderer: ' in words and 'renderer software' in words and 'gpu: ' in words and 'adapters: ' in words, words[:600])
    # A pause is no gap, however long it lasts: each of its passes counts on its own.
    pause_keys = os.path.join(root, 'pause_keys.txt')
    with open(pause_keys, 'w', encoding='ascii') as f:
        f.write('3 down P\n3 up P\n30 down P\n30 up P\n35 down Escape\n')

    def pause_run(name, program=exe):
        rd = os.path.join(root, name)
        os.makedirs(rd, exist_ok=True)
        return rd, subprocess.run([program, '--hidden-window', '--config', cfg, '--run-dir', rd, '--key-script', pause_keys, '--render'], capture_output=True, text=True, env=wenv, timeout=120)

    rd, r = pause_run('w_pause')
    check('a pause gets no line in the log', r.returncode == 0 and 'run ended' in text_of(rd) and 'ms, frame 3,' not in text_of(rd), text_of(rd)[-400:])
    # Planted bugs in the loop's marks: the program must then get its stall wrong.
    with open(os.path.join(PC, 'pc_main.c'), newline='') as f:
        text = f.read()
    plants = (
        ('        pc_stall_part(PC_STALL_PRESENT);\n        if (test_monitor_hz', '        if (test_monitor_hz', 'present', 'no mark at the present, so its stall is called something else'),
        ('    pc_stall_part(PC_STALL_EVENTS);\n    key_script_due((int)frame + script_ticks);\n    window_events();\n    while', '    key_script_due((int)frame + script_ticks);\n    window_events();\n    while', 'events', 'no mark at the event poll'),
    )
    with open(exe + '.rsp', encoding='utf-8') as f:
        objects = [line.strip().strip('"') for line in f if line.strip()]
    plants += (('        pc_stall_pass_end(frame, pc_cart_host_ticks());\n        pc_stall_part(PC_STALL_PACE);\n', '        pc_stall_part(PC_STALL_PACE);\n', None, 'a pause counts as one long pass'),)
    for n, (a, b, part, what) in enumerate(plants):
        check('plant in pc_main.c: ' + what, text.count(a) == 1)
        where = os.path.join(root, 'plant%d' % n)
        os.makedirs(where, exist_ok=True)
        src = os.path.join(where, 'pc_main.c')
        with open(src, 'w', newline='') as f:
            f.write(text.replace(a, b))
        job = pc_build.machine_jobs(where, '-O1', ('pc_main',))[0]
        obj = pc_build.compile_jobs([(src, os.path.join(where, 'pc_main.o'), job[2], [])], 1)[0]
        planted = os.path.join(where, 'pc_port.exe')
        pc_build.link([obj if os.path.basename(o) == 'pc_main.o' else o for o in objects], planted, pc_build.sdl_link_args(sdl) + ['-lwinmm'])
        shutil.copy2(pc_build.sdl_dll(sdl), os.path.join(where, 'SDL2.dll'))
        if part:
            rd, r = window_run('w_plant%d' % n, part, program=planted)
            caught = r.returncode == 0 and 'frame 5, vsync off, in %s:' % part not in text_of(rd)
        else:
            rd, r = pause_run('w_plant%d' % n, program=planted)
            caught = r.returncode == 0 and 'ms, frame 3,' in text_of(rd)
        check('plant in pc_main.c: %s: caught' % what, caught, text_of(rd)[-400:])


# The menu bar's planted faults: file, the text, what replaces it, what it breaks. tests/pc_menu_test.c must fail on each.
MENU_PLANTS = (
    ('    if (it->look & PC_MENU_GREY)\n        return;\n    if (it->look & PC_MENU_OPENS) {', '    if (it->look & PC_MENU_OPENS) {', 'a grey item can be chosen'),
    ('        actions |= (unsigned)it->arg;', '        (void)0;', 'an item that does what a key does gives nothing'),
    ('    int s = h / 336;', '    int s = 1;', 'the bar keeps one size in every window'),
    ('        peek = y >= 0 && y < BAR_ROWS * scale;', '        peek = 0;', 'in full screen the mouse at the top does not bring the bar back'),
    ('        else if (key == KEY_LEFT || key == KEY_ESCAPE)\n            open_list = -1;', '        else if (key == KEY_ESCAPE)\n            open_list = -1;',
     'Left does not close a list'),
    ('    for (gap = 6; gap > 1; gap--)\n', '    for (gap = 6; gap > 6; gap--)\n', 'the bar\'s titles keep their room in the smallest window, and the last runs off it'),
    ('        if (y > PAD * scale && y + h + PAD * scale > room) {', '        if (0) {', 'a list longer than the window stays one column, cut at the bottom'),
    ('static int page_rows(void) { int n = (out_h / scale - 4 * PAD) / (CELL_H + 2) - 3; return n < 1 ? 1 : n; }',
     'static int page_rows(void) { return 1000; }', 'a page longer than the window is drawn in one part, cut at the bottom'),
    ('static int page_fit(void) { int n = (out_w / scale - 4 * PAD) / CELL_W; return n < 8 ? 8 : n; }',
     'static int page_fit(void) { return 1000; }', 'a page\'s lines are never broken to fit across the window'),
    ('    if (page && page_part + 1 < page_parts())\n        page_part++;\n    else if (pc_menu_is_open())', '    if (pc_menu_is_open())',
     'the menu input, a pad\'s Menu bar button among them, closes a page shown in parts at its first part'),
)


def test_menu(tmp, check):
    """The menu bar alone (pc_menu.c): no SDL, no window. The scenes are in pc_menu_test.c; its pictures stay in the scratch folder."""
    folder = os.path.join(tmp, 'pc_menu')
    os.makedirs(folder, exist_ok=True)
    menu = os.path.join(PC, 'pc_menu.c')

    def build(name, source):
        exe = os.path.join(folder, name + '.exe')
        r = subprocess.run([pc_build.GCC, '-std=gnu11', '-O1', '-Wall', '-Werror', '-I', PC, '-o', exe, os.path.join(HERE, 'pc_menu_test.c'), source,
                            os.path.join(PC, 'pc_font.c')], capture_output=True, text=True, env=harness.gcc_env())
        return exe, r
    exe, r = build('pc_menu_test', menu)
    check('the menu bar\'s test builds, with no warnings', r.returncode == 0, r.stderr[:800])
    if r.returncode:
        return
    r = subprocess.run([exe, folder], capture_output=True, text=True, env=harness.gcc_env(), cwd=folder, timeout=120)
    check('the menu bar: sizes, keys, the mouse, a list, full screen, pages and drawing', r.returncode == 0 and ' 0 failed' in r.stdout, r.stdout[-800:])
    with open(menu, encoding='utf-8') as f:
        source = f.read()
    for n, (a, b, what) in enumerate(MENU_PLANTS):
        check('plant in the source: ' + what, source.count(a) == 1)
        path = os.path.join(folder, 'plant%d_pc_menu.c' % n)
        with open(path, 'w', encoding='utf-8') as f:
            f.write(source.replace(a, b))
        exe2, r2 = build('plant%d' % n, path)
        run = subprocess.run([exe2], capture_output=True, text=True, env=harness.gcc_env(), cwd=folder, timeout=120) if r2.returncode == 0 else None
        check('menu plant: %s: caught' % what, r2.returncode == 0 and run.returncode != 0, r2.stderr[:400])


def run(tmp, config, check):
    test_stall_log(tmp, check)
    test_menu(tmp, check)
    test_times(check)
    test_emission(tmp, check)
    test_fallback_times(tmp, check)
    test_machine_alone(tmp, config, check)
    test_console(tmp, config, check)
    test_renderer(tmp, check)
    test_config(tmp, check)
    test_mix_and_audio(tmp, check)
    if config:
        test_window(tmp, config, check)
        test_filters(tmp, config, check)
        test_controls(tmp, config, check)
        test_cart(tmp, config, check)
        test_sound_core_pins(tmp, config, check)
        test_z80_against_firmware(tmp, config, check)
        test_whole_program(tmp, config, check)
        test_console_program(tmp, config, check)
        test_interrupt_order(tmp, config, check)
