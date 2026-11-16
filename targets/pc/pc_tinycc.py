# Copyright (C) 2026 Marsupial Solutions LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds TinyCC 0.9.27 for 64-bit Windows from its own source archive, for the players' package.

    python -I -B pc_tinycc.py SOURCE_ARCHIVE OUT_FOLDER [--gcc PATH_TO_GCC] [--work FOLDER]

The package carries the TinyCC this script builds, and the source archive it was built from (source\\tcc-0.9.27.tar.bz2), so the compiler a
player runs and its source correspond exactly. The steps are those of the archive's own win32\\build-tcc.bat for the 64-bit target, with gcc
as the compiler that builds TinyCC:

  1. config.h names the version (from the archive's VERSION file) and the runtime library's file name, libtcc1-64.a.
  2. gcc -Os -s -static builds libtcc.dll from libtcc.c, then tcc.exe from tcc.c against it, with -DTCC_TARGET_PE -DTCC_TARGET_X86_64. Two
     options are ours: -Wl,--no-insert-timestamp, so two builds of the same source give the same bytes, and -Wl,-Map, a linker map kept
     beside the build (not in the package) for the package's leak check.
  3. TinyCC's own headers (include\\*.h and tcclib.h) join the Windows headers of win32\\include, as build-tcc.bat copies them.
  4. The new tcc.exe compiles its runtime library, lib\\libtcc1-64.a, from the archive's files, with the commands build-tcc.bat uses.

What is left out of build-tcc.bat's output: the 32-bit cross compiler (i386-win32-tcc.exe) and its 32-bit runtime library, the examples, the
documents and the libtcc folder for programs that embed TinyCC. None of them is used to build the game.

OUT_FOLDER gets tcc.exe, libtcc.dll, include\\ and lib\\ (libtcc1-64.a and the archive's four .def files), the layout TinyCC looks for beside
itself. Exit 0, or 1 with the failing step's own words.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile

ARCHIVE_SHA256 = 'de23af78fca90ce32dff2dd45b3432b2334740bb9bb7b05bf60fdbfc396ceb9c'   # tcc-0.9.27.tar.bz2, as targets/pc/pc_package.py pins it
TOP = 'tcc-0.9.27'
DEFINES = ['-DTCC_TARGET_PE', '-DTCC_TARGET_X86_64']
# The runtime library's parts, in build-tcc.bat's order: (source relative to win32\, extra option).
RUNTIME_PARTS = [('../lib/libtcc1.c', ''), ('lib/crt1.c', ''), ('lib/crt1w.c', ''), ('lib/wincrt1.c', ''), ('lib/wincrt1w.c', ''),
                 ('lib/dllcrt1.c', ''), ('lib/dllmain.c', ''), ('lib/chkstk.S', ''), ('../lib/bcheck.c', '-w'), ('../lib/alloca86_64.S', ''),
                 ('../lib/alloca86_64-bt.S', '')]
DEF_FILES = ('gdi32.def', 'kernel32.def', 'msvcrt.def', 'user32.def')


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def run(cmd, cwd, env, log):
    r = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True, errors='replace')
    log.append('%s\n%s%s' % (' '.join(cmd), r.stdout, r.stderr))
    if r.returncode:
        raise RuntimeError('%s failed (exit %d): %s' % (os.path.basename(cmd[0]), r.returncode, (r.stdout + r.stderr)[-2000:]))


def build(archive, out, gcc, work):
    """Builds TinyCC from archive into out, using gcc, with work as the build folder (emptied first). Returns the build's log lines.
    The linker maps of tcc.exe and libtcc.dll are left in work as tcc.exe.map and libtcc.dll.map."""
    if sha256(archive) != ARCHIVE_SHA256:
        raise RuntimeError('%s is not the published tcc-0.9.27.tar.bz2' % archive)
    if os.path.exists(work):
        shutil.rmtree(work)
    os.makedirs(work)
    with tarfile.open(archive, 'r:bz2') as t:
        t.extractall(work, filter='data')
    src = os.path.join(work, TOP)
    win = os.path.join(src, 'win32')
    with open(os.path.join(src, 'VERSION'), encoding='ascii') as f:
        version = f.read().strip()
    with open(os.path.join(src, 'config.h'), 'w', encoding='ascii', newline='\n') as f:
        f.write('#define TCC_VERSION "%s"\n#ifdef TCC_TARGET_X86_64\n#define TCC_LIBTCC1 "libtcc1-64.a"\n#else\n#define TCC_LIBTCC1 "libtcc1-32.a"\n#endif\n'
                % version)
    env = dict(os.environ)
    env['PATH'] = os.path.dirname(gcc) + os.pathsep + env.get('PATH', '')
    cc = [gcc, '-Os', '-s', '-static', '-Wl,--no-insert-timestamp']
    log = []
    map_dll, map_exe = os.path.join(work, 'libtcc.dll.map'), os.path.join(work, 'tcc.exe.map')
    run(cc + ['-Wl,-Map,' + map_dll, '-o', 'libtcc.dll', '-shared', '../libtcc.c'] + DEFINES + ['-DLIBTCC_AS_DLL'], win, env, log)
    run(cc + ['-Wl,-Map,' + map_exe, '-o', 'tcc.exe', '../tcc.c', 'libtcc.dll'] + DEFINES + ['-DONE_SOURCE=0'], win, env, log)
    include = os.path.join(win, 'include')
    for name in sorted(os.listdir(os.path.join(src, 'include'))):
        if name.endswith('.h'):
            shutil.copyfile(os.path.join(src, 'include', name), os.path.join(include, name))
    shutil.copyfile(os.path.join(src, 'tcclib.h'), os.path.join(include, 'tcclib.h'))
    objects = []
    for part, extra in RUNTIME_PARTS:
        run([os.path.join(win, 'tcc.exe'), '-m64'] + ([extra] if extra else []) + ['-c', part], win, env, log)
        objects.append(os.path.splitext(os.path.basename(part))[0] + '.o')
    run([os.path.join(win, 'tcc.exe'), '-m64', '-ar', 'lib/libtcc1-64.a'] + objects, win, env, log)

    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(os.path.join(out, 'lib'))
    for name in ('tcc.exe', 'libtcc.dll'):
        shutil.copyfile(os.path.join(win, name), os.path.join(out, name))
    shutil.copytree(include, os.path.join(out, 'include'))
    shutil.copyfile(os.path.join(win, 'lib', 'libtcc1-64.a'), os.path.join(out, 'lib', 'libtcc1-64.a'))
    for name in DEF_FILES:
        shutil.copyfile(os.path.join(win, 'lib', name), os.path.join(out, 'lib', name))
    return log


def main(argv=None):
    p = argparse.ArgumentParser()
    p.add_argument('archive')
    p.add_argument('out')
    p.add_argument('--gcc', default=shutil.which('gcc') or r'C:\msys64\mingw64\bin\gcc.exe')
    p.add_argument('--work', help='the build folder; by default OUT.build')
    args = p.parse_args(argv)
    try:
        build(os.path.abspath(args.archive), os.path.abspath(args.out), args.gcc, os.path.abspath(args.work or args.out + '.build'))
    except (RuntimeError, OSError) as x:
        print(x)
        return 1
    print('TinyCC built in', os.path.abspath(args.out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
