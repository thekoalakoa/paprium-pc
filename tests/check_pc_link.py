# Copyright (C) 2026 Marsupial Solutions LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks what the PC program really links: no reference emulator's code, and not the firmware's old module.

A symbol check alone cannot see code that was copied under another name, so this looks in three places:
  - the link line: no object from rig/, no reference machine, nothing from the firmware's legacy/ folder
  - the finished program's symbols: none of the old module's decoders, none of the reference machine's names
  - the firmware's source files that the program compiles: none says it was ported or copied from a reference emulator

Usage: check_pc_link.py --exe PROGRAM --rsp LINK_RESPONSE_FILE [--firmware FIRMWARE_TREE]
Exit 0 or 1, with RESULT: PASS or RESULT: FAIL.
"""
import argparse
import importlib.util
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

BAD_OBJECT = re.compile(r'(?i)(^|[/\\_])legacy[/\\_]|[/\\]rig[/\\]|[/\\]gpgx[/\\]|(^|[/\\])(ls_core|rig_main|lock_glue)\.o(bj)?$')
# The firmware has names of its own that start like a chip's, such as z80_packet: only the reference machine's whole names are listed for the Z80.
BAD_SYMBOL = re.compile(r'^_*(ppm_unpack|paprium_|m68k_|m68ki_|z80_(init|reset|run|irq)|YM2612|OPN2_|SN76489|fm_write|fm_read|psg_write|sound_update|blip_|vdp_68k_|vdp_z80_|lock_fw_|lock_irq|cart_hw_)')
# Their own chip models have names like the reference machine's. Each pinned core is allowed its exact names, from its one object, built by targets/pc/pc_build.py from a pinned source. A name of this kind from any other object is a fault.
PINNED_SYMBOLS = {'ym3438.o': {'OPN2_Reset', 'OPN2_SetChipType', 'OPN2_Clock', 'OPN2_Write', 'OPN2_SetTestPin', 'OPN2_ReadTestPin', 'OPN2_ReadIRQPin', 'OPN2_Read'},
                  'pc_z80.o': {'z80_init', 'z80_reset'}}
BAD_SOURCE_LINE = re.compile(r'(?i)\b(ported|copied|taken|lifted|adapted)\b[^\n]{0,40}\b(genesis plus|gpgx|musashi)\b')


def load_firmware_list():
    spec = importlib.util.spec_from_file_location('pc_firmware', os.path.join(REPO, 'targets', 'pc', 'pc_firmware.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def object_problems(objects):
    return ['the link line holds %s, which the PC program must not link' % o for o in objects if BAD_OBJECT.search(o.replace('\\', '/'))]


def symbol_problems(nm_output, allowed=frozenset()):
    problems = []
    for line in nm_output.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[-2] not in ('U', 'u') and BAD_SYMBOL.match(parts[-1]) and parts[-1] not in allowed:
            problems.append('the program defines %s, which the PC program must not hold' % parts[-1])
    return problems


def pinned_allowed(objects):
    """The names the pinned cores may define: those of a pinned object on the link line, when no other object on the line defines one of them."""
    allowed, problems = set(), []
    pinned = {}
    for o in objects:
        base = os.path.basename(o.replace('\\', '/'))
        if base in PINNED_SYMBOLS:
            pinned[base] = o
    if not pinned:
        return frozenset(), []
    names = set().union(*(PINNED_SYMBOLS[b] for b in pinned))
    for o in objects:
        base = os.path.basename(o.replace('\\', '/'))
        defined = {p.split()[-1].lstrip('_') for p in program_symbols(o).splitlines() if len(p.split()) >= 2 and p.split()[-2] not in ('U', 'u')}
        if base in PINNED_SYMBOLS:
            allowed |= defined & PINNED_SYMBOLS[base]
        else:
            for n in sorted(defined & names):
                problems.append('%s defines %s, which only a pinned core may' % (o, n))
    return frozenset(allowed), problems


def source_problems(firmware_tree, names):
    problems = []
    for name in names:
        with open(os.path.join(firmware_tree, name), encoding='utf-8', errors='replace') as f:
            for number, line in enumerate(f, 1):
                if BAD_SOURCE_LINE.search(line):
                    problems.append('%s:%d says it comes from a reference emulator' % (name, number))
    return problems


def program_symbols(exe):
    gcc = shutil.which('gcc') or r'C:\msys64\mingw64\bin\gcc.exe'
    env = dict(os.environ)
    env['PATH'] = os.path.dirname(gcc) + os.pathsep + env.get('PATH', '')
    r = subprocess.run([os.path.join(os.path.dirname(gcc), 'nm'), exe], capture_output=True, text=True, env=env)
    if r.returncode:
        raise RuntimeError('nm failed on %s: %s' % (exe, r.stderr[:300]))
    return r.stdout


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--exe', required=True)
    p.add_argument('--rsp', required=True)
    p.add_argument('--firmware')
    args = p.parse_args()
    with open(args.rsp, encoding='utf-8') as f:
        objects = [line.strip().strip('"') for line in f if line.strip()]
    allowed, pin_problems = pinned_allowed(objects)
    problems = object_problems(objects) + symbol_problems(program_symbols(args.exe), allowed) + pin_problems
    if args.firmware:
        firmware = load_firmware_list()
        problems += source_problems(args.firmware, firmware.sources(args.firmware) + firmware.wavpack_sources())
    print('%d objects checked' % len(objects))
    for x in problems:
        print(x)
    print('RESULT: FAIL' if problems else 'RESULT: PASS')
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
