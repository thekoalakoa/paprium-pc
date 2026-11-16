# Copyright (C) 2026 Marsupial Solutions LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""A 68000 instruction decoder that returns structured operands.

decode(r16, pc) reads words through the callable r16(address) and returns an Insn, or raises
DecodeError when the words are not a legal 68000 instruction. Illegal addressing modes are
rejected on purpose: the code discovery pass uses that to stop when it walks into data.

Operand forms:
  ('d', n) ('a', n) ('ind', n) ('post', n) ('pre', n) ('disp', n, d16)
  ('idx', n, d8, ('d'|'a', m), 2|4)
  ('absw', address) ('absl', address)        the address is already sign extended to 32 bits
  ('pcd', address)                           program counter relative, already resolved
  ('pci', base, d8, ('d'|'a', m), 2|4)       base is the address of the extension word
  ('imm', value)                             masked to the operand size
  ('sr',) ('ccr',) ('usp',)

Insn.sz is the operand size in bytes (1, 2, 4) or 0 when it does not apply.
Condition codes use the names in CC. Branch and dbcc targets are in Insn.target.
For movem, Insn.regs lists the registers in d0..d7, a0..a7 order and Insn.mdir says the direction.
"""

CC = ['t', 'f', 'hi', 'ls', 'cc', 'cs', 'ne', 'eq', 'vc', 'vs', 'pl', 'mi', 'ge', 'lt', 'gt', 'le']

# Addressing mode classes, as bit sets over the twelve modes.
_DN, _AN, _IND, _POST, _PRE, _DISP, _IDX, _ABSW, _ABSL, _PCD, _PCI, _IMM = (1 << i for i in range(12))
ALL = (1 << 12) - 1
DATA = ALL & ~_AN
MEM = DATA & ~_DN
ALT = _DN | _AN | _IND | _POST | _PRE | _DISP | _IDX | _ABSW | _ABSL
DALT = ALT & ~_AN
MALT = ALT & ~(_AN | _DN)
CTRL = _IND | _DISP | _IDX | _ABSW | _ABSL | _PCD | _PCI
CALT = _IND | _DISP | _IDX | _ABSW | _ABSL


class DecodeError(Exception):
    pass


class Insn:
    __slots__ = ('pc', 'n', 'op', 'sz', 'src', 'dst', 'cc', 'target', 'regs', 'mdir')

    def __init__(self, pc):
        self.pc = pc
        self.n = 0
        self.op = None
        self.sz = 0
        self.src = None
        self.dst = None
        self.cc = None
        self.target = None
        self.regs = None
        self.mdir = None

    def __repr__(self):
        return '%06X %s.%s %s,%s' % (self.pc, self.op, self.sz, self.src, self.dst)


class _Reader:
    def __init__(self, r16, pc):
        self.r16 = r16
        self.p = pc
        self.pc = pc

    def w(self):
        v = self.r16(self.p)
        self.p += 2
        return v

    def sw(self):
        v = self.w()
        return v - 0x10000 if v & 0x8000 else v

    def l(self):
        h = self.w()
        return (h << 16) | self.w()

    def _brief(self):
        base = self.p
        e = self.w()
        if e & 0x0700:              # the 68020 full format, or scale bits a 68000 program never sets
            raise DecodeError('extension word form')
        d8 = e & 0xFF
        if d8 >= 0x80:
            d8 -= 0x100
        return base, d8, ('a' if e & 0x8000 else 'd', (e >> 12) & 7), 4 if e & 0x0800 else 2

    def ea(self, mode, reg, sz, allowed=ALL):
        if mode <= 6:
            bit = 1 << mode
        elif reg <= 4:
            bit = 1 << (7 + reg)
        else:
            raise DecodeError('bad addressing mode')
        if not allowed & bit:
            raise DecodeError('addressing mode not allowed here')
        if mode == 0:
            return ('d', reg)
        if mode == 1:
            return ('a', reg)
        if mode == 2:
            return ('ind', reg)
        if mode == 3:
            return ('post', reg)
        if mode == 4:
            return ('pre', reg)
        if mode == 5:
            return ('disp', reg, self.sw())
        if mode == 6:
            _, d8, ix, isz = self._brief()
            return ('idx', reg, d8, ix, isz)
        if reg == 0:
            return ('absw', self.sw() & 0xFFFFFFFF)
        if reg == 1:
            return ('absl', self.l())
        if reg == 2:
            base = self.p
            return ('pcd', (base + self.sw()) & 0xFFFFFFFF)
        if reg == 3:
            base, d8, ix, isz = self._brief()
            return ('pci', base, d8, ix, isz)
        if sz == 1:
            v = self.w()
            if (v & 0xFF00) not in (0, 0xFF00):     # assemblers write a negative byte as a negative word
                raise DecodeError('byte immediate with a high byte')
            return ('imm', v & 0xFF)
        if sz == 2:
            return ('imm', self.w())
        return ('imm', self.l())


_SZ = (1, 2, 4)


def decode(r16, pc):
    if pc & 1:
        raise DecodeError('odd address')
    d = _Reader(r16, pc)
    I = Insn(pc)
    op = d.w()
    _dispatch(d, I, op)
    I.n = d.p - pc
    return I


def _simple_regs(d, I, op, rr, reg, mem_form, name):
    I.op = name
    I.sz = _SZ[(op >> 6) & 3]
    if mem_form:
        I.src, I.dst = ('pre', reg), ('pre', rr)
    else:
        I.src, I.dst = ('d', reg), ('d', rr)


def _dispatch(d, I, op):
    hi = op >> 12
    mode, reg = (op >> 3) & 7, op & 7
    rr = (op >> 9) & 7
    om = (op >> 6) & 7
    s = (op >> 6) & 3

    if hi == 0:
        if op & 0x0100:
            if mode == 1:
                I.op = 'movep'
                I.sz = 4 if op & 0x40 else 2
                if op & 0x80:
                    I.src, I.dst = ('d', rr), ('disp', reg, d.sw())
                else:
                    I.src, I.dst = ('disp', reg, d.sw()), ('d', rr)
                return
            kind = s
            I.op = ('btst', 'bchg', 'bclr', 'bset')[kind]
            I.sz = 4 if mode == 0 else 1
            I.src = ('d', rr)
            I.dst = d.ea(mode, reg, 1, DATA if kind == 0 else DALT)
            return
        sub = rr
        if sub == 4:
            kind = s
            I.op = ('btst', 'bchg', 'bclr', 'bset')[kind]
            I.sz = 4 if mode == 0 else 1
            ext = d.w()
            if ext & 0xFF00:
                raise DecodeError('static bit number with a high byte')
            I.src = ('imm', ext)
            I.dst = d.ea(mode, reg, 1, (DATA & ~_IMM) if kind == 0 else DALT)
            return
        names = {0: 'ori', 1: 'andi', 2: 'subi', 3: 'addi', 5: 'eori', 6: 'cmpi'}
        if sub not in names or s == 3:
            raise DecodeError('line 0')
        I.op = names[sub]
        if sub in (0, 1, 5) and (op & 0xFF) == 0x3C:
            I.sz = 1
            I.src = d.ea(7, 4, 1)
            I.dst = ('ccr',)
            return
        if sub in (0, 1, 5) and (op & 0xFF) == 0x7C:
            I.sz = 2
            I.src = d.ea(7, 4, 2)
            I.dst = ('sr',)
            return
        I.sz = _SZ[s]
        I.src = d.ea(7, 4, I.sz)
        I.dst = d.ea(mode, reg, I.sz, DALT)
        return

    if hi in (1, 2, 3):
        I.sz = {1: 1, 3: 2, 2: 4}[hi]
        I.src = d.ea(mode, reg, I.sz, ALL if I.sz > 1 else ALL & ~_AN)
        if om == 1:
            if I.sz == 1:
                raise DecodeError('movea.b')
            I.op = 'movea'
            I.dst = ('a', rr)
        else:
            I.op = 'move'
            I.dst = d.ea(om, rr, I.sz, DALT)
        return

    if hi == 4:
        fixed = {0x4E71: 'nop', 0x4E75: 'rts', 0x4E73: 'rte', 0x4E70: 'reset', 0x4E76: 'trapv',
                 0x4E77: 'rtr', 0x4AFC: 'illegal'}
        if op in fixed:
            I.op = fixed[op]
            return
        if op == 0x4E72:
            I.op = 'stop'
            I.src = ('imm', d.w())
            return
        if (op & 0xFFF0) == 0x4E40:
            I.op = 'trap'
            I.src = ('imm', op & 15)
            return
        if (op & 0xFFF8) == 0x4E50:
            I.op = 'link'
            I.sz = 2
            I.dst = ('a', reg)
            I.src = ('imm', d.sw() & 0xFFFFFFFF)
            return
        if (op & 0xFFF8) == 0x4E58:
            I.op = 'unlk'
            I.dst = ('a', reg)
            return
        if (op & 0xFFF0) == 0x4E60:
            I.op = 'move'
            I.sz = 4
            if op & 8:
                I.src, I.dst = ('usp',), ('a', reg)
            else:
                I.src, I.dst = ('a', reg), ('usp',)
            return
        if (op & 0xFFC0) == 0x4E80:
            I.op = 'jsr'
            I.src = d.ea(mode, reg, 4, CTRL)
            return
        if (op & 0xFFC0) == 0x4EC0:
            I.op = 'jmp'
            I.src = d.ea(mode, reg, 4, CTRL)
            return
        if (op & 0xFFC0) == 0x40C0:
            I.op = 'move'
            I.sz = 2
            I.src = ('sr',)
            I.dst = d.ea(mode, reg, 2, DALT)
            return
        if (op & 0xFFC0) == 0x44C0:
            I.op = 'move'
            I.sz = 2
            I.src = d.ea(mode, reg, 2, DATA)
            I.dst = ('ccr',)
            return
        if (op & 0xFFC0) == 0x46C0:
            I.op = 'move'
            I.sz = 2
            I.src = d.ea(mode, reg, 2, DATA)
            I.dst = ('sr',)
            return
        if (op & 0xFFF8) == 0x4840:
            I.op = 'swap'
            I.sz = 4
            I.dst = ('d', reg)
            return
        if (op & 0xFFF8) == 0x4880:
            I.op = 'ext'
            I.sz = 2
            I.dst = ('d', reg)
            return
        if (op & 0xFFF8) == 0x48C0:
            I.op = 'ext'
            I.sz = 4
            I.dst = ('d', reg)
            return
        if (op & 0xFFC0) == 0x4840:
            I.op = 'pea'
            I.sz = 4
            I.src = d.ea(mode, reg, 4, CTRL)
            return
        if (op & 0xFFC0) == 0x4800:
            I.op = 'nbcd'
            I.sz = 1
            I.dst = d.ea(mode, reg, 1, DALT)
            return
        if (op & 0xFB80) == 0x4880:
            I.op = 'movem'
            I.sz = 4 if op & 0x40 else 2
            mask = d.w()
            to_regs = bool(op & 0x0400)
            ea = d.ea(mode, reg, I.sz, (CTRL | _POST) if to_regs else (CALT | _PRE))
            regs = []
            for k in range(16):
                bit = (15 - k) if ea[0] == 'pre' else k
                if mask & (1 << bit):
                    regs.append(('d' if k < 8 else 'a', k & 7))
            I.regs = regs
            if to_regs:
                I.mdir = 'm2r'
                I.src = ea
            else:
                I.mdir = 'r2m'
                I.dst = ea
            return
        if (op & 0xF1C0) == 0x41C0:
            I.op = 'lea'
            I.sz = 4
            I.src = d.ea(mode, reg, 4, CTRL)
            I.dst = ('a', rr)
            return
        if (op & 0xF1C0) == 0x4180:
            I.op = 'chk'
            I.sz = 2
            I.src = d.ea(mode, reg, 2, DATA)
            I.dst = ('d', rr)
            return
        if (op & 0xFFC0) == 0x4AC0:
            I.op = 'tas'
            I.sz = 1
            I.dst = d.ea(mode, reg, 1, DALT)
            return
        names = {0x40: 'negx', 0x42: 'clr', 0x44: 'neg', 0x46: 'not', 0x4A: 'tst'}
        k = op >> 8
        if k in names and s != 3:
            I.op = names[k]
            I.sz = _SZ[s]
            I.dst = d.ea(mode, reg, I.sz, DALT)
            return
        raise DecodeError('line 4')

    if hi == 5:
        if s == 3:
            I.cc = CC[(op >> 8) & 15]
            if mode == 1:
                I.op = 'dbcc'
                I.src = ('d', reg)
                base = d.p
                I.target = (base + d.sw()) & 0xFFFFFFFF
                if I.target & 1:
                    raise DecodeError('odd branch target')
                return
            I.op = 'scc'
            I.sz = 1
            I.dst = d.ea(mode, reg, 1, DALT)
            return
        I.op = 'subq' if op & 0x100 else 'addq'
        I.sz = _SZ[s]
        I.src = ('imm', rr or 8)
        I.dst = d.ea(mode, reg, I.sz, ALT if I.sz > 1 else DALT)
        return

    if hi == 6:
        c = (op >> 8) & 15
        disp = op & 0xFF
        base = d.p
        if disp == 0xFF:
            raise DecodeError('32-bit branch')
        if disp == 0:
            disp = d.sw()
        elif disp >= 0x80:
            disp -= 0x100
        I.target = (base + disp) & 0xFFFFFFFF
        if I.target & 1:
            raise DecodeError('odd branch target')
        if c == 0:
            I.op = 'bra'
        elif c == 1:
            I.op = 'bsr'
        else:
            I.op = 'bcc'
            I.cc = CC[c]
        return

    if hi == 7:
        if op & 0x100:
            raise DecodeError('moveq')
        v = op & 0xFF
        if v >= 0x80:
            v -= 0x100
        I.op = 'moveq'
        I.sz = 4
        I.src = ('imm', v & 0xFFFFFFFF)
        I.dst = ('d', rr)
        return

    if hi == 8:
        if om == 3 or om == 7:
            I.op = 'divu' if om == 3 else 'divs'
            I.sz = 2
            I.src = d.ea(mode, reg, 2, DATA)
            I.dst = ('d', rr)
            return
        if (op & 0x1F0) == 0x100:
            I.op = 'sbcd'
            I.sz = 1
            I.src, I.dst = (('pre', reg), ('pre', rr)) if mode & 1 else (('d', reg), ('d', rr))
            return
        I.op = 'or'
        I.sz = _SZ[s]
        if om < 3:
            I.src = d.ea(mode, reg, I.sz, DATA)
            I.dst = ('d', rr)
        else:
            I.src = ('d', rr)
            I.dst = d.ea(mode, reg, I.sz, MALT)
        return

    if hi in (9, 0xD):
        base = 'sub' if hi == 9 else 'add'
        if om == 3 or om == 7:
            I.op = base + 'a'
            I.sz = 2 if om == 3 else 4
            I.src = d.ea(mode, reg, I.sz)
            I.dst = ('a', rr)
            return
        I.sz = _SZ[s]
        if om < 3:
            I.op = base
            I.src = d.ea(mode, reg, I.sz, ALL if I.sz > 1 else ALL & ~_AN)
            I.dst = ('d', rr)
            return
        if mode in (0, 1):
            _simple_regs(d, I, op, rr, reg, mode == 1, base + 'x')
            return
        I.op = base
        I.src = ('d', rr)
        I.dst = d.ea(mode, reg, I.sz, MALT)
        return

    if hi == 0xB:
        if om == 3 or om == 7:
            I.op = 'cmpa'
            I.sz = 2 if om == 3 else 4
            I.src = d.ea(mode, reg, I.sz)
            I.dst = ('a', rr)
            return
        I.sz = _SZ[s]
        if om < 3:
            I.op = 'cmp'
            I.src = d.ea(mode, reg, I.sz, ALL if I.sz > 1 else ALL & ~_AN)
            I.dst = ('d', rr)
            return
        if mode == 1:
            I.op = 'cmpm'
            I.src, I.dst = ('post', reg), ('post', rr)
            return
        I.op = 'eor'
        I.src = ('d', rr)
        I.dst = d.ea(mode, reg, I.sz, DALT)
        return

    if hi == 0xC:
        if om == 3 or om == 7:
            I.op = 'mulu' if om == 3 else 'muls'
            I.sz = 2
            I.src = d.ea(mode, reg, 2, DATA)
            I.dst = ('d', rr)
            return
        if (op & 0x1F0) == 0x100:
            I.op = 'abcd'
            I.sz = 1
            I.src, I.dst = (('pre', reg), ('pre', rr)) if mode & 1 else (('d', reg), ('d', rr))
            return
        x = op & 0x1F8
        if x in (0x140, 0x148, 0x188):
            I.op = 'exg'
            I.sz = 4
            I.src = ('a' if x == 0x148 else 'd', rr)
            I.dst = ('d' if x == 0x140 else 'a', reg)
            return
        I.op = 'and'
        I.sz = _SZ[s]
        if om < 3:
            I.src = d.ea(mode, reg, I.sz, DATA)
            I.dst = ('d', rr)
        else:
            I.src = ('d', rr)
            I.dst = d.ea(mode, reg, I.sz, MALT)
        return

    if hi == 0xE:
        left = bool(op & 0x100)
        if s == 3:
            if op & 0x800:
                raise DecodeError('bit field or 68020 form')
            I.op = ('as', 'ls', 'rox', 'ro')[rr] + ('l' if left else 'r')
            I.sz = 2
            I.src = ('imm', 1)
            I.dst = d.ea(mode, reg, 2, MALT)
            return
        I.op = ('as', 'ls', 'rox', 'ro')[(op >> 3) & 3] + ('l' if left else 'r')
        I.sz = _SZ[s]
        I.src = ('d', rr) if op & 0x20 else ('imm', rr or 8)
        I.dst = ('d', reg)
        return

    raise DecodeError('line A or line F')
