# Copyright (C) 2026 Marsupial Solutions LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""A tiny 68000 assembler for test programs we write ourselves. No game code is involved.

Operands: d(n), a(n), ind(n), post(n), pre(n), disp(n, d), idx(n, d8, 'd'|'a', m, 2|4),
absw(x), absl(x), imm(x), pcd(label).
"""
SZ = {1: 0, 2: 1, 4: 2}
MOVE_SZ = {1: 1, 2: 3, 4: 2}
CC = {'t': 0, 'f': 1, 'hi': 2, 'ls': 3, 'cc': 4, 'cs': 5, 'ne': 6, 'eq': 7, 'vc': 8, 'vs': 9, 'pl': 10, 'mi': 11,
      'ge': 12, 'lt': 13, 'gt': 14, 'le': 15}


def d(n): return ('d', n)
def a(n): return ('a', n)
def ind(n): return ('ind', n)
def post(n): return ('post', n)
def pre(n): return ('pre', n)
def disp(n, x): return ('disp', n, x)
def idx(n, x, k, m, s): return ('idx', n, x, k, m, s)
def absw(x): return ('absw', x)
def absl(x): return ('absl', x)
def imm(x): return ('imm', x)


class Asm:
    def __init__(self, base):
        self.base = base
        self.w = []
        self.labels = {}
        self.fix = []

    def here(self):
        return self.base + 2 * len(self.w)

    def label(self, name):
        self.labels[name] = self.here()

    def words(self, *v):
        self.w.extend(x & 0xFFFF for x in v)

    def long(self, v):
        self.words(v >> 16, v)

    def _ea(self, e, sz):
        """Returns (mode, reg, extension words)."""
        k = e[0]
        if k == 'd': return 0, e[1], []
        if k == 'a': return 1, e[1], []
        if k == 'ind': return 2, e[1], []
        if k == 'post': return 3, e[1], []
        if k == 'pre': return 4, e[1], []
        if k == 'disp': return 5, e[1], [e[2] & 0xFFFF]
        if k == 'idx':
            return 6, e[1], [((0x8000 if e[3] == 'a' else 0) | (e[4] << 12) | (0x800 if e[5] == 4 else 0) | (e[2] & 0xFF))]
        if k == 'absw': return 7, 0, [e[1] & 0xFFFF]
        if k == 'absl': return 7, 1, [(e[1] >> 16) & 0xFFFF, e[1] & 0xFFFF]
        if k == 'imm':
            if sz == 4: return 7, 4, [(e[1] >> 16) & 0xFFFF, e[1] & 0xFFFF]
            return 7, 4, [e[1] & (0xFF if sz == 1 else 0xFFFF)]
        raise ValueError(e)

    def _one(self, base, e, sz):
        m, r, x = self._ea(e, sz)
        self.words(base | (m << 3) | r, *x)

    def move(self, sz, s, t):
        ms, rs, xs = self._ea(s, sz)
        mt, rt, xt = self._ea(t, sz)
        self.words((MOVE_SZ[sz] << 12) | (rt << 9) | (mt << 6) | (ms << 3) | rs, *(xs + xt))

    def moveq(self, v, n): self.words(0x7000 | (n << 9) | (v & 0xFF))
    def lea(self, s, n): self._one(0x41C0 | (n << 9), s, 4)
    def pea(self, s): self._one(0x4840, s, 4)
    def clr(self, sz, e): self._one(0x4200 | (SZ[sz] << 6), e, sz)
    def neg(self, sz, e): self._one(0x4400 | (SZ[sz] << 6), e, sz)
    def not_(self, sz, e): self._one(0x4600 | (SZ[sz] << 6), e, sz)
    def tst(self, sz, e): self._one(0x4A00 | (SZ[sz] << 6), e, sz)
    def swap(self, n): self.words(0x4840 | n)
    def ext(self, sz, n): self.words((0x4880 if sz == 2 else 0x48C0) | n)
    def nop(self): self.words(0x4E71)
    def rts(self): self.words(0x4E75)
    def rte(self): self.words(0x4E73)
    def link(self, n, x): self.words(0x4E50 | n, x)
    def unlk(self, n): self.words(0x4E58 | n)
    def jsr(self, e): self._one(0x4E80, e, 4)
    def jmp(self, e): self._one(0x4EC0, e, 4)

    def arith(self, name, sz, s, n):
        """add, sub, cmp, and, or with a data register destination."""
        base = {'add': 0xD000, 'sub': 0x9000, 'cmp': 0xB000, 'and': 0xC000, 'or': 0x8000}[name]
        self._one(base | (n << 9) | (SZ[sz] << 6), s, sz)

    def arith_to_mem(self, name, sz, n, e):
        base = {'add': 0xD100, 'sub': 0x9100, 'and': 0xC100, 'or': 0x8100, 'eor': 0xB100}[name]
        self._one(base | (n << 9) | (SZ[sz] << 6), e, sz)

    def adda(self, sz, s, n): self._one(0xD0C0 | (n << 9) | (0x100 if sz == 4 else 0), s, sz)
    def suba(self, sz, s, n): self._one(0x90C0 | (n << 9) | (0x100 if sz == 4 else 0), s, sz)
    def cmpa(self, sz, s, n): self._one(0xB0C0 | (n << 9) | (0x100 if sz == 4 else 0), s, sz)

    def immop(self, name, sz, v, e):
        base = {'ori': 0x0000, 'andi': 0x0200, 'subi': 0x0400, 'addi': 0x0600, 'eori': 0x0A00, 'cmpi': 0x0C00}[name]
        m, r, x = self._ea(e, sz)
        iw = [(v >> 16) & 0xFFFF, v & 0xFFFF] if sz == 4 else [v & (0xFF if sz == 1 else 0xFFFF)]
        self.words(base | (SZ[sz] << 6) | (m << 3) | r, *(iw + x))

    def addq(self, sz, v, e): self._one(0x5000 | ((v & 7) << 9) | (SZ[sz] << 6), e, sz)
    def subq(self, sz, v, e): self._one(0x5100 | ((v & 7) << 9) | (SZ[sz] << 6), e, sz)

    def shift(self, kind, sz, count, n):
        """kind in asl asr lsl lsr rol ror; count is imm(1..8) or d(m)."""
        t = {'as': 0, 'ls': 1, 'ro': 3}[kind[:2]]
        left = 0x100 if kind[2] == 'l' else 0
        if count[0] == 'imm':
            self.words(0xE000 | ((count[1] & 7) << 9) | left | (SZ[sz] << 6) | (t << 3) | n)
        else:
            self.words(0xE020 | (count[1] << 9) | left | (SZ[sz] << 6) | (t << 3) | n)

    def bit(self, kind, count, e):
        k = {'btst': 0, 'bchg': 1, 'bclr': 2, 'bset': 3}[kind]
        m, r, x = self._ea(e, 1)
        if count[0] == 'imm':
            self.words(0x0800 | (k << 6) | (m << 3) | r, count[1], *x)
        else:
            self.words(0x0100 | (count[1] << 9) | (k << 6) | (m << 3) | r, *x)

    def mulu(self, s, n): self._one(0xC0C0 | (n << 9), s, 2)
    def muls(self, s, n): self._one(0xC1C0 | (n << 9), s, 2)
    def divu(self, s, n): self._one(0x80C0 | (n << 9), s, 2)
    def divs(self, s, n): self._one(0x81C0 | (n << 9), s, 2)
    def scc(self, cc, e): self._one(0x50C0 | (CC[cc] << 8), e, 1)
    def exg_dd(self, x, y): self.words(0xC140 | (x << 9) | y)
    def exg_aa(self, x, y): self.words(0xC148 | (x << 9) | y)
    def exg_da(self, x, y): self.words(0xC188 | (x << 9) | y)
    def move_to_sr(self, e): self._one(0x46C0, e, 2)
    def move_to_ccr(self, e): self._one(0x44C0, e, 2)
    def move_from_sr(self, e): self._one(0x40C0, e, 2)

    def movem_to_mem(self, sz, mask, e):
        m, r, x = self._ea(e, sz)
        self.words(0x4880 | (0x40 if sz == 4 else 0) | (m << 3) | r, mask, *x)

    def movem_to_regs(self, sz, mask, e):
        m, r, x = self._ea(e, sz)
        self.words(0x4C80 | (0x40 if sz == 4 else 0) | (m << 3) | r, mask, *x)

    def branch(self, op, label):
        """A 16-bit branch. op is bra, bsr or a condition name."""
        c = {'bra': 0, 'bsr': 1}.get(op, CC.get(op))
        self.words(0x6000 | (c << 8), 0)
        self.fix.append((len(self.w) - 1, label, 'rel'))

    def dbra(self, n, label):
        self.words(0x51C8 | n, 0)
        self.fix.append((len(self.w) - 1, label, 'rel'))

    def jsr_label(self, label):
        self.words(0x4EB9, 0, 0)
        self.fix.append((len(self.w) - 2, label, 'abs'))

    def jmp_label(self, label):
        self.words(0x4EF9, 0, 0)
        self.fix.append((len(self.w) - 2, label, 'abs'))

    def lea_label(self, label, n):
        self.words(0x41F9 | (n << 9), 0, 0)
        self.fix.append((len(self.w) - 2, label, 'abs'))

    def dc_long_label(self, label):
        self.words(0, 0)
        self.fix.append((len(self.w) - 2, label, 'abs'))

    def bytes(self):
        for i, label, kind in self.fix:
            t = self.labels[label]
            if kind == 'rel':
                self.w[i] = (t - (self.base + 2 * i)) & 0xFFFF
            else:
                self.w[i] = (t >> 16) & 0xFFFF
                self.w[i + 1] = t & 0xFFFF
        return b''.join(x.to_bytes(2, 'big') for x in self.w)


def image(code_asm, stack=0xFFFF00, reset=None, vectors=None, size=0x10000):
    """A whole test ROM: vector table, then the code at its base address."""
    code = code_asm.bytes()
    rom = bytearray(size)
    rom[0:4] = stack.to_bytes(4, 'big')
    rom[4:8] = (reset if reset is not None else code_asm.base).to_bytes(4, 'big')
    # every other vector points at a stop word, so the table shape is valid for discovery
    stop_at = 0x100
    rom[stop_at:stop_at + 2] = (0x4E73).to_bytes(2, 'big')
    for v in range(2, 64):
        rom[4 * v:4 * v + 4] = stop_at.to_bytes(4, 'big')
    for v, t in (vectors or {}).items():
        rom[4 * v:4 * v + 4] = t.to_bytes(4, 'big')
    rom[code_asm.base:code_asm.base + len(code)] = code
    return bytes(rom)
