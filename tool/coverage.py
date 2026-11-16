# Copyright (C) 2026 Marsupial Solutions LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""What ran, over several lockstep runs of one translation: counts only.

  python -m tool.coverage TRANSLATION_FOLDER RUN_FOLDER [RUN_FOLDER ...] [--each] [--write FILE]

For each run, and for all of them together: distinct instructions and bytes that ran as translated code,
addresses that ran in the fallback, and how much of what was decoded that is. --each also prints how many
instructions each run added to the ones before it. --write keeps the list of block addresses that ran, for
the size and speed work; it names program addresses, so it goes in the output folder only.
"""
import argparse
import json
import os
import sys


def bits(data):
    """The even addresses set in a coverage table."""
    out = set()
    for i, b in enumerate(data):
        if b:
            for k in range(8):
                if (b >> k) & 1:
                    out.add((i << 4) | (k << 1))
    return out


def load(run):
    with open(os.path.join(run, 'visited.bin'), 'rb') as f:
        d = f.read()
    return bits(d[:0x100000]), bits(d[0x100000:0x200000])


def main(argv=None):
    p = argparse.ArgumentParser()
    p.add_argument('translation')
    p.add_argument('runs', nargs='+')
    p.add_argument('--each', action='store_true')
    p.add_argument('--write')
    args = p.parse_args(argv)
    with open(os.path.join(args.translation, 'blocks.json'), encoding='ascii') as f:
        blocks = {int(k, 16): v for k, v in json.load(f).items()}
    with open(os.path.join(args.translation, 'discover.json'), encoding='ascii') as f:
        disc = json.load(f)
    all_t, all_f = set(), set()
    for r in args.runs:
        if not os.path.exists(os.path.join(r, 'visited.bin')):
            print('%s: no coverage table' % os.path.basename(r))
            continue
        t, fb = load(r)
        t &= set(blocks)
        before = sum(blocks[pc][0] for pc in all_t)
        all_t |= t
        all_f |= fb
        if args.each:
            print('%-16s blocks %6d instructions %7d added %7d fallback addresses %d' % (
                os.path.basename(r), len(t), sum(blocks[pc][0] for pc in t),
                sum(blocks[pc][0] for pc in all_t) - before, len(fb)))
    ins = sum(v[0] for v in blocks.values())
    ins_bytes = sum(v[1] for v in blocks.values())
    ran = sum(blocks[pc][0] for pc in all_t)
    ran_bytes = sum(blocks[pc][1] for pc in all_t)
    out = {
        'instructions_in_blocks': ins,
        'bytes_in_blocks': ins_bytes,
        'instructions_decoded': len(disc['pcs']),
        'blocks': len(blocks),
        'blocks_ran': len(all_t),
        'instructions_ran': ran,
        'bytes_ran': ran_bytes,
        'percent_of_instructions': round(100.0 * ran / ins, 2),
        'percent_of_bytes': round(100.0 * ran_bytes / ins_bytes, 2),
        'addresses_in_fallback': len(all_f),
        'not_lifted_blocks_ran': sum(1 for pc in all_t if blocks[pc][2]),
    }
    print(json.dumps(out, indent=1))
    if args.write:
        with open(args.write, 'w', newline='\n') as f:
            json.dump(sorted(all_t), f)
    return 0


if __name__ == '__main__':
    sys.exit(main())
