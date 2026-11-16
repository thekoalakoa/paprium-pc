# Copyright (C) 2026 Marsupial Solutions LLC
# SPDX-License-Identifier: GPL-3.0-or-later
r"""Makes the players' package: the folder a player unzips and starts, with no Python, compiler or setup of their own.

    python -I -B targets/pc/pc_package.py --config LOCAL.json --out FOLDER --version TEXT [--firmware-zip ZIP | --no-firmware] [--jobs N]
                                          [--keep-work] [--no-check]

The package holds no part of the game. It holds our programs, built here from this repository and the firmware's tree, Little-Man Firmware 1.0's
file as its release published it, and the tools the launcher uses to build the game on the player's PC from the player's own ROM, each copied from
its publisher's archive after a check of that archive's hash (TOOL_PINS):

    Paprium.exe            the launcher (pl_launcher.c): the one program a player starts
    little_man_firmware.bin  Little-Man Firmware 1.0, the firmware file of the Paprium core 0.3.0 for the Analogue Pocket, byte for byte as that
                           release published it: the game program reads CPU MATE's brain from it (pc_brain.h), and beside Paprium.exe is the
                           first place it looks (pc_main.c, brain_places), so CPU MATE works with no other download. It is taken only from the
                           release's own zip, Little-Man_Firmware_1.0.zip, named by --firmware-zip or by firmware_zip in the local configuration,
                           after the zip's SHA-256 and the file's MD5 and SHA-256 matched the published values (FIRMWARE_PINS). This repository
                           never holds it. --no-firmware makes a package without it, as the tests' packages for a made-up ROM are made
    pc_port.exe, SDL2.dll  the game program, built to load the game DLL the launcher builds (pc_game_dll.h); it holds no generated code
    gamecontrollerdb.txt   SDL's community controller database, as published: the pads SDL does not know itself (pc_input.c)
    READ_ME.txt            how to play, and the terms, in the words they were given in
    NOTICES.txt, licences\, source\    every notice and licence text of the parts above, and the sources their licences ask for
    tools\rules.json       what the launcher and the translator need to know: the hash of the ROM, the hash of the boot-patched image, the
                           block addresses the game's log watches, a list of code addresses for discovery, the hash of the DLL built and
                           tested here. Rule data: none of it works without the player's own ROM
    tools\pc_boot_image.exe, tools\translator\, tools\rt\     the firmware's boot tool, the translator's source and the runtime's headers
    tools\python\          Python 3.12, embeddable, with NumPy in tools\python\site
    tools\tcc\             TinyCC 0.9.27, built here from its source archive by targets/pc/pc_tinycc.py; the archive and that script are in
                           source\, so the compiler and its source correspond exactly

Our three programs are shipped without their symbol tables: the package carries the twins pc_build.link_shipped links with -s and checks
against the programs with them, which stay in the work folder for tests/check_pc_link.py and the linker maps.

The local configuration names, besides the usual fields, `player_tools`: the archives of the embeddable Python, the NumPy wheel and TinyCC's
source. Making the package needs the ROM, as every build does: the hashes above are measured from it, and the last step builds the game once
with the package's own launcher, from a copy of the package, as a player would, to record the DLL's hash and to prove the package works. That
build's player folder is in the work folder (its app-data folder is pointed there), never in this PC's own app-data folder. Every game-derived
file of that (the work folder's translation, the copy's ROM copy and DLL) is deleted at the end, and tests/check_package.py checks the finished
folder for pieces of the ROM, and that the only firmware file in it is the published one, beside Paprium.exe. Exit 0 with RESULT: PASS, or 1.
"""
import argparse
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, REPO)

import ptool  # noqa: E402
from targets.pc import pc_build, pc_tinycc  # noqa: E402
from tool import gate  # noqa: E402

# The tools the package carries, as their publishers' archives: file name, SHA-256, and where the hash was checked against.
TOOL_PINS = {
    'python_embed': ('python-3.12.10-embed-amd64.zip', '4acbed6dd1c744b0376e3b1cf57ce906f9dc9e95e68824584c8099a63025a3c3',
                     'python.org publishes this file\'s MD5, fe8ef205f2e9c3ba44d0cf9954e1abd3, which it matches'),
    'numpy_wheel': ('numpy-2.5.3-cp312-cp312-win_amd64.whl', '0a59a421a32580a009e8a1751345bf829631b990dc1794b80514ab722b435def',
                    'the SHA-256 PyPI publishes for this file'),
    'tcc_source': ('tcc-0.9.27.tar.bz2', 'de23af78fca90ce32dff2dd45b3432b2334740bb9bb7b05bf60fdbfc396ceb9c',
                   'the SHA-256 Debian publishes for the same file, as tcc_0.9.27.orig.tar.bz2'),
}

# Little-Man Firmware 1.0's file, as the Paprium core 0.3.0's release published it: the release's zip, with the SHA-256 the release's SHA256SUMS
# gives for it, and in that zip little_man_firmware.bin, 256 KB, with the MD5 and the SHA-256 the release notes give for it. The package carries
# exactly this file, beside Paprium.exe; tests/check_package.py refuses any other firmware file, and this one anywhere else.
FIRMWARE_ZIP = 'Little-Man_Firmware_1.0.zip'
FIRMWARE_FILE = 'little_man_firmware.bin'
FIRMWARE_BYTES = 0x40000                # 256 KB; not a game address
FIRMWARE_PINS = {'zip_sha256': '0cbecafd4910deb00b27349d820e8507f3e882a9d7aa5764af5398d5ec5d501f',
                 'md5': 'f8cda67209d0e4d35beb077ae88fb16b',
                 'sha256': '6fb90204322c6a5596bfb4d3a743696cbdced5744cf8d4b9f09aa9fc322873e2'}
# The translator's files the launcher's translation step needs, from this repository.
TRANSLATOR = ['ptool.py', 'targets/pc/pl_translate.py', 'targets/pc/pc_build.py', 'targets/pc/pc_firmware.py', 'tool/__init__.py', 'tool/gate.py',
              'tool/image.py', 'tool/patchsrc.py', 'tool/discover.py', 'tool/ramcode.py', 'tool/dec68k.py', 'tool/regions.py', 'tool/flagsopt.py',
              'tool/lift.py', 'tool/emit_c.py', 'tool/cycles68k.py']
# What TinyCC compiles the generated code against, on the player's PC.
RUNTIME = ['runtime/pt_rt.h', 'runtime/pt_port.h', 'runtime/pt_flags.h', 'targets/pc/pc_game_exports.c']
# Folders and files of NumPy that the translator never loads: its tests, its Fortran wrapper tool, its packaging hooks, its documents, its type
# stubs. The rest stays as NumPy publishes it.
NUMPY_DROP_DIRS = ('tests', 'f2py', '_pyinstaller', 'doc')
NUMPY_DROP_EXT = ('.pyi',)
# The keys of the rules' hints the translation reads (tool/discover.py): code addresses to start from, and the ranges that hold code. Nothing
# else may go into the package's rules, so a hint of any other kind in the local configuration stops the package.
ALLOWED_HINT_KEYS = ('extra_seeds', 'text_ranges')
WINDOWS_ONLY_PATH = r'C:\Windows\system32;C:\Windows;C:\Windows\System32\Wbem'
NEED_FREE_MB = 96
# Below a package folder whose path is at most this long (PL_MAX_FOLDER in pl_launcher.c, which tells the player to move a longer one when
# something fails), every file of the package stays inside Windows' 259 characters: no file may lie deeper than DEEPEST below the folder.
MAX_PACKAGE_FOLDER = 160
DEEPEST = 259 - MAX_PACKAGE_FOLDER - 1
MSYS_LICENCES = os.path.join(os.path.dirname(os.path.dirname(pc_build.GCC)), 'share', 'licenses')


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def checked_tool(tools, key):
    """The path of one tool's archive, after its hash matched its pin."""
    name, want, _ = TOOL_PINS[key]
    path = (tools or {}).get(key)
    if not path or not os.path.exists(path):
        raise SystemExit('player_tools names no %s (%s) in the local configuration' % (key, name))
    got = sha256(path)
    if got != want:
        raise SystemExit('%s is not the published %s: SHA-256 %s' % (path, name, got))
    return path


def firmware_from_zip(path, pins=None):
    """little_man_firmware.bin out of Little-Man Firmware 1.0's published zip, read in memory: the zip's SHA-256 must be the published one, the zip
    must hold that file once, at its top level, and the file's length, MD5 and SHA-256 must be the published ones. pins replaces FIRMWARE_PINS for
    the tests' made-up zips only. Returns the file's bytes; stops with the reason otherwise."""
    pins = pins or FIRMWARE_PINS
    if not path or not os.path.isfile(path):
        raise SystemExit('no firmware zip at %s' % path)
    with open(path, 'rb') as f:
        data = f.read()
    got = hashlib.sha256(data).hexdigest()
    if got != pins['zip_sha256']:
        raise SystemExit('%s is not the published %s: SHA-256 %s' % (path, FIRMWARE_ZIP, got))
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        names = [n for n in z.namelist() if n.split('/')[-1].lower() == FIRMWARE_FILE]
        if names != [FIRMWARE_FILE]:
            raise SystemExit('%s holds %s as %s, not once at its top level' % (path, FIRMWARE_FILE, names))
        fw = z.read(FIRMWARE_FILE)
    md5, sha = hashlib.md5(fw).hexdigest(), hashlib.sha256(fw).hexdigest()
    if len(fw) != FIRMWARE_BYTES or md5 != pins['md5'] or sha != pins['sha256']:
        raise SystemExit('%s in %s is not the published file: %d bytes, MD5 %s, SHA-256 %s' % (FIRMWARE_FILE, path, len(fw), md5, sha))
    return fw


def unzip(archive, into, keep=lambda name: True, strip=''):
    with zipfile.ZipFile(archive) as z:
        for info in z.infolist():
            name = info.filename
            if strip:
                if not name.startswith(strip):
                    continue
                name = name[len(strip):]
            if not name or name.endswith('/') or not keep(name):
                continue
            target = os.path.join(into, *name.split('/'))
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with z.open(info) as src, open(target, 'wb') as dst:
                shutil.copyfileobj(src, dst)


def numpy_keep(name):
    parts = name.split('/')
    if parts[0] == 'numpy' and any(p in NUMPY_DROP_DIRS for p in parts[1:-1]):
        return False
    return not name.endswith(NUMPY_DROP_EXT)


def text_from_tar(archive, member):
    with tarfile.open(archive, 'r:bz2') as t:
        return t.extractfile(member).read()


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copyfile(src, dst)


def windows_only_env(extra=None):
    """What a player's PC has: Windows' own folders on PATH and nothing of Python, gcc or MSYS2."""
    keep = ('SYSTEMROOT', 'SYSTEMDRIVE', 'WINDIR', 'TEMP', 'TMP', 'USERPROFILE', 'LOCALAPPDATA', 'APPDATA', 'COMSPEC', 'PATHEXT', 'USERNAME',
            'NUMBER_OF_PROCESSORS', 'PROCESSOR_ARCHITECTURE')
    e = {k: v for k, v in os.environ.items() if k.upper() in keep}
    e['PATH'] = WINDOWS_ONLY_PATH
    e.update(extra or {})
    return e


def translation_rules(cfg, patch_name):
    """The rules that decide what the translation writes. Seeds from earlier runs' logs become a plain list of code addresses."""
    hints = dict(cfg.get('hints') or {})
    other = sorted(set(hints) - set(ALLOWED_HINT_KEYS))
    if other:
        raise SystemExit('the local configuration\'s hints hold %s: only %s may go into the package' % (', '.join(other), ', '.join(ALLOWED_HINT_KEYS)))
    seeds = ptool.seeds_from_logs(cfg)
    if seeds:
        hints['extra_seeds'] = sorted(set(list(hints.get('extra_seeds', [])) + seeds))
    return {
        'rom_size': None,
        'rom_sha256': cfg['rom_sha256'].lower(),
        'patch_source': ('patches/' + patch_name) if patch_name else '',
        'hints': hints,
        'pc_watch': list(cfg.get('pc_watch') or []),
        'bus_low_device_end': cfg.get('bus_low_device_end', 0),
    }


def build_id(tools_dir, rules):
    """One hash over every file the build on the player's PC reads but the rules file, and the rules that decide the translation."""
    h = hashlib.sha256(json.dumps(rules, sort_keys=True).encode())
    for root, _, files in sorted(os.walk(tools_dir)):
        for name in sorted(files):
            path = os.path.join(root, name)
            rel = os.path.relpath(path, tools_dir).replace('\\', '/')
            if rel == 'rules.json':
                continue
            h.update(rel.encode() + b'\0' + sha256(path).encode() + b'\n')
    return h.hexdigest()


def package_text(name, **fields):
    """One of the package's texts from targets/package: its header lines go, the rest is filled in."""
    with open(os.path.join(REPO, 'targets', 'package', name), encoding='utf-8') as f:
        text = f.read().split('#---- everything below this line goes into the package\n', 1)[1]
    for key, value in fields.items():
        text = text.replace('@%s@' % key, value)
    return text


def sdl_licence(sdl_dir):
    """SDL2's licence: at the top of its development release, or in the folder above the one with the headers."""
    for folder in (sdl_dir, os.path.dirname(pc_build._sdl_folder(sdl_dir))):
        if os.path.exists(os.path.join(folder, 'LICENSE.txt')):
            return os.path.join(folder, 'LICENSE.txt')
    raise SystemExit('no LICENSE.txt in SDL2\'s files ' + sdl_dir)


def main(argv=None):
    p = argparse.ArgumentParser()
    p.add_argument('--config', required=True)
    p.add_argument('--out', required=True)
    p.add_argument('--version', required=True)
    p.add_argument('--jobs', type=int, default=2)
    p.add_argument('--work', help='the build folder, outside the package; by default OUT.work')
    p.add_argument('--keep-work', action='store_true', help='keep the compiled objects for the next run (the game-derived files go anyway)')
    p.add_argument('--no-check', action='store_true', help='skip tests/check_package.py at the end (the tests run it themselves)')
    p.add_argument('--fw-commit', default='', help='for NOTICES.txt: the commit of the published firmware/ folder (the Paprium core 0.3.0, tag v0.3.0) '
                   'whose files equal those of the firmware tree the programs are built from')
    p.add_argument('--firmware-zip', help='the published Little-Man_Firmware_1.0.zip, whose little_man_firmware.bin goes beside Paprium.exe; by default '
                   'firmware_zip in the local configuration')
    p.add_argument('--no-firmware', action='store_true', help='a package without little_man_firmware.bin (the tests\' packages for a made-up ROM)')
    args = p.parse_args(argv)
    try:
        cfg = gate.load_config(args.config)
        rom = gate.check_rom(cfg)
    except gate.GateRefused as x:
        print(x)
        return 1
    out = os.path.abspath(args.out)
    work = os.path.abspath(args.work or out + '.work')
    if os.path.exists(out) and os.listdir(out):
        print('the package folder %s is not empty' % out)
        return 1
    firmware = None
    if not args.no_firmware:
        source = args.firmware_zip or cfg.get('firmware_zip')
        if not source:
            print('name the published %s with --firmware-zip or firmware_zip in the local configuration, or make a package without it with --no-firmware'
                  % FIRMWARE_ZIP)
            return 1
        firmware = firmware_from_zip(source)
        print('%s from %s: %d bytes, MD5 %s, SHA-256 %s, as published' % (FIRMWARE_FILE, FIRMWARE_ZIP, len(firmware), FIRMWARE_PINS['md5'],
                                                                          FIRMWARE_PINS['sha256']), flush=True)
    tools = cfg.get('player_tools') or {}
    archives = {k: checked_tool(tools, k) for k in TOOL_PINS}
    controller_db, controller_db_licence = pc_build.controller_db_files(cfg.get('controller_db'))
    os.makedirs(out, exist_ok=True)
    os.makedirs(work, exist_ok=True)
    tree, sdl, cores = cfg['fw_source'], cfg['sdl_dir'], cfg['sound_cores']

    # ---- the rule data, measured from the ROM: the boot tool's list and the translation give the image hash.
    print('the boot tool', flush=True)
    boot_exe = pc_build.build_player_boot_image(tree, work, workers=args.jobs)
    boot = os.path.join(work, 'boot_image.json')
    r = subprocess.run([boot_exe, cfg['rom_path'], boot], capture_output=True, text=True, env=pc_build.env())
    if r.returncode:
        print('the boot tool failed:', r.stdout[-500:], r.stderr[-500:])
        return 1
    tr = os.path.join(work, 'tr')
    os.makedirs(tr, exist_ok=True)
    tcfg = dict(cfg)
    tcfg['local_patch_files'] = [boot]
    print('the translation, for the image hash', flush=True)
    import contextlib
    import io
    with contextlib.redirect_stdout(io.StringIO()):
        ptool.do_translate(tcfg, rom, tr, watch=True)
    with open(os.path.join(tr, 'image_check.txt'), encoding='ascii') as f:
        image_check = f.read().strip()

    # ---- our programs
    print('the game program and the launcher', flush=True)
    exe = pc_build.build_player_program(tree, work, sdl, cfg['rom_sha256'].lower(), image_check, cores, workers=args.jobs)
    launcher = pc_build.build_launcher(work, workers=args.jobs)
    r = subprocess.run([sys.executable, '-I', '-B', os.path.join(REPO, 'tests', 'check_pc_link.py'), '--exe', exe, '--rsp', exe + '.rsp', '--firmware', tree],
                       capture_output=True, text=True, env=pc_build.env())
    print(r.stdout.strip().splitlines()[-1] if r.stdout.strip() else '', '(tests/check_pc_link.py on the game program)')
    if r.returncode:
        print(r.stdout[-1500:])
        print('RESULT: FAIL: the game program links what it must not')
        return 1

    # ---- the package
    print('the package', flush=True)
    t = os.path.join(out, 'tools')
    # Our programs go without their symbol tables: their twins, which run exactly the same code and data (pc_build.link_shipped).
    copy(pc_build.shipped(launcher), os.path.join(out, 'Paprium.exe'))
    copy(pc_build.shipped(exe), os.path.join(out, pc_build.GAME_EXE))
    copy(pc_build.shipped(boot_exe), os.path.join(t, 'pc_boot_image.exe'))
    copy(os.path.join(work, 'SDL2.dll'), os.path.join(out, 'SDL2.dll'))
    copy(controller_db, os.path.join(out, 'gamecontrollerdb.txt'))
    if firmware is not None:
        with open(os.path.join(out, FIRMWARE_FILE), 'wb') as f:
            f.write(firmware)
    for rel in TRANSLATOR:
        name = os.path.basename(rel) if rel.startswith('targets/pc/pl_') else rel
        copy(os.path.join(REPO, rel), os.path.join(t, 'translator', *name.split('/')))
    patch_name = ''
    if cfg.get('patch_source'):
        patch_name = os.path.basename(cfg['patch_source'])
        copy(cfg['patch_source'], os.path.join(t, 'translator', 'patches', patch_name))
    for rel in RUNTIME:
        copy(os.path.join(REPO, rel), os.path.join(t, 'rt', os.path.basename(rel)))
    pc_build.game_dll_def(os.path.join(t, 'rt', 'pc_port_import.def'), pc_build.GAME_EXE, 'import')
    py = os.path.join(t, 'python')
    unzip(archives['python_embed'], py)
    with open(os.path.join(py, 'python312._pth'), 'w', encoding='ascii', newline='\n') as f:
        f.write('python312.zip\n.\nsite\n')
    unzip(archives['numpy_wheel'], os.path.join(py, 'site'), numpy_keep)
    print('TinyCC, from its source', flush=True)
    tcc_work = os.path.join(work, 'tcc_build')
    pc_tinycc.build(archives['tcc_source'], os.path.join(t, 'tcc'), pc_build.GCC, tcc_work)
    for name in ('tcc.exe.map', 'libtcc.dll.map'):
        shutil.copyfile(os.path.join(tcc_work, name), os.path.join(work, name))     # for tests/check_package.py --maps

    lic = os.path.join(out, 'licences')
    copy(os.path.join(REPO, 'LICENSE'), os.path.join(lic, 'COPYING_GPL3.txt'))
    copy(sdl_licence(sdl), os.path.join(lic, 'SDL2_LICENSE.txt'))
    copy(controller_db_licence, os.path.join(lic, pc_build.CONTROLLER_DB_LICENCE))
    for name, licence in pc_build.SOUND_CORE_LICENCES.items():
        copy(os.path.join(cores[name], 'LICENSE'), os.path.join(lic, licence))
    copy(os.path.join(tree, 'THIRD_PARTY_NOTICES.md'), os.path.join(lic, 'THIRD_PARTY_NOTICES_firmware.md'))
    copy(os.path.join(MSYS_LICENCES, 'crt', 'COPYING.MinGW-w64-runtime.txt'), os.path.join(lic, 'MINGW_RUNTIME_LICENSE.txt'))
    copy(os.path.join(MSYS_LICENCES, 'gcc-libs', 'COPYING.RUNTIME'), os.path.join(lic, 'GCC_RUNTIME_LIBRARY_EXCEPTION.txt'))
    copy(os.path.join(py, 'LICENSE.txt'), os.path.join(lic, 'PYTHON_LICENSE.txt'))
    copy(os.path.join(py, 'site', 'numpy-2.5.3.dist-info', 'licenses', 'LICENSE.txt'), os.path.join(lic, 'NUMPY_LICENSE.txt'))
    with open(os.path.join(lic, 'TINYCC_COPYING.txt'), 'wb') as f:
        f.write(text_from_tar(archives['tcc_source'], 'tcc-0.9.27/COPYING'))
    # The GNU GPL version 2, for the part of TinyCC's runtime library under it (lib\libtcc1.c). Nuked-PSG's pinned licence file is that text.
    copy(os.path.join(cores['psg'], 'LICENSE'), os.path.join(lic, 'COPYING_GPL2.txt'))
    src = os.path.join(out, 'source')
    copy(archives['tcc_source'], os.path.join(src, TOOL_PINS['tcc_source'][0]))
    copy(os.path.join(HERE, 'pc_tinycc.py'), os.path.join(src, 'pc_tinycc.py'))
    for name, files in pc_build.SOUND_CORE_PINS.items():
        for rel in files:
            copy(os.path.join(cores[name], rel), os.path.join(src, 'sound_cores', name, *rel.split('/')))
    for name in pc_build.SHADER_PINS:
        copy(os.path.join(pc_build.SHADERS, name), os.path.join(src, 'shaders', name))
    with open(os.path.join(src, 'SOURCE.txt'), 'w', encoding='utf-8', newline='\r\n') as f:
        f.write(package_text('SOURCE.txt', VERSION=args.version))
    with open(os.path.join(out, 'READ_ME.txt'), 'w', encoding='utf-8', newline='\r\n') as f:
        f.write(package_text('READ_ME.txt', VERSION=args.version))
    with open(os.path.join(out, 'NOTICES.txt'), 'w', encoding='utf-8', newline='\r\n') as f:
        f.write(package_text('NOTICES.txt', VERSION=args.version, FW=args.fw_commit or 'named by the package\'s makers'))

    deepest = max(len(os.path.relpath(os.path.join(a, n), out)) for a, _, ns in os.walk(out) for n in ns)
    print('the deepest file lies %d characters below the package\'s folder (at most %d)' % (deepest, DEEPEST))
    if deepest > DEEPEST:
        print('RESULT: FAIL: a file lies deeper than the launcher\'s path rule allows')
        return 1

    rules = translation_rules(cfg, patch_name)
    rules['rom_size'] = len(rom)
    identity = build_id(t, rules)
    full = {'copyright': 'Copyright (C) 2026 Marsupial Solutions LLC', 'spdx': 'GPL-3.0-or-later',
            'note': 'Rule data for the launcher and the translator. It names hashes of the game file and addresses in it, and is no use without '
                    'the player\'s own copy of the game.',
            'package_version': args.version, 'build_id': identity, 'game_exe': pc_build.GAME_EXE, 'need_free_mb': NEED_FREE_MB,
            'image_check': image_check, 'tested_dll_sha256': ''}
    full.update(rules)
    rules_path = os.path.join(t, 'rules.json')
    with open(rules_path, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(full, f, indent=1)

    # ---- the proof: a copy of the package builds the game as a player's PC would, with only Windows on PATH
    print('the launcher builds the game from a copy of the package', flush=True)
    trial = os.path.join(work, 'trial')
    trial_appdata = os.path.join(work, 'trial_appdata')
    for x in (trial, trial_appdata):
        if os.path.exists(x):
            shutil.rmtree(x)
    shutil.copytree(out, trial)
    os.makedirs(trial_appdata)
    before = sorted(os.path.relpath(os.path.join(a, n), trial) for a, _, ns in os.walk(trial) for n in ns)
    r = subprocess.run([os.path.join(trial, 'Paprium.exe'), '--headless', '--no-play', '--rom', cfg['rom_path']], capture_output=True,
                       text=True, encoding='utf-8', errors='replace', env=windows_only_env({'LOCALAPPDATA': trial_appdata}), cwd=work)
    print(r.stdout[-1500:])
    dll = os.path.join(trial_appdata, 'Paprium PC', 'pc_game.dll')
    if r.returncode or not os.path.exists(dll):
        print('RESULT: FAIL: the package\'s launcher did not build the game (exit %d)' % r.returncode)
        return 1
    after = sorted(os.path.relpath(os.path.join(a, n), trial) for a, _, ns in os.walk(trial) for n in ns)
    if after != before:
        print('RESULT: FAIL: the build wrote into the package\'s own folder:', sorted(set(after) - set(before))[:10])
        return 1
    full['tested_dll_sha256'] = sha256(dll)
    with open(rules_path, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(full, f, indent=1)
    print('tested build: pc_game.dll %d bytes, SHA-256 %s' % (os.path.getsize(dll), full['tested_dll_sha256']))

    # ---- what the game made goes, then the check of the finished folder, with the programs' linker maps; the objects may stay for the next run
    shutil.rmtree(trial)
    shutil.rmtree(trial_appdata)
    shutil.rmtree(tr)
    os.remove(boot)
    failed = False
    if not args.no_check:
        r = subprocess.run([sys.executable, '-I', '-B', os.path.join(REPO, 'tests', 'check_package.py'), '--config', args.config, '--package', out,
                            '--maps', work, '--firmware', 'absent' if firmware is None else 'required'])
        failed = r.returncode != 0
    if not args.keep_work:
        shutil.rmtree(work)
    if failed:
        print('RESULT: FAIL: tests/check_package.py found something')
        return 1
    if firmware is not None:
        with open(os.path.join(out, FIRMWARE_FILE), 'rb') as f:
            kept = f.read()
        if kept != firmware:
            print('RESULT: FAIL: %s beside Paprium.exe changed while the package was made' % FIRMWARE_FILE)
            return 1
        print('%s beside Paprium.exe: %d bytes, MD5 %s, SHA-256 %s; published: MD5 %s, SHA-256 %s' % (
            FIRMWARE_FILE, len(kept), hashlib.md5(kept).hexdigest(), hashlib.sha256(kept).hexdigest(), FIRMWARE_PINS['md5'], FIRMWARE_PINS['sha256']))
    else:
        print('no %s in this package (--no-firmware)' % FIRMWARE_FILE)
    print('package', out, 'version', args.version, 'build id', identity)
    print('RESULT: PASS')
    return 0






if __name__ == '__main__':
    sys.exit(main())
