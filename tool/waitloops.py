# Copyright (C) 2026 Marsupial Solutions LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Finds wait loops in a translation and says how much of a run they took.

A wait loop here is a block that branches back to itself on a condition, has no store and is not a counted
loop, or a chain of two such blocks. A port does not spin in these: its frame wait and its cart calls return
when the thing is done. So instructions in them say nothing about the load the game puts on a CPU.

  python tool/waitloops.py TRANSLATION_FOLDER [RUN_FOLDER ...] [--list]

Prints counts. --list also prints the loops' addresses, so its output stays in the output folder.
"""
import argparse
import glob
import json
import os
import re
import struct
import sys

BLOCK = re.compile(r'^B_([0-9A-F]{6}): ;$')
WRITES = re.compile(r'\b(RAM_)?W(8|16|32)\(')
CALLS = re.compile(r'\bf_[0-9A-F]{6}\(|pt_enter\(|pt_take_irq')


def find_loops(gen):
    loops = {}
    for path in sorted(glob.glob(os.path.join(gen, 'gen_*.c'))):
        if path.endswith('gen_map.c'):
            continue
        with open(path, encoding='ascii') as f:
            lines = f.read().split('\n')
        i = 0
        while i < len(lines):
            m = BLOCK.match(lines[i])
            if not m:
                i += 1
                continue
            pc = m.group(1)
            j = i + 1
            while j < len(lines) and not BLOCK.match(lines[j]) and lines[j] != '}':
                j += 1
            body = lines[i + 1:j]
            text = '\n'.join(body)
            if ('goto B_%s;' % pc) in text and not WRITES.search(text) and not CALLS.search(text) and '& 0xFFFFu) != 0xFFFFu' not in text:
                m2 = re.search(r'PT_BLOCK\(0x[0-9A-F]+u, (\d+),', text)
                loops[int(pc, 16)] = int(m2.group(1)) if m2 else 1
            i = j
    return loops


def main(argv=None):
    p = argparse.ArgumentParser()
    p.add_argument('translation')
    p.add_argument('runs', nargs='*')
    p.add_argument('--list', action='store_true')
    args = p.parse_args(argv)
    loops = find_loops(os.path.join(args.translation, 'gen'))
    print('wait loops found:', len(loops))
    with open(os.path.join(args.translation, 'blocks.json'), encoding='ascii') as f:
        blocks = {int(k, 16): v for k, v in json.load(f).items()}
    for run in args.runs:
        rows = {}
        with open(os.path.join(run, 'block_counts.bin'), 'rb') as f:
            data = f.read()
        total = 0
        for i in range(0, len(data), 8):
            pc, n = struct.unpack_from('<II', data, i)
            rows[pc] = n
            total += blocks.get(pc, [1])[0] * n
        wait = sum(loops[pc] * n for pc, n in rows.items() if pc in loops)
        print('%s: instructions %d, in wait loops %d (%.1f%%), distinct wait loops that ran %d' % (
            os.path.basename(run), total, wait, 100.0 * wait / max(1, total), sum(1 for pc in rows if pc in loops)))
        if args.list:
            for pc, n in sorted(((pc, n) for pc, n in rows.items() if pc in loops), key=lambda r: -loops[r[0]] * r[1])[:40]:
                print('  %06X insns %d runs %d' % (pc, loops[pc], n))
    return 0


if __name__ == '__main__':
    sys.exit(main())
