# Copyright (C) 2026 Marsupial Solutions LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Shared test helpers: translate a test ROM, build it with gcc, run it, and run it on Unicorn.

Only test programs we assemble ourselves go through here. Scratch files live in a temporary
folder given by the caller, outside the repository.
"""
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, REPO)
sys.path.insert(0, HERE)

from tool import discover, emit_c, flagsopt, image, regions  # noqa: E402

RUNTIME = ['pt_rt.c', 'pt_fallback.c', 'pt_port_null.c', 'pt_main_host.c']
GCC = shutil.which('gcc') or r'C:\msys64\mingw64\bin\gcc.exe'
EXIT_PC = 0x7F0000


def gcc_env():
    env = dict(os.environ)
    env['PATH'] = os.path.dirname(GCC) + os.pathsep + env.get('PATH', '')
    return env


def translate(rom, gen_dir, every_insn=False, plant_flag_bug=False):
    img = image.Image(rom)
    d = discover.Discovery(img).run()
    blocks, regs = regions.build(d, every_insn=every_insn)
    flagsopt.analyse(blocks, regs, all_live=every_insn)
    if plant_flag_bug:             # a negative control: pretend nothing is live, so flags are not stored
        for b in blocks.values():
            b.live_out = 0
    prog = emit_c.Program(d, blocks, regs, all_live=every_insn)
    files = prog.write(gen_dir)
    return prog, files


def build(files, gen_dir, exe, extra=()):
    rt = os.path.join(REPO, 'runtime')
    cmd = [GCC, '-O1', '-w', '-std=gnu11', '-I', gen_dir, '-I', rt, '-I', os.path.join(REPO, 'rig')]
    cmd += list(extra) + files + [os.path.join(rt, x) for x in RUNTIME] + ['-o', exe]
    r = subprocess.run(cmd, capture_output=True, text=True, env=gcc_env())
    if r.returncode:
        raise RuntimeError('gcc failed:\n' + r.stderr[:4000])


def run_native(exe, rom_path, ram_path, cwd, interp=False):
    cmd = [exe, rom_path, '%X' % EXIT_PC, ram_path] + (['interp'] if interp else [])
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=cwd, env=gcc_env())
    state = {'rc': r.returncode, 'stderr': r.stderr}
    for line in r.stdout.splitlines():
        k, v = line.split(None, 1)
        state[k] = v
    if os.path.exists(ram_path):
        with open(ram_path, 'rb') as f:
            state['ram'] = f.read()
    return state


def run_unicorn(rom, count=2000000):
    """The same test ROM on Unicorn's 68000, as the reference for tests without a machine."""
    from unicorn import Uc, UC_ARCH_M68K, UC_MODE_BIG_ENDIAN
    from unicorn import m68k_const as K
    from unicorn import unicorn_const as UK
    uc = Uc(UC_ARCH_M68K, UC_MODE_BIG_ENDIAN)
    uc.ctl_set_cpu_model(UK.UC_CPU_M68K_M68000 if hasattr(UK, 'UC_CPU_M68K_M68000') else K.UC_CPU_M68K_M68000)
    uc.mem_map(0, 0x100000)
    uc.mem_map(0xFF0000, 0x10000)
    uc.mem_map(EXIT_PC & ~0xFFF, 0x1000)
    uc.mem_write(0, rom)
    start = int.from_bytes(rom[4:8], 'big')
    uc.reg_write(K.UC_M68K_REG_A7, int.from_bytes(rom[0:4], 'big'))
    uc.reg_write(K.UC_M68K_REG_SR, 0x2700)
    uc.emu_start(start, EXIT_PC, count=count)
    state = {'pc': '%06X' % uc.reg_read(K.UC_M68K_REG_PC)}
    for i in range(8):
        state['d%d' % i] = '%08X' % uc.reg_read(getattr(K, 'UC_M68K_REG_D%d' % i))
        state['a%d' % i] = '%08X' % uc.reg_read(getattr(K, 'UC_M68K_REG_A%d' % i))
    state['ram'] = bytes(uc.mem_read(0xFF0000, 0x10000))
    return state
