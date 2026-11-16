# Copyright (C) 2026 Marsupial Solutions LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""The translation step of the players' launcher (pl_launcher.c), run on the Python that comes in the package.

    python -I -B pl_translate.py --rules RULES.json --rom GAMEFILE --boot BOOT_IMAGE.json --out FOLDER

It does what `ptool.py pc-build` does before the compiler: the gate on the whole file's SHA-256, the boot image's changes, discovery, the regions,
the flags, the lift and the C back end, with the same settings, so the C it writes is the same, byte for byte. The settings come from the package's
rules file instead of a local configuration: the ROM's hash, the patch source, the block addresses the game program's log watches, and a list of
code addresses that discovery takes as extra seeds (rule data: none of it works without the player's own ROM). It writes FOLDER/gen, as pc-build
does, and FOLDER/gen/pt_game_build.h, which names the image hash for the DLL's exports (pc_game_exports.c).

PROGRESS lines on the output tell the launcher how far it is. Exit 0 done, 2 bad arguments, 3 the gate refused the file.
"""
import argparse
import contextlib
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import ptool  # noqa: E402
from tool import emit_c, flagsopt, gate, regions  # noqa: E402


def progress(percent, what):
    print('PROGRESS %d %s' % (percent, what), flush=True)


def wrap(owner, name, percent, what):
    """Says PROGRESS when ptool reaches this step. The step itself is unchanged."""
    original = getattr(owner, name)

    def step(*args, **kwargs):
        progress(percent, what)
        return original(*args, **kwargs)
    setattr(owner, name, step)


def config_from_rules(rules, rom, boot, out):
    """The local configuration pc-build would have used, from the package's rules."""
    patch = rules.get('patch_source') or ''
    return {
        'rom_path': rom,
        'rom_sha256': rules['rom_sha256'],
        'out_dir': out,
        'patch_source': os.path.join(HERE, patch) if patch else '',
        'local_patch_files': [boot],
        'hints': dict(rules.get('hints') or {}),
        'seed_logs': [],
        'bus_low_device_end': rules['bus_low_device_end'],
        'pc_watch': list(rules.get('pc_watch') or []),
    }


def main(argv=None):
    p = argparse.ArgumentParser()
    p.add_argument('--rules', required=True)
    p.add_argument('--rom', required=True)
    p.add_argument('--boot', required=True)
    p.add_argument('--out', required=True)
    args = p.parse_args(argv)
    with open(args.rules, encoding='utf-8') as f:
        rules = json.load(f)
    cfg = config_from_rules(rules, args.rom, args.boot, args.out)
    progress(2, 'gate')
    try:
        rom = gate.check_rom(cfg)
    except gate.GateRefused as x:
        print(x, flush=True)
        return 3
    os.makedirs(args.out, exist_ok=True)
    wrap(regions, 'build', 45, 'regions')
    wrap(flagsopt, 'analyse', 52, 'flags')
    wrap(emit_c.Program, 'write', 70, 'C back end')
    progress(5, 'discovery')
    start = time.perf_counter()
    with contextlib.redirect_stdout(sys.stderr):
        ptool.do_translate(cfg, rom, args.out, watch=True)
    with open(os.path.join(args.out, 'image_check.txt'), encoding='ascii') as f:
        image = f.read().strip()
    if image != rules.get('image_check'):
        print('the translation gives image %s, and the package was made for %s' % (image, rules.get('image_check')), flush=True)
        return 3
    with open(os.path.join(args.out, 'gen', 'pt_game_build.h'), 'w', encoding='ascii', newline='\n') as f:
        f.write('/* Written by pl_translate.py on this PC: the image this translation was made from. */\n')
        f.write('#define PT_GAME_IMAGE_CHECK "%s"\n' % image)
    progress(100, 'translated in %.1f s' % (time.perf_counter() - start))
    return 0


if __name__ == '__main__':
    sys.exit(main())
