# Copyright (C) 2026 Marsupial Solutions LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Finds routines the program copies from ROM into RAM, so they can be translated at their RAM address.

The search looks for a copy loop in the discovered code:

    source register loaded with a constant ROM address
    target register loaded with a constant RAM address
    a move through both registers (indexed or post-incremented)
    a counted loop back: cmpi #N then bls or bcs, or dbra, close after the move

Each find becomes an overlay on the image. The translated code at that RAM address is only valid
once the copy has run, so the runtime checks the RAM against the source on the first entry and
watches the range for later writes (see DESIGN, "RAM code"). What this search does not find, the
fallback interpreter runs, and each use is logged.
"""
RAM_LOW = 0xE00000
BUS_MASK = 0xFFFFFF


def _reg_of(ea):
    if ea[0] in ('idx', 'post', 'ind'):
        return ea[1]
    return None


def _loop_count(disc, I):
    """The number of elements the loop around this move copies, or None if it is not clear."""
    a = I.pc + I.n
    for _ in range(6):
        J = disc.ins.get(a)
        if J is None:
            return None
        if J.op == 'cmpi' and J.src[0] == 'imm' and J.dst[0] == 'd':
            K = disc.ins.get(a + J.n)
            if K is not None and K.op == 'bcc' and K.target <= I.pc:
                if K.cc == 'ls':
                    return J.src[1] + 1
                if K.cc == 'cs':
                    return J.src[1]
            return None
        if J.op == 'dbcc' and J.cc == 'f' and J.target <= I.pc:
            n = _const_data_before(disc, I.pc, J.src[1])
            return None if n is None else (n & 0xFFFF) + 1
        a += J.n
    return None


def _const_data_before(disc, site, reg, limit=12):
    a_list = disc._order
    i = disc._index[site]
    for j in range(i - 1, max(i - limit, -1), -1):
        J = disc.ins[a_list[j]]
        if J.dst == ('d', reg):
            if J.op in ('moveq', 'move') and J.src and J.src[0] == 'imm':
                return J.src[1]
            return None
    return None


def find_copies(disc):
    disc.const_before(next(iter(disc.ins)), 0, limit=1)    # builds the address order
    disc._order = sorted(disc.ins)
    disc._index = {a: i for i, a in enumerate(disc._order)}
    found = []
    for a in disc._order:
        I = disc.ins[a]
        if I.op != 'move' or not I.src or not I.dst or I.sz < 2:
            continue
        rs, rd = _reg_of(I.src), _reg_of(I.dst)
        if rs is None or rd is None or rs == rd:
            continue
        src = disc.const_before(a, rs, limit=16)
        dst = disc.const_before(a, rd, limit=16)
        if src is None or dst is None or dst < RAM_LOW or not disc.img.in_rom(src):
            continue
        n = _loop_count(disc, I)
        if n is None or n <= 0:
            continue
        found.append({'site': a, 'ram': dst, 'source': src, 'length': n * I.sz})
    return found


def find_and_add(disc):
    if not disc.ins:
        return
    for c in find_copies(disc):
        if c not in disc.copies:
            disc.copies.append(c)
            disc.img.add_overlay(c['ram'], c['source'], c['length'], 'copied at boot')
