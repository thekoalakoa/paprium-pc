# Copyright (C) 2026 Marsupial Solutions LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Cuts the discovered instructions into regions (one C function each) and basic blocks.

A region starts at a function head and holds what its direct branches reach without crossing
another head. A block starts at: a region head, a branch target, the instruction after any call
or other control transfer, the instruction after a long NOP run, the instruction after a write to
the status register, and any address the run-time logs say was entered.

Safe points are the blocks whose entry may take an interrupt (see DESIGN, "Interrupts"): region
heads, return sites, targets of backward branches, the end of a long NOP run, the instruction
after a status register write, and every SAFE_EVERY-th instruction of a straight run. With
every_insn=True each instruction becomes its own block and its own safe point, which is the strict
lockstep build.
"""
NOP_RUN_MIN = 4     # shorter NOP runs are alignment or small waits and generate no code at all
SAFE_EVERY = 64     # a straight run this long gets a safe point, so a pending interrupt never waits for hundreds of instructions


class Block:
    __slots__ = ('pc', 'insns', 'succs', 'kind', 'safe', 'enterable', 'region', 'nop_cycles',
                 'live_in', 'live_out')

    def __init__(self, pc):
        self.pc = pc
        self.insns = []
        self.succs = []         # direct successors inside the program, by address
        self.kind = 'fall'      # fall, goto, cond, call, call_ind, ret, rte, jmp_ind, switch, stop
        self.safe = False
        self.enterable = False  # the address map holds an entry for this block
        self.region = None
        self.nop_cycles = 0
        self.live_in = 0
        self.live_out = 0


class Region:
    __slots__ = ('head', 'blocks')

    def __init__(self, head):
        self.head = head
        self.blocks = []


ENDS_BLOCK = {'bra', 'bcc', 'bsr', 'dbcc', 'jsr', 'jmp', 'rts', 'rte', 'rtr', 'stop', 'illegal', 'trap'}
SR_DST = (('sr',), ('ccr',))


def _writes_sr(I):
    return I.dst == ('sr',)


def _direct_target(I):
    if I.op in ('bra', 'bcc', 'dbcc', 'bsr'):
        return I.target
    if I.op in ('jmp', 'jsr') and I.src[0] in ('absl', 'absw', 'pcd'):
        return I.src[1] & 0xFFFFFF
    return None


def build(disc, every_insn=False, extra_enterable=(), only=None, safe_every=SAFE_EVERY):
    """only: a set of addresses. Other instructions are left out, so they run in the fallback."""
    ins = disc.ins
    if only is not None:
        ins = {a: I for a, I in disc.ins.items() if a in only}
    heads = set(a for a in disc.func_heads if a in ins)
    heads.update(t for t in disc.const_target.values() if t in ins)     # direct calls for constant-loaded registers
    leaders = set(heads)
    safe = set(heads)
    returns = set()
    nop_end = {}

    # Long NOP runs end their block, and the block after them is a safe point.
    order = sorted(ins)
    k = 0
    while k < len(order):
        a = order[k]
        if ins[a].op == 'nop':
            j = k
            p = a
            while j < len(order) and order[j] == p and ins[p].op == 'nop':
                p += 2
                j += 1
            if j - k >= NOP_RUN_MIN and p in ins:
                leaders.add(p)
                safe.add(p)
                nop_end[p] = (j - k) * 4
            k = j
        else:
            k += 1

    for a in order:
        I = ins[a]
        nxt = a + I.n
        t = _direct_target(I)
        if t is not None and t in ins:
            leaders.add(t)
            if t <= a:
                safe.add(t)
        if I.op in ENDS_BLOCK:
            if nxt in ins:
                leaders.add(nxt)
            if I.op in ('jsr', 'bsr') and nxt in ins:
                returns.add(nxt)
                safe.add(nxt)
        if _writes_sr(I) and nxt in ins:
            leaders.add(nxt)
            safe.add(nxt)
    for sw in disc.switches.values():
        leaders.update(t for t in sw[1] if t in ins)
    enterable = set(heads) | returns | set(a for a in extra_enterable if a in ins)
    leaders |= enterable
    if safe_every:
        run = 0
        for a in order:
            if a in leaders:
                run = 0
            elif run >= safe_every:
                leaders.add(a)
                safe.add(a)
                run = 0
            run += 1
            if ins[a].op in ENDS_BLOCK or a + ins[a].n not in ins:
                run = 0
    if every_insn:
        leaders = set(order)
        safe = set(order)

    # Blocks
    blocks = {}
    for lead in sorted(leaders):
        b = Block(lead)
        blocks[lead] = b
        a = lead
        while True:
            I = ins[a]
            b.insns.append(I)
            nxt = a + I.n
            _set_kind(b, I, nxt, ins, disc)
            if I.op in ENDS_BLOCK:
                break
            if nxt in leaders or nxt not in ins:
                if nxt in ins:
                    b.kind, b.succs = 'fall', [nxt]
                break
            a = nxt
        b.safe = lead in safe
        b.enterable = lead in enterable
        b.nop_cycles = nop_end.get(lead, 0)

    # Regions: from each head, follow direct successors that are not heads.
    regions = {}
    for h in sorted(heads):
        if h not in blocks:
            continue
        r = Region(h)
        regions[h] = r
        stack = [h]
        while stack:
            pc = stack.pop()
            b = blocks.get(pc)
            if b is None or b.region is not None:
                continue
            b.region = h
            r.blocks.append(b)
            for s in b.succs:
                if s not in heads:
                    stack.append(s)
        r.blocks.sort(key=lambda x: x.pc)

    # Blocks no head reaches (for example after an indirect jump): each starts a region of its own.
    for pc in sorted(blocks):
        b = blocks[pc]
        if b.region is None:
            r = Region(pc)
            regions[pc] = r
            stack = [pc]
            b.enterable = True
            while stack:
                x = stack.pop()
                c = blocks.get(x)
                if c is None or c.region is not None:
                    continue
                c.region = pc
                r.blocks.append(c)
                stack.extend(s for s in c.succs if s not in heads)
            r.blocks.sort(key=lambda x: x.pc)

    # A branch that leaves its region enters the other region's function at that block.
    for b in blocks.values():
        for s in b.succs:
            c = blocks.get(s)
            if c is not None and c.region != b.region:
                c.enterable = True
    return blocks, regions


def _set_kind(b, I, nxt, ins, disc):
    op = I.op
    if op == 'bra':
        b.kind, b.succs = 'goto', [I.target]
    elif op in ('bcc', 'dbcc'):
        b.kind, b.succs = 'cond', [I.target, nxt]
    elif op == 'bsr':
        b.kind, b.succs = 'call', [nxt]
    elif op == 'jsr':
        if I.src[0] in ('absl', 'absw', 'pcd'):
            b.kind, b.succs = 'call', [nxt]
        else:
            b.kind, b.succs = 'call_ind', [nxt]
    elif op == 'jmp':
        if I.src[0] in ('absl', 'absw', 'pcd'):
            b.kind, b.succs = 'goto', [I.src[1] & 0xFFFFFF]
        elif I.pc in disc.switches:
            b.kind, b.succs = 'switch', list(dict.fromkeys(disc.switches[I.pc][1]))
        else:
            b.kind, b.succs = 'jmp_ind', []
    elif op == 'rts':
        b.kind, b.succs = 'ret', []
    elif op in ('rte', 'rtr'):
        b.kind, b.succs = 'rte', []
    elif op in ('stop', 'illegal', 'trap'):
        b.kind, b.succs = 'stop', []
    else:
        b.kind, b.succs = 'fall', [nxt] if nxt in ins else []
