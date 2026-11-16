# Copyright (C) 2026 Marsupial Solutions LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Lifts 68000 instructions to the intermediate form, one basic block at a time.

The intermediate form is a list of small operations on 32-bit values. A value is
    ('d', n) ('a', n)   a 68000 register
    ('t', n)            a temporary, alive within one instruction
    ('i', v)            a constant

Operations:
    ('mov', dst, a)
    ('bin', 'add'|'sub'|'and'|'or'|'xor'|'mul', dst, a, b)
    ('un', 'not'|'zx8'|'zx16'|'sx8'|'sx16'|'rot16', dst, a)
    ('ld', size, dst, addr, cls)    size in bytes; cls is 'any', 'ram' or 'img' (program counter relative)
    ('st', size, addr, val, cls)
    ('setlo', size, reg, val)       write the low 1 or 2 bytes of a data register
    ('shift', kind, size, dst, a, count)    also sets the local C and V flags
    ('div', 'u'|'s', reg, src, pc)  also sets the local flags
    ('fl', flag, kind, size, a, b, r)   compute one flag; kind is n, z, zbit, vadd, cadd, vsub, csub
    ('flc', flag, value)            set one flag to a constant
    ('setcc', dst, cc)
    ('sr_read', dst) ('sr_write', a) ('ccr_write', a) ('usp_get', dst) ('usp_set', a)

Each block ends in one terminator, see lift_block. Flags are computed only where the analysis in
flagsopt.py says something reads them. Register values are always kept whole and exact.

An instruction that is not handled raises Unlifted. Instructions that read the X flag are refused
on purpose: the program this tool targets has none, and X is not kept.
"""
from . import flagsopt
from .flagsopt import N, Z, V, C

BUS_MASK = 0xFFFFFF
RAM_LOW = 0xE00000
_M = {1: 0xFF, 2: 0xFFFF, 4: 0xFFFFFFFF}


class Unlifted(Exception):
    pass


class BlockIR:
    __slots__ = ('block', 'ops', 'term', 'ntemps', 'gen', 'defined', 'starts')

    def __init__(self, block):
        self.block = block
        self.ops = []
        self.term = None
        self.ntemps = 0
        self.gen = 0            # flags read here before being written here
        self.defined = 0        # flags fully written here
        self.starts = []        # for each instruction, the index in ops of its first operation


class _L:
    def __init__(self, blockir, disc):
        self.b = blockir
        self.ops = blockir.ops
        self.disc = disc
        self.nt = 0
        self.live = 0
        self.I = None

    # ---- values
    def t(self):
        v = ('t', self.nt)
        self.nt += 1
        if self.nt > self.b.ntemps:
            self.b.ntemps = self.nt
        return v

    def e(self, *op):
        self.ops.append(op)

    def mov(self, a):
        d = self.t()
        self.e('mov', d, a)
        return d

    def bin(self, op, a, b):
        d = self.t()
        self.e('bin', op, d, a, b)
        return d

    def un(self, op, a):
        d = self.t()
        self.e('un', op, d, a)
        return d

    # ---- memory
    @staticmethod
    def _cls(addr):
        if addr[0] == 'i' and (addr[1] & BUS_MASK) >= RAM_LOW:
            return 'ram'
        return 'any'

    def ld(self, sz, addr, cls=None):
        d = self.t()
        self.e('ld', sz, d, addr, cls or self._cls(addr))
        return d

    def st(self, sz, addr, val):
        self.e('st', sz, addr, val, self._cls(addr))

    def push32(self, val):
        self.e('bin', 'sub', ('a', 7), ('a', 7), ('i', 4))
        self.st(4, ('a', 7), val)

    def pop32(self):
        v = self.ld(4, ('a', 7))
        self.e('bin', 'add', ('a', 7), ('a', 7), ('i', 4))
        return v

    # ---- effective addresses
    def index(self, ix, isz):
        r = ix[0], ix[1]
        return self.un('sx16', r) if isz == 2 else r

    def addr_of(self, ea, sz):
        k = ea[0]
        if k == 'ind':
            return ('a', ea[1])
        if k == 'post':
            inc = 2 if sz == 1 and ea[1] == 7 else sz
            v = self.mov(('a', ea[1]))
            self.e('bin', 'add', ('a', ea[1]), ('a', ea[1]), ('i', inc))
            return v
        if k == 'pre':
            inc = 2 if sz == 1 and ea[1] == 7 else sz
            self.e('bin', 'sub', ('a', ea[1]), ('a', ea[1]), ('i', inc))
            return self.mov(('a', ea[1]))
        if k == 'disp':
            return self.bin('add', ('a', ea[1]), ('i', ea[2] & 0xFFFFFFFF))
        if k == 'idx':
            ix = self.index(ea[3], ea[4])
            return self.bin('add', self.bin('add', ('a', ea[1]), ix), ('i', ea[2] & 0xFFFFFFFF))
        if k in ('absw', 'absl', 'pcd'):
            return ('i', ea[1] & 0xFFFFFFFF)
        if k == 'pci':
            ix = self.index(ea[3], ea[4])
            return self.bin('add', ix, ('i', (ea[1] + ea[2]) & 0xFFFFFFFF))
        raise Unlifted('address of %s' % (ea,))

    def loc(self, ea, sz):
        """A place to read and write once. Side effects of the addressing mode happen here, once."""
        if ea[0] in ('d', 'a'):
            return ('reg', ea)
        return ('mem', self.addr_of(ea, sz), 'img' if ea[0] in ('pcd', 'pci') else None)

    def get(self, loc, sz):
        if loc[0] == 'reg':
            r = loc[1]
            if sz == 4:
                return r
            return self.un('zx8' if sz == 1 else 'zx16', r)
        return self.ld(sz, loc[1], loc[2])

    def put(self, loc, sz, val):
        if loc[0] == 'reg':
            r = loc[1]
            if sz == 4 or r[0] == 'a':
                self.e('mov', r, val)
            else:
                self.e('setlo', sz, r, val)
            return
        self.st(sz, loc[1], val)

    def read(self, ea, sz):
        if ea[0] == 'imm':
            return ('i', ea[1] & _M[sz])
        return self.get(self.loc(ea, sz), sz)

    def write(self, ea, sz, val):
        self.put(self.loc(ea, sz), sz, val)

    # ---- flags
    def fl(self, flag, kind, sz, a=('i', 0), b=('i', 0), r=('i', 0)):
        if self.live & flag:
            self.e('fl', flag, kind, sz * 8, a, b, r)

    def flc(self, flag, value):
        if self.live & flag:
            self.e('flc', flag, value)

    def logic(self, sz, r):
        self.fl(N, 'n', sz, r=r)
        self.fl(Z, 'z', sz, r=r)
        self.flc(V, 0)
        self.flc(C, 0)

    def add_flags(self, sz, a, b, r):
        self.fl(N, 'n', sz, r=r)
        self.fl(Z, 'z', sz, r=r)
        self.fl(V, 'vadd', sz, a, b, r)
        self.fl(C, 'cadd', sz, a, b, r)

    def sub_flags(self, sz, a, b, r):
        self.fl(N, 'n', sz, r=r)
        self.fl(Z, 'z', sz, r=r)
        self.fl(V, 'vsub', sz, a, b, r)
        self.fl(C, 'csub', sz, a, b, r)

    # ---- instructions
    def insn(self, I):
        self.I = I
        self.nt = 0
        op = I.op
        h = getattr(self, 'i_' + op, None)
        if h is None:
            raise Unlifted(op)
        h(I)

    def i_nop(self, I):
        pass

    def i_move(self, I):
        s, d = I.src, I.dst
        if s == ('sr',):
            t = self.t()
            self.e('sr_read', t)
            self.write(d, 2, t)
            return
        if s == ('usp',):
            t = self.t()
            self.e('usp_get', t)
            self.write(d, 4, t)
            return
        if d == ('usp',):
            self.e('usp_set', s)
            return
        if d == ('sr',):
            self.e('sr_write', self.read(s, 2))
            return
        if d == ('ccr',):
            self.e('ccr_write', self.read(s, 2))
            return
        v = self.read(s, I.sz)
        if I.sz == 4 and d[0] in ('pre', 'post') and v[0] == 'a':
            v = self.mov(v)                 # the source is the value from before the destination's step
        if I.sz == 4 and d[0] == 'pre':     # the reference writes a long to -(An) low word first
            addr = self.addr_of(d, 4)
            self.st(2, self.bin('add', addr, ('i', 2)), self.un('zx16', v))
            self.st(2, addr, self.un('zx16', self.un('rot16', v)))
        else:
            self.write(d, I.sz, v)
        self.logic(I.sz, v)

    def i_moveq(self, I):
        self.e('mov', I.dst, ('i', I.src[1]))
        self.logic(4, I.dst)

    def i_movea(self, I):
        v = self.read(I.src, I.sz)
        if I.sz == 2:
            v = self.un('sx16', v)
        self.e('mov', I.dst, v)

    def i_lea(self, I):
        self.e('mov', I.dst, self.addr_of(I.src, 4))

    def i_pea(self, I):
        v = self.addr_of(I.src, 4)
        if v[0] == 'a':         # a copy: pea (a7) pushes the stack pointer from before the push
            v = self.mov(v)
        self.push32(v)

    def i_clr(self, I):
        self.write(I.dst, I.sz, ('i', 0))
        self.flc(N, 0)
        self.flc(Z, 1)
        self.flc(V, 0)
        self.flc(C, 0)

    def i_tst(self, I):
        self.logic(I.sz, self.read(I.dst, I.sz))

    def i_tas(self, I):
        # On this machine the write half of the bus cycle does not reach memory; a register does change.
        loc = self.loc(I.dst, 1)
        v = self.get(loc, 1)
        self.logic(1, v)
        if loc[0] == 'reg':
            self.put(loc, 1, self.bin('or', v, ('i', 0x80)))

    def _addsub(self, I, name):
        if I.dst[0] == 'a':                 # addq / subq on an address register: whole register, no flags
            self.e('bin', name, I.dst, I.dst, ('i', I.src[1]))
            return
        b = self.read(I.src, I.sz)
        loc = self.loc(I.dst, I.sz)
        a = self.get(loc, I.sz)
        r = self.bin(name, a, b)
        # Flags first: for a long the operand 'a' is the destination register itself.
        (self.add_flags if name == 'add' else self.sub_flags)(I.sz, a, b, r)
        self.put(loc, I.sz, r)

    def i_add(self, I):
        self._addsub(I, 'add')

    i_addi = i_add
    i_addq = i_add

    def i_sub(self, I):
        self._addsub(I, 'sub')

    i_subi = i_sub
    i_subq = i_sub

    def _addr_arith(self, I, name):
        v = self.read(I.src, I.sz)
        if I.sz == 2:
            v = self.un('sx16', v)
        self.e('bin', name, I.dst, I.dst, v)

    def i_adda(self, I):
        self._addr_arith(I, 'add')

    def i_suba(self, I):
        self._addr_arith(I, 'sub')

    def i_cmp(self, I):
        b = self.read(I.src, I.sz)
        a = self.read(I.dst, I.sz)
        self.sub_flags(I.sz, a, b, self.bin('sub', a, b))

    i_cmpi = i_cmp
    i_cmpm = i_cmp

    def i_cmpa(self, I):
        b = self.read(I.src, I.sz)
        if I.sz == 2:
            b = self.un('sx16', b)
        self.sub_flags(4, I.dst, b, self.bin('sub', I.dst, b))

    def _logic2(self, I, name):
        if I.dst in (('sr',), ('ccr',)):
            t = self.t()
            self.e('sr_read', t)
            r = self.bin(name, t, ('i', I.src[1]))
            self.e('sr_write' if I.dst == ('sr',) else 'ccr_write', r)
            return
        b = self.read(I.src, I.sz)
        loc = self.loc(I.dst, I.sz)
        r = self.bin(name, self.get(loc, I.sz), b)
        self.put(loc, I.sz, r)
        self.logic(I.sz, r)

    def i_and(self, I):
        self._logic2(I, 'and')

    i_andi = i_and

    def i_or(self, I):
        self._logic2(I, 'or')

    i_ori = i_or

    def i_eor(self, I):
        self._logic2(I, 'xor')

    i_eori = i_eor

    def i_not(self, I):
        loc = self.loc(I.dst, I.sz)
        r = self.un('not', self.get(loc, I.sz))
        self.put(loc, I.sz, r)
        self.logic(I.sz, r)

    def i_neg(self, I):
        loc = self.loc(I.dst, I.sz)
        v = self.get(loc, I.sz)
        r = self.bin('sub', ('i', 0), v)
        self.sub_flags(I.sz, ('i', 0), v, r)        # flags first, as in _addsub
        self.put(loc, I.sz, r)

    def i_ext(self, I):
        r = I.dst
        if I.sz == 2:
            v = self.un('sx8', r)
            self.e('setlo', 2, r, v)
        else:
            v = self.un('sx16', r)
            self.e('mov', r, v)
        self.logic(I.sz, v)

    def i_swap(self, I):
        self.e('un', 'rot16', I.dst, I.dst)
        self.logic(4, I.dst)

    def i_exg(self, I):
        t = self.mov(I.src)
        self.e('mov', I.src, I.dst)
        self.e('mov', I.dst, t)

    def _shift(self, I, kind):
        if I.dst[0] != 'd':                 # the memory form shifts one word by one bit
            count = ('i', 1)
            sz = 2
        else:
            sz = I.sz
            count = ('i', I.src[1]) if I.src[0] == 'imm' else I.src
        loc = self.loc(I.dst, sz)
        d = self.t()
        self.e('shift', kind, sz * 8, d, self.get(loc, sz), count)
        self.put(loc, sz, d)
        self.fl(N, 'n', sz, r=d)
        self.fl(Z, 'z', sz, r=d)

    def i_lsl(self, I):
        self._shift(I, 'lsl')

    def i_lsr(self, I):
        self._shift(I, 'lsr')

    def i_asl(self, I):
        self._shift(I, 'asl')

    def i_asr(self, I):
        self._shift(I, 'asr')

    def i_rol(self, I):
        self._shift(I, 'rol')

    def i_ror(self, I):
        self._shift(I, 'ror')

    def _bit(self, I, write):
        wide = I.dst[0] == 'd'
        sz = 4 if wide else 1
        loc = self.loc(I.dst, sz)
        count = ('i', I.src[1] & (31 if wide else 7)) if I.src[0] == 'imm' else \
            self.bin('and', I.src, ('i', 31 if wide else 7))
        v = self.get(loc, sz)
        d = self.t()
        self.e('bitmask', d, count)
        self.fl(Z, 'zbit', sz, v, d)
        if write == 'set':
            self.put(loc, sz, self.bin('or', v, d))
        elif write == 'clr':
            self.put(loc, sz, self.bin('and', v, self.un('not', d)))
        elif write == 'chg':
            self.put(loc, sz, self.bin('xor', v, d))

    def i_btst(self, I):
        self._bit(I, None)

    def i_bset(self, I):
        self._bit(I, 'set')

    def i_bclr(self, I):
        self._bit(I, 'clr')

    def i_bchg(self, I):
        self._bit(I, 'chg')

    def i_mulu(self, I):
        s = self.read(I.src, 2)
        r = self.bin('mul', self.un('zx16', I.dst), s)
        self.e('mov', I.dst, r)
        self.logic(4, r)

    def i_muls(self, I):
        s = self.un('sx16', self.read(I.src, 2))
        r = self.bin('mul', self.un('sx16', I.dst), s)
        self.e('mov', I.dst, r)
        self.logic(4, r)

    def i_divu(self, I):
        self.e('div', 'u', I.dst, self.read(I.src, 2), I.pc)

    def i_divs(self, I):
        self.e('div', 's', I.dst, self.read(I.src, 2), I.pc)

    def i_scc(self, I):
        d = self.t()
        self.e('setcc', d, I.cc)
        self.write(I.dst, 1, d)

    def i_link(self, I):
        v = self.mov(I.dst)     # link a7 pushes the stack pointer from before the push, as FX68K does; see rig/fuzz/fuzz_main.c
        self.push32(v)
        self.e('mov', I.dst, ('a', 7))
        self.e('bin', 'add', ('a', 7), ('a', 7), ('i', I.src[1]))

    def i_unlk(self, I):
        self.e('mov', ('a', 7), I.dst)
        v = self.pop32()
        self.e('mov', I.dst, v)

    def i_movem(self, I):
        # The order of accesses is the reference 68000's, so the lockstep comparison sees the same list.
        step = I.sz
        if I.mdir == 'r2m':
            ea = I.dst
            if ea[0] == 'pre':
                cur = self.mov(('a', ea[1]))
                for r in reversed(I.regs):
                    self.e('bin', 'sub', cur, cur, ('i', step))
                    if step == 4:
                        self.st(2, self.bin('add', cur, ('i', 2)), self.un('zx16', r))
                        self.st(2, cur, self.un('zx16', self.un('rot16', r)))
                    else:
                        self.st(2, cur, self.un('zx16', r))
                self.e('mov', ('a', ea[1]), cur)
                return
            cur = self.mov(self.addr_of(ea, step))
            for r in I.regs:
                self.st(step, cur, r)
                self.e('bin', 'add', cur, cur, ('i', step))
            return
        ea = I.src
        if ea[0] == 'post':
            cur = self.mov(('a', ea[1]))
        else:
            cur = self.mov(self.addr_of(ea, step))
        for r in I.regs:
            v = self.ld(step, cur, 'img' if ea[0] in ('pcd', 'pci') else None)
            if step == 2:
                v = self.un('sx16', v)
            self.e('mov', r, v)
            self.e('bin', 'add', cur, cur, ('i', step))
        if ea[0] == 'post':
            self.e('mov', ('a', ea[1]), cur)
        self.ld(2, cur)     # the 68000's extra read at the end: a bus read in the reference, even after a PC relative list


def _ctl_value(L, I):
    """The target of jmp or jsr through a register or a computed address."""
    return L.addr_of(I.src, 4)


def lift_block(b, disc, all_live=False, hook_uses=None):
    """Returns a BlockIR. hook_uses: the switched hooks' sites, see flagsopt.analyse. Terminators:
        ('jump', pc)
        ('cond', cc, taken_pc, fall_pc)
        ('dbcc', cc, reg, taken_pc, fall_pc)
        ('call', pc, return_pc)
        ('call_ind', value, return_pc, constant_guess or None)
        ('ret', value)  ('rte', value)
        ('jmp_ind', value)
        ('switch', value, [targets])
        ('stop', name, pc)
    """
    ir = BlockIR(b)
    L = _L(ir, disc)
    hook_uses = hook_uses or {}
    after = flagsopt.live_after_each(b, hook_uses)
    seen = 0
    for i, I in enumerate(b.insns):
        u, d = flagsopt.uses(I) | hook_uses.get(I.pc, 0), flagsopt.defines(I)
        ir.gen |= u & ~seen
        seen |= d
        L.live = after[i] | (flagsopt.ALL if all_live else 0)
        ir.starts.append(len(ir.ops))
        if i == len(b.insns) - 1 and b.kind != 'fall':
            _terminator(L, I, b, ir, disc)
        else:
            L.insn(I)
    if ir.term is None:
        # With no successor the next instruction did not decode: the dispatcher sends that address to
        # the fallback, which stops only if it really is not an instruction.
        last = b.insns[-1]
        ir.term = ('jump', b.succs[0] if b.succs else last.pc + last.n)
    ir.defined = seen
    return ir


def _terminator(L, I, b, ir, disc):
    op = I.op
    if op == 'bra':
        ir.term = ('jump', I.target)
    elif op == 'bcc':
        ir.term = ('cond', I.cc, I.target, I.pc + I.n)
    elif op == 'dbcc':
        ir.term = ('dbcc', I.cc, I.src, I.target, I.pc + I.n)
    elif op == 'bsr':
        L.push32(('i', I.pc + I.n))
        ir.term = ('call', I.target, I.pc + I.n)
    elif op == 'jsr':
        if I.src[0] in ('absl', 'absw', 'pcd'):
            L.push32(('i', I.pc + I.n))
            ir.term = ('call', I.src[1] & BUS_MASK, I.pc + I.n)
        else:
            v = L.mov(_ctl_value(L, I))
            L.push32(('i', I.pc + I.n))
            ir.term = ('call_ind', v, I.pc + I.n, disc.const_target.get(I.pc))
    elif op == 'jmp':
        if I.src[0] in ('absl', 'absw', 'pcd'):
            ir.term = ('jump', I.src[1] & BUS_MASK)
        elif I.pc in disc.switches:
            ir.term = ('switch', L.mov(_ctl_value(L, I)), list(dict.fromkeys(disc.switches[I.pc][1])))
        else:
            ir.term = ('jmp_ind', L.mov(_ctl_value(L, I)))
    elif op == 'rts':
        ir.term = ('ret', L.pop32())
    elif op == 'rte':
        sr = L.ld(2, ('a', 7))
        L.e('bin', 'add', ('a', 7), ('a', 7), ('i', 2))
        pc = L.pop32()
        L.e('sr_write', sr)
        ir.term = ('rte', pc)
    elif op in ('stop', 'illegal', 'trap', 'rtr'):
        ir.term = ('stop', op, I.pc)
    else:
        raise Unlifted(op)
