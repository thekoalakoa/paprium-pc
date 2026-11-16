# Copyright (C) 2026 Marsupial Solutions LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Flag analysis: which of N, Z, V, C are read before they are written again.

The translated code computes a flag only where this analysis says something can read it, and it
stores a flag in the context only when it is live at the end of a block. The X flag is not kept at
all: instructions that read it stop the translation (see x_readers).

The analysis is a backward data flow over all blocks. A return feeds from the return sites of the
region's callers; a jump through an unknown address feeds from every enterable block.

A switched hook of the patch source (patchsrc.py) leaves the instructions at its site out while its switch is
on. hook_uses names those sites: each counts as reading every flag, so the flags before it are whole for the
switch's way. ret_all names the regions of switch caves: their returns store every flag, as the code they
return to cannot be seen from here.
"""
N, Z, V, C = 8, 4, 2, 1
ALL = 15

CC_USE = {'t': 0, 'f': 0, 'hi': C | Z, 'ls': C | Z, 'cc': C, 'cs': C, 'ne': Z, 'eq': Z, 'vc': V, 'vs': V,
          'pl': N, 'mi': N, 'ge': N | V, 'lt': N | V, 'gt': N | Z | V, 'le': N | Z | V}

SETS_ALL = {'move', 'moveq', 'clr', 'tst', 'and', 'andi', 'or', 'ori', 'eor', 'eori', 'not', 'swap', 'ext',
            'mulu', 'muls', 'divu', 'divs', 'add', 'addi', 'addq', 'sub', 'subi', 'subq', 'neg', 'cmp', 'cmpi',
            'cmpa', 'cmpm', 'asl', 'asr', 'lsl', 'lsr', 'rol', 'ror', 'tas'}
BIT_OPS = {'btst', 'bset', 'bclr', 'bchg'}
X_READERS = {'addx', 'subx', 'negx', 'abcd', 'sbcd', 'nbcd', 'roxl', 'roxr'}


def uses(I):
    """The flags this instruction reads."""
    if I.op in ('bcc', 'scc', 'dbcc'):
        return CC_USE[I.cc]
    if I.op == 'move' and I.src == ('sr',):
        return ALL
    if I.dst in (('sr',), ('ccr',)) and I.op in ('ori', 'andi', 'eori'):
        return ALL
    return 0


def defines(I):
    """The flags this instruction overwrites completely."""
    op = I.op
    if op == 'move':
        if I.src == ('usp',) or I.dst == ('usp',) or I.src == ('sr',):
            return 0
        return ALL
    if op in ('addq', 'subq') and I.dst[0] == 'a':
        return 0
    if op in SETS_ALL:
        return ALL
    if op in BIT_OPS:
        return Z
    if op in ('rte', 'rtr'):
        return ALL
    return 0


def x_readers(disc):
    """Addresses of instructions that read X. The translator refuses these rather than get them wrong."""
    return sorted(a for a, I in disc.ins.items() if I.op in X_READERS)


def analyse(blocks, regions, all_live=False, hook_uses=None, ret_all=()):
    """Fill live_in and live_out on every block. hook_uses: {site: flags} for switched hooks; ret_all: region heads whose returns
    keep every flag."""
    hook_uses = hook_uses or {}
    if all_live:
        for b in blocks.values():
            b.live_in = b.live_out = ALL
        return

    gen = {}
    kill = {}
    for pc, b in blocks.items():
        g, k = 0, 0
        for I in b.insns:
            g |= (uses(I) | hook_uses.get(I.pc, 0)) & ~k
            k |= defines(I)
        gen[pc], kill[pc] = g, k

    # who calls whom, and which returns feed which sites
    callers = {h: set() for h in regions}          # region -> return sites of its direct callers
    tail_from = {h: set() for h in regions}        # region -> regions that jump into it
    ind_returns = set()
    for pc, b in blocks.items():
        last = b.insns[-1]
        if b.kind == 'call':
            t = last.target if last.op == 'bsr' else last.src[1] & 0xFFFFFF
            tb = blocks.get(t)
            if tb is not None and b.succs:
                callers[tb.region].add(b.succs[0])
        elif b.kind == 'call_ind' and b.succs:
            ind_returns.add(b.succs[0])
        for s in b.succs:
            c = blocks.get(s)
            if c is not None and c.region != b.region and b.kind != 'call':
                tail_from[c.region].add(b.region)

    enterable = [pc for pc, b in blocks.items() if b.enterable]
    ret_live = {h: (ALL if h in ret_all else 0) for h in regions}
    entry_live = 0
    changed = True
    live_in = {pc: 0 for pc in blocks}
    order = sorted(blocks, reverse=True)
    while changed:
        changed = False
        ind_live = 0
        for s in ind_returns:
            ind_live |= live_in.get(s, 0)
        for h in regions:
            v = ind_live | (ALL if h in ret_all else 0)
            for s in callers[h]:
                v |= live_in.get(s, 0)
            for f in tail_from[h]:
                v |= ret_live[f]
            if v != ret_live[h]:
                ret_live[h] = v
                changed = True
        e = 0
        for pc in enterable:
            e |= live_in[pc]
        if e != entry_live:
            entry_live = e
            changed = True
        for pc in order:
            b = blocks[pc]
            out = 0
            if b.kind == 'call':
                last = b.insns[-1]
                t = last.target if last.op == 'bsr' else last.src[1] & 0xFFFFFF
                if t in blocks:
                    out |= live_in[t]
                else:       # a target the program cannot see: anything may be read, and it returns here
                    out |= ALL
            elif b.kind == 'call_ind' or b.kind == 'jmp_ind':
                out |= entry_live
            elif b.kind == 'ret':
                out |= ret_live[b.region]
            elif b.kind == 'rte':       # back to the interrupted block, which may read any flag
                out = ALL
            if b.kind != 'call':
                for s in b.succs:
                    out |= live_in.get(s, ALL)      # a successor outside the program may read anything
            new = gen[pc] | (out & ~kill[pc])
            if new != live_in[pc] or out != b.live_out:
                live_in[pc] = new
                b.live_out = out
                b.live_in = new
                changed = True


def live_after_each(b, hook_uses=None):
    """For one block: the live flags after each instruction, in order. A switched hook's site reads every flag (see analyse)."""
    hook_uses = hook_uses or {}
    out = [0] * len(b.insns)
    live = b.live_out
    for i in range(len(b.insns) - 1, -1, -1):
        out[i] = live
        I = b.insns[i]
        live = uses(I) | hook_uses.get(I.pc, 0) | (live & ~defines(I))
    return out
