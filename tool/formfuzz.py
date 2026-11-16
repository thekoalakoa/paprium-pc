# Copyright (C) 2026 Marsupial Solutions LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""The form fuzz: every distinct instruction encoding, alone, against a reference 68000.

Routes cannot reach everything the translator decoded. This puts each distinct encoding in a slot of its own in a
test image, translates each with the real lifter and back end, and writes what rig/fuzz needs to run it: one
instruction at a time, with random registers, flags and memory, on the reference and on the translated code and on
the fallback interpreter, comparing every register, every flag the tool promises, every read and every write.

  python tool/formfuzz.py prepare --config LOCAL.json --out FOLDER       the program's encodings
  python tool/formfuzz.py prepare --all-opcodes --out FOLDER             every opcode word the decoder accepts
  python tool/formfuzz.py summary FOLDER RESULT.csv [--never LIST] [--log LOG]

Options of prepare:
  --seed N             the random extension words of --all-opcodes
  --limit N            at most N encodings, for quick tests
  --plant ADDR:KIND    a negative control: one wrong line in the block at ADDR (hex), KIND value, flag or register
  --plant-auto         three negative controls, picked from the slots

The encodings of a program are read from the program, so FOLDER stays in the local output folder and is never
committed. --all-opcodes reads no program.
"""
import argparse
import collections
import json
import os
import random
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from tool import dec68k, emit_c, flagsopt, forms, image, regions  # noqa: E402

SLOT_BASE = 0x100000
IMAGE_SIZE = 0x400000

KIND_REFUSED = 1            # the translation must stop here rather than run it
KIND_DIV = 2                # may raise a divide by zero exception
KIND_READS_SR = 4           # the status word becomes visible, so X must be clear on entry: the tool does not keep X
KIND_DIVS = 8               # a signed divide: see the known differences in rig/fuzz/fuzz_main.c
KIND_LINK_A7 = 16           # link a7: the same
NO_READ = {'lea', 'pea', 'jmp', 'jsr'}


class _Disc:
    """What the lifter and the back end want from a discovery, for slots that have nothing to do with each other."""

    def __init__(self, ins, img):
        self.ins = ins
        self.img = img
        self.func_heads = set(ins)
        self.const_target = {}
        self.switches = {}


class FuzzProgram(emit_c.Program):
    """The real back end, except that no block goes on to another slot: every transfer returns its address."""

    def _jump(self, b, target):
        return 'return 0x%06Xu;' % target

    def _call_expr(self, target):
        return 'pt_enter(c, 0x%06Xu)' % target


def program_encodings(config_path):
    """The distinct encodings of the program, as raw big-endian bytes. Runs the gate first."""
    import ptool
    from tool import discover, gate
    cfg = gate.load_config(config_path)
    rom = gate.check_rom(cfg)
    img = ptool.prepare(cfg, rom)
    d = discover.Discovery(img, dict(cfg.get('hints') or {})).run()
    seen = collections.Counter()
    for a, I in sorted(d.ins.items()):
        seen[bytes(img.r8(a + k) for k in range(I.n))] += 1
    return seen


def all_opcode_encodings(seed):
    """Each opcode word the decoder accepts, with random extension words, a few times over for the ones that have any."""
    rng = random.Random(seed)
    seen = collections.Counter()
    for op in range(65536):
        for _ in range(4):
            words = [op] + [rng.getrandbits(16) & (0xF8FF if rng.random() < 0.5 else 0xFFFF) for _ in range(5)]

            def r16(a, words=words):
                return words[(a - SLOT_BASE) // 2]

            try:
                I = dec68k.decode(r16, SLOT_BASE)
            except (dec68k.DecodeError, IndexError):
                continue
            seen[b''.join(struct.pack('>H', w) for w in words[:I.n // 2])] += 1
            if I.n == 2:
                break
    return seen


def place(encodings):
    """A test image holding each encoding in its own slot. Returns (image bytes above SLOT_BASE, slot addresses)."""
    body = bytearray()
    slots = []
    for enc in encodings:
        slots.append(SLOT_BASE + len(body))
        body += enc
        body += b'\x4A\xFC'         # one word between slots, so that no instruction runs on into the next
    return bytes(body), slots


def row_for(I, kind):
    """The table row the fuzz program reads: where the slot is, and what it needs to know about a PC relative read."""
    base, index, isz, rd = 0, -1, 0, 0
    for x in (I.src, I.dst):
        if isinstance(x, tuple) and x and x[0] == 'pcd':
            base = x[1] & 0xFFFFFFFF
            rd = 0 if I.op in NO_READ else I.sz
        elif isinstance(x, tuple) and x and x[0] == 'pci':
            base = (x[1] + x[2]) & 0xFFFFFFFF
            index = x[3][1] + (8 if x[3][0] == 'a' else 0)
            isz = x[4]
            rd = 0 if I.op in NO_READ else I.sz
    return struct.pack('<IIIIiII', I.pc, I.pc + I.n, kind, base, index, isz, rd)


def prepare(args):
    if args.all_opcodes:
        seen = all_opcode_encodings(args.seed)
    else:
        seen = program_encodings(args.config)
    encodings = sorted(seen)
    if args.limit:
        random.Random(args.seed).shuffle(encodings)
        encodings = sorted(encodings[:args.limit])
    body, slots = place(encodings)
    if SLOT_BASE + len(body) >= IMAGE_SIZE:
        raise SystemExit('too many encodings for the test image')
    img = image.Image(bytes(SLOT_BASE) + body)
    ins = {}
    for s, enc in zip(slots, encodings):
        I = dec68k.decode(img.r16, s)
        if I.n != len(enc):
            raise SystemExit('the decoder reads %s differently in its slot' % enc.hex())
        ins[s] = I
    disc = _Disc(ins, img)
    blocks, regs = regions.build(disc, every_insn=True)
    flagsopt.analyse(blocks, regs, all_live=True)
    plant = {}
    for text in args.plant:
        a, k = text.split(':')
        plant[int(a, 16)] = k
    prog = FuzzProgram(disc, blocks, regs, all_live=True, plant=plant)
    if args.plant_auto:
        want = {'value': None, 'flag': None, 'register': None}
        for s in slots:
            ir = prog.ir[s]
            if ir is None:
                continue
            lines = [emit_c._op_c(op) for op in ir.ops]
            for kind in want:
                if want[kind] is None:
                    try:
                        emit_c._plant(lines, kind)
                        want[kind] = s
                        break
                    except ValueError:
                        pass
        prog.plant = {s: k for k, s in want.items() if s is not None}
        plant = prog.plant
    out = args.out
    gen = os.path.join(out, 'gen')
    os.makedirs(gen, exist_ok=True)
    for name in os.listdir(gen):
        if name.startswith('gen_') and name.endswith('.c'):
            os.remove(os.path.join(gen, name))
    prog.write(gen)
    rows = []
    meta = []
    refused = collections.Counter()
    for s, enc in zip(slots, encodings):
        I = ins[s]
        ir = prog.ir[s]
        kind = 0
        if ir is None or (ir.term and ir.term[0] == 'stop'):
            kind |= KIND_REFUSED
            refused[forms.form(I)] += 1
        if I.op in ('divu', 'divs'):
            kind |= KIND_DIV
        if I.op == 'divs':
            kind |= KIND_DIVS
        if I.op == 'link' and I.dst == ('a', 7):
            kind |= KIND_LINK_A7
        if I.op == 'move' and I.src == ('sr',):
            kind |= KIND_READS_SR
        rows.append(row_for(I, kind))
        meta.append({'hex': enc.hex().upper(), 'form': forms.form(I), 'op': I.op, 'pc': s, 'kind': kind, 'count': seen[enc],
                     'rd': struct.unpack('<IIIIiII', rows[-1])[6]})
    with open(os.path.join(out, 'table.bin'), 'wb') as f:
        f.write(struct.pack('<II', len(rows), SLOT_BASE))
        f.write(b''.join(rows))
    with open(os.path.join(out, 'slots.bin'), 'wb') as f:
        f.write(body)
    with open(os.path.join(out, 'meta.json'), 'w', newline='\n') as f:
        json.dump(meta, f)
    with open(os.path.join(out, 'plant.json'), 'w', newline='\n') as f:
        json.dump({'%X' % k: v for k, v in plant.items()}, f)
    formset = collections.Counter(m['form'] for m in meta)
    print(json.dumps({'encodings': len(encodings), 'instructions_behind_them': sum(seen[e] for e in encodings),
                      'forms': len(formset), 'bytes': len(body), 'refused_encodings': sum(refused.values()),
                      'refused_forms': len(refused), 'planted': {'%X' % k: v for k, v in plant.items()}}, indent=1))
    return 0


def read_never(path):
    """The forms list tool/forms.py --list prints: a count and a form per line."""
    never = set()
    if path:
        with open(path, encoding='ascii') as f:
            for line in f:
                m = re.match(r'\s*\d+ (\S+ \S+)\s*$', line)
                if m:
                    never.add(m.group(1))
    return never


def summary(args):
    with open(os.path.join(args.folder, 'meta.json'), encoding='ascii') as f:
        meta = json.load(f)
    never = read_never(args.never)
    per_form = {}
    totals = collections.Counter()
    bad = []
    with open(args.result, encoding='ascii') as f:
        head = f.readline().strip().split(',')
        for line in f:
            r = dict(zip(head, (int(x) for x in line.strip().split(','))))
            m = meta[r['idx']]
            p = per_form.setdefault(m['form'], collections.Counter())
            for k, v in r.items():
                if k != 'idx':
                    p[k] += v
                    totals[k] += v
            p['encodings'] += 1
            totals['encodings'] += 1
            if r.get('no_valid'):
                p['encodings_no_valid'] += 1
                totals['encodings_no_valid'] += 1
            if r['fails']:
                bad.append((m, r))
    print(json.dumps({'totals': totals, 'forms': len(per_form), 'forms_with_a_difference': sum(1 for p in per_form.values() if p['fails']),
                      'never_ran_forms_seen': sum(1 for k in per_form if k in never),
                      'never_ran_cases': sum(p['cases'] for k, p in per_form.items() if k in never),
                      'never_ran_forms_with_a_difference': sum(1 for k, p in per_form.items() if k in never and p['fails'])}, indent=1))
    kinds = collections.Counter()
    if args.log and os.path.exists(args.log):
        with open(args.log, encoding='latin-1') as f:
            for line in f:
                m = re.match(r'DIFFERENCE \d+: (\w+) at block', line)
                if m:
                    kinds[m.group(1)] += 1
    if kinds:
        print('difference kinds', dict(kinds))
    for k, p in sorted(per_form.items(), key=lambda kv: (-kv[1]['fails'], kv[0])):
        if p['fails']:
            print('DIFFERS %-24s encodings %5d cases %9d fails %7d' % (k, p['encodings'], p['cases'], p['fails']))
    if args.forms:
        for k, p in sorted(per_form.items()):
            print('%s %-24s encodings %5d cases %9d rejected odd %8d mode %7d cross %6d div0 %6d refused %5d' % (
                'never' if k in never else 'ran  ', k, p['encodings'], p['cases'], p['rej_odd'], p['rej_mode'], p['rej_cross'], p['div0'], p['refused']))
    return 0


def main(argv=None):
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest='command', required=True)
    q = sub.add_parser('prepare')
    q.add_argument('--config')
    q.add_argument('--all-opcodes', action='store_true')
    q.add_argument('--out', required=True)
    q.add_argument('--seed', type=int, default=1)
    q.add_argument('--limit', type=int, default=0)
    q.add_argument('--plant', action='append', default=[])
    q.add_argument('--plant-auto', action='store_true')
    q = sub.add_parser('summary')
    q.add_argument('folder')
    q.add_argument('result')
    q.add_argument('--never')
    q.add_argument('--log')
    q.add_argument('--forms', action='store_true')
    args = p.parse_args(argv)
    if args.command == 'prepare':
        return prepare(args)
    return summary(args)


if __name__ == '__main__':
    sys.exit(main())
