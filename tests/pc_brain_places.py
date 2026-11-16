# Copyright (C) 2026 Marsupial Solutions LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Where the PC program looks for CPU MATE's brain file, little_man_firmware.bin, and the words that name the place: scenes on the developers' program
built from a made-up ROM, run from tests/test_pc_machine.py's whole-program test (targets/pc/pc_main.c, brain_places). With no cpumate_brain line the
file is looked for beside the program the player started first, then beside the settings file; the first place where anything of that name is there
is used. Every firmware file here is made up: no byte of the real brain, which is never part of this repository."""
import os
import random
import shutil
import stat
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, REPO)

from targets.pc import pc_build  # noqa: E402

PC = os.path.join(REPO, 'targets', 'pc')


def _made_up_firmware(seed):
    """A made-up Little-Man Firmware file: 256 KB of random bytes with the file's first word and its kind word (built with the brain), each 16-bit pair
    swapped as the file stores it. Its brain is random bytes too: no byte of the real one, so the program reads it and refuses it by the brain's hashes."""
    rnd = random.Random(seed)
    mcu = bytearray(rnd.getrandbits(8) for _ in range(1 << 18))
    mcu[0:4] = b'FAFR'
    struct.pack_into('<I', mcu, 0x20, 0x01000002)
    disk = bytearray(len(mcu))
    disk[0::2], disk[1::2] = mcu[1::2], mcu[0::2]
    return bytes(disk)


def _sparse_file(path, size):
    """A file of size bytes that takes no room on the disk: NTFS is told it is sparse, then its end is set, so nothing is written. Returns whether
    Windows did both (False on a drive without sparse files, where the file is left empty rather than written in full)."""
    import ctypes
    import msvcrt
    from ctypes import wintypes
    k = ctypes.WinDLL('kernel32', use_last_error=True)
    k.DeviceIoControl.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                                  ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
    k.DeviceIoControl.restype = wintypes.BOOL
    k.SetFilePointerEx.argtypes = [wintypes.HANDLE, ctypes.c_longlong, ctypes.c_void_p, wintypes.DWORD]
    k.SetFilePointerEx.restype = wintypes.BOOL
    k.SetEndOfFile.argtypes = [wintypes.HANDLE]
    k.SetEndOfFile.restype = wintypes.BOOL
    with open(path, 'wb') as f:
        handle = msvcrt.get_osfhandle(f.fileno())
        done = wintypes.DWORD(0)
        ok = k.DeviceIoControl(handle, 0x900C4, None, 0, None, 0, ctypes.byref(done), None)    # FSCTL_SET_SPARSE, a Windows code: not a game address
        ok = ok and k.SetFilePointerEx(handle, size, None, 0) and k.SetEndOfFile(handle)
    return bool(ok) and os.path.getsize(path) == size


def _brain_scenes(exe, rom_path, folder, env, menu_script, hold_with_no_sharing):
    """Where CPU MATE's brain file is looked for, and the few words that name it on the window and in the menu, on a hidden window with megacd=on
    (targets/pc/pc_main.c, brain_places; the team's decision). With no cpumate_brain line, little_man_firmware.bin is looked for beside the
    program first (here a copy of the developers' pc_port.exe in a folder of its own), then beside the settings file, here in
    %LOCALAPPDATA%\\Paprium PC as the players' launcher keeps it. The first place where anything of that name is there is used, even a wrong, cut,
    zero-byte or held file, or a folder: the window names that place, the console gives the reason, and the other place is not looked at. With
    neither, the window names both. The console keeps the whole paths. A cpumate_brain line names the one place, whole or from the settings file's
    folder. Every file here is made up: no byte of the real brain. Returns {scene: (ok, detail)}."""
    folder = os.path.abspath(folder)        # the program names its places in Windows' full form, with back slashes
    prog = os.path.join(folder, 'prog')
    alone = os.path.join(folder, 'alone')
    for where in (prog, alone):
        os.makedirs(where, exist_ok=True)
        for name in ('pc_port.exe', 'SDL2.dll', 'gamecontrollerdb.txt'):
            if os.path.exists(os.path.join(os.path.dirname(exe), name)):
                shutil.copyfile(os.path.join(os.path.dirname(exe), name), os.path.join(where, name))
    run_exe = os.path.join(prog, 'pc_port.exe')
    app = os.path.join(folder, 'app data')
    mine = os.path.join(app, 'Paprium PC')
    elsewhere = os.path.join(folder, 'elsewhere')
    copy = os.path.join(prog, 'my_copy')
    for x in (mine, elsewhere, copy):
        os.makedirs(x, exist_ok=True)
    e = dict(env, LOCALAPPDATA=app)
    own = 'rom=%s\nsave=%s\nvsync=off\nmegacd=on\n' % (rom_path.replace('\\', '/'), os.path.join(folder, 'play.sav').replace('\\', '/'))
    first = os.path.join(prog, 'little_man_firmware.bin')
    second = os.path.join(mine, 'little_man_firmware.bin')
    fw = _made_up_firmware(23)
    before = sorted(os.listdir(prog))

    def put(path, data=None, folder_of_that_name=False):
        if folder_of_that_name:
            os.makedirs(path)
        else:
            with open(path, 'wb') as f:
                f.write(data)

    def clear():
        for where in (prog, mine, elsewhere):
            for name in os.listdir(where):
                p = os.path.join(where, name)
                if name.lower() == 'little_man_firmware.bin' or name == 'my brains':
                    if os.path.isdir(p):
                        shutil.rmtree(p)
                    else:
                        os.chmod(p, stat.S_IWRITE)
                        os.remove(p)

    def start(where, extra, presses=None, program=run_exe):
        cfg = os.path.join(where, 'paprium_pc.cfg')
        with open(cfg, 'w', encoding='ascii', newline='') as f:
            f.write(own + extra)
        keys = os.path.join(folder, 'keys.txt')
        if presses is None:
            with open(keys, 'w', encoding='ascii') as f:
                f.write('8 down Escape\n9 up Escape\n')
        else:
            menu_script(keys, presses, end=60)
        run_dir = os.path.join(folder, 'run')
        os.makedirs(run_dir, exist_ok=True)
        r = subprocess.run([program, '--hidden-window', '--config', cfg, '--run-dir', run_dir, '--key-script', keys, '--frames', '40'],
                           capture_output=True, text=True, env=e, timeout=180)
        window = [x for x in r.stdout.splitlines() if x.startswith('on the window for 10 s: ')]
        return r, (window[-1][len('on the window for 10 s: '):] if window else '')

    found = {}
    b1 = 'little_man_firmware.bin beside pc_port.exe'
    b2 = 'little_man_firmware.bin in %LOCALAPPDATA%\\Paprium PC'
    wrong = ' is not the Little-Man Firmware 1.0 file with the brain, so a partner stands still. The console says why.'
    not_used = ' While that file is there, %s is not used.' % b2

    clear()
    r, w = start(mine, 'cpumate=on\n')
    found['neither place: the window names both, the console both whole paths'] = (
        r.returncode == 0 and w == 'CPU MATE: no little_man_firmware.bin beside pc_port.exe or in %LOCALAPPDATA%\\Paprium PC, so a partner stands still. '
        'Put your little_man_firmware.bin (Little-Man Firmware 1.0) in either place, then Soft Reset.' and app not in w and
        ('nothing of that name at %s (beside pc_port.exe) or at %s (in %%LOCALAPPDATA%%\\Paprium PC)' % (first, second)) in r.stdout, w + ' | ' + r.stdout[-400:])

    put(second, bytes(1000))
    r, w = start(mine, 'cpumate=on\n')
    found['the settings file\'s folder only: that file is used and named'] = (
        r.returncode == 0 and w == 'CPU MATE: ' + b2 + wrong and 'that file is 262,144 bytes, this one 1000' in r.stdout and second in r.stdout,
        w + ' | ' + r.stdout[-400:])

    clear()
    put(first, bytes(1000))
    r, w = start(mine, 'cpumate=on\n')
    found['beside the program only: that file is used and named'] = (
        r.returncode == 0 and w == 'CPU MATE: ' + b1 + wrong + not_used and 'that file is 262,144 bytes, this one 1000' in r.stdout and first in r.stdout,
        w + ' | ' + r.stdout[-400:])

    put(second, fw)
    r, w = start(mine, 'cpumate=on\n')
    found['both places, the first one wrong: it is used, the second is not looked at'] = (
        r.returncode == 0 and w == 'CPU MATE: ' + b1 + wrong + not_used and 'this one 1000' in r.stdout and 'other than Little-Man Firmware 1.0' not in r.stdout
        and ('so %s is not looked at while it is there' % second) in r.stdout, w + ' | ' + r.stdout[-400:])

    clear()
    put(first, bytes(0))
    put(second, fw)
    r, w = start(mine, 'cpumate=on\n')
    found['a file of zero bytes beside the program is used, and told as not the file'] = (
        r.returncode == 0 and w == 'CPU MATE: ' + b1 + wrong + not_used and 'this one 0' in r.stdout, w + ' | ' + r.stdout[-400:])

    clear()
    put(first, folder_of_that_name=True)
    put(second, fw)
    r, w = start(mine, 'cpumate=on\n')
    found['a folder named little_man_firmware.bin beside the program is used, and told as not the file'] = (
        r.returncode == 0 and w == 'CPU MATE: ' + b1 + wrong + not_used and 'holds no brain.bin' in r.stdout and 'other than Little-Man' not in r.stdout,
        w + ' | ' + r.stdout[-400:])

    clear()
    put(os.path.join(prog, 'LITTLE_MAN_FIRMWARE.BIN'), bytes(1000))
    put(second, fw)
    r, w = start(mine, 'cpumate=on\n')
    found['the name in other letter case beside the program is the same file to Windows: used'] = (
        r.returncode == 0 and w == 'CPU MATE: ' + b1 + wrong + not_used and 'this one 1000' in r.stdout, w + ' | ' + r.stdout[-400:])

    clear()
    put(first, bytes((i * 37 + 11) & 0xFF for i in range(256 * 1024)))
    put(second, fw)
    let_go = hold_with_no_sharing(first)
    try:
        r, w = start(mine, 'cpumate=on\n')
    finally:
        let_go()
    found['a file beside the program another program holds: it is used, told as unreadable, the second not looked at'] = (
        r.returncode == 0 and w == 'CPU MATE: ' + b1 + ' is there but could not be read, so a partner stands still. The console says why.' + not_used and
        'little_man_firmware.bin cannot be read (' in r.stdout and 'other than Little-Man' not in r.stdout, w + ' | ' + r.stdout[-400:])

    clear()
    put(second, fw)
    os.chmod(second, stat.S_IREAD)
    r, w = start(mine, 'cpumate=on\n')
    found['a read-only file in the settings file\'s folder is read'] = (
        r.returncode == 0 and w == 'CPU MATE: ' + b2 + wrong and 'carries a CPU MATE brain other than Little-Man Firmware 1.0\'s' in r.stdout,
        w + ' | ' + r.stdout[-400:])

    # Files of 2 GB or more, which the C library's 32-bit file sizes cannot hold: each is refused by its true length, unread, and never told as gone.
    for size in (3 << 30, (4 << 30) + (256 << 10)):
        clear()
        put(second, fw)
        sparse = _sparse_file(first, size)
        r, w = start(mine, 'cpumate=on\n')
        found['a file of %d bytes beside the program is used, and told as not the file by its length' % size] = (
            sparse and r.returncode == 0 and w == 'CPU MATE: ' + b1 + wrong + not_used and ('that file is 262,144 bytes, this one %d' % size) in r.stdout
            and 'went away' not in r.stdout, 'sparse file made: %s | %s | %s' % (sparse, w, r.stdout[-400:]))

    clear()
    holder = os.path.join(elsewhere, 'holder.txt')
    put(holder, b'a text file whose stream lm holds a made-up firmware file\n')
    put(holder + ':lm', fw)
    r, w = start(elsewhere, 'cpumate=on\ncpumate_brain=holder.txt:lm\n')
    found['a cpumate_brain line naming a stream of a file (holder.txt:lm) reads that stream'] = (
        r.returncode == 0 and w == 'CPU MATE: holder.txt:lm' + wrong and 'other than Little-Man Firmware 1.0' in r.stdout and 'went away' not in r.stdout,
        w + ' | ' + r.stdout[-400:])
    os.remove(holder)

    clear()
    r, w = start(elsewhere, 'cpumate=on\n')
    found['settings in another folder: the window names the folder of paprium_pc.cfg as the second place'] = (
        r.returncode == 0 and w.startswith('CPU MATE: no little_man_firmware.bin beside pc_port.exe or beside paprium_pc.cfg, so a partner stands still.')
        and elsewhere not in w, w)
    r, w = start(copy, 'cpumate=on\n')
    found['settings in my_copy beside the program: the window names that folder as the second place'] = (
        r.returncode == 0 and w.startswith('CPU MATE: no little_man_firmware.bin beside pc_port.exe or in my_copy, beside Paprium.exe, so a partner stands still.'),
        w)
    r, w = start(alone, 'cpumate=on\n', program=os.path.join(alone, 'pc_port.exe'))
    found['settings beside the program: one folder, looked at once and named once'] = (
        r.returncode == 0 and w == 'CPU MATE: no little_man_firmware.bin beside pc_port.exe, so a partner stands still. Put your little_man_firmware.bin '
        '(Little-Man Firmware 1.0) there, then Soft Reset.', w)

    put(first, bytes(1000))
    r, w = start(elsewhere, 'cpumate=on\ncpumate_brain=my brains\\lm.bin\n')
    found['a cpumate_brain line with no file there: that place only, named as written, with words that fit another name'] = (
        r.returncode == 0 and w == 'CPU MATE: no file at my brains\\lm.bin, the place the cpumate_brain line names, so a partner stands still. Copy your '
        'little_man_firmware.bin (Little-Man Firmware 1.0) to that path, or take the line out, then Soft Reset.' and
        ('no brain at %s' % os.path.join(elsewhere, 'my brains', 'lm.bin')) in r.stdout and 'this one 1000' not in r.stdout, w + ' | ' + r.stdout[-400:])
    os.makedirs(os.path.join(elsewhere, 'my brains'))
    put(os.path.join(elsewhere, 'my brains', 'lm.bin'), fw)
    r, w = start(elsewhere, 'cpumate=on\ncpumate_brain=my brains\\lm.bin\n')
    found['a relative cpumate_brain line starts from the settings file\'s folder, and the file beside the program is not used'] = (
        r.returncode == 0 and w == 'CPU MATE: my brains\\lm.bin' + wrong and os.path.join(elsewhere, 'my brains', 'lm.bin') in r.stdout and
        'other than Little-Man Firmware 1.0' in r.stdout and 'this one 1000' not in r.stdout, w + ' | ' + r.stdout[-400:])
    whole = os.path.join(elsewhere, 'my brains', 'lm.bin')
    r, w = start(mine, 'cpumate=on\ncpumate_brain=%s\n' % whole)
    found['a whole path in cpumate_brain names that file'] = (
        r.returncode == 0 and w == 'CPU MATE: ' + whole + wrong and 'this one 1000' not in r.stdout, w + ' | ' + r.stdout[-400:])

    clear()
    r, w = start(mine, '', presses=['Right'] * 5 + ['Down'] * 3 + ['Return'])
    told = [x for x in r.stdout.splitlines() if x.startswith('menu: CPU MATE on')]
    found['the menu\'s CPU MATE line with no file in either place names both'] = (
        r.returncode == 0 and told == ['menu: CPU MATE on, with the Mega CD, from the next Soft Reset (Game menu). Its brain comes from your '
                                       'little_man_firmware.bin (Little-Man Firmware 1.0): put it beside pc_port.exe or in %LOCALAPPDATA%\\Paprium PC first.'],
        str(told) + r.stdout[-300:])
    put(second, fw)
    r, w = start(mine, '', presses=['Right'] * 5 + ['Down'] * 3 + ['Return'])
    told = [x for x in r.stdout.splitlines() if x.startswith('menu: CPU MATE on')]
    found['the menu\'s CPU MATE line names the file it will use, by its place only (it is not read there)'] = (
        r.returncode == 0 and told == ['menu: CPU MATE on, with the Mega CD, from the next Soft Reset (Game menu). Its brain is read from '
                                       'little_man_firmware.bin in %LOCALAPPDATA%\\Paprium PC.'], str(told) + r.stdout[-300:])
    put(first, bytes(1000))
    r, w = start(mine, '', presses=['Right'] * 5 + ['Down'] * 3 + ['Return'])
    told = [x for x in r.stdout.splitlines() if x.startswith('menu: CPU MATE on')]
    found['the menu\'s CPU MATE line names the file beside the program first, here a wrong one, without calling it Little-Man Firmware 1.0'] = (
        r.returncode == 0 and told == ['menu: CPU MATE on, with the Mega CD, from the next Soft Reset (Game menu). Its brain is read from '
                                       'little_man_firmware.bin beside pc_port.exe.'], str(told) + r.stdout[-300:])
    clear()
    found['the program wrote nothing into its own folder'] = (sorted(os.listdir(prog)) == before, str(sorted(os.listdir(prog))))
    return found


def test_brain_words_in_program(exe, rom_path, folder, env, sdl, check, menu_script, hold_with_no_sharing):
    """The scenes below on the program exe, then each planted fault in the look-up on a copy of the program built with it: each must lose its
    scene. menu_script and hold_with_no_sharing are tests/test_pc_machine.py's helpers of those names."""
    found = _brain_scenes(exe, rom_path, folder, env, menu_script, hold_with_no_sharing)
    for name, (ok, detail) in found.items():
        check('CPU MATE\'s brain file, ' + name, ok, detail)
    # Planted faults in the look-up: each must lose its scene.
    with open(exe + '.rsp', encoding='utf-8') as f:
        objects = [line.strip().strip('"') for line in f if line.strip()]
    with open(os.path.join(PC, 'pc_main.c'), newline='') as f:
        text = f.read()
    plants = (
        ('        if (GetFileAttributesA(bp->path[i]) != INVALID_FILE_ATTRIBUTES)\n            return i;\n',
         '        if (GetFileAttributesA(bp->path[i]) != INVALID_FILE_ATTRIBUTES && i == bp->n - 1)\n            return i;\n',
         'beside the program only: that file is used and named', 'the place beside the program is never used'),
        ('            rc = pc_brain_load(bp.path[at], why, sizeof(why));\n',
         '            rc = pc_brain_load(bp.path[at], why, sizeof(why));\n            if (rc && at + 1 < bp.n)\n'
         '                rc = pc_brain_load(bp.path[++at], why, sizeof(why));\n',
         'both places, the first one wrong: it is used, the second is not looked at', 'a wrong first file falls through to the second place'),
        ('    if (console->brain[0]) {\n        bp->n = 1;\n', '    if (0) {\n        bp->n = 1;\n',
         'a whole path in cpumate_brain names that file', 'a cpumate_brain line is not taken'),
        ('    bp->n = _stricmp(program, settings_folder) ? 2 : 1;\n', '    bp->n = 2;\n',
         'settings beside the program: one folder, looked at once and named once', 'one folder is looked at twice'),
    )
    for n, (a, b, scene, what) in enumerate(plants):
        check('plant in pc_main.c: ' + what, text.count(a) == 1)
        where = os.path.join(folder, 'plant%d' % n)
        os.makedirs(where, exist_ok=True)
        src = os.path.join(where, 'pc_main.c')
        with open(src, 'w', newline='') as f:
            f.write(text.replace(a, b))
        job = pc_build.machine_jobs(where, '-O1', ('pc_main',))[0]
        obj = pc_build.compile_jobs([(src, os.path.join(where, 'pc_main.o'), job[2], [])], 1)[0]
        planted = os.path.join(where, 'pc_port.exe')
        pc_build.link([obj if os.path.basename(o) == 'pc_main.o' else o for o in objects], planted, pc_build.sdl_link_args(sdl) + ['-lwinmm'])
        shutil.copy2(pc_build.sdl_dll(sdl), os.path.join(where, 'SDL2.dll'))
        check('plant in pc_main.c: %s: caught' % what, not _brain_scenes(planted, rom_path, os.path.join(where, 'runs'), env, menu_script,
                                                                         hold_with_no_sharing)[scene][0])
