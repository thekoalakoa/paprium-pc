# Copyright (C) 2026 Marsupial Solutions LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""The back end for this phase: portable C from the intermediate form.

Each region becomes one C function. Inside a region, branches are gotos. A call is a C call, and the
real return address is also pushed on the 68000 stack in work RAM, so RAM stays exact. A function
returns the next program address; the caller compares it with the address it expected and passes
anything else upward until the dispatcher in the runtime takes it (see DESIGN, "Control flow").

The same C file builds for speed, for profile and for lockstep: only the macros differ.

A switched hook of the patch source (patchsrc.py) is written into the block that holds its site: before the site's
instruction, a test of its bit in pt_switches takes the switch's way (skip the site's instructions, or call its cave
and come back after them), and otherwise the game's own instructions run as they always did. A switch point calls
pt_switch_point there. See runtime/pt_rt.h.

Everything this module writes goes to the output folder. It holds game code, so it is never committed.
"""
import json
import os
import struct

from . import cycles68k, flagsopt, lift

_FLAG = {flagsopt.N: 'fn', flagsopt.Z: 'fz', flagsopt.V: 'fv', flagsopt.C: 'fc'}
_FLAG_CTX = {flagsopt.N: 'c->fn', flagsopt.Z: 'c->fz', flagsopt.V: 'c->fv', flagsopt.C: 'c->fc'}
_COND = {'t': '1', 'f': '0', 'hi': '!(fc|fz)', 'ls': '(fc|fz)', 'cc': '!fc', 'cs': 'fc', 'ne': '!fz', 'eq': 'fz',
         'vc': '!fv', 'vs': 'fv', 'pl': '!fn', 'mi': 'fn', 'ge': '!(fn^fv)', 'lt': '(fn^fv)',
         'gt': '!(fz|(fn^fv))', 'le': '(fz|(fn^fv))'}
_BIN = {'add': '+', 'sub': '-', 'and': '&', 'or': '|', 'xor': '^', 'mul': '*'}
_UN = {'not': '~%s', 'zx8': '(%s & 0xFFu)', 'zx16': '(%s & 0xFFFFu)', 'sx8': 'SX8(%s)', 'sx16': 'SX16(%s)',
       'rot16': 'ROT16(%s)'}


def _v(x):
    k = x[0]
    if k == 'd':
        return 'D(%d)' % x[1]
    if k == 'a':
        return 'A(%d)' % x[1]
    if k == 't':
        return 't%d' % x[1]
    if k == 'i':
        return '0x%Xu' % (x[1] & 0xFFFFFFFF)
    raise ValueError(x)


def _op_c(op):
    k = op[0]
    if k == 'mov':
        return '%s = %s;' % (_v(op[1]), _v(op[2]))
    if k == 'bin':
        return '%s = %s %s %s;' % (_v(op[2]), _v(op[3]), _BIN[op[1]], _v(op[4]))
    if k == 'un':
        return '%s = %s;' % (_v(op[2]), _UN[op[1]] % _v(op[3]))
    if k == 'ld':
        pre = {'any': 'R', 'ram': 'RAM_R', 'img': 'IMG_R'}[op[4]]
        return '%s = %s%d(%s);' % (_v(op[2]), pre, op[1] * 8, _v(op[3]))
    if k == 'st':
        pre = {'any': 'W', 'ram': 'RAM_W'}[op[4]]
        return '%s%d(%s, %s);' % (pre, op[1] * 8, _v(op[2]), _v(op[3]))
    if k == 'setlo':
        return '%s = SETLO%d(%s, %s);' % (_v(op[2]), op[1] * 8, _v(op[2]), _v(op[3]))
    if k == 'shift':
        return '%s = pt_%s(%d, %s, %s, &fc, &fv);' % (_v(op[3]), op[1], op[2], _v(op[4]), _v(op[5]))
    if k == 'div':
        return 'PT_DIV%s(%s, %s, 0x%06Xu);' % (op[1].upper(), _v(op[2]), _v(op[3]), op[4])
    if k == 'fl':
        f = _FLAG[op[1]]
        kind, sz, a, b, r = op[2], op[3], _v(op[4]), _v(op[5]), _v(op[6])
        if kind == 'n':
            return '%s = FL_N(%d, %s);' % (f, sz, r)
        if kind == 'z':
            return '%s = FL_Z(%d, %s);' % (f, sz, r)
        if kind == 'zbit':
            return '%s = ((%s & %s) == 0);' % (f, a, b)
        return '%s = FL_%s(%d, %s, %s, %s);' % (f, kind.upper(), sz, a, b, r)
    if k == 'flc':
        return '%s = %d;' % (_FLAG[op[1]], op[2])
    if k == 'bitmask':
        return '%s = 1u << %s;' % (_v(op[1]), _v(op[2]))
    if k == 'setcc':
        return '%s = (%s) ? 0xFFu : 0u;' % (_v(op[1]), _COND[op[2]])
    if k == 'sr_read':
        return '%s = PT_SR_READ();' % _v(op[1])
    if k == 'sr_write':
        return 'PT_SR_WRITE(%s);' % _v(op[1])
    if k == 'ccr_write':
        return 'PT_CCR_WRITE(%s);' % _v(op[1])
    if k == 'usp_get':
        return '%s = c->usp;' % _v(op[1])
    if k == 'usp_set':
        return 'c->usp = %s;' % _v(op[1])
    raise ValueError(op)


def _plant(lines, kind):
    """A negative control: one wrong line in one block. The lockstep run must report it."""
    out = list(lines)
    for i, line in enumerate(out):
        text = line.strip()
        if kind == 'value' and text.startswith(('W8(', 'W16(', 'W32(', 'RAM_W')):
            out[i] = line.rstrip()[:-2] + ' ^ 1u);'
            return out
        if kind == 'flag' and text[:4] in ('fn =', 'fz =', 'fv =', 'fc ='):
            out[i] = line + ' %s = !%s;' % (text[:2], text[:2])
            return out
        if kind == 'register' and text.startswith(('D(', 'A(')):
            out[i] = line + ' %s += 1u;' % text.split(' =')[0]
            return out
    raise ValueError('nothing to plant in this block')


def interrupt_blocks(disc, blocks):
    """The blocks an interrupt handler can run: what the level 4 and level 6 vectors reach, and whatever can reach an rte, with everything those call.

    A handler reached through a table of addresses cannot be followed from its vector, so the second rule finds it from its end. Marking a block too many
    costs one store before each of its device accesses, so the rules may overshoot.
    """
    img = disc.img
    seeds = set()
    for base in disc.vector_tables:
        for vector in (28, 30):
            for bias in [0] + sorted(disc.vector_bias):
                seeds.add((img.r32(base + 4 * vector) + bias) & 0xFFFFFF)
    preds = {}
    for pc, b in blocks.items():
        for s in b.succs:
            preds.setdefault(s, []).append(pc)
    stack = [pc for pc, b in blocks.items() if b.kind == 'rte']
    seen = set()
    while stack:
        pc = stack.pop()
        if pc not in seen:
            seen.add(pc)
            stack.extend(preds.get(pc, ()))
    stack = list(seeds | seen)
    seen = set()
    while stack:
        pc = stack.pop()
        b = blocks.get(pc)
        if b is None or pc in seen:
            continue
        seen.add(pc)
        stack.extend(b.succs)
        last = b.insns[-1]
        if b.kind == 'call' and last.op == 'bsr':
            stack.append(last.target)
        elif b.kind == 'call':
            stack.append(last.src[1] & 0xFFFFFF)
        elif b.kind == 'call_ind' and disc.const_target.get(last.pc) is not None:
            stack.append(disc.const_target[last.pc])
    return seen


class Program:
    """The whole translation: blocks, regions, the intermediate form and the entry tables."""

    def __init__(self, disc, blocks, regions, all_live=False, low_device_end=0, plant=None, warm=None, watch=(), hooks=None, hook_uses=None):
        self.warm = warm                # blocks known to run; a function with none of them is marked cold (size, not speed)
        self.disc = disc
        self.blocks = blocks
        self.regions = regions
        self.all_live = all_live
        self.low_device_end = low_device_end
        self.plant = plant or {}        # negative controls: block address -> 'value' or 'flag'
        self.watch = {pc: i for i, pc in enumerate(watch)}     # blocks that report each entry to the machine's log; addresses from the local configuration
        self.handler_blocks = interrupt_blocks(disc, blocks) if getattr(disc, 'vector_tables', None) else set()
        self.hooks = dict(hooks or {})  # the patch source's switched hooks and switch points, by site (patchsrc.switch_plan)
        # Each switched site reads every flag (flagsopt.analyse). A test passes hook_uses={} as a negative control.
        self.hook_uses = hook_uses if hook_uses is not None else {pc: flagsopt.ALL for pc, h in self.hooks.items() if 'mask' in h}
        self.unlifted = {}
        self.ir = {}
        self.entries = {}
        for h, r in regions.items():
            n = 1
            e = {h: 0}
            for b in r.blocks:
                if b.enterable and b.pc != h:
                    e[b.pc] = n
                    n += 1
            self.entries[h] = e
        for pc, b in blocks.items():
            try:
                self.ir[pc] = lift.lift_block(b, disc, all_live, self.hook_uses)
            except lift.Unlifted as x:
                self.unlifted[str(x)] = self.unlifted.get(str(x), 0) + len(b.insns)
                self.ir[pc] = None

    def _jump(self, b, target):
        t = self.blocks.get(target)
        if t is None:
            return 'return 0x%06Xu;' % target
        if t.region == b.region:
            return 'goto B_%06X;' % target
        return 'return f_%06X(c, %d);' % (t.region, self.entries[t.region][target])

    def _call_expr(self, target):
        t = self.blocks.get(target)
        if t is None:
            return 'pt_enter(c, 0x%06Xu)' % target
        return 'f_%06X(c, %d)' % (t.region, self.entries[t.region][target])

    def _block(self, b, out):
        ir = self.ir[b.pc]
        out.append('B_%06X: ;' % b.pc)
        size = sum(I.n for I in b.insns)
        in_ram = self.disc.img.in_overlay(b.pc)
        if in_ram:
            source = self.disc.img.source_of(b.pc)
            if b.enterable:
                out.append('  PT_RAMCODE_ENTRY(0x%06Xu, 0x%06Xu, %d);' % (b.pc, source, size))
        out.append('  PT_BLOCK(0x%06Xu, %d, %d, %d, %d);' % (b.pc, len(b.insns), b.live_in, int(b.safe), cycles68k.block_clocks(b.insns)))
        for I in b.insns:
            if I.pc in self.watch:
                out.append('  PT_WATCH(%d);' % self.watch[I.pc])
        if in_ram:
            out.append('  PT_RAM_CODE(0x%06Xu, 0x%06Xu, %d);' % (b.pc, source, size))
        if b.pc < self.low_device_end:
            out.append('  PT_LOW_CODE(0x%06Xu, %d);' % (b.pc, size))
        if b.nop_cycles:
            out.append('  PT_DELAY(%d);' % b.nop_cycles)
        if ir is None:
            out.append('  return pt_stopped(c, 0x%06Xu, "instruction not lifted");' % b.pc)
            return
        for flag, name in _FLAG.items():
            if ir.gen & flag:
                out.append('  %s = %s;' % (name, _FLAG_CTX[flag]))
        lines = self._body(b, ir)
        if b.pc in self.plant:
            lines = _plant(lines, self.plant[b.pc])
        out.extend(lines)
        for flag, name in _FLAG.items():
            if ir.defined & b.live_out & flag:
                out.append('  %s = (u8)%s;' % (_FLAG_CTX[flag], name))
        self._term(b, ir.term, out)

    def _hook(self, b, h, here):
        """The switch's way at a hook's site, as one line of C (so a planted fault, see _plant, never lands in it). here: the addresses of this
        block's instructions, for a way back that stays in it.

        skip: on to the instruction after the site's, with the flags as they were. call: the flags into the context, the return address pushed
        as a jsr pushes it, the cave called; the flags it returns with back from the context, then on after the site's instructions."""
        if h['kind'] == 'switch_point':
            return ['  pt_switch_point();']
        store = 'c->fn = (u8)fn; c->fz = (u8)fz; c->fv = (u8)fv; c->fc = (u8)fc;'
        back = 'goto H_%06X;' % h['resume'] if h['resume'] in here else self._jump(b, h['resume'])
        if h['kind'] == 'skip':
            if h['resume'] in here:
                return ['  if (PT_SWITCH(0x%Xu)) %s' % (h['mask'], back)]
            return ['  if (PT_SWITCH(0x%Xu)) { %s %s }' % (h['mask'], store, back)]
        return ['  if (PT_SWITCH(0x%Xu)) { %s A(7) = A(7) - 0x4u; W32(A(7), 0x%Xu); { u32 r_ = %s; if (r_ != 0x%06Xu) return r_; } '
                'fn = c->fn; fz = c->fz; fv = c->fv; fc = c->fc; %s }' % (h['mask'], store, h['resume'], self._call_expr(h['cave']), h['resume'], back)]

    def _body(self, b, ir):
        """The block's operations, with the charges only the run can decide, and in a handler each device access saying what is left of its block."""
        lines = []
        marked = b.pc in self.handler_blocks
        clocks = [cycles68k.clocks(I) for I in b.insns]
        limits = ir.starts[1:] + [len(ir.ops)]
        here = set(I.pc for I in b.insns)
        backs = set(h['resume'] for pc, h in self.hooks.items() if pc in here and 'resume' in h and h['resume'] in here)
        for k, I in enumerate(b.insns):
            if I.pc in backs:
                lines.append('H_%06X: ;' % I.pc)
            if I.pc in self.hooks:
                lines.extend(self._hook(b, self.hooks[I.pc], here))
            at_done = not marked
            for op in ir.ops[ir.starts[k]:limits[k]]:
                if not at_done and ((op[0] == 'ld' and op[4] == 'any') or (op[0] == 'st' and op[4] == 'any')):
                    lines.append('  PT_AT(%d);' % (sum(clocks[k + 1:]) + 4))
                    at_done = True
                if op[0] == 'shift' and op[5][0] != 'i' and I.dst[0] == 'd':
                    lines.append('  PT_CHARGE_SHIFT(%s);' % _v(op[5]))
                elif op[0] == 'bin' and op[1] == 'mul':
                    lines.append('  PT_CHARGE_%s(%s);' % (I.op.upper(), _v(op[4])))
                elif op[0] == 'div':
                    lines.append('  PT_CHARGE_DIV%s(%s, %s);' % (op[1].upper(), _v(op[2]), _v(op[3])))
                elif op[0] == 'setcc' and I.dst[0] == 'd':
                    lines.append('  if (%s) PT_CHARGE(2);' % _COND[op[2]])
                lines.append('  ' + _op_c(op))
        return lines

    def _term(self, b, term, out):
        k = term[0]
        if k == 'jump':
            out.append('  ' + self._jump(b, term[1]))
        elif k == 'cond':
            out.append('  if (%s) { PT_CHARGE(%d); %s }' % (_COND[term[1]], cycles68k.branch_taken_extra(b.insns[-1]), self._jump(b, term[2])))
            out.append('  ' + self._jump(b, term[3]))
        elif k == 'dbcc':
            r = _v(term[2])
            out.append('  if (!(%s)) {' % _COND[term[1]])
            out.append('    %s = SETLO16(%s, %s - 1);' % (r, r, r))
            out.append('    if ((%s & 0xFFFFu) != 0xFFFFu) { %s }' % (r, self._jump(b, term[3])))
            out.append('    PT_CHARGE(4);')
            out.append('  } else {')
            out.append('    PT_CHARGE(2);')
            out.append('  }')
            out.append('  ' + self._jump(b, term[4]))
        elif k == 'call':
            out.append('  { u32 r_ = %s; if (r_ != 0x%06Xu) return r_; }' % (self._call_expr(term[1]), term[2]))
            out.append('  ' + self._jump(b, term[2]))
        elif k == 'call_ind':
            v, ret, guess = _v(term[1]), term[2], term[3]
            if guess is not None and guess in self.blocks:
                out.append('  { u32 r_ = (%s == 0x%06Xu) ? (PT_CNT(guard_hit), %s) : (PT_CNT(guard_miss), pt_enter(c, %s)); '
                           'if (r_ != 0x%06Xu) return r_; }' % (v, guess, self._call_expr(guess), v, ret))
            else:
                out.append('  { u32 r_ = (PT_CNT(call_map), pt_enter(c, %s)); if (r_ != 0x%06Xu) return r_; }' % (v, ret))
            out.append('  ' + self._jump(b, ret))
        elif k == 'ret':
            out.append('  return %s;' % _v(term[1]))
        elif k == 'rte':
            out.append('  c->rte_flag = 1;')
            out.append('  return %s;' % _v(term[1]))
        elif k == 'jmp_ind':
            out.append('  PT_CNT(call_map);')
            out.append('  return pt_enter(c, %s);' % _v(term[1]))
        elif k == 'switch':
            out.append('  switch (%s & 0xFFFFFFu) {' % _v(term[1]))
            for t in term[2]:
                out.append('  case 0x%06Xu: PT_CNT(switch_hit); %s' % (t, self._jump(b, t)))
            out.append('  default: PT_CNT(switch_miss); return pt_enter(c, %s);' % _v(term[1]))
            out.append('  }')
        elif k == 'stop':
            out.append('  return pt_stopped(c, 0x%06Xu, "%s");' % (term[2], term[1]))
        else:
            raise ValueError(term)

    def _region(self, h, out):
        r = self.regions[h]
        ntemps = max((self.ir[b.pc].ntemps for b in r.blocks if self.ir[b.pc] is not None), default=0)
        cold = self.warm is not None and not any(b.pc in self.warm for b in r.blocks)
        out.append('%su32 f_%06X(PtCtx *c, u32 e) {' % ('PT_COLD ' if cold else '', h))
        if ntemps:
            out.append('  u32 ' + ', '.join('t%d' % i for i in range(ntemps)) + ';')
        out.append('  u32 fn = 0, fz = 0, fv = 0, fc = 0;')
        ent = self.entries[h]
        if len(ent) > 1 or r.blocks[0].pc != h:
            out.append('  if (e) switch (e) {')
            for pc, n in ent.items():
                if n:
                    out.append('  case %d: goto B_%06X;' % (n, pc))
            out.append('  default: return pt_stopped(c, 0x%06Xu, "bad entry");' % h)
            out.append('  }')
        if r.blocks[0].pc != h:
            out.append('  goto B_%06X;' % h)
        for b in r.blocks:
            self._block(b, out)
        out.append('}')
        out.append('')

    def write(self, gen_dir, blocks_per_file=4000):
        """Write gen_NNN.c files, pt_gen.h and gen_map.c. Returns the list of C files."""
        os.makedirs(gen_dir, exist_ok=True)
        heads = sorted(self.regions)
        with open(os.path.join(gen_dir, 'pt_gen.h'), 'w', newline='\n') as f:
            f.write('#include "pt_rt.h"\n')
            for h in heads:
                f.write('u32 f_%06X(PtCtx *c, u32 e);\n' % h)
        files = []
        cur, count, idx = [], 0, 0

        def flush():
            nonlocal cur, count, idx
            if not cur:
                return
            p = os.path.join(gen_dir, 'gen_%03d.c' % idx)
            with open(p, 'w', newline='\n') as f:
                f.write('#include "pt_gen.h"\n\n')
                f.write('\n'.join(cur))
                f.write('\n')
            files.append(p)
            idx += 1
            cur, count = [], 0

        for h in heads:
            self._region(h, cur)
            count += len(self.regions[h].blocks)
            if count >= blocks_per_file:
                flush()
        flush()

        rows = []
        for h in heads:
            for pc, n in self.entries[h].items():
                rows.append((pc, h, n))
        rows.sort()
        p = os.path.join(gen_dir, 'gen_map.c')
        with open(p, 'w', newline='\n') as f:
            f.write('#include "pt_gen.h"\n\n')
            f.write('const PtEntry pt_map[] = {\n')
            for pc, h, n in rows:
                f.write('  {0x%06Xu, f_%06X, %du},\n' % (pc, h, n))
            f.write('};\n')
            f.write('const u32 pt_map_n = %d;\n\n' % len(rows))
            f.write('const PtOverlay pt_overlays[] = {\n')
            for base, src, length, _ in self.disc.img.overlays:
                f.write('  {0x%06Xu, 0x%06Xu, 0x%Xu, 0},\n' % (base, src, length))
            f.write('  {0, 0, 0, 0}\n};\n')
        files.append(p)
        return files

    def write_tables(self, out_dir):
        """Address lists for the rig and the run statistics. They name game addresses, so they stay local."""
        with open(os.path.join(out_dir, 'safe_points.bin'), 'wb') as f:
            f.write(struct.pack('<%dI' % sum(1 for b in self.blocks.values() if b.safe),
                                *sorted(pc for pc, b in self.blocks.items() if b.safe)))
        with open(os.path.join(out_dir, 'blocks.json'), 'w', newline='\n') as f:
            json.dump({'%X' % pc: [len(b.insns), sum(I.n for I in b.insns), int(self.ir[pc] is None)]
                       for pc, b in self.blocks.items()}, f)

    def stats(self):
        return {
            'regions': len(self.regions),
            'blocks': len(self.blocks),
            'map_entries': sum(len(e) for e in self.entries.values()),
            'safe_points': sum(1 for b in self.blocks.values() if b.safe),
            'handler_blocks': len(self.handler_blocks),
            'blocks_not_lifted': sum(1 for v in self.ir.values() if v is None),
            'unlifted_by_mnemonic': self.unlifted,
        }
