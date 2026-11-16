# Copyright (C) 2026 Marsupial Solutions LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Code discovery: which addresses of the program are instructions.

Everything here is computed from the image at run time. The passes, in order:

  1. entries     vector tables found by their shape, then the reset and interrupt vectors
  2. immediates  addresses loaded by lea, pea, move, cmp that look like the start of a function
  3. ram code    routines the program copies into RAM at boot (see ramcode.py)
  4. pointers    long words anywhere in the image that point at a function head, and runs of long
                 words in uncovered gaps that all point at plausible code
  5. orphans     function heads (link a6) in the code hull that nothing above reached

Discovery cannot be complete, and it does not have to be. A miss at run time goes to the fallback
interpreter, is logged, and the logged address comes back here as an extra seed.
"""
import collections

import numpy as np

from . import dec68k, ramcode

STOP_OPS = {'rts', 'rte', 'rtr', 'stop', 'illegal', 'trap'}
HEAD_OPS = {'link', 'movem', 'move', 'movea', 'lea', 'moveq', 'clr', 'tst', 'jsr', 'bra', 'pea', 'subq',
            'addq', 'rts', 'cmpi', 'nop', 'btst', 'add', 'sub', 'and', 'or'}
LINK_A6 = 0x4E56
DEFAULT_HULL_GAP = 0x8000
RUN_MIN = 3

RAM_LOW = 0xE00000
BUS_MASK = 0xFFFFFF


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


class Discovery:
    def __init__(self, img, hints=None):
        self.img = img
        self.hints = hints or {}
        self.ins = {}
        self.work = []
        self.bad = 0
        self.seeds = collections.Counter()
        self.switches = {}              # site -> (table address, list of targets)
        self.indirect = []              # sites of jsr / jmp through a register or a computed address
        self.outside = collections.Counter()    # direct targets not in the image: RAM and others
        self.imm_pointers = set()
        self.func_heads = set()         # addresses reached as call targets or from pointers
        self.vector_tables = []
        self.const_target = {}          # indirect site -> constant target found in the same function
        self.copies = []                # RAM copies found, see ramcode.py
        self.vector_bias = set()        # constants the program adds to its vectors at start-up
        self.hull = []
        self.stages = {}

    # ---- the walker
    def code_address(self, a):
        return (a & 1) == 0 and self.img.readable(a)

    def seed(self, a, kind):
        a &= BUS_MASK
        if self.code_address(a) and a not in self.ins:
            self.work.append(a)
            self.seeds[kind] += 1
            return True
        return False

    def _target(self, a, call):
        a &= BUS_MASK
        if self.code_address(a):
            self.work.append(a)
            if call:
                self.func_heads.add(a)
        else:
            self.outside[a] += 1

    def _note_pointers(self, I):
        if I.op in ('lea', 'pea', 'move', 'movea', 'cmpi', 'cmpa') and I.src:
            k = I.src[0]
            if k in ('imm', 'absl', 'absw', 'pcd'):
                self.imm_pointers.add(I.src[1] & BUS_MASK)

    def _switch(self, I):
        kind, base, d8, _, _ = I.src
        table = (base + d8) & BUS_MASK
        targets = []
        limit = 0x10000
        k = table
        while k < table + limit and k + 2 <= self.img.size:
            off = s16(self.img.r16(k))
            t = table + off
            if off <= 0 or not self.code_address(t):
                break
            targets.append(t)
            limit = min(limit, min(x - table for x in targets))
            k += 2
        self.switches[I.pc] = (table, targets)
        for t in targets:
            self.work.append(t)
            self.seeds['switch'] += 1

    def walk(self):
        r16 = self.img.r16
        while self.work:
            a = self.work.pop()
            while a not in self.ins and self.code_address(a):
                try:
                    I = dec68k.decode(r16, a)
                except (dec68k.DecodeError, IndexError):
                    self.bad += 1
                    break
                self.ins[a] = I
                self._note_pointers(I)
                op = I.op
                if op in STOP_OPS:
                    break
                if op in ('jsr', 'jmp'):
                    k = I.src[0]
                    if k == 'pci':
                        self._switch(I)
                    elif k in ('absl', 'absw', 'pcd'):
                        self._target(I.src[1], op == 'jsr')
                    else:
                        self.indirect.append(a)
                    if op == 'jmp':
                        break
                elif op in ('bra', 'bcc', 'bsr', 'dbcc'):
                    self._target(I.target, op == 'bsr')
                    if op == 'bra':
                        break
                a += I.n

    # ---- the passes
    def find_vector_tables(self):
        """A vector table is 64 long words: a stack pointer, then even addresses in the image or in RAM."""
        img = self.img
        found = []
        for base in range(0, min(img.size, 0x100000), 0x100):
            good = True
            for v in range(1, 64):
                x = img.r32(base + 4 * v) & BUS_MASK
                if x & 1 or x < 0x100 or not (img.in_rom(x) or x >= RAM_LOW):
                    good = False
                    break
            if good:
                found.append(base)
        return found

    def pass_entries(self):
        self.vector_tables = self.find_vector_tables()
        for base in self.vector_tables:
            for v in range(1, 64):
                t = self.img.r32(base + 4 * v) & BUS_MASK
                if self.seed(t, 'vector'):
                    self.func_heads.add(t)
        for t in self.hints.get('extra_seeds', []):
            if self.seed(t, 'hint'):
                self.func_heads.add(t)
        self.walk()

    def _decodes(self, p):
        try:
            dec68k.decode(self.img.r16, p)
            return True
        except (dec68k.DecodeError, IndexError):
            return False

    def _plausible_head(self, p):
        try:
            return dec68k.decode(self.img.r16, p).op in HEAD_OPS
        except (dec68k.DecodeError, IndexError):
            return False

    def pass_immediates(self, rounds=6):
        done = set()
        for _ in range(rounds):
            new = [p for p in self.imm_pointers if p not in self.ins and p not in done]
            for p in new:
                done.add(p)
                if self.in_hull(p) and self._plausible_head(p):
                    if self.seed(p, 'immediate'):
                        self.func_heads.add(p)
            self.walk()

    def pass_vector_edit(self):
        """Some programs move their interrupt handlers at start-up by adding a constant to each vector.

        The shape: addi.l #K,(An) in a loop that counts to 63, the last vector. When we find it, the
        handlers at vector + K are entries too.
        """
        for a, I in list(self.ins.items()):
            if I.op != 'addi' or I.sz != 4 or I.dst[0] != 'ind' or I.src[1] in self.vector_bias:
                continue
            p = a + I.n
            for _ in range(4):
                J = self.ins.get(p)
                if J is None:
                    break
                if J.op == 'cmpi' and J.src == ('imm', 63):
                    self.vector_bias.add(I.src[1])
                    for base in self.vector_tables:
                        for v in range(1, 64):
                            t = (self.img.r32(base + 4 * v) + I.src[1]) & BUS_MASK
                            if self.seed(t, 'vector_edit'):
                                self.func_heads.add(t)
                    break
                p += J.n
        self.walk()

    def pass_ramcode(self):
        ramcode.find_and_add(self)
        for a in sorted(self.outside):
            if self.img.in_overlay(a) and self.seed(a, 'ram'):
                self.func_heads.add(a)
        self.walk()

    def compute_hull(self, gap=DEFAULT_HULL_GAP):
        """The code hull: intervals of reached code, joined across gaps smaller than 'gap'."""
        if 'text_ranges' in self.hints:
            self.hull = [tuple(r) for r in self.hints['text_ranges']]
            return
        rom = sorted(a for a in self.ins if self.img.in_rom(a) and a >= 0x100)
        hull = []
        for a in rom:
            if hull and a - hull[-1][1] < gap:
                hull[-1][1] = a + self.ins[a].n
            else:
                hull.append([a, a + self.ins[a].n])
        self.hull = [tuple(h) for h in hull]

    def in_hull(self, a):
        return (a & 1) == 0 and any(lo <= a < hi for lo, hi in self.hull)

    def _coverage(self):
        cov = np.zeros(self.img.size, dtype=bool)
        for a, I in self.ins.items():
            if self.img.in_rom(a):
                cov[a:a + I.n] = True
        return cov

    def _is_link_head(self, a):
        """link.w a6,#d with d zero or negative and even, which is how the compiler opens a function."""
        try:
            if self.img.r16(a) != LINK_A6:
                return False
            d = s16(self.img.r16(a + 2))
        except IndexError:
            return False
        return d <= 0 and (d & 1) == 0

    def pass_pointers(self):
        data = np.frombuffer(bytes(self.img.bytes), dtype=np.uint8)
        n = (self.img.size - 4) // 2
        evens = np.arange(n) * 2
        longs = (data[evens].astype(np.uint32) << 24) | (data[evens + 1].astype(np.uint32) << 16) | \
                (data[evens + 2].astype(np.uint32) << 8) | data[evens + 3].astype(np.uint32)
        # A long word that points at a function head counts wherever it sits and wherever it points:
        # a 32-bit value that lands on the head pattern by accident is very unlikely.
        even_in_rom = ((longs & 1) == 0) & (longs >= 0x100) & (longs < self.img.size - 4)
        candidates = longs[even_in_rom]
        heads = 0
        for t in candidates[data[candidates] == (LINK_A6 >> 8)]:
            t = int(t)
            if self._is_link_head(t):
                heads += 1
                if self.seed(t, 'pointer'):
                    self.func_heads.add(t)
        self.walk()
        mask = np.zeros(n, dtype=bool)
        for lo, hi in self.hull:
            mask |= (longs >= lo) & (longs < hi)
        mask &= (longs & 1) == 0
        hits = evens[mask]
        # Runs of long words in uncovered gaps that all point at legal instructions, most of them
        # plausible function starts: tables of handlers.
        cov = self._coverage()
        run = []
        tables = 0

        def flush():
            nonlocal tables
            if len(run) >= RUN_MIN and sum(self._plausible_head(t) for t in run) * 4 >= len(run) * 3:
                tables += 1
                for t in run:
                    if self.seed(t, 'pointer_run'):
                        self.func_heads.add(t)
            run.clear()

        prev = None
        for off in hits:
            off = int(off)
            if prev is not None and off != prev + 4:
                flush()
            t = int(longs[off // 2])
            if not cov[off:off + 4].any() and self._decodes(t):
                run.append(t)
            else:
                flush()
            prev = off
        flush()
        self.walk()
        self.stages['pointer_heads'] = heads
        self.stages['pointer_tables'] = tables

    def pass_orphans(self):
        orphans = 0
        for lo, hi in self.hull:
            for a in range(lo & ~1, hi, 2):
                if a not in self.ins and self._is_link_head(a):
                    orphans += 1
                    if self.seed(a, 'orphan'):
                        self.func_heads.add(a)
        self.walk()
        self.stages['orphan_heads'] = orphans

    # ---- after the walk
    def const_before(self, site, reg, limit=400):
        """If register a<reg> holds a constant at 'site', loaded earlier in the same function, return it."""
        if not hasattr(self, '_order'):
            self._order = sorted(self.ins)
            self._index = {a: i for i, a in enumerate(self._order)}
        i = self._index[site]
        for j in range(i - 1, max(i - limit, -1), -1):
            J = self.ins[self._order[j]]
            if J.op == 'link':
                return None
            if J.dst == ('a', reg):
                s = J.src
                if J.op == 'lea' and s and s[0] in ('absl', 'absw', 'pcd'):
                    return s[1] & BUS_MASK
                if J.op == 'movea' and s and s[0] == 'imm':
                    return s[1] & BUS_MASK
                return None
            if J.op in ('movem', 'exg', 'unlk'):
                if J.op == 'movem' and J.mdir == 'm2r' and ('a', reg) in J.regs:
                    return None
                if J.op != 'movem':
                    return None
        return None

    def classify_indirect(self):
        for site in self.indirect:
            I = self.ins[site]
            if I.src[0] == 'ind':
                v = self.const_before(site, I.src[1])
                if v is not None:
                    self.const_target[site] = v

    def run(self, max_rounds=8):
        """The passes feed each other: more code gives a larger hull, which admits more pointers."""
        self.pass_entries()
        self.stages['entries'] = len(self.ins)
        for round_ in range(max_rounds):
            before = len(self.ins)
            self.compute_hull()
            self.pass_vector_edit()
            self.pass_ramcode()
            self.compute_hull()
            self.pass_immediates(rounds=2)
            self.compute_hull()
            self.pass_pointers()
            self.compute_hull()
            self.pass_orphans()
            self.stages['round_%d' % round_] = len(self.ins)
            if len(self.ins) == before:
                break
        self.classify_indirect()
        return self

    def report(self):
        """Counts only. Addresses go to the output folder through save(), never into the repository."""
        mix = collections.Counter(I.op for I in self.ins.values())
        return {
            'instructions': len(self.ins),
            'code_bytes': sum(I.n for I in self.ins.values()),
            'by_stage': self.stages,
            'seeds': dict(self.seeds),
            'undecodable': self.bad,
            'vector_tables': len(self.vector_tables),
            'switches': len(self.switches),
            'switch_targets': sum(len(t) for _, t in self.switches.values()),
            'switches_empty': sum(1 for _, t in self.switches.values() if not t),
            'indirect_sites': len(self.indirect),
            'indirect_constant': len(self.const_target),
            'indirect_unresolved': len(self.indirect) - len(self.const_target),
            'targets_outside_image': len(self.outside),
            'ram_copies': len(self.copies),
            'hull_intervals': len(self.hull),
            'overlaps': self.count_overlaps(),
            'mnemonics': len(mix),
        }

    def count_overlaps(self):
        n = 0
        prev_end = -1
        for a in sorted(self.ins):
            if a < prev_end:
                n += 1
            prev_end = max(prev_end, a + self.ins[a].n)
        return n

    def save(self):
        """Everything that names addresses, for the output folder."""
        return {
            'report': self.report(),
            'pcs': sorted(self.ins),
            'func_heads': sorted(self.func_heads),
            'vector_tables': self.vector_tables,
            'hull': self.hull,
            'switches': {str(k): {'table': v[0], 'targets': v[1]} for k, v in self.switches.items()},
            'indirect': sorted(self.indirect),
            'const_target': {str(k): v for k, v in self.const_target.items()},
            'outside': {str(k): v for k, v in sorted(self.outside.items())},
            'copies': self.copies,
        }
