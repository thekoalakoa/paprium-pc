# Copyright (C) 2026 Marsupial Solutions LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""68000 instruction times, from the published tables (M68000 User's Manual, instruction execution times).

clocks(I) is what one instruction charges when its block starts: the time in the 68000's own clocks with no wait states, for the case that needs no
charge of its own. Some times depend on the run. For those the back end adds a charge at the point where the run decides it, and this module says how much:

    a conditional branch   the time of "not taken" here; taken adds branch_taken_extra(I)
    dbcc                   the time of "taken" (10) here; condition true adds 2, count run out adds 4
    scc on a register      the time of "false" (4) here; true adds 2
    shift by a register    the time of count 0 here; each bit of the count adds 2
    mulu, muls             38 plus the source's time here; the rest goes by the bits of the source (PT_CHARGE_MULU, PT_CHARGE_MULS in the runtime)
    divu, divs             only the source's time here; the rest goes by the values (PT_CHARGE_DIVU, PT_CHARGE_DIVS in the runtime)

Refresh and the Z80's use of the bus make the real machine slower: the machine model scales this time, see targets/pc/pc_clock.h.

Checked form by form, every form the program uses, on the FX68K core in simulation. Two rows of the published tables gave way to it: move.l from a register to -(An) takes 12, not
the 14 printed, and andi.l #,Dn takes 16, not 14.
"""
_EA_BW = {'d': 0, 'a': 0, 'ind': 4, 'post': 4, 'pre': 6, 'disp': 8, 'idx': 10, 'absw': 8, 'absl': 12, 'pcd': 8, 'pci': 10, 'imm': 4}
_EA_L = {'d': 0, 'a': 0, 'ind': 8, 'post': 8, 'pre': 10, 'disp': 12, 'idx': 14, 'absw': 12, 'absl': 16, 'pcd': 12, 'pci': 14, 'imm': 8}

_SRC_ORDER = ['d', 'a', 'ind', 'post', 'pre', 'disp', 'idx', 'absw', 'absl', 'pcd', 'pci', 'imm']
_DST_ORDER = ['d', 'a', 'ind', 'post', 'pre', 'disp', 'idx', 'absw', 'absl']
_MOVE_BW = [
    [4, 4, 8, 8, 8, 12, 14, 12, 16],
    [4, 4, 8, 8, 8, 12, 14, 12, 16],
    [8, 8, 12, 12, 12, 16, 18, 16, 20],
    [8, 8, 12, 12, 12, 16, 18, 16, 20],
    [10, 10, 14, 14, 14, 18, 20, 18, 22],
    [12, 12, 16, 16, 16, 20, 22, 20, 24],
    [14, 14, 18, 18, 18, 22, 24, 22, 26],
    [12, 12, 16, 16, 16, 20, 22, 20, 24],
    [16, 16, 20, 20, 20, 24, 26, 24, 28],
    [12, 12, 16, 16, 16, 20, 22, 20, 24],
    [14, 14, 18, 18, 18, 22, 24, 22, 26],
    [8, 8, 12, 12, 12, 16, 18, 16, 20],
]
_MOVE_L = [
    [4, 4, 12, 12, 12, 16, 18, 16, 20],
    [4, 4, 12, 12, 12, 16, 18, 16, 20],
    [12, 12, 20, 20, 20, 24, 26, 24, 28],
    [12, 12, 20, 20, 20, 24, 26, 24, 28],
    [14, 14, 22, 22, 22, 26, 28, 26, 30],
    [16, 16, 24, 24, 24, 28, 30, 28, 32],
    [18, 18, 26, 26, 26, 30, 32, 30, 34],
    [16, 16, 24, 24, 24, 28, 30, 28, 32],
    [20, 20, 28, 28, 28, 32, 34, 32, 36],
    [16, 16, 24, 24, 24, 28, 30, 28, 32],
    [18, 18, 26, 26, 26, 30, 32, 30, 34],
    [12, 12, 20, 20, 20, 24, 26, 24, 28],
]
_JMP = {'ind': 8, 'disp': 10, 'idx': 14, 'absw': 10, 'absl': 12, 'pcd': 10, 'pci': 14}
_JSR = {'ind': 16, 'disp': 18, 'idx': 22, 'absw': 18, 'absl': 20, 'pcd': 18, 'pci': 22}
_LEA = {'ind': 4, 'disp': 8, 'idx': 12, 'absw': 8, 'absl': 12, 'pcd': 8, 'pci': 12}
_PEA = {'ind': 12, 'disp': 16, 'idx': 20, 'absw': 16, 'absl': 20, 'pcd': 16, 'pci': 20}
_MM_LOAD = {'ind': 12, 'post': 12, 'disp': 16, 'idx': 18, 'absw': 16, 'absl': 20, 'pcd': 16, 'pci': 18}
_MM_STORE = {'ind': 8, 'pre': 8, 'disp': 12, 'idx': 14, 'absw': 12, 'absl': 16}

INTERRUPT_ENTRY = 44    # from the interrupt being taken to the handler's first instruction
REFUSED = {'addx', 'subx', 'negx', 'abcd', 'sbcd', 'nbcd', 'roxl', 'roxr', 'chk', 'movep', 'reset', 'trapv'}


def _ea(o, sz):
    return (_EA_L if sz == 4 else _EA_BW)[o[0]]


def _is_mem(o):
    return o[0] not in ('d', 'a', 'imm')


def _long_to_reg(s):
    """add.l, sub.l, and.l and or.l to a register take 8 from a register or immediate and 6 from memory."""
    return (6 if _is_mem(s) else 8) + _ea(s, 4)


def clocks(I):
    """The clocks one instruction charges when its block starts. See the module's first lines."""
    op, sz, s, d = I.op, I.sz, I.src, I.dst
    if op in ('move', 'movea'):
        if d in (('sr',), ('ccr',)):
            return 12 + _ea(s, 2)
        if s == ('sr',):
            return 6 if d[0] == 'd' else 8 + _ea(d, 2)
        if s == ('usp',) or d == ('usp',):
            return 4
        return (_MOVE_L if sz == 4 else _MOVE_BW)[_SRC_ORDER.index(s[0])][_DST_ORDER.index(d[0])]
    if op == 'moveq':
        return 4
    if op in ('add', 'sub', 'adda', 'suba', 'and', 'or'):
        if d[0] == 'a':
            return _long_to_reg(s) if sz == 4 else 8 + _ea(s, 2)
        if d[0] == 'd':
            return _long_to_reg(s) if sz == 4 else 4 + _ea(s, sz)
        return (12 if sz == 4 else 8) + _ea(d, sz)
    if op == 'eor':
        if d[0] == 'd':
            return 8 if sz == 4 else 4
        return (12 if sz == 4 else 8) + _ea(d, sz)
    if op in ('cmp', 'cmpa'):
        if d[0] == 'a':
            return 6 + _ea(s, sz)
        return (6 if sz == 4 else 4) + _ea(s, sz)
    if op == 'cmpm':
        return 20 if sz == 4 else 12
    if op in ('addi', 'subi', 'ori', 'eori', 'andi'):
        if d in (('sr',), ('ccr',)):
            return 20
        if d[0] == 'd':
            return 16 if sz == 4 else 8
        return (20 if sz == 4 else 12) + _ea(d, sz)
    if op == 'cmpi':
        if d[0] == 'd':
            return 14 if sz == 4 else 8
        return (12 if sz == 4 else 8) + _ea(d, sz)
    if op in ('addq', 'subq'):
        if d[0] == 'd':
            return 8 if sz == 4 else 4
        if d[0] == 'a':
            return 8
        return (12 if sz == 4 else 8) + _ea(d, sz)
    if op in ('clr', 'neg', 'not'):
        if d[0] == 'd':
            return 6 if sz == 4 else 4
        return (12 if sz == 4 else 8) + _ea(d, sz)
    if op == 'tst':
        return 4 + _ea(d, sz)
    if op == 'tas':
        return 4 if d[0] == 'd' else 10 + _ea(d, 1)
    if op == 'scc':
        return 4 if d[0] == 'd' else 8 + _ea(d, 1)
    if op in ('asl', 'asr', 'lsl', 'lsr', 'rol', 'ror'):
        if d[0] != 'd':
            return 8 + _ea(d, 2)
        base = 8 if sz == 4 else 6
        return base + 2 * s[1] if s[0] == 'imm' else base
    if op in ('btst', 'bset', 'bclr', 'bchg'):
        by_reg = s[0] == 'd'
        if d[0] == 'd':
            if op == 'btst':
                return 6 if by_reg else 10
            low = (6 if by_reg else 10) + (2 if op == 'bclr' else 0)
            return low + 2 if not by_reg and s[1] >= 16 else low
        if op == 'btst':
            return (4 if by_reg else 8) + _ea(d, 1)
        return (8 if by_reg else 12) + _ea(d, 1)
    if op == 'bcc':
        return 12 if I.n == 4 else 8
    if op == 'bra':
        return 10
    if op == 'bsr':
        return 18
    if op == 'dbcc':
        return 10
    if op == 'jmp':
        return _JMP[s[0]]
    if op == 'jsr':
        return _JSR[s[0]]
    if op == 'lea':
        return _LEA[s[0]]
    if op == 'pea':
        return _PEA[s[0]]
    if op == 'movem':
        each = (8 if sz == 4 else 4) * len(I.regs)
        return each + (_MM_STORE[d[0]] if I.mdir == 'r2m' else _MM_LOAD[s[0]])
    if op in ('mulu', 'muls'):
        return 38 + _ea(s, 2)
    if op in ('divu', 'divs'):
        return _ea(s, 2)
    if op in ('exg',):
        return 6
    if op in ('ext', 'swap', 'nop', 'stop'):
        return 4
    if op == 'link':
        return 16
    if op == 'unlk':
        return 12
    if op == 'rts':
        return 16
    if op in ('rte', 'rtr'):
        return 20
    if op in ('illegal', 'trap'):
        return 34
    if op in REFUSED:
        return 4        # the translator refuses these: a block that holds one stops the run, so its time does not matter
    raise KeyError(op)


def branch_taken_extra(I):
    """What a conditional branch adds when taken: 10 taken, against 8 or 12 not taken."""
    return 2 if I.n == 2 else -2


def block_clocks(insns):
    return sum(clocks(I) for I in insns)
