# Copyright (C) 2026 Marsupial Solutions LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""The command line of the translator. Every command runs the gate first.

  python ptool.py gate --config LOCAL.json
  python ptool.py discover --config LOCAL.json        write discover.json to the output folder
  python ptool.py translate --config LOCAL.json       write generated C to the output folder
  python ptool.py pc-boot-image --config LOCAL.json   build the firmware and list what its boot changes in the program, in the output folder
  python ptool.py pc-build --config LOCAL.json        the boot image, the translation and the PC program, in the output folder

translate options, all for the lockstep rig:
  --out-name NAME     a folder under out_root instead of out_dir
  --every-insn        one block per instruction, all flags live: the strict build
  --only-pcs FILE     with --every-insn: translate only these addresses, the rest goes to the fallback
  --plant ADDR:KIND   a negative control: one wrong line in the block at ADDR (hex); KIND is value or flag
  --cold-from FILE    a JSON list of block addresses known to run: functions with none of them are marked cold

pc-build options:
  --jobs N            how many files gcc compiles at a time
  --opt FLAG          gcc's optimisation for the generated code and the machine, -O2 by default
  --no-translate      keep the generated code already in the output folder
The PC commands read fw_source (a copy of the firmware's tree) from the local configuration, sdl_dir (SDL2's development files: without it the program builds with no window), sound_cores (the folders of the Z80, FM and PSG cores, see targets/pc/pc_build.py),
controller_db (the folder of SDL's community controller database, optional for a build, see targets/pc/pc_build.py), and pc_watch, an optional list of block addresses in hexadecimal for the run's log.

Exit codes: 0 done, 2 bad arguments, 3 the gate refused (nothing was written).
"""
import argparse
import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from tool import discover, emit_c, flagsopt, gate, image, patchsrc, regions  # noqa: E402
from targets.pc import pc_build  # noqa: E402


def prepare(cfg, rom):
    """The image the translation works from: the ROM, then the patch source, then local test patches."""
    img = image.Image(rom)
    src = cfg.get('patch_source')
    if src:
        patchsrc.apply(img, patchsrc.load(src))
    for path in cfg.get('local_patch_files', []):
        with open(path, encoding='utf-8') as f:
            for row in json.load(f):
                img.patch(row['address'], bytes.fromhex(row['hex']), row.get('why', 'local test patch'))
    return img


def seeds_from_logs(cfg):
    """Addresses earlier runs entered through the fallback. Each becomes a discovery seed."""
    seeds = []
    for path in cfg.get('seed_logs', []):
        if os.path.exists(path):
            with open(path, encoding='ascii') as f:
                for line in f:
                    m = re.match(r'fallback ([0-9A-F]{6}) ', line)
                    if m:
                        seeds.append(int(m.group(1), 16))
    return sorted(set(seeds))


def do_discover(cfg, rom, out):
    img = prepare(cfg, rom)
    hints = dict(cfg.get('hints') or {})
    seeds = seeds_from_logs(cfg)
    if seeds:
        hints['extra_seeds'] = list(hints.get('extra_seeds', [])) + seeds
    d = discover.Discovery(img, hints).run()
    # The switches' caves join only now, so discovery saw the game as it is and cuts its code as before (tool/patchsrc.py).
    patchsrc.write_switch_caves(d)
    with open(os.path.join(out, 'discover.json'), 'w', newline='\n') as f:
        json.dump(d.save(), f)
    print(json.dumps(d.report(), indent=1))
    return d, seeds


def image_hash(img):
    """FNV-1a of the image in the host's word layout: the rig compares it with the reference's memory, and the PC program with its own. The
    switches' caves are left out: neither holds them (tool/patchsrc.py)."""
    data = patchsrc.image_check_bytes(img)
    h = 0xCBF29CE484222325
    swapped = bytearray(data)
    swapped[0::2], swapped[1::2] = data[1::2], data[0::2]
    for b in swapped:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return '%016X' % h


def do_translate(cfg, rom, out, every_insn=False, only=None, plant=None, warm=None, watch=False):
    d, seeds = do_discover(cfg, rom, out)
    refused = flagsopt.x_readers(d)
    blocks, regs = regions.build(d, every_insn=every_insn, only=only)
    plan = patchsrc.switch_plan(d.img)
    held = set(I.pc for b in blocks.values() for I in b.insns)
    lost = sorted(h['id'] for pc, h in plan['hooks'].items() if pc not in held or h.get('resume', pc) not in held or h.get('cave', pc) not in held)
    if lost and only is None:
        raise patchsrc.PatchError('The translation does not hold the code these hooks need: ' + ', '.join(lost))
    hook_uses = {pc: flagsopt.ALL for pc, h in plan['hooks'].items() if 'mask' in h}
    flagsopt.analyse(blocks, regs, all_live=every_insn, hook_uses=hook_uses, ret_all=set(c['base'] for c in plan['caves']))
    prog = emit_c.Program(d, blocks, regs, all_live=every_insn, low_device_end=cfg.get('bus_low_device_end', 0),
                          plant=plant, warm=warm, watch=[int(x, 16) for x in cfg.get('pc_watch', ())] if watch else (), hooks=plan['hooks'])
    files = prog.write(os.path.join(out, 'gen'))
    prog.write_tables(out)
    stats = prog.stats()
    stats['instructions_reading_x'] = len(refused)
    stats['c_files'] = len(files)
    stats['seeds_from_logs'] = len(seeds)
    stats['switch_hooks'] = len(plan['hooks'])
    stats['switch_caves'] = len(plan['caves'])
    stats['instructions_translated'] = sum(len(b.insns) for pc, b in blocks.items() if prog.ir[pc] is not None)
    with open(os.path.join(out, 'translate.json'), 'w', newline='\n') as f:
        json.dump(stats, f, indent=1)
    with open(os.path.join(out, 'image_check.txt'), 'w', newline='\n') as f:
        f.write(image_hash(d.img) + '\n')
    print(json.dumps(stats, indent=1))
    return prog


def pc_boot_image(cfg, out, jobs):
    """Builds the firmware's boot image tool, runs it on the player's file, and returns the path of its list."""
    exe = pc_build.build_boot_image(cfg['fw_source'], out, workers=jobs)
    path = os.path.join(out, 'boot_image.json')
    r = subprocess.run([exe, cfg['rom_path'], path], capture_output=True, text=True, env=pc_build.env())
    print(r.stdout)
    if r.returncode:
        raise RuntimeError('the boot image tool failed: ' + r.stderr[:500])
    return path


def pc_build_all(cfg, rom, out, args):
    path = pc_boot_image(cfg, out, args.jobs)
    cfg = dict(cfg)
    cfg['local_patch_files'] = [path]
    gen = os.path.join(out, 'gen')
    if not args.no_translate:
        do_translate(cfg, rom, out, watch=True)
    with open(os.path.join(out, 'image_check.txt'), encoding='ascii') as f:
        image_check = f.read().strip()
    exe = pc_build.build_program(cfg['fw_source'], gen, out, opt=args.opt, workers=args.jobs, sdl_dir=cfg.get('sdl_dir') or None, rom_sha256=cfg['rom_sha256'], image_check=image_check, sound_cores=cfg.get('sound_cores'),
                                 controller_db=cfg.get('controller_db') or None)
    print('built', exe, os.path.getsize(exe))
    print('RESULT: PASS')


def main(argv=None):
    p = argparse.ArgumentParser()
    p.add_argument('command', choices=('gate', 'discover', 'translate', 'pc-boot-image', 'pc-build'))
    p.add_argument('--jobs', type=int, default=2)
    p.add_argument('--opt', default='-O2')
    p.add_argument('--no-translate', action='store_true')
    p.add_argument('--config')
    p.add_argument('--every-insn', action='store_true', help='one block per instruction, all flags live: the strict lockstep build')
    p.add_argument('--only-pcs', help='with --every-insn: a JSON list of the addresses to translate')
    p.add_argument('--out-name', help='a folder under out_root instead of out_dir')
    p.add_argument('--plant', action='append', default=[], help='ADDR:KIND, a negative control')
    p.add_argument('--cold-from', help='a JSON list of block addresses known to run')
    args = p.parse_args(argv)
    try:
        cfg = gate.load_config(args.config)
        rom = gate.check_rom(cfg)
    except gate.GateRefused as x:
        print(x)
        return 3
    if args.command == 'gate':
        print('The ROM is the one this tool knows.')
        return 0
    if args.out_name:
        cfg = dict(cfg)
        cfg['out_dir'] = os.path.join(cfg['out_root'], args.out_name)
    out = gate.out_dir(cfg)
    if args.command == 'pc-boot-image':
        print(pc_boot_image(cfg, out, args.jobs))
        return 0
    if args.command == 'pc-build':
        pc_build_all(cfg, rom, out, args)
        return 0
    if args.command == 'discover':
        do_discover(cfg, rom, out)
    else:
        only = None
        if args.only_pcs:
            with open(args.only_pcs, encoding='ascii') as f:
                only = set(json.load(f))
        plant = {}
        for text in args.plant:
            a, kind = text.split(':')
            plant[int(a, 16)] = kind
        warm = None
        if args.cold_from:
            with open(args.cold_from, encoding='ascii') as f:
                warm = set(json.load(f))
        do_translate(cfg, rom, out, args.every_insn, only, plant, warm)
    return 0


if __name__ == '__main__':
    sys.exit(main())
