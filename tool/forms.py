# Copyright (C) 2026 Marsupial Solutions LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Which instruction forms the runs have exercised: a form is a mnemonic, a size and two addressing modes.

  python tool/forms.py --config LOCAL.json TRANSLATION_FOLDER RUN_FOLDER [RUN_FOLDER ...] [--list]

Code that never ran has not been compared with anything. If its forms all ran somewhere else, a lifter bug
can hide there only in a rare combination; a form that never ran at all is a bigger unknown. Prints counts.
--list prints the forms that never ran, with how many instructions use each: mnemonics only, no addresses.
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import ptool  # noqa: E402
from tool import discover, gate  # noqa: E402
from tool.coverage import load  # noqa: E402


def form(I):
    def mode(x):
        return x[0] if isinstance(x, tuple) and x else str(x) if x is not None else '-'
    return '%s.%s %s,%s' % (I.op, I.sz or '', mode(getattr(I, 'src', None)), mode(getattr(I, 'dst', None)))


def main(argv=None):
    p = argparse.ArgumentParser()
    p.add_argument('--config', required=True)
    p.add_argument('translation')
    p.add_argument('runs', nargs='+')
    p.add_argument('--list', action='store_true')
    args = p.parse_args(argv)
    cfg = gate.load_config(args.config)
    rom = gate.check_rom(cfg)
    d = discover.Discovery(ptool.prepare(cfg, rom), dict(cfg.get('hints') or {})).run()
    with open(os.path.join(args.translation, 'blocks.json'), encoding='ascii') as f:
        blocks = {int(k, 16): v for k, v in json.load(f).items()}
    ran = set()
    for r in args.runs:
        t, _ = load(r)
        for pc in t & set(blocks):
            a = pc
            for _ in range(blocks[pc][0]):
                I = d.ins.get(a)
                if I is None:
                    break
                ran.add(a)
                a += I.n
    total, seen = {}, {}
    for a, I in d.ins.items():
        k = form(I)
        total[k] = total.get(k, 0) + 1
        if a in ran:
            seen[k] = seen.get(k, 0) + 1
    never = {k: n for k, n in total.items() if k not in seen}
    print(json.dumps({'forms': len(total), 'forms_ran': len(seen), 'forms_never_ran': len(never),
                      'instructions': sum(total.values()), 'instructions_ran': len(ran),
                      'instructions_in_forms_that_never_ran': sum(never.values())}, indent=1))
    if args.list:
        for k, n in sorted(never.items(), key=lambda r: -r[1]):
            print('%6d %s' % (n, k))
    return 0


if __name__ == '__main__':
    sys.exit(main())
