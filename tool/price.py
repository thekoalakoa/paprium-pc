# Copyright (C) 2026 Marsupial Solutions LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prices a window of a lockstep run on a 32-bit RISC CPU: how many native instructions the translated code
really takes, and how a cache of a given size would behave. The program is the replay build for RV32IM
(runtime/pt_main_bare.c, linked at 1 MB), run under Unicorn with a run's record behind it.

  python tool/price.py ELF RUN_FOLDER --from-frame F --to-frame T [--spin N] [--dcache] [--whole-check]

Prints JSON: 68000 instructions in the window (from the run's frame log), native instructions, native per
68000 instruction, translation-block count, the busiest blocks' share, instruction cache misses at several
sizes, and with --dcache data cache misses too. It holds no game data: everything comes from the run folder,
which stays in the output folder.

This counts one RISC CPU's instructions on a simulator, so it is a measurement of expansion and of memory
behaviour, not of any console's speed.
"""
import argparse
import bisect
import collections
import json
import os
import struct
import sys
import time

INFO = 0x0F000000
STACK_TOP = 0x0F000000
DATA = 0x10000000


def read_elf(path):
    with open(path, 'rb') as f:
        d = f.read()
    entry = struct.unpack_from('<I', d, 24)[0]
    phoff = struct.unpack_from('<I', d, 28)[0]
    shoff = struct.unpack_from('<I', d, 32)[0]
    phentsize, phnum, shentsize, shnum, shstrndx = struct.unpack_from('<HHHHH', d, 42)
    segs = []
    for i in range(phnum):
        t, off, vaddr, _, filesz, memsz, _, _ = struct.unpack_from('<8I', d, phoff + i * phentsize)
        if t == 1:
            segs.append((vaddr, d[off:off + filesz], memsz))
    syms = {}
    for i in range(shnum):
        name, stype, _, _, off, size, link, _, _, entsize = struct.unpack_from('<10I', d, shoff + i * shentsize)
        if stype == 2:
            stroff = struct.unpack_from('<I', d, shoff + link * shentsize + 16)[0]
            for k in range(size // 16):
                n, value, _, _, _, _ = struct.unpack_from('<IIIBBH', d, off + k * 16)
                end = d.index(b'\0', stroff + n)
                syms[d[stroff + n:end].decode('ascii', 'replace')] = value
    return entry, segs, syms


class Cache:
    """Direct-mapped, tags only."""

    def __init__(self, size, line):
        self.size, self.line, self.n = size, line, size // line
        self.tags = [-1] * self.n
        self.hits = self.misses = 0
        self.dirty = [False] * self.n
        self.writebacks = 0

    def touch(self, line_address, write=False):
        i = line_address % self.n
        if self.tags[i] == line_address:
            self.hits += 1
        else:
            self.misses += 1
            if self.dirty[i]:
                self.writebacks += 1
            self.tags[i] = line_address
            self.dirty[i] = False
        if write:
            self.dirty[i] = True


def main(argv=None):
    p = argparse.ArgumentParser()
    p.add_argument('elf')
    p.add_argument('run')
    p.add_argument('--from-frame', type=int, required=True)
    p.add_argument('--to-frame', type=int, required=True)
    p.add_argument('--dcache', action='store_true')
    p.add_argument('--top', type=int, default=12)
    args = p.parse_args(argv)
    from unicorn import Uc, UC_ARCH_RISCV, UC_MODE_RISCV32, UC_HOOK_BLOCK, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE, UC_MEM_WRITE
    entry, segs, syms = read_elf(args.elf)

    with open(os.path.join(args.run, 'replay_start.bin'), 'rb') as f:
        start = f.read()
    with open(os.path.join(args.run, 'replay_reads.bin'), 'rb') as f:
        reads = f.read()
    with open(os.path.join(args.run, 'replay_irqs.bin'), 'rb') as f:
        irqs = f.read()
    with open(os.path.join(args.run, 'replay_pokes.bin'), 'rb') as f:
        pokes = f.read()
    with open(os.path.join(args.run, 'replay_end.txt'), encoding='ascii') as f:
        end = f.read().split()
    end_safe = int(end[1])
    pc, rom_size = struct.unpack_from('<II', start, 0)
    ctx_size = 8 * 4 + 8 * 4 + 4 + 8
    ctx = start[8:8 + ctx_size]
    ram = start[8 + ctx_size:8 + ctx_size + 0x10000]
    rom = start[8 + ctx_size + 0x10000:]

    # frame log: frame -> 68000 instructions and interrupts so far
    frames = {}
    with open(os.path.join(args.run, 'frames.txt'), encoding='ascii') as f:
        for line in f:
            w = line.split()
            frames[int(w[0])] = (int(w[w.index('ins') + 1]), int(w[w.index('irq') + 1]))
    i0, q0 = frames[args.from_frame]
    i1, q1 = frames[args.to_frame]

    uc = Uc(UC_ARCH_RISCV, UC_MODE_RISCV32)
    uc.mem_map(0, 0x0F000000)           # code, its data and the stack
    uc.mem_map(INFO, 0x1000)
    top = DATA + len(rom) + len(ctx) + len(ram) + len(reads) + len(irqs) + len(pokes) + 0x10000
    uc.mem_map(DATA, ((top - DATA) + 0xFFFFF) & ~0xFFFFF)
    for vaddr, data, memsz in segs:
        uc.mem_write(vaddr, data)
    a = DATA
    places = {}
    for name, blob in (('rom', rom), ('ctx', ctx), ('ram', ram), ('reads', reads), ('irqs', irqs), ('pokes', pokes)):
        uc.mem_write(a, blob)
        places[name] = a
        a = (a + len(blob) + 15) & ~15
    info = struct.pack('<16I', pc, rom_size, places['rom'], places['ctx'], places['ram'], places['reads'], len(reads) // 4,
                       places['irqs'], len(irqs) // 32, places['pokes'], len(pokes) // 32, end_safe & 0xFFFFFFFF, end_safe >> 32,
                       q0, q1, 0)
    uc.mem_write(INFO, info)

    # to the window at full speed
    t0 = time.time()
    uc.emu_start(entry, syms['bare_mark_from'], count=0)
    t1 = time.time()

    total = 0
    tbs = 0
    by_tb = collections.Counter()
    i_caches = {s: Cache(s, 32) for s in (8192, 16384, 32768)}
    d_caches = {s: Cache(s, 16) for s in (8192, 16384)}
    loads = stores = 0

    def on_block(u, address, size, _):
        nonlocal total, tbs
        total += size // 4
        tbs += 1
        by_tb[address] += size // 4
        line = address >> 5
        last = (address + size - 1) >> 5
        for c in i_caches.values():
            for x in range(line, last + 1):
                c.touch(x)

    def on_mem(u, access, address, size, value, _):
        nonlocal loads, stores
        write = access == UC_MEM_WRITE
        if write:
            stores += 1
        else:
            loads += 1
        for c in d_caches.values():
            c.touch(address >> 4, write)

    uc.hook_add(UC_HOOK_BLOCK, on_block)
    if args.dcache:
        uc.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, on_mem)
    uc.ctl_flush_tb()                   # code translated before the hooks has none of them
    resume = uc.reg_read(_pc_reg())
    uc.emu_start(resume, syms['bare_mark_to'], count=0)
    t2 = time.time()

    ins68 = i1 - i0
    nframes = args.to_frame - args.from_frame
    top_tb = by_tb.most_common(args.top)
    # the same counts by function, from the program's symbol table
    names = sorted((v, k) for k, v in syms.items() if not k.startswith('.') and 0x100000 <= v < 0x0F000000)
    starts = [v for v, _ in names]
    by_fn = collections.Counter()
    for a_, n in by_tb.items():
        by_fn[names[bisect.bisect_right(starts, a_) - 1][1]] += n
    out = {
        'frames': nframes,
        'instructions_68000': ins68,
        'native_instructions': total,
        'native_per_68000': round(total / ins68, 2) if ins68 else 0,
        'native_per_frame': round(total / nframes),
        'translation_blocks': tbs,
        'top_block_shares': [[('%X' % a_), round(100.0 * n / total, 2)] for a_, n in top_tb],
        'top_functions': [[k, n, round(100.0 * n / total, 2)] for k, n in by_fn.most_common(args.top * 2)],
        'icache_32B_line': {str(s): {'misses_per_frame': round(c.misses / nframes), 'miss_per_line_touch': round(c.misses / max(1, c.misses + c.hits), 4)}
                            for s, c in i_caches.items()},
        'seconds_to_window': round(t1 - t0, 1),
        'seconds_in_window': round(t2 - t1, 1),
    }
    if args.dcache:
        out['loads_per_frame'] = round(loads / nframes)
        out['stores_per_frame'] = round(stores / nframes)
        out['dcache_16B_line'] = {str(s): {'misses_per_frame': round(c.misses / nframes), 'writebacks_per_frame': round(c.writebacks / nframes),
                                           'miss_rate': round(c.misses / max(1, c.misses + c.hits), 4)}
                                  for s, c in d_caches.items()}
    print(json.dumps(out, indent=1))
    return 0


def _pc_reg():
    from unicorn import riscv_const
    return riscv_const.UC_RISCV_REG_PC


if __name__ == '__main__':
    sys.exit(main())
