# Copyright (C) 2026 Marsupial Solutions LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Counts from a lockstep run, joined with the translation that ran: what was translated, what ran, what
went through the fallback, and which blocks took the most instructions.

  python -m tool.runstats TRANSLATION_FOLDER RUN_FOLDER [--top N]

It prints counts. With --top it also prints the busiest blocks' addresses, so its output stays in the
output folder. Nothing it reads or writes belongs in the repository.
"""
import argparse
import json
import os
import struct


def load_visited(path):
    with open(path, 'rb') as f:
        data = f.read()
    return data[:0x100000], data[0x100000:0x200000]


def bit(table, pc):
    return (table[pc >> 4] >> ((pc >> 1) & 7)) & 1


def load_counts(path):
    rows = {}
    if os.path.exists(path):
        with open(path, 'rb') as f:
            data = f.read()
        for i in range(0, len(data), 8):
            pc, n = struct.unpack_from('<II', data, i)
            rows[pc] = n
    return rows


def main(argv=None):
    p = argparse.ArgumentParser()
    p.add_argument('translation')
    p.add_argument('run')
    p.add_argument('--top', type=int, default=0)
    args = p.parse_args(argv)
    with open(os.path.join(args.translation, 'blocks.json'), encoding='ascii') as f:
        blocks = {int(k, 16): v for k, v in json.load(f).items()}
    with open(os.path.join(args.translation, 'discover.json'), encoding='ascii') as f:
        disc = json.load(f)
    t, fb = load_visited(os.path.join(args.run, 'visited.bin'))
    counts = load_counts(os.path.join(args.run, 'block_counts.bin'))
    pcs = disc['pcs']
    ran_blocks = [pc for pc in blocks if bit(t, pc)]
    ran_ins = sum(blocks[pc][0] for pc in ran_blocks)
    fb_pcs = sum(bin(b).count('1') for b in fb)
    out = {
        'instructions_decoded': len(pcs),
        'bytes_decoded': disc['report']['code_bytes'],
        'decoded_above_the_bus_rom': sum(1 for a in pcs if 0x400000 <= a < 0xE00000),
        'blocks_total': len(blocks),
        'blocks_not_lifted': sum(1 for v in blocks.values() if v[2]),
        'blocks_ran': len(ran_blocks),
        'distinct_instructions_ran_translated': ran_ins,
        'distinct_addresses_ran_in_fallback': fb_pcs,
        'not_lifted_blocks_ran': sum(1 for pc in ran_blocks if blocks[pc][2]),
        'indirect_sites': disc['report']['indirect_sites'],
        'indirect_constant_sites': disc['report']['indirect_constant'],
        'indirect_map_sites': disc['report']['indirect_unresolved'],
        'switch_sites': disc['report']['switches'],
    }
    total = sum(blocks.get(pc, [1])[0] * n for pc, n in counts.items())
    out['instructions_from_block_counts'] = total
    print(json.dumps(out, indent=1))
    if args.top:
        rows = sorted(counts.items(), key=lambda r: -blocks.get(r[0], [1])[0] * r[1])[:args.top]
        run = 0
        for pc, n in rows:
            k = blocks.get(pc, [1])[0]
            run += k * n
            print('%06X runs %10d insns %3d share %5.2f%% cumulative %5.2f%%' % (
                pc, n, k, 100.0 * k * n / total, 100.0 * run / total))
    return 0


if __name__ == '__main__':
    main()
