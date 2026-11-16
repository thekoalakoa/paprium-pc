# Copyright (C) 2026 Marsupial Solutions LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Runs a whole lockstep run's record on a second CPU: the replay build for RV32IM (runtime/pt_main_bare.c), under
Unicorn, from the first instruction to the last, and compares its end state with the run's.

  python tool/secondhost.py ELF RUN_FOLDER [--expect-full HEX]

The run's own end state, in replay_end.txt, was hashed on the host that made it: all of work RAM and the 16 registers.
--expect-full also compares the flags, the mask and the user stack pointer, against what the same code's replay printed
on the other host. Exit 0 when the end states are equal. Prints JSON. It holds no game data: everything comes from the
run folder, which stays in the output folder.
"""
import argparse
import json
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from tool import price  # noqa: E402


def main(argv=None):
    p = argparse.ArgumentParser()
    p.add_argument('elf')
    p.add_argument('run')
    p.add_argument('--expect-full')
    p.add_argument('--slice-seconds', type=int, default=120)
    args = p.parse_args(argv)
    from unicorn import Uc, UC_ARCH_RISCV, UC_MODE_RISCV32
    from unicorn import riscv_const
    entry, segs, syms = price.read_elf(args.elf)
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
    end_safe, end_reads, end_irqs, end_hash = int(end[1]), int(end[5]), int(end[7]), int(end[9], 16)
    pc, rom_size = struct.unpack_from('<II', start, 0)
    ctx_size = 8 * 4 + 8 * 4 + 4 + 8
    ctx = start[8:8 + ctx_size]
    ram = start[8 + ctx_size:8 + ctx_size + 0x10000]
    rom = start[8 + ctx_size + 0x10000:]

    uc = Uc(UC_ARCH_RISCV, UC_MODE_RISCV32)
    uc.mem_map(0, 0x0F000000)
    uc.mem_map(price.INFO, 0x1000)
    top = price.DATA + len(rom) + len(ctx) + len(ram) + len(reads) + len(irqs) + len(pokes) + 0x10000
    uc.mem_map(price.DATA, ((top - price.DATA) + 0xFFFFF) & ~0xFFFFF)
    for vaddr, data, _ in segs:
        uc.mem_write(vaddr, data)
    a = price.DATA
    places = {}
    for name, blob in (('rom', rom), ('ctx', ctx), ('ram', ram), ('reads', reads), ('irqs', irqs), ('pokes', pokes)):
        uc.mem_write(a, blob)
        places[name] = a
        a = (a + len(blob) + 15) & ~15
    never = 0xFFFFFFFF      # no marks: the whole run counts
    uc.mem_write(price.INFO, struct.pack('<16I', pc, rom_size, places['rom'], places['ctx'], places['ram'], places['reads'], len(reads) // 4,
                                         places['irqs'], len(irqs) // 32, places['pokes'], len(pokes) // 32, end_safe & 0xFFFFFFFF, end_safe >> 32,
                                         never, never, 0))
    t0 = time.time()
    at = entry
    stop = syms['bare_stop']
    while True:
        # A time limit lets a long run show that it is alive. The run goes on from where the slice ended.
        uc.emu_start(at, stop, timeout=args.slice_seconds * 1000000)
        at = uc.reg_read(riscv_const.UC_RISCV_REG_PC)
        if at == stop:
            break
        print('still running after %.0f s' % (time.time() - t0), flush=True)
    info = struct.unpack('<7I', bytes(uc.mem_read(price.INFO + 15 * 4, 7 * 4)))
    done, reads_at, irqs_at, h_low, h_high, f_low, f_high = info
    h = (h_high << 32) | h_low
    full = (f_high << 32) | f_low
    same = bool(done) and h == end_hash and reads_at == end_reads and irqs_at == end_irqs
    full_same = None
    if args.expect_full:
        full_same = full == int(args.expect_full, 16)
        same = same and full_same
    print(json.dumps({'elf': os.path.basename(args.elf), 'run': os.path.basename(os.path.normpath(args.run)), 'seconds': round(time.time() - t0, 1),
                      'instructions_68000': int(end[3]), 'finished': bool(done), 'reads_used': reads_at, 'reads_in_record': end_reads,
                      'interrupts_taken': irqs_at, 'interrupts_in_record': end_irqs, 'state': '%016X' % h, 'state_in_record': '%016X' % end_hash,
                      'state_with_flags_and_usp': '%016X' % full, 'state_with_flags_and_usp_equal': full_same, 'equal': same}, indent=1))
    print('RESULT: PASS' if same else 'RESULT: FAIL: the end states differ')
    return 0 if same else 1


if __name__ == '__main__':
    sys.exit(main())
