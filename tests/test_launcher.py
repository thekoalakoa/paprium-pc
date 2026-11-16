# Copyright (C) 2026 Marsupial Solutions LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""The players' package and its launcher, on made-up ROMs only.

    python -I -B tests/test_launcher.py [scratch folder]        with PAPRIUM_PC_CONFIG naming a local configuration with player_tools

It makes a package with targets/pc/pc_package.py for a made-up ROM (tests/test_pc_machine.py's), so the package's rules name that ROM's hash,
then plays a player's cases on copies of it, every one hidden: the launcher with --headless or with --hidden (its window never shown), the game
on SDL's dummy drivers. PATH holds only Windows' own folders, so nothing of Python, gcc or MSYS2 is in reach. Every copy gets an app-data folder
of its own (LOCALAPPDATA points into the scratch folder), so no test writes this PC's own app-data folder. The cases:
  - what the package holds: TinyCC built from its source, with that source and the script beside it; the GPL version 2 text of its own; no
    symbol table in our programs, and a manifest asking for long paths; READ_ME's words for the player's folder, the terms and the fallback;
    a package made with --no-firmware holds no firmware file
  - files that are refused, with the reason and with nothing kept: a text file, an empty file, a zip, a 7-Zip file, a damaged copy of the right
    size, a cut-short copy, a folder, a file far too large; and no file at all; a game file whose path is longer than 259 characters
  - the right file in the other byte order and with a copier's header, with the package and the app-data folder both in folders whose names have
    spaces and letters outside the code page: the game is built in the app-data folder, its copy kept in the plain order, the DLL the one the
    package's maker built, the game runs from it, and the package's own folder holds exactly what was unzipped
  - the second start, from the cache, with no build; a new version of the package and a changed DLL, each built again
  - CPU MATE's brain file, made up, beside Paprium.exe, in the player's files folder, in both and in neither: the game's window names the place
    it used, beside Paprium.exe first, and the launcher starts from the cache with the file beside it, the package folder otherwise as unzipped
  - a build cut short by ending the launcher: its tools end with it, and the next start builds again; each step failing; too little disk space
  - the interpreter: when a build step fails, when the game program cannot load the DLL it is given, and when Windows refuses a damaged DLL
    whose record matches it (in a package whose rules name no tested build, as one that does builds such a DLL again), the game plays through
    the interpreter and the notice says it is not the tested build; through the hidden window too; and the next start builds again; a game
    program that gives no reason: the notice points at launcher_log.txt
  - a DLL that is not the tested one: the player is told, and it is never started from the cache: every start builds it again and tells it
    while it differs; a package with a changed file, then the download unzipped again into a new folder with the same player's folder: the
    tested game is built at once, with no notice; a game that stops with a fault: the player is pointed at game_log.txt
  - the menu bar's Soft Reset: a game that ends with exit code 8 is started again the same way, less a test's key script, which belongs to the
    first start; exit code 7 stays a fault and is not; the interpreter's start is marked as the launcher's, so a Soft Reset there comes back to
    the launcher too
  - the true reason in every notice: what the game program said about a DLL it could not use (a virus scanner or Smart App Control named only
    when Windows' code says so); a tool of the build that could not be started at all; a long folder path blamed, not the game file, and a
    player's folder too deep for TinyCC named as the cause (this program's folder only when it is long too); a record
    that could not be written after the game was put in place, with no "Nothing was changed"
  - two launchers at once, and a second one while the game runs after the first was ended from outside; a package with a piece missing; a
    package folder whose path is too long; a package folder that cannot be written (the app-data folder is used, named plainly, with a space
    and with letters outside plain ASCII, where TinyCC then gets a copy of its files); an app-data folder that cannot be written (my_copy beside
    the program); neither
  - the settings file: written with the line naming the save, and that line added to a settings file without one, one with a byte order mark
    too; a settings file the launcher cannot read is never written over; one another program holds: the game says why it does not start; one
    another program holds but lets others read: the launcher's log says the save line could not be added, and the game plays; one saved as
    UTF-16: left as it is, and the game says how to save it again; a relative save line, and the save's default name, start from the player's
    folder
  - the drop on the hidden window, through the same message a real drop sends
  - the game program alone: no game file, no DLL, a DLL for another image, the interpreter
  - a kept copy damaged on the disk; the file named again when the game is built, with the disk nearly full; a build folder that is a link to
    another folder, whose files must stay
  - every name the DLL imports is declared in the runtime's headers, and an import list with one name left out makes TinyCC refuse the build
  - the package check's own rules (tests/check_package.py) on made-up bytes: random runs of 12 to 16 bytes are strong, counting tables weak,
    a run of 5 byte values and an A A B B run strong, and planted runs are found whole; and CPU MATE's brain refused, with a made-up firmware file
    as the brain reference: by the file's name, by its first word under another name, by 64 of its brain's bytes inside another file; and an
    archive that is not a pinned third-party file, here the made-up firmware file compressed with bzip2 in a zip, refused as an archive
  - the one firmware file a package may hold (tests/check_package.py, is_published_firmware), with a made-up firmware file standing for Little-Man
    Firmware 1.0's by its hashes (firmware_pins_made_up): taken beside Paprium.exe, with its hashes printed beside the pins; refused in another
    folder, under another name, cut short, doubled, with one byte changed, as another build, and as a second copy beside the taken one; required
    and absent as --firmware asks; and the package script's firmware rule (targets/pc/pc_package.py, firmware_from_zip) on made-up zips: the file
    taken from the right zip, refused from another zip, from a zip holding it in a folder or twice, and with other bytes; the package script stops
    when it is given no firmware zip and no --no-firmware, and refuses a zip that is not the published one; the texts name the published hashes
  - READ_ME and NOTICES quote only the words the team's decision allows (ALLOWED_QUOTES): the check lists those words and refuses any other long
    quote, so it holds no other words itself; made-up texts with a planted extra quote and with an allowed one left out show it catches both
Ends with RESULT: PASS or RESULT: FAIL. A shown window, a message box, the file dialog and a real drop from Explorer are not reached by these tests.
"""
import ctypes
import hashlib
import io
import json
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, REPO)
sys.path.insert(0, HERE)

import check_tree  # noqa: E402
import test_pc_machine  # noqa: E402
from targets.pc import pc_build, pc_package  # noqa: E402

RESULTS = []
ODD_NAME = 'Pap rium Äpfel Привет 日本'     # spaces, and letters outside an English Windows' code page
ODD_APPDATA = 'App Data Ölçü été'          # an app-data folder below a user name with letters outside plain ASCII


def default_check(name, ok, detail=''):
    RESULTS.append((name, ok))
    print('%s %s %s' % ('ok  ' if ok else 'FAIL', name, detail if not ok else ''), flush=True)


SAFE = {}


def env_for_player(extra=None):
    e = pc_package.windows_only_env(dict(SAFE, **(extra or {})))
    e['SDL_VIDEODRIVER'] = 'dummy'
    e['SDL_AUDIODRIVER'] = 'dummy'
    e['SDL_RENDER_DRIVER'] = 'software'
    e.update(test_pc_machine.NO_REAL_PADS)     # a pad plugged into the test PC is not read
    return e


def appdata(folder, name):
    """A new app-data folder for one copy of the package, and the environment that points the launcher at it (in Windows' own form, as
    Windows sets LOCALAPPDATA)."""
    path = os.path.normpath(os.path.abspath(os.path.join(folder, name)))
    os.makedirs(path, exist_ok=True)
    return path, env_for_player({'LOCALAPPDATA': path})


def files_of(app):
    """The player's folder the launcher keeps in an app-data folder."""
    return os.path.join(app, 'Paprium PC')


def launcher(pkg, *args, env=None, timeout=600):
    r = subprocess.run([os.path.join(pkg, 'Paprium.exe')] + list(args), capture_output=True, timeout=timeout, env=env or env_for_player(),
                       cwd=os.path.dirname(pkg))
    return r.returncode, r.stdout.decode('utf-8', 'replace') + r.stderr.decode('utf-8', 'replace')


def text(path):
    try:
        with open(path, encoding='utf-8', errors='replace') as f:
            return f.read()
    except OSError:
        return ''


def sha(path):
    with open(path, 'rb') as f:
        return hashlib.sha256(f.read()).hexdigest()


def listing(folder):
    return sorted(os.path.relpath(os.path.join(a, n), folder).replace('\\', '/') for a, _, ns in os.walk(folder) for n in ns)


def kept(work):
    """What a refused file must not leave: a copy of a game file, a built game, a record."""
    return [n for n in ('paprium.bin', 'pc_game.dll', 'build_record.txt') if os.path.exists(os.path.join(work, n))]


def game_args(folder, frames=20):
    """The game through its window's path on SDL's dummy drivers, ended by its Escape key at a frame, its logs in folder."""
    os.makedirs(folder, exist_ok=True)
    keys = os.path.join(folder, 'keys.txt')
    with open(keys, 'w', encoding='ascii') as f:
        f.write('%d down Escape\n' % frames)
    out = []
    for a in ('--hidden-window', '--run-dir', folder, '--key-script', keys):
        out += ['--game-arg', a]
    return out


def frames_of(folder):
    return text(os.path.join(folder, 'frames.txt')).splitlines()


def hold_exclusive(path):
    """Opens a file with no sharing, as a running launcher holds its lock. Returns the handle."""
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    k32.CreateFileW.restype = ctypes.c_void_p
    h = k32.CreateFileW(path, 0x40000000, 0, None, 2, 0x80, None)
    return k32, h


def hold_reading(path):
    """Opens an existing file to read it and lets others read it too, but not write, rename or delete it, as an editor or a backup program can.
    Returns the handle."""
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    k32.CreateFileW.restype = ctypes.c_void_p
    h = k32.CreateFileW(path, 0x80000000, 1, None, 3, 0x80, None)
    return k32, h


def pe_facts(path):
    """A program's symbol-table record count and whether its manifest asks for long paths."""
    with open(path, 'rb') as f:
        data = f.read()
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    return struct.unpack_from('<I', data, pe + 16)[0], b'longPathAware' in data


def long_paths_on():
    f = getattr(ctypes.WinDLL('ntdll'), 'RtlAreLongPathsEnabled', None)
    if f is None:
        return False
    f.restype = ctypes.c_ubyte
    return bool(f())


def deny(folder, user):
    """The folder refuses new files and folders for this user, as a folder under Program Files does for a player; its own files stay readable.
    (Denying write rights on the files too, by inheritance, stops Windows from starting a program there at all on this PC.)"""
    return subprocess.run(['icacls', folder, '/deny', '%s:(WD,AD)' % user], capture_output=True, text=True).returncode == 0


def undeny(folder, user):
    subprocess.run(['icacls', folder, '/remove:d', user, '/C'], capture_output=True)


def test_all_empty_patch_source(folder):
    """An empty patch source named as the repository's (tests/test_all.py, empty_patch_source): the made-up ROM holds none of the real game's patch sites."""
    path = os.path.join(folder, 'empty_patches', 'patch_source.json')
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        json.dump({'format': 2, 'name': 'patch source', 'switches': [], 'hooks': [], 'caves': [], 'words': []}, f)
    return path


def made_up_firmware(seed):
    """A made-up Little-Man Firmware file: 256 KB of random bytes with the file's first word and its kind word (built with the brain), each 16-bit
    pair swapped as the file stores it. Its brain is random bytes too: no byte of the real one."""
    rnd = random.Random(seed)
    mcu = bytearray(rnd.getrandbits(8) for _ in range(1 << 18))
    mcu[0:4] = b'FAFR'
    struct.pack_into('<I', mcu, 0x20, 0x01000002)
    return check_tree.swapped_pairs(bytes(mcu))


def brain_refused(check, folder, local, base):
    """The package check refuses CPU MATE's brain (tests/check_package.py): a small copy of the package's text files with nothing added passes;
    with a made-up little_man_firmware.bin beside them it is refused by the file's name and by the brain's bytes; with that file under another name, by
    its first word; with 64 bytes of the made-up brain's code, each 16-bit pair swapped, inside another file, by those bytes; in a zip (bzip2), as an
    archive that is not one of the pinned third-party files. The made-up firmware file
    is the brain reference the check is given, as a package's maker names the real one, with brain_reference_made_up; without that line the check
    fails, as the made-up brain is not Little-Man Firmware 1.0's by its pins."""
    import check_package
    fw = os.path.join(folder, 'brain_ref', 'little_man_firmware.bin')
    os.makedirs(os.path.dirname(fw))
    with open(fw, 'wb') as f:
        f.write(made_up_firmware(11))
    code = check_package.brain_parts(fw)[0]
    # The check's own ROM control plants pieces of the ROM it is given, of kinds a real game holds (runs of only 5 byte values, 32-bit A A B B
    # tables), which the made-up program's ROM, nearly all zero bytes, does not. So it is given a made-up ROM of random bytes with such runs in it,
    # 100,000 bytes long, a size no file here has.
    rnd = random.Random(13)
    check_rom = bytearray(rnd.getrandbits(8) for _ in range(25 * 4000))
    for k in range(16):
        check_rom[1000 + 4000 * k:1016 + 4000 * k] = bytes([1, 7, 2, 9, 4] * 3 + [1])
        check_rom[3000 + 4000 * k:3016 + 4000 * k] = bytes([0x13, 0x57, 0x9B, 0xDF] * 2 + [0x2A, 0x4C, 0x6E, 0x80] * 2)
    check_rom_path = os.path.join(folder, 'brain_ref', 'check_rom.md')
    with open(check_rom_path, 'wb') as f:
        f.write(check_rom)
    cfg = dict(local, brain_reference=fw, brain_reference_made_up=True, rom_path=check_rom_path, rom_sha256=hashlib.sha256(bytes(check_rom)).hexdigest())
    cfg_path = os.path.join(folder, 'local_brain.json')
    with open(cfg_path, 'w', encoding='utf-8') as f:
        json.dump(cfg, f)
    pins_path = os.path.join(folder, 'local_brain_pins.json')
    with open(pins_path, 'w', encoding='utf-8') as f:
        json.dump(dict(cfg, brain_reference_made_up=False), f)

    def small(name, extra, config=cfg_path, firmware=None):
        d = os.path.join(folder, 'brain_' + name)
        os.makedirs(os.path.join(d, 'tools'))
        for rel in ('READ_ME.txt', 'NOTICES.txt', 'tools/rules.json'):
            shutil.copyfile(os.path.join(base, *rel.split('/')), os.path.join(d, *rel.split('/')))
        for rel, data in extra.items():
            with open(os.path.join(d, *rel.split('/')), 'wb') as f:
                f.write(data)
        r = subprocess.run([sys.executable, '-I', '-B', os.path.join(HERE, 'check_package.py'), '--config', config, '--package', d] +
                           (['--firmware', firmware] if firmware else []), capture_output=True, text=True, encoding='utf-8', errors='replace')
        return r.returncode, r.stdout
    rc, out = small('none', {})
    check('the package check with a brain reference: its control found, nothing of the brain in the package\'s own text files',
          rc == 0 and 'control found True' in out and 'RESULT: PASS' in out, out[-600:])
    rc, out = small('pins', {}, pins_path)
    check('a brain reference that is not Little-Man Firmware 1.0\'s brain by its pins fails the check, unless it is marked as a test\'s made-up file',
          rc == 1 and 'is not the brain of Little-Man Firmware 1.0 by the pins' in out and 'RESULT: PASS' not in out, out[-600:])
    with open(fw, 'rb') as f:
        whole = f.read()
    rc, out = small('named', {'little_man_firmware.bin': whole})
    check('a package holding little_man_firmware.bin is refused: by the file\'s name and by the brain\'s bytes', rc == 1 and 'a CPU MATE brain file, by its name' in out
          and "bytes of CPU MATE's brain" in out, out[-600:])
    rc, out = small('renamed', {'readme_extra.dat': whole})
    check('the same file under another name is refused, by its first word', rc == 1 and 'a Little-Man Firmware file, by its first word' in out, out[-600:])
    rnd = random.Random(12)
    piece = check_tree.swapped_pairs(code[4096:4160])
    rc, out = small('piece', {'notes.dll': bytes(rnd.getrandbits(8) for _ in range(5000)) + piece + bytes(rnd.getrandbits(8) for _ in range(5000))})
    check('64 bytes of the brain\'s code, each 16-bit pair swapped, inside another file: refused by those bytes', rc == 1 and
          "bytes of CPU MATE's brain (brain swapped) at offset" in out, out[-600:])
    rc, out = small('names', {'cf.bin': bytes(64)})
    check('a file named as one of the brain\'s parts is refused by its name', rc == 1 and 'cf.bin: a CPU MATE brain file, by its name' in out, out[-600:])
    # The check does not look inside archives: an archive of ours is refused, whatever it holds. Here the made-up firmware file compressed with
    # bzip2 in a zip under another name, which none of the rules above can see. (Deflate would keep the made-up file's random bytes as they are,
    # where the brain's bytes rule finds them; the real brain's code compresses, so a deflated copy of it would be as hidden as this one.)
    packed = io.BytesIO()
    with zipfile.ZipFile(packed, 'w', zipfile.ZIP_BZIP2) as z:
        z.writestr('little_man_firmware.bin', whole)
    rc, out = small('zipped', {'downloads.zip': packed.getvalue()})
    check('an archive that is not one of the pinned third-party files is refused, and by that rule alone (here the made-up firmware file, bzip2 in a '
          'zip)', rc == 1 and 'downloads.zip: a zip archive that is not one of the pinned third-party files' in out and 'by its name' not in out and
          'first word' not in out and "CPU MATE's brain (" not in out, out[-600:])
    firmware_rule(check, small, cfg, folder, whole)


def firmware_rule(check, small, cfg, folder, whole):
    """The one firmware file a package may hold (tests/check_package.py, is_published_firmware): Little-Man Firmware 1.0's file, beside Paprium.exe,
    by its published hashes. Here the made-up firmware file that is the brain reference stands for it, taken by its own hashes through
    firmware_pins_made_up, so the brain's bytes rule would find it anywhere else too."""
    import check_package
    md5, sha = check_package.firmware_hashes(whole)
    path = os.path.join(folder, 'local_firmware.json')
    with open(path, 'w', encoding='utf-8') as f:
        json.dump(dict(cfg, firmware_pins_made_up=[md5, sha]), f)
    taken = 'little_man_firmware.bin beside Paprium.exe: %d bytes, MD5 %s, SHA-256 %s; published: MD5 %s, SHA-256 %s: equal (a test\'s made-up hashes)' % (
        len(whole), md5, sha, md5, sha)
    rc, out = small('fw_beside', {'little_man_firmware.bin': whole}, path, 'required')
    check('the firmware file beside Paprium.exe with the pinned hashes is taken, its hashes printed beside the pins, and --firmware required passes',
          rc == 0 and taken in out and 'RESULT: PASS' in out and "CPU MATE's brain (" not in out, out[-600:])
    rc, out = small('fw_two', {'little_man_firmware.bin': whole, 'tools/little_man_firmware.bin': whole}, path)
    check('a second copy in another folder is refused by its name, its first word and its bytes, while the one beside Paprium.exe is taken',
          rc == 1 and taken in out and 'tools/little_man_firmware.bin: a CPU MATE brain file, by its name' in out and
          'tools/little_man_firmware.bin: a Little-Man Firmware file, by its first word' in out and
          re.search(r"tools/little_man_firmware\.bin: \d+ bytes of CPU MATE's brain", out) is not None and
          'little_man_firmware.bin: a firmware file beside Paprium.exe' not in out, out[-900:])
    rc, out = small('fw_moved', {'tools/little_man_firmware.bin': whole}, path, 'required')
    check('the same file in another folder only: refused, and --firmware required finds none beside Paprium.exe', rc == 1 and
          'tools/little_man_firmware.bin: a CPU MATE brain file, by its name' in out and 'no little_man_firmware.bin as published beside Paprium.exe' in out,
          out[-600:])
    rc, out = small('fw_renamed', {'lmf_1.0.bin': whole}, path)
    check('the same file under another name beside Paprium.exe: refused by its first word and its bytes', rc == 1 and
          'lmf_1.0.bin: a Little-Man Firmware file, by its first word' in out and re.search(r"lmf_1\.0\.bin: \d+ bytes of CPU MATE's brain", out) is not None,
          out[-600:])
    flipped = bytearray(whole)
    flipped[0x1000] ^= 1        # a place in the file, not a game address
    other = made_up_firmware(23)
    for name, data in (('cut short', whole[:len(whole) // 2]), ('doubled', whole + whole), ('one byte changed', bytes(flipped)), ('another build', other)):
        rc, out = small('fw_' + name.replace(' ', '_'), {'little_man_firmware.bin': data}, path)
        check('the firmware file beside Paprium.exe %s: refused as not the published file, and by its name' % name, rc == 1 and
              'little_man_firmware.bin: a firmware file beside Paprium.exe that is not Little-Man Firmware 1.0 as published (%d bytes, MD5 %s'
              % (len(data), check_package.firmware_hashes(data)[0]) in out and 'little_man_firmware.bin: a CPU MATE brain file, by its name' in out,
              out[-600:])
    rc, out = small('fw_absent', {'little_man_firmware.bin': whole}, path, 'absent')
    check('--firmware absent refuses a package holding it', rc == 1 and 'little_man_firmware.bin beside Paprium.exe, and this package must hold none' in out,
          out[-600:])
    rc, out = small('fw_real_pins', {'little_man_firmware.bin': whole}, os.path.join(folder, 'local_brain.json'))
    check('without the made-up pins the same file is refused: only the published hashes are taken', rc == 1 and
          'is not Little-Man Firmware 1.0 as published' in out, out[-600:])


def brain_places(check, pkg, files, env, names, where):
    """CPU MATE's brain file in the players' build (targets/pc/pc_main.c, brain_places; the team's decision): with megacd and cpumate on,
    the game looks for little_man_firmware.bin beside Paprium.exe first, then in the player's files folder, and the window (in game_log.txt) names the
    place it used in a few words. A file beside Paprium.exe is no damage to the package: the game starts from the cache, and the package folder is
    what was unzipped plus that one file. Made-up files only: no byte of the real brain."""
    beside = os.path.abspath(os.path.join(pkg, 'little_man_firmware.bin'))       # as the game names them: Windows' full form, back slashes
    mine = os.path.abspath(os.path.join(files, 'little_man_firmware.bin'))
    cfg = os.path.join(files, 'paprium_pc.cfg')
    with open(cfg, 'rb') as f:
        kept = f.read()
    with open(cfg, 'wb') as f:
        f.write(kept + (b'' if kept.endswith(b'\n') else b'\n') + b'megacd=on\ncpumate=on\n')
    first = 'little_man_firmware.bin beside Paprium.exe'
    second = 'little_man_firmware.bin in %LOCALAPPDATA%\\Paprium PC'
    wrong = ' is not the Little-Man Firmware 1.0 file with the brain, so a partner stands still. game_log.txt says why.'

    def start(name):
        rc, out = launcher(pkg, '--headless', *game_args(os.path.join(where, name)), env=env)
        log = text(os.path.join(files, 'game_log.txt'))
        w = [x for x in log.splitlines() if x.startswith('on the window for 10 s: ')]
        return rc, out, log, (w[-1][len('on the window for 10 s: '):] if w else '')
    try:
        rc, out, log, w = start('brain0')
        check('the players\' build with CPU MATE and no firmware file: the window names both places, beside Paprium.exe first, the log both whole paths',
              rc == 0 and 'from the cache' in out and w == 'CPU MATE: no little_man_firmware.bin beside Paprium.exe or in %LOCALAPPDATA%\\Paprium PC, so a '
              'partner stands still. Put your little_man_firmware.bin (Little-Man Firmware 1.0) in either place, then Soft Reset.' and beside in log and
              mine in log, w + ' | ' + log[-500:])
        with open(beside, 'wb') as f:
            f.write(made_up_firmware(31))
        rc, out, log, w = start('brain1')
        check('a firmware file beside Paprium.exe: used and named so, while the files folder is not looked at; the game starts from the cache',
              rc == 0 and 'from the cache' in out and w == 'CPU MATE: ' + first + wrong + ' While that file is there, ' + second + ' is not used.' and
              'other than Little-Man Firmware 1.0' in log, w + ' | ' + out[-300:] + log[-300:])
        check('the package folder is what was unzipped and that one file: the launcher takes it as no damage', set(listing(pkg)) == names | {'little_man_firmware.bin'},
              str(sorted(set(listing(pkg)) ^ names))[:300])
        with open(mine, 'wb') as f:
            f.write(bytes(1000))
        rc, out, log, w = start('brain2')
        check('a file in both places: the one beside Paprium.exe is used, the other is not read',
              rc == 0 and w == 'CPU MATE: ' + first + wrong + ' While that file is there, ' + second + ' is not used.' and 'this one 1000' not in log,
              w + ' | ' + log[-300:])
        os.remove(beside)
        rc, out, log, w = start('brain3')
        check('the files folder only: that file is used and named so', rc == 0 and w == 'CPU MATE: ' + second + wrong and 'this one 1000' in log,
              w + ' | ' + log[-300:])
    finally:
        for x in (beside, mine):
            if os.path.exists(x):
                os.remove(x)
        with open(cfg, 'wb') as f:
            f.write(kept)
    check('and after it the package folder is what was unzipped again', set(listing(pkg)) == names)


def run(tmp, config_path, report=default_check):
    def check(name, ok, detail=''):
        # A detail can hold the folder name with letters outside the code page, and a test log may be written in that code page.
        report(name.encode('ascii', 'backslashreplace').decode('ascii'), ok, detail.encode('ascii', 'backslashreplace').decode('ascii'))

    package_check_rules(check)
    quote_rules(check)
    package_words_rules(check)
    with open(config_path, encoding='utf-8') as f:
        cfg = json.load(f)
    firmware_zip_rules(check, tmp, config_path)
    if not cfg.get('player_tools') or not cfg.get('fw_source') or not cfg.get('sdl_dir'):
        print('skip the launcher tests: the local configuration names no player_tools, fw_source or sdl_dir')
        return
    folder = os.path.join(tmp, 'launcher')
    if os.path.exists(folder):
        shutil.rmtree(folder)
    os.makedirs(folder)
    SAFE['LOCALAPPDATA'] = os.path.normpath(os.path.abspath(os.path.join(folder, 'appdata_default')))     # no test writes the real app-data folder
    os.makedirs(SAFE['LOCALAPPDATA'])
    rom = test_pc_machine.made_up_rom()
    rom_path = os.path.join(folder, 'made_up.md')
    with open(rom_path, 'wb') as f:
        f.write(rom)
    local = dict(cfg)
    local.update({'rom_path': rom_path, 'rom_sha256': hashlib.sha256(rom).hexdigest(), 'out_dir': folder, 'out_root': folder,
                  'patch_source': test_all_empty_patch_source(folder), 'local_patch_files': [], 'hints': {}, 'seed_logs': [],
                  'pc_watch': [], 'bus_low_device_end': 65536})
    local_path = os.path.join(folder, 'local.json')
    with open(local_path, 'w', encoding='utf-8') as f:
        json.dump(local, f)
    user = os.environ.get('USERNAME', '')

    # ---- the import list against the runtime's headers
    headers = ''.join(text(os.path.join(REPO, 'runtime', n)) for n in ('pt_rt.h', 'pt_port.h'))
    missing = [n for n in pc_build.GAME_DLL_DATA if not re.search(r'PT_DATA [^;(]*\b%s\b' % n, headers)]
    missing += [n for n in pc_build.GAME_DLL_FUNCTIONS if not re.search(r'\b%s\(' % n, headers)]
    check('every name the game DLL imports is declared in the runtime\'s headers, the data as PT_DATA', not missing, str(missing))
    check('the game program\'s file name the DLL imports from is the same in pc_build.py and pc_game_dll.h',
          '#define PC_GAME_PROGRAM "%s"' % pc_build.GAME_EXE in text(os.path.join(REPO, 'targets', 'pc', 'pc_game_dll.h')))

    # ---- the package's hint rule: a hint the translation does not read stops the package
    bad_local = dict(local, hints={'extra_seeds': [], 'bytes': 'anything'})
    try:
        pc_package.translation_rules(bad_local, '')
        check('a hint of a kind the translation does not read stops the package', False)
    except SystemExit as x:
        check('a hint of a kind the translation does not read stops the package', 'bytes' in str(x), str(x))

    # ---- the package, for the made-up ROM: built, and its own launcher builds the game from a copy of it
    base = os.path.join(folder, 'pkg')
    t0 = time.time()
    r = subprocess.run([sys.executable, '-I', '-B', os.path.join(REPO, 'targets', 'pc', 'pc_package.py'), '--config', local_path, '--out', base,
                        '--version', 'test-1', '--work', os.path.join(folder, 'pkg_work'), '--keep-work', '--no-check', '--no-firmware',
                        '--jobs', os.environ.get('PC_TEST_JOBS', '2')], capture_output=True, text=True, encoding='utf-8', errors='replace')
    check('pc_package.py makes a package for a made-up ROM, and its launcher builds the game from a copy of it', r.returncode == 0 and 'RESULT: PASS' in r.stdout,
          (r.stdout + r.stderr)[-1500:])
    if r.returncode:
        return
    print('package made in %.0f s' % (time.time() - t0))
    with open(os.path.join(base, 'tools', 'rules.json'), encoding='utf-8') as f:
        rules = json.load(f)
    check('the rules name the made-up ROM and the DLL the package\'s maker built', rules['rom_sha256'] == local['rom_sha256'] and len(rules['tested_dll_sha256']) == 64)
    check('the rules name the patch source by its new name', rules['patch_source'] == 'patches/patch_source.json', rules['patch_source'])
    names = set(listing(base))
    check('the package holds no game file, no built game, no player folder', not any(n.startswith('my_copy') or n.endswith(('pc_game.dll', '.bin', '.gen', '.smd'))
                                                                                     or os.path.getsize(os.path.join(base, n)) == len(rom) for n in names))
    for need in ('Paprium.exe', 'pc_port.exe', 'SDL2.dll', 'gamecontrollerdb.txt', 'licences/GAMECONTROLLERDB_LICENSE.txt', 'READ_ME.txt', 'NOTICES.txt', 'licences/TINYCC_COPYING.txt', 'licences/COPYING_GPL2.txt',
                 'licences/PYTHON_LICENSE.txt', 'licences/NUMPY_LICENSE.txt', 'licences/COPYING_GPL3.txt', 'source/tcc-0.9.27.tar.bz2', 'source/pc_tinycc.py',
                 'source/SOURCE.txt', 'tools/rules.json', 'tools/tcc/tcc.exe', 'tools/tcc/libtcc.dll', 'tools/tcc/lib/libtcc1-64.a',
                 'tools/translator/patches/patch_source.json'):
        check('the package holds ' + need, need in names)
    check('TinyCC is our build from its source: no 32-bit compiler, no 32-bit library, no examples', not any(
        n.startswith(('tools/tcc/i386', 'tools/tcc/examples', 'tools/tcc/doc')) or n.endswith('libtcc1-32.a') for n in names))
    check('the script in source is the one that built TinyCC', sha(os.path.join(base, 'source', 'pc_tinycc.py')) == sha(os.path.join(REPO, 'targets', 'pc', 'pc_tinycc.py')))
    with open(os.path.join(base, 'licences', 'COPYING_GPL2.txt'), 'rb') as f:
        gpl2 = f.read()
    check('COPYING_GPL2.txt is the GNU GPL version 2', b'GNU GENERAL PUBLIC LICENSE' in gpl2[:200] and b'Version 2, June 1991' in gpl2[:200])
    for prog in ('Paprium.exe', 'pc_port.exe', 'tools/pc_boot_image.exe'):
        records, long_ok = pe_facts(os.path.join(base, prog))
        check('%s carries no symbol table, and its manifest asks for long paths' % prog, records == 0 and long_ok, 'records %d long %s' % (records, long_ok))
    readme = text(os.path.join(base, 'READ_ME.txt')).replace('\r\n', '\n')
    notices = text(os.path.join(base, 'NOTICES.txt')).replace('\r\n', '\n')
    check('READ_ME names the app-data folder and says to delete it with the program\'s folder to remove everything',
          '  %LOCALAPPDATA%\\Paprium PC\n' in readme and "delete the program's folder and the\nfolder %LOCALAPPDATA%\\Paprium PC." in readme and
          'my_copy, beside Paprium.exe' in readme, readme[-2500:])
    # USB pads are named in the controls screen's paragraph; a USB stick to unzip onto must not be, as the program's files go to the app-data folder.
    check('READ_ME: no USB stick line, and no registry claim beyond the program\'s own', re.search(r'USB\s+stick', readme) is None and
          'nothing goes into the Windows registry' not in readme)
    check('READ_ME and NOTICES quote only the GPL, no-sale and no-sharing terms, the phrase for what is built and the name answer',
          *quotes_check(readme, notices))
    check('READ_ME tells of the interpreter and that it is not the tested build', 'not the build we tested' in readme)
    check('READ_ME and NOTICES say what this version changes for the player: ' + ', '.join(w[2] for w in PACKAGE_WORDS), not package_words(readme, notices),
          str(package_words(readme, notices)))
    check('NOTICES points TinyCC\'s GPL version 2 text at its own file, and says TinyCC was built from its source', 'licences\\COPYING_GPL2.txt' in notices and
          'NUKED_PSG_LICENSE.txt (the same' not in notices and 'We built it from its own source archive' in notices)
    deepest = max(len(n) for n in names)
    check('no file lies deeper than the launcher\'s path rule allows (%d characters)' % deepest, deepest <= pc_package.DEEPEST)
    check('the package made with --no-firmware holds no firmware or brain file: no little_man_firmware.bin, brain.bin, cf.bin or rodata.bin', not any(
        os.path.basename(n).lower().startswith('little_man_firmware') or os.path.basename(n).lower() in ('brain.bin', 'cf.bin', 'rodata.bin') for n in names))
    brain_refused(check, folder, local, base)

    # ---- refusals, on one copy
    a = os.path.join(folder, 'A')
    shutil.copytree(base, a)
    app_a, env_a = appdata(folder, 'appdata_A')
    work_a = files_of(app_a)
    bad = os.path.join(folder, 'bad')
    os.makedirs(bad)
    cases = []

    def bad_file(name, data):
        p = os.path.join(bad, name)
        with open(p, 'wb') as f:
            f.write(data)
        return p
    cases.append(('a text file', bad_file('notes.txt', b'hello, this is not a game\r\n'), 'not a Paprium game file'))
    cases.append(('an empty file', bad_file('empty.md', b''), 'empty'))
    zpath = os.path.join(bad, 'game.zip')
    with zipfile.ZipFile(zpath, 'w') as z:
        z.writestr('inside.md', rom[:4096])
    cases.append(('a zip file', zpath, 'Unzip it first'))
    cases.append(('a 7-Zip file', bad_file('game.7z', b'7z\xbc\xaf\x27\x1c' + bytes(64)), 'Unpack it first'))
    damaged = bytearray(rom)
    damaged[len(damaged) // 3] ^= 0x10
    cases.append(('a damaged copy of the right size', bad_file('damaged.md', bytes(damaged)), 'the size of the game file, but'))
    cases.append(('a cut-short copy', bad_file('short.md', rom[:len(rom) // 2]), 'cut short'))
    cases.append(('a folder', bad, 'folder'))
    big = os.path.join(bad, 'big.bin')
    with open(big, 'wb') as f:
        f.truncate(65 << 20)
    cases.append(('a file far too large', big, 'far larger'))
    for what, path, words in cases:
        rc, out = launcher(a, '--headless', '--rom', path, env=env_a)
        check('refused: %s, with the reason, and nothing kept' % what, rc == 3 and words in out and not kept(work_a), 'rc %d kept %s: %s' % (rc, kept(work_a), out[-400:]))
    rc, out = launcher(a, '--headless', env=env_a)
    check('no game file at all: it says so', rc == 2 and 'no game file' in out, out[-300:])
    check('the refusals wrote nothing into the package\'s own folder', set(listing(a)) == names, str(sorted(set(listing(a)) - names))[:300])
    rc, out = launcher(a, '--test-layout', env=env_a)
    check('the window, never shown: each control inside it, each text fitting its control: the longest refusal, the four notices before a game and '
          'both folder texts too', rc == 0 and 'every control and text fits' in out and out.count('layout: text') == 10, out[-1200:])
    # the game program started alone, with no game file: it points to the launcher (no message box: its window is hidden)
    r = subprocess.run([os.path.join(a, 'pc_port.exe'), '--hidden-window'], capture_output=True, text=True, env=env_a, cwd=a)
    check('the game program alone, with no game file: it says to start Paprium.exe', r.returncode == 2 and 'Start Paprium.exe' in r.stdout, r.stdout[-300:])
    # a package folder whose path is too long, its files missing as an unzip tool may leave them: it says to unzip into a shorter path
    deep = os.path.join(folder, 'deep', 'd' * max(10, 175 - len(os.path.join(folder, 'deep'))))
    os.makedirs(deep)
    shutil.copyfile(os.path.join(base, 'Paprium.exe'), os.path.join(deep, 'Paprium.exe'))
    app_deep, env_deep = appdata(folder, 'appdata_deep')
    rc, out = launcher(deep, '--headless', '--rom', rom_path, env=env_deep)
    check('a package folder whose path is %d characters long, with files missing: it names the long path, and writes nothing' % len(deep), rc == 2 and
          'a folder with a shorter path' in out and os.listdir(deep) == ['Paprium.exe'] and os.listdir(app_deep) == [], 'rc %d %s' % (rc, out[-300:]))
    shutil.rmtree(os.path.join(folder, 'deep'))
    rc, out = launcher(a, '--hidden', '--test-drop', os.path.join(bad, 'notes.txt'), env=env_a)
    check('refused through the hidden window\'s drop', rc == 3 and 'not a Paprium game file' in out and not kept(work_a), out[-300:])
    rc, out = launcher(a, '--hidden', '--test-drop', os.path.join(bad, 'game.zip'), env=env_a)
    check('a zip dropped on the hidden window: unzip it first', rc == 3 and 'Unzip it first' in out, out[-300:])

    # ---- the right file, swapped, with the package and the app-data folder both below names with spaces and letters outside the code page
    b = os.path.join(folder, ODD_NAME, 'Paprium PC')
    shutil.copytree(base, b)
    app_b, env_b = appdata(os.path.join(folder, ODD_NAME), ODD_APPDATA)
    mc = files_of(app_b)
    swapped = bytearray(rom)
    swapped[0::2], swapped[1::2] = rom[1::2], rom[0::2]
    sw_path = os.path.join(folder, ODD_NAME, 'my game été.md')
    with open(sw_path, 'wb') as f:
        f.write(bytes(swapped))
    g1 = os.path.join(folder, ODD_NAME, 'run1')
    t0 = time.time()
    rc, out = launcher(b, '--headless', '--rom', sw_path, *game_args(g1), env=env_b)
    first = time.time() - t0
    check('the right game file in the other byte order, in folders with spaces and other letters: built and played', rc == 0, 'rc %d %s' % (rc, out[-800:]))
    there = set(os.listdir(mc)) if os.path.isdir(mc) else set()
    want = {'paprium.bin', 'pc_game.dll', 'build_record.txt', 'launcher_log.txt', 'build_log.txt', 'game_log.txt', 'paprium_pc.cfg'}
    check('the player\'s files are in the app-data folder %s\\Paprium PC: the copy, the DLL, its record, the logs, the settings' % ODD_APPDATA,
          want <= there and there <= want | {'stalls.txt'}, str(sorted(there)))
    check('the package\'s own folder holds exactly what was unzipped: no copy of the game file, no built game, no save, no log', set(listing(b)) == names,
          str(sorted(set(listing(b)) ^ names))[:300])
    check('its copy is kept in the plain byte order', os.path.exists(os.path.join(mc, 'paprium.bin')) and sha(os.path.join(mc, 'paprium.bin')) == local['rom_sha256'])
    check('the DLL built there is byte-equal to the one the package\'s maker built', os.path.exists(os.path.join(mc, 'pc_game.dll')) and
          sha(os.path.join(mc, 'pc_game.dll')) == rules['tested_dll_sha256'])
    log = text(os.path.join(mc, 'launcher_log.txt'))
    check('the launcher says so in its log, and names the app-data folder as the one to delete too', 'byte-equal to the one the package' in log and
          'kept in the app-data folder' in log and 'that folder is deleted too' in log, log[-800:])
    check('the game ran from the DLL through its window\'s path, 21 frames to its Escape', len(frames_of(g1)) == 21 and 'RESULT: PASS' in text(os.path.join(mc, 'game_log.txt')),
          text(os.path.join(mc, 'game_log.txt'))[-500:])
    cfg_text = text(os.path.join(mc, 'paprium_pc.cfg'))
    check('the settings file names the save in the same folder', ('save=' + os.path.join(mc, 'paprium.sav')) in cfg_text, cfg_text[-300:])
    check('the game was given that settings file and that freeze log', '--config "%s"' % os.path.join(mc, 'paprium_pc.cfg') in log and
          '--stall-log "%s"' % os.path.join(mc, 'stalls.txt') in log)
    check('the build folder is gone after the build', not os.path.exists(os.path.join(mc, 'build.tmp')))
    print('first load with a build: %.1f s' % first)
    build_log_time = os.path.getmtime(os.path.join(mc, 'build_log.txt'))

    # the second start, from the cache
    g2 = os.path.join(folder, ODD_NAME, 'run2')
    t0 = time.time()
    rc, out = launcher(b, '--headless', *game_args(g2), env=env_b)
    second = time.time() - t0
    check('the second start comes from the cache with no build', rc == 0 and 'from the cache' in out and os.path.getmtime(os.path.join(mc, 'build_log.txt')) == build_log_time,
          out[-400:])
    check('the second start plays the same frames', frames_of(g2) == frames_of(g1))
    print('second start, with the game\'s 21 frames: %.1f s' % second)
    brain_places(check, b, mc, env_b, names, os.path.join(folder, ODD_NAME))

    # a settings file without a save line gets one, and keeps its own lines
    with open(os.path.join(mc, 'paprium_pc.cfg'), 'w', encoding='ascii', newline='\n') as f:
        f.write('# made up\nscale=2\n')
    g2b = os.path.join(folder, ODD_NAME, 'run2b')
    rc, out = launcher(b, '--headless', *game_args(g2b), env=env_b)
    cfg_text = text(os.path.join(mc, 'paprium_pc.cfg'))
    check('a settings file with no save line: the line is added, its own lines stay, and the game plays', rc == 0 and 'scale=2\n' in cfg_text and
          ('save=' + os.path.join(mc, 'paprium.sav')) in cfg_text and len(frames_of(g2b)) == 21, cfg_text[-300:])
    rc, out = launcher(b, '--headless', '--no-play', env=env_b)
    check('and a settings file that names its save is left as it is', rc == 0 and text(os.path.join(mc, 'paprium_pc.cfg')) == cfg_text)
    # a relative save line starts from the player's folder (the settings file's), never from the program's; the game writes its save at frame 5
    with open(os.path.join(mc, 'paprium_pc.cfg'), 'w', encoding='ascii', newline='\n') as f:
        f.write('# made up\nsave=relative_test.sav\n')
    g2c = os.path.join(folder, ODD_NAME, 'run2c')
    rc, out = launcher(b, '--headless', *game_args(g2c), '--game-arg', '--test-save-change', '--game-arg', '5', env=env_b)
    check('a relative save line: the save is written in the player\'s folder, not beside the program', rc == 0 and len(frames_of(g2c)) == 21 and
          os.path.exists(os.path.join(mc, 'relative_test.sav')) and set(listing(b)) == names, 'rc %d %s' % (rc, str(sorted(set(listing(b)) ^ names))[:200]))
    # a settings file the launcher cannot read here (over the 1 MB it reads) is the player's: never written over, and the game reads it itself;
    # with no save line in it, the save's default name starts from the player's folder too
    for leftover in ('relative_test.sav', 'relative_test.sav.bak', 'paprium.sav', 'paprium.sav.bak'):
        if os.path.exists(os.path.join(mc, leftover)):
            os.remove(os.path.join(mc, leftover))
    with open(os.path.join(mc, 'paprium_pc.cfg'), 'w', encoding='ascii', newline='\n') as f:
        f.write('# a made-up settings file, larger than the launcher reads\n' * 20000 + 'scale=2\n')
    big_sha = sha(os.path.join(mc, 'paprium_pc.cfg'))
    g2d = os.path.join(folder, ODD_NAME, 'run2d')
    rc, out = launcher(b, '--headless', *game_args(g2d), '--game-arg', '--test-save-change', '--game-arg', '5', env=env_b)
    check('a settings file the launcher cannot read: left as it is, the log says why, the game plays and saves in the player\'s folder',
          rc == 0 and sha(os.path.join(mc, 'paprium_pc.cfg')) == big_sha and 'could not be read here' in text(os.path.join(mc, 'launcher_log.txt')) and
          len(frames_of(g2d)) == 21 and os.path.exists(os.path.join(mc, 'paprium.sav')) and set(listing(b)) == names, 'rc %d %s' % (rc, out[-400:]))
    # a settings file saved as "UTF-8 with BOM" with no save line: the launcher adds one after the player's lines, and the game reads the file,
    # its byte order mark skipped
    cfg_b = os.path.join(mc, 'paprium_pc.cfg')
    with open(cfg_b, 'wb') as f:
        f.write(b'\xef\xbb\xbf# my settings, saved with a byte order mark\r\nscale=2\r\n')
    g2f = os.path.join(folder, ODD_NAME, 'run2f')
    rc, out = launcher(b, '--headless', *game_args(g2f), env=env_b)
    check('a settings file with a byte order mark: the save line added after the player\'s lines, and the game reads it and plays',
          rc == 0 and len(frames_of(g2f)) == 21 and text(cfg_b).startswith('\ufeff# my settings, saved with a byte order mark\nscale=2\n') and
          text(cfg_b).count('save=') == 1 and ('save=' + os.path.join(mc, 'paprium.sav')) in text(cfg_b), 'rc %d %s' % (rc, out[-400:]))
    # a settings file another program holds open with no sharing: the launcher leaves it, and the game says in Windows' words why it does not start
    k32, h = hold_exclusive(cfg_b)
    try:
        g2g = os.path.join(folder, ODD_NAME, 'run2g')
        rc, out = launcher(b, '--headless', *game_args(g2g), env=env_b)
    finally:
        k32.CloseHandle(ctypes.c_void_p(h))
    glog = text(os.path.join(mc, 'game_log.txt'))
    check('a settings file another program holds: the game does not start, and says the file is there but could not be read, with Windows\' words',
          rc == 2 and 'could not be read here' in text(os.path.join(mc, 'launcher_log.txt')) and
          ('The settings file %s is there, but it could not be read (' % cfg_b) in glog and 'no configuration file' not in glog and not frames_of(g2g),
          'rc %d %s | %s' % (rc, out[-300:], glog[-300:]))
    # a settings file saved as UTF-16 (Notepad's "Unicode"): the launcher writes nothing into it, and the game says how to save it again
    with open(cfg_b, 'wb') as f:
        f.write(b'\xff\xfe' + '# my settings\r\nscale=2\r\n'.encode('utf-16-le'))
    u16_sha = sha(cfg_b)
    g2h = os.path.join(folder, ODD_NAME, 'run2h')
    rc, out = launcher(b, '--headless', *game_args(g2h), env=env_b)
    glog = text(os.path.join(mc, 'game_log.txt'))
    check('a settings file saved as UTF-16: left as it is, and the game does not start, saying it is not plain text and how to save it as UTF-8',
          rc == 2 and sha(cfg_b) == u16_sha and 'holds zero bytes' in text(os.path.join(mc, 'launcher_log.txt')) and
          ('The settings file %s is not plain text' % cfg_b) in glog and 'Save it again as UTF-8' in glog and not frames_of(g2h),
          'rc %d %s | %s' % (rc, out[-300:], glog[-300:]))
    # a settings file with no save line that another program holds but lets others read: the launcher cannot add the line and says so in its log,
    # and the game reads the file as it is and plays, its save by its default name in the player's folder
    with open(cfg_b, 'wb') as f:
        f.write(b'# made up, held by another program\r\nscale=2\r\n')
    held_sha = sha(cfg_b)
    for leftover in ('paprium.sav', 'paprium.sav.bak'):
        if os.path.exists(os.path.join(mc, leftover)):
            os.remove(os.path.join(mc, leftover))
    k32, h = hold_reading(cfg_b)
    try:
        g2i = os.path.join(folder, ODD_NAME, 'run2i')
        rc, out = launcher(b, '--headless', *game_args(g2i), '--game-arg', '--test-save-change', '--game-arg', '5', env=env_b)
    finally:
        k32.CloseHandle(ctypes.c_void_p(h))
    check('a settings file another program holds but lets others read: the launcher\'s log says the save line could not be added, the file stays, '
          'and the game plays and saves in the player\'s folder',
          rc == 0 and sha(cfg_b) == held_sha and 'could not be added to it' in text(os.path.join(mc, 'launcher_log.txt')) and len(frames_of(g2i)) == 21 and
          os.path.exists(os.path.join(mc, 'paprium.sav')) and set(listing(b)) == names, 'rc %d %s' % (rc, out[-400:]))
    os.remove(os.path.join(mc, 'paprium_pc.cfg'))
    g2e = os.path.join(folder, ODD_NAME, 'run2e')
    rc, out = launcher(b, '--headless', *game_args(g2e), env=env_b)
    check('with no settings file, the launcher writes it again with the save line before the game starts', rc == 0 and len(frames_of(g2e)) == 21 and
          ('save=' + os.path.join(mc, 'paprium.sav')) in text(os.path.join(mc, 'paprium_pc.cfg')), 'rc %d %s' % (rc, out[-300:]))

    # a game file whose path is longer than 259 characters
    long_dir = os.path.normpath(os.path.abspath(os.path.join(folder, 'long', *(['folder with a long name %02d' % i for i in range(12)]))))
    os.makedirs('\\\\?\\' + long_dir)
    long_rom = os.path.join(long_dir, 'my game.md')
    with open('\\\\?\\' + long_rom, 'wb') as f:
        f.write(rom)
    rc, out = launcher(b, '--headless', '--no-play', '--rom', long_rom, env=env_b)
    if long_paths_on():
        check('a game file at a path of %d characters, with long paths on for this PC: it is read (the manifest asks for long paths)' % len(long_rom),
              rc == 0 and 'checks good' in out, 'rc %d %s' % (rc, out[-300:]))
    else:
        check('a game file at a path of %d characters, with long paths off for this PC: the reason says to move it' % len(long_rom),
              rc == 3 and 'shorter path' in out, 'rc %d %s' % (rc, out[-300:]))
    shutil.rmtree('\\\\?\\' + os.path.normpath(os.path.abspath(os.path.join(folder, 'long'))))

    # a new version of the package: built again
    rp = os.path.join(b, 'tools', 'rules.json')
    r2 = dict(rules)
    r2['package_version'] = 'test-2'
    with open(rp, 'w', encoding='utf-8') as f:
        json.dump(r2, f, indent=1)
    rc, out = launcher(b, '--headless', '--no-play', env=env_b)
    check('a new version of the package builds the game again', rc == 0 and 'another version' in out and 'package_version=test-2' in text(os.path.join(mc, 'build_record.txt')), out[-400:])
    rc, out = launcher(b, '--headless', '--no-play', env=env_b)
    check('and the start after that comes from the cache', rc == 0 and 'from the cache' in out, out[-300:])

    # a DLL that is not the tested one: the player is told
    r3 = dict(r2)
    r3['tested_dll_sha256'] = '0' * 64
    with open(rp, 'w', encoding='utf-8') as f:
        json.dump(r3, f, indent=1)
    os.remove(os.path.join(mc, 'build_record.txt'))
    g6 = os.path.join(folder, 'run6')
    rc, out = launcher(b, '--headless', *game_args(g6), env=env_b)
    check('a built game that is not the tested one: the player is told to unzip again, and the game plays', rc == 0 and
          'not byte for byte the one we built and tested' in out and 'unzip the whole download again' in out and len(frames_of(g6)) == 21, 'rc %d %s' % (rc, out[-500:]))
    g6b = os.path.join(folder, 'run6b')
    rc, out = launcher(b, '--headless', *game_args(g6b), env=env_b)
    check('and the next start does not take it from the cache: it builds the game again, and tells it again while it still differs, in the log too',
          rc == 0 and 'from the cache' not in out and 'so it is built again' in out and 'each start builds the game again' in out and
          'not byte for byte the one we built and tested' in text(os.path.join(mc, 'launcher_log.txt')) and len(frames_of(g6b)) == 21, 'rc %d %s' % (rc, out[-500:]))
    with open(rp, 'w', encoding='utf-8') as f:
        json.dump(r2, f, indent=1)
    os.remove(os.path.join(mc, 'build_record.txt'))
    rc, out = launcher(b, '--headless', '--no-play', env=env_b)
    check('with the rules as published, the build is the tested one again', rc == 0 and sha(os.path.join(mc, 'pc_game.dll')) == rules['tested_dll_sha256'], out[-300:])
    # the notice's advice followed: a package with a changed file builds a game that is not the tested one; the whole download unzipped again
    # into a new folder, started with the same player's folder, builds the tested game at once and says nothing about it
    dmg = os.path.join(folder, 'changed_pkg')
    shutil.copytree(base, dmg)
    with open(os.path.join(dmg, 'tools', 'rt', 'pc_game_exports.c'), 'a', encoding='ascii', newline='\n') as f:
        f.write('int pl_test_changed_file = 1;\n')
    app_u, env_u = appdata(folder, 'appdata_unzip_again')
    md = files_of(app_u)
    gd = os.path.join(folder, 'run_changed')
    rc, out = launcher(dmg, '--headless', '--rom', rom_path, *game_args(gd), env=env_u)
    check('a package with a changed file builds a game that is not the tested one: the player is told, and the game plays', rc == 0 and
          'not byte for byte the one we built and tested' in out and os.path.exists(os.path.join(md, 'pc_game.dll')) and
          sha(os.path.join(md, 'pc_game.dll')) != rules['tested_dll_sha256'] and 'matches_tested_build=no' in text(os.path.join(md, 'build_record.txt')) and
          len(frames_of(gd)) == 21, 'rc %d %s' % (rc, out[-500:]))
    fresh = os.path.join(folder, 'fresh_pkg')
    shutil.copytree(base, fresh)
    gd2 = os.path.join(folder, 'run_fresh')
    rc, out = launcher(fresh, '--headless', *game_args(gd2), env=env_u)
    check('the download unzipped again into a new folder, with the same player\'s folder: the tested game is built at once, with no notice, and plays',
          rc == 0 and 'so it is built again' in out and 'not byte for byte the one we built and tested' not in out and
          sha(os.path.join(md, 'pc_game.dll')) == rules['tested_dll_sha256'] and 'matches_tested_build=yes' in text(os.path.join(md, 'build_record.txt')) and
          len(frames_of(gd2)) == 21, 'rc %d %s' % (rc, out[-500:]))
    rc, out = launcher(fresh, '--headless', '--no-play', env=env_u)
    check('and the start after it comes from the cache', rc == 0 and 'from the cache' in out, out[-300:])
    shutil.rmtree(dmg)
    shutil.rmtree(fresh)
    shutil.rmtree(app_u)

    # a changed DLL: built again
    with open(os.path.join(mc, 'pc_game.dll'), 'r+b') as f:
        f.seek(4096)
        x = f.read(1)
        f.seek(4096)
        f.write(bytes([x[0] ^ 1]))
    rc, out = launcher(b, '--headless', '--no-play', env=env_b)
    check('a DLL changed since its build is built again', rc == 0 and 'has changed' in out and sha(os.path.join(mc, 'pc_game.dll')) == rules['tested_dll_sha256'], out[-300:])

    # a build cut short by ending the launcher, and the next start
    os.remove(os.path.join(mc, 'build_record.txt'))
    p = subprocess.Popen([os.path.join(b, 'Paprium.exe'), '--headless', '--no-play'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env_b,
                         cwd=folder)
    cut = False
    for _ in range(600):
        time.sleep(0.05)
        if 'pl_translate.py' in text(os.path.join(mc, 'build_log.txt')) and os.path.exists(os.path.join(mc, 'build.tmp', 'boot_image.json')):
            p.kill()
            cut = True
            break
        if p.poll() is not None:
            break
    p.wait()
    time.sleep(1.0)
    check('a build cut short during the translation: the launcher ended there', cut)
    check('its tools ended with it: nothing runs from the package\'s tools folder', not leftover_tools(b), str(leftover_tools(b)))
    check('it left no record', not os.path.exists(os.path.join(mc, 'build_record.txt')))
    held = []
    for name in ('launcher_log.txt', 'build_log.txt'):
        p_ = os.path.join(mc, name)
        try:
            os.replace(p_, p_ + '.moved')
            os.replace(p_ + '.moved', p_)
        except OSError as x:
            held.append('%s: %s' % (name, x))
    check('no process still holds its logs (a tool that inherited them would)', not held, str(held))
    rc, out = launcher(b, '--headless', '--no-play', env=env_b)
    check('the next start builds the game again, and the old build folder is gone', rc == 0 and not os.path.exists(os.path.join(mc, 'build.tmp')) and
          sha(os.path.join(mc, 'pc_game.dll')) == rules['tested_dll_sha256'], out[-300:])

    # each step failing, and too little disk space, with --no-play: nothing in place changes
    dll_before = sha(os.path.join(mc, 'pc_game.dll'))
    outs = {}
    for step, code, words in (('boot', 5, 'start could not be read'), ('translate', 5, 'translation of the game failed'), ('compile', 5, 'TinyCC could not build'),
                              ('move', 4, 'could not be put in place'), ('record', 4, 'its record could not be written')):
        os.remove(os.path.join(mc, 'build_record.txt')) if os.path.exists(os.path.join(mc, 'build_record.txt')) else None
        rc, out = launcher(b, '--headless', '--no-play', '--test-fail', step, env=env_b)
        outs[step] = out
        check('the %s step failing: the reason, exit %d, no record, the build folder gone' % (step, code), rc == code and words in out and
              not os.path.exists(os.path.join(mc, 'build_record.txt')) and not os.path.exists(os.path.join(mc, 'build.tmp')), 'rc %d %s' % (rc, out[-300:]))
    check('the move failing names the system\'s own reason, not a guess, and that nothing was changed', 'There is not enough space on the disk' in outs['move'] and
          'may be full' not in outs['move'] and 'Nothing was changed.' in outs['move'], outs['move'][-300:])
    check('the record failing after the game is in place: it says so and that the next start builds again, never "Nothing was changed"',
          'is in place, but its record could not be written' in outs['record'] and 'next start builds the game again' in outs['record'] and
          'Nothing was changed' not in outs['record'], outs['record'][-300:])
    if len(b) > 160:
        check('a step failing below a folder path of %d characters: the path is blamed, not the game file' % len(b),
              'move the folder to a shorter path' in outs['boot'] and 'from your game file' not in outs['boot'], outs['boot'][-400:])
    else:
        check('a step failing below a folder path of %d characters: the game file is named, no path advice' % len(b),
              'from your game file' in outs['boot'] and 'move the folder' not in outs['boot'], outs['boot'][-400:])
    # a tool of the build that Windows will not start (execution denied on the boot tool): named, with Windows' words, not the game file
    boot_tool = os.path.join(b, 'tools', 'pc_boot_image.exe')
    denied = subprocess.run(['icacls', boot_tool, '/deny', '%s:(X)' % user], capture_output=True, text=True).returncode == 0
    try:
        os.remove(os.path.join(mc, 'build_record.txt')) if os.path.exists(os.path.join(mc, 'build_record.txt')) else None
        rc, out = launcher(b, '--headless', '--no-play', env=env_b)
    finally:
        subprocess.run(['icacls', boot_tool, '/remove:d', user], capture_output=True)
    check('the boot tool that Windows will not start: named, with Windows\' own words, and the game file not blamed (%s)' % ('denied' if denied else 'deny failed'),
          denied and rc == 5 and 'tools\\pc_boot_image.exe, could not be started (' in out and 'from your game file' not in out, 'rc %d %s' % (rc, out[-400:]))
    # a player's folder so deep that TinyCC's copied headers lie past 259 characters (the app-data folder of a user whose profile path is very long):
    # the build really fails at TinyCC, and the player's folder is named as the cause, with its length, not the game file, and not this program's
    # folder while that one is short
    work_long = os.path.join(folder, 'appdata_long')
    room = 225 - len(os.path.join(work_long, 'x', 'Paprium PC')) + 1
    if not long_paths_on() or room < 1:
        print('skip the deep player folder: %s' % ('long paths are off on this PC' if room >= 1 else 'the test folder is too deep already'))
    else:
        app_long, env_long = appdata(work_long, 'x' * room)
        mc_long = files_of(app_long)
        rc, out = launcher(a, '--headless', '--no-play', '--rom', rom_path, env=env_long)
        program_long = len(a) > 160
        check('a player\'s folder %d characters long: TinyCC fails, and the player\'s folder is named with its length, not the game file%s' % (
              len(mc_long), ' (this program\'s folder is long too, and named)' if program_long else ', not this program\'s folder'),
              rc == 5 and 'TinyCC could not build' in out and 'The folder that keeps your files, %s, has a path %d characters long' % (mc_long, len(mc_long)) in out and
              'from your game file' not in out and ("This program's folder path is" in out) == program_long and
              not os.path.exists(os.path.join(mc_long, 'build_record.txt')), 'rc %d %s' % (rc, out[-700:]))
        check('and the launcher\'s log says at the start that the player\'s folder is that long', 'the player\'s folder\'s path is %d characters long' % len(mc_long)
              in text(os.path.join(mc_long, 'launcher_log.txt')))
        shutil.rmtree('\\\\?\\' + os.path.normpath(os.path.abspath(work_long)))
    rc, out = launcher(b, '--headless', '--no-play', '--test-free-mb', '10', env=env_b)
    check('too little disk space: said before anything is written', rc == 4 and 'MB free' in out and not os.path.exists(os.path.join(mc, 'build.tmp')), out[-300:])
    check('the DLL from before is still there, unchanged', sha(os.path.join(mc, 'pc_game.dll')) == dll_before)

    # the interpreter when a build step fails: the game plays, the notice says it is not the tested build, and the next start builds again
    for step in ('translate', 'compile'):
        g7 = os.path.join(folder, 'run7_' + step)
        rc, out = launcher(b, '--headless', '--test-fail', step, *game_args(g7), env=env_b)
        game_log = text(os.path.join(mc, 'game_log.txt'))
        check('the %s step failing with the game to play: it plays through the interpreter, 21 frames, and the notice says it is not the tested build' % step,
              rc == 0 and 'not the build we tested' in out and 'own interpreter' in out and 'interpreter runs every instruction' in game_log and
              '--interpreter' in text(os.path.join(mc, 'launcher_log.txt')) and len(frames_of(g7)) == 21, 'rc %d %s' % (rc, out[-600:]))
        check('and it left no record, so the next start tries the build again', not os.path.exists(os.path.join(mc, 'build_record.txt')))
    rc, out = launcher(b, '--headless', '--no-play', env=env_b)
    check('then the build goes through', rc == 0 and sha(os.path.join(mc, 'pc_game.dll')) == rules['tested_dll_sha256'], out[-300:])
    rc, out = launcher(b, '--headless', '--no-play', '--rom', sw_path, '--test-free-mb', '10', env=env_b)
    check('the file named again when the game is built, with 10 MB free: nothing is written, so no room is asked, and the game is ready from the cache',
          rc == 0 and 'from the cache' in out, 'rc %d %s' % (rc, out[-300:]))

    # two launchers at once
    k32, h = hold_exclusive(os.path.join(mc, 'launcher.lock'))
    try:
        rc, out = launcher(b, '--headless', '--no-play', env=env_b)
        check('a second launcher while one runs: it says so and stops', rc == 7, 'rc %d %s' % (rc, out[-200:]))
    finally:
        k32.CloseHandle(ctypes.c_void_p(h))

    # the game program alone
    gp = os.path.join(b, 'pc_port.exe')
    r = subprocess.run([gp, '--rom', os.path.join(mc, 'paprium.bin'), '--run-dir', os.path.join(folder, 'g_nodll'), '--frames', '5', '--game-dll', 'nothing_here.dll'],
                       capture_output=True, text=True, env=env_b)
    check('the game program with no DLL: exit 6 and a sentence for the player', r.returncode == 6 and 'has not been built on this PC yet' in r.stdout, r.stdout[-300:])
    for what, source, words in made_up_dlls():
        made = os.path.join(folder, 'dll_' + what.split()[0])
        os.makedirs(made, exist_ok=True)
        with open(os.path.join(made, 'x.c'), 'w', encoding='ascii') as f:
            f.write(source)
        tc = subprocess.run([os.path.join(base, 'tools', 'tcc', 'tcc.exe'), '-B' + os.path.join(base, 'tools', 'tcc'), '-shared', '-o', 'x.dll', 'x.c'], cwd=made,
                            capture_output=True, text=True, env=env_b)
        if what.startswith('text'):
            with open(os.path.join(made, 'x.dll'), 'w', encoding='ascii') as f:
                f.write('this is not a program\n')
        r = subprocess.run([gp, '--rom', os.path.join(mc, 'paprium.bin'), '--run-dir', made, '--frames', '5', '--game-dll', os.path.join(made, 'x.dll')],
                           capture_output=True, text=True, env=env_b)
        check('the game program refuses %s: exit 6, the reason' % what, (tc.returncode == 0 or what.startswith('text')) and r.returncode == 6 and words in r.stdout,
              'tcc %d rc %d %s' % (tc.returncode, r.returncode, r.stdout[-300:]))
    # TinyCC with its folder given as a relative path holding spaces, in quotes as the launcher gives it
    spaced = os.path.join(folder, 'T C C dir')
    shutil.copytree(os.path.join(base, 'tools', 'tcc'), spaced)
    made = os.path.join(folder, 'dll_spaced')
    os.makedirs(made)
    with open(os.path.join(made, 'x.c'), 'w', encoding='ascii') as f:
        f.write(made_up_dlls()[0][1])
    line = '"%s" "-B..\\T C C dir" -shared -o x.dll x.c' % os.path.join(spaced, 'tcc.exe')
    tc = subprocess.run(line, cwd=made, capture_output=True, text=True, env=env_b)
    check('TinyCC takes its folder as a relative path with spaces, in quotes', tc.returncode == 0 and os.path.exists(os.path.join(made, 'x.dll')),
          'rc %d %s' % (tc.returncode, (tc.stdout + tc.stderr)[-300:]))
    shutil.rmtree(spaced)
    shutil.rmtree(made)
    os.makedirs(os.path.join(folder, 'g_int'), exist_ok=True)
    r = subprocess.run([gp, '--rom', os.path.join(mc, 'paprium.bin'), '--run-dir', os.path.join(folder, 'g_int'), '--frames', '20', '--interpreter'],
                       capture_output=True, text=True, env=env_b)
    check('the interpreter plays with no DLL', r.returncode == 0 and 'RESULT: PASS' in r.stdout and 'interpreter runs every instruction' in r.stdout, r.stdout[-300:])

    # the interpreter when the game program cannot load the DLL it is given: the record is forgotten and the game plays
    g8 = os.path.join(folder, 'run8')
    rc, out = launcher(b, '--headless', '--game-arg', '--game-dll', '--game-arg', 'nothing_here.dll', *game_args(g8), env=env_b)
    check('when the game cannot load its DLL: the record is forgotten, the game plays through the interpreter, and the notice says it is not the tested build',
          rc == 0 and not os.path.exists(os.path.join(mc, 'build_record.txt')) and 'not the build we tested' in out and len(frames_of(g8)) == 21 and
          'interpreter runs every instruction' in text(os.path.join(mc, 'game_log.txt')), 'rc %d %s' % (rc, out[-500:]))
    check('what the game program said goes into the launcher\'s log', 'the game program said: ' in text(os.path.join(mc, 'launcher_log.txt')) and
          'has not been built on this PC yet' in text(os.path.join(mc, 'launcher_log.txt')))
    check('and the notice gives its reason, with no guess about a virus scanner', 'The game program said: The game has not been built on this PC yet' in out
          and 'virus' not in out.lower() and 'Smart App Control' not in out, out[-600:])
    rc, out = launcher(b, '--headless', '--no-play', env=env_b)
    check('the next start builds the game again', rc == 0 and sha(os.path.join(mc, 'pc_game.dll')) == rules['tested_dll_sha256'], out[-300:])
    # a damaged DLL whose record matches it, as a file a virus scanner changed after the build: it is not the tested one, so the game is built
    # again instead of started from the cache
    def damage_with_record():
        with open(os.path.join(mc, 'pc_game.dll'), 'wb') as f:
            f.write(b'MZ' + bytes(4094))
        rec = text(os.path.join(mc, 'build_record.txt'))
        with open(os.path.join(mc, 'build_record.txt'), 'w', encoding='ascii', newline='\n') as f:
            f.write(re.sub(r'dll_sha256=\w+', 'dll_sha256=' + sha(os.path.join(mc, 'pc_game.dll')), rec))
    damage_with_record()
    g9 = os.path.join(folder, 'run9')
    rc, out = launcher(b, '--headless', *game_args(g9), env=env_b)
    check('a DLL changed after its build, with its record changed to match: not started from the cache, but built again, the tested one, and it plays',
          rc == 0 and 'from the cache' not in out and 'so it is built again' in out and 'could not be loaded' not in out and
          'not byte for byte the one we built and tested' not in out and sha(os.path.join(mc, 'pc_game.dll')) == rules['tested_dll_sha256'] and
          len(frames_of(g9)) == 21, 'rc %d %s' % (rc, out[-500:]))
    # Windows refusing a DLL whose record matches it, through the launcher: reached in a package whose rules name no tested build, where a start
    # from the cache takes the DLL its record names; the game then plays through the interpreter
    r_none = dict(r2)
    r_none['tested_dll_sha256'] = ''
    with open(rp, 'w', encoding='utf-8') as f:
        json.dump(r_none, f, indent=1)
    damage_with_record()
    g9b = os.path.join(folder, 'run9b')
    rc, out = launcher(b, '--headless', *game_args(g9b), env=env_b)
    check('a DLL Windows will not load, with its record matching (a package naming no tested build): started from the cache, then played through the '
          'interpreter with the notice', rc == 0 and 'from the cache' in out and 'could not be loaded' in out and 'not the build we tested' in out and
          len(frames_of(g9b)) == 21, 'rc %d %s' % (rc, out[-500:]))
    check('the notice gives Windows\' reason for the damaged file, with no guess about a virus scanner or Smart App Control',
          'The game program said: Windows would not load the game' in out and 'The file is damaged, or it is not a program at all.' in out and
          'virus' not in out.lower() and 'Smart App Control' not in out, out[-700:])
    with open(rp, 'w', encoding='utf-8') as f:
        json.dump(r2, f, indent=1)
    rc, out = launcher(b, '--headless', '--no-play', env=env_b)
    check('and the next start builds the game again', rc == 0 and 'did not finish, or its game could not be loaded' in out and
          sha(os.path.join(mc, 'pc_game.dll')) == rules['tested_dll_sha256'], out[-300:])

    # a game that stops with a fault: the player is pointed at game_log.txt
    f_pkg = os.path.join(folder, 'F')
    shutil.copytree(base, f_pkg)
    with open(os.path.join(f_pkg, 'tools', 'rules.json'), 'w', encoding='utf-8') as f:
        json.dump(r2, f, indent=1)                       # the version b's player folder was built by
    app_f, env_f = appdata(folder, 'appdata_F')
    shutil.copytree(mc, files_of(app_f))
    with open(os.path.join(folder, 'fault.c'), 'w', encoding='ascii') as f:
        # a made-up game program that writes a file by a relative name, then ends with a fault's code
        f.write('#include <stdio.h>\nint main(void) { FILE *f = fopen("cwd_probe.txt", "w"); if (f) { fputs("x", f); fclose(f); } return 5; }\n')
    tc = subprocess.run([os.path.join(base, 'tools', 'tcc', 'tcc.exe'), '-B' + os.path.join(base, 'tools', 'tcc'), '-o', os.path.join(f_pkg, 'pc_port.exe'),
                         os.path.join(folder, 'fault.c')], capture_output=True, text=True, env=env_f)
    rc, out = launcher(f_pkg, '--headless', env=env_f)
    check('a game that stops with a fault: the player is told, and pointed at game_log.txt', tc.returncode == 0 and rc == 5 and
          'stopped because of a fault (exit code 5)' in out and os.path.join(files_of(app_f), 'game_log.txt') in out, 'tcc %d rc %d %s' % (tc.returncode, rc, out[-400:]))
    check('the game runs in the player\'s folder: a file it writes by a relative name lands there, not in the program\'s folder',
          os.path.exists(os.path.join(files_of(app_f), 'cwd_probe.txt')) and not os.path.exists(os.path.join(f_pkg, 'cwd_probe.txt')))
    # The menu bar's Soft Reset: the game ends with exit code 8 and the launcher starts it again, the same way. A made-up game program counts its starts
    # in a file in the player's folder, asks for a Soft Reset at its first start and ends normally at its second.
    count = os.path.join(files_of(app_f), 'starts.txt')

    def made_up_game(name, first_code):
        with open(os.path.join(folder, name + '.c'), 'w', encoding='ascii') as f:
            f.write('#include <stdio.h>\n#include <string.h>\nint main(int argc, char **argv) { int n = 0, i, from = 0, keys = 0; FILE *f = fopen("starts.txt", "r");\n'
                    ' if (f) { if (fscanf(f, "%d", &n) != 1) n = 0; fclose(f); }\n'
                    ' for (i = 1; i < argc; i++) { if (!strcmp(argv[i], "--from-launcher")) from = 1; if (!strcmp(argv[i], "--key-script")) keys = 1; }\n'
                    ' f = fopen("starts.txt", "w"); if (f) { fprintf(f, "%d %d %d", n + 1, from, keys); fclose(f); }\n'
                    ' return n == 0 ? ' + str(first_code) + ' : 0; }\n')
        if os.path.exists(count):
            os.remove(count)
        return subprocess.run([os.path.join(base, 'tools', 'tcc', 'tcc.exe'), '-B' + os.path.join(base, 'tools', 'tcc'), '-o', os.path.join(f_pkg, 'pc_port.exe'),
                               os.path.join(folder, name + '.c')], capture_output=True, text=True, env=env_f)

    tc = made_up_game('soft8', 8)
    rc, out = launcher(f_pkg, '--headless', env=env_f)
    check('Soft Reset (exit code 8): the launcher starts the game again, the same way, and says so; no fault is told',
          tc.returncode == 0 and rc == 0 and text(count) == '2 1 0' and 'Soft Reset: the game starts again' in out and 'because of a fault' not in out,
          'tcc %d rc %d starts %r %s' % (tc.returncode, rc, text(count) if os.path.exists(count) else None, out[-500:]))
    # A test's key script belongs to the first start only, as in the game program's own restart: played again, it would ask for the Soft Reset again.
    os.remove(count)
    keys_txt = os.path.join(folder, 'soft8_keys.txt')
    with open(keys_txt, 'w', encoding='ascii') as f:
        f.write('1 down F10\n')
    rc, out = launcher(f_pkg, '--headless', '--game-arg', '--key-script', '--game-arg', keys_txt, '--game-arg', '--frames', '--game-arg', '5', env=env_f)
    flog = text(os.path.join(files_of(app_f), 'launcher_log.txt'))
    check('after a Soft Reset the game gets the same arguments but a test\'s key script, which belongs to the first start',
          rc == 0 and text(count) == '2 1 0' and flog.count('--key-script') == 1 and flog.count('"--frames" "5"') == 2,
          'tcc %d rc %d starts %r %s' % (tc.returncode, rc, text(count) if os.path.exists(count) else None, out[-500:]))
    # Exit code 7 is a fault of the cart's chip program, never a Soft Reset: the player is told, and the game is not started again.
    tc = made_up_game('fault7', 7)
    rc, out = launcher(f_pkg, '--headless', env=env_f)
    check('exit code 7 is a fault: the player is told, and the game is not started again', tc.returncode == 0 and rc == 7 and text(count) == '1 1 0'
          and 'stopped because of a fault (exit code 7)' in out and 'Soft Reset' not in out,
          'tcc %d rc %d starts %r %s' % (tc.returncode, rc, text(count) if os.path.exists(count) else None, out[-500:]))
    # a game program that cannot use the DLL and prints no sentence about it (exit 6), then plays through the interpreter: the notice points at
    # launcher_log.txt, which keeps what it printed, as the interpreter's start writes game_log.txt again
    with open(os.path.join(folder, 'quiet6.c'), 'w', encoding='ascii') as f:
        f.write('#include <stdio.h>\n#include <string.h>\nint main(int argc, char **argv) { int i; for (i = 1; i < argc; i++) if (!strcmp(argv[i], "--interpreter")) '
                '{ puts("made-up interpreter run"); return 0; } puts("made-up words with no sentence for the player"); return 6; }\n')
    tc = subprocess.run([os.path.join(base, 'tools', 'tcc', 'tcc.exe'), '-B' + os.path.join(base, 'tools', 'tcc'), '-o', os.path.join(f_pkg, 'pc_port.exe'),
                         os.path.join(folder, 'quiet6.c')], capture_output=True, text=True, env=env_f)
    rc, out = launcher(f_pkg, '--headless', env=env_f)
    flog = text(os.path.join(files_of(app_f), 'launcher_log.txt'))
    check('a game program that gives no reason for a DLL it cannot use: the notice points at launcher_log.txt, which holds what it printed',
          tc.returncode == 0 and rc == 0 and ('the details are in %s\\launcher_log.txt' % files_of(app_f)) in out and 'what the game program printed is in' not in out and
          'made-up words with no sentence for the player' in flog and 'made-up interpreter run' in text(os.path.join(files_of(app_f), 'game_log.txt')),
          'tcc %d rc %d %s' % (tc.returncode, rc, out[-600:]))
    check('the interpreter\'s start is marked as the launcher\'s too, so a Soft Reset there comes back to the launcher', '--interpreter --from-launcher' in flog,
          flog[-600:])
    shutil.rmtree(f_pkg)

    # the hidden window's whole path: a drop on a fresh copy builds and plays
    c = os.path.join(folder, 'C')
    shutil.copytree(base, c)
    app_c, env_c = appdata(folder, 'appdata_C')
    cm = files_of(app_c)
    g3 = os.path.join(folder, 'run3')
    rc, out = launcher(c, '--hidden', '--test-drop', rom_path, *game_args(g3), env=env_c)
    check('the right file dropped on the hidden window: built, then played', rc == 0 and len(frames_of(g3)) == 21, 'rc %d %s' % (rc, out[-500:]))
    check('the same frames as the headless start', frames_of(g3) == frames_of(g1))
    # and through the hidden window, a failing build: the window's notice, then the interpreter
    os.remove(os.path.join(cm, 'build_record.txt'))
    g3b = os.path.join(folder, 'run3b')
    rc, out = launcher(c, '--hidden', '--test-fail', 'compile', *game_args(g3b), env=env_c)
    check('a failing build through the hidden window: the notice, then the game through the interpreter', rc == 0 and 'not the build we tested' in out and
          len(frames_of(g3b)) == 21 and 'interpreter runs every instruction' in text(os.path.join(cm, 'game_log.txt')), 'rc %d %s' % (rc, out[-500:]))
    rc, out = launcher(c, '--headless', '--no-play', env=env_c)
    check('and the start after it builds the game', rc == 0 and sha(os.path.join(cm, 'pc_game.dll')) == rules['tested_dll_sha256'], out[-300:])

    # the launcher ended from outside while the game runs: the game holds the lock, so a second launcher stays out until the game ends
    g5 = os.path.join(folder, 'run5')
    p = subprocess.Popen([os.path.join(c, 'Paprium.exe'), '--headless'] + game_args(g5, 900), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         env=env_c, cwd=folder)
    game_pid = None
    for _ in range(600):
        time.sleep(0.05)
        m = re.search(r'the game runs as process (\d+)', text(os.path.join(cm, 'launcher_log.txt')))
        if m and len(frames_of(g5)) > 5:
            game_pid = int(m.group(1))
            break
        if p.poll() is not None:
            break
    p.kill()
    p.wait()
    rc, out = launcher(c, '--headless', '--no-play', env=env_c)
    check('a second launcher while the game runs, the first one ended from outside: it says so and stops', game_pid is not None and rc == 7,
          'pid %s rc %d %s' % (game_pid, rc, out[-200:]))
    for _ in range(1200):
        if len(frames_of(g5)) >= 901 or not game_pid or not process_alive(game_pid):
            break
        time.sleep(0.1)
    if game_pid and process_alive(game_pid):
        stop_process(game_pid)      # only the game this test started, by the number its launcher logged
    rc, out = launcher(c, '--headless', '--no-play', env=env_c)
    check('once the game has ended, the next launcher starts from the cache', rc == 0 and 'from the cache' in out and len(frames_of(g5)) == 901,
          'rc %d frames %d %s' % (rc, len(frames_of(g5)), out[-200:]))

    # a build folder that is a link to another folder: the build removes the link and leaves that folder's files alone
    canary = os.path.join(folder, 'canary')
    os.makedirs(canary)
    with open(os.path.join(canary, 'keep.txt'), 'w', encoding='ascii') as f:
        f.write('this file must stay\n')
    os.remove(os.path.join(cm, 'build_record.txt'))
    # mklink reads a forward slash as the start of a switch, so the paths go in Windows' own form
    j = subprocess.run(['cmd', '/c', 'mklink', '/J', os.path.normpath(os.path.join(cm, 'build.tmp')), os.path.normpath(canary)], capture_output=True, text=True)
    rc, out = launcher(c, '--headless', '--no-play', env=env_c)
    check('a build folder that is a junction to another folder: built, the link gone, the other folder\'s file still there', j.returncode == 0 and rc == 0 and
          os.path.exists(os.path.join(canary, 'keep.txt')) and not os.path.exists(os.path.join(cm, 'build.tmp')), 'mklink %d rc %d %s' % (j.returncode, rc, out[-300:]))

    # the kept copy damaged on the disk: not used, and the player is told why the window asks again
    with open(os.path.join(cm, 'paprium.bin'), 'r+b') as f:
        f.seek(100)
        x = f.read(1)
        f.seek(100)
        f.write(bytes([x[0] ^ 0x40]))
    rc, out = launcher(c, '--headless', env=env_c)
    check('a kept copy changed on the disk: not used, and the reason is said', rc == 2 and 'has changed since it was checked' in out, 'rc %d %s' % (rc, out[-300:]))

    # a package with a piece missing
    d = os.path.join(folder, 'D')
    shutil.copytree(base, d)
    os.remove(os.path.join(d, 'tools', 'python', 'python.exe'))
    app_d, env_d = appdata(folder, 'appdata_D')
    rc, out = launcher(d, '--headless', '--rom', rom_path, env=env_d)
    check('a package with a piece missing: it names the piece, writes nothing', rc == 2 and 'python.exe' in out and not os.path.exists(os.path.join(d, 'my_copy'))
          and os.listdir(app_d) == [], out[-300:])
    shutil.rmtree(d)

    # an import list with one name left out: TinyCC refuses the build (the negative control for the list)
    e = os.path.join(folder, 'E')
    shutil.copytree(base, e)
    app_e, env_e = appdata(folder, 'appdata_E')
    pc_build.game_dll_def(os.path.join(e, 'tools', 'rt', 'pc_port_import.def'), pc_build.GAME_EXE, 'import', leave_out=('pt_clock',))
    rc, out = launcher(e, '--headless', '--no-play', '--rom', rom_path, env=env_e)
    check('an import list without pt_clock: TinyCC refuses, the build fails with its reason', rc == 5 and 'TinyCC could not build' in out and
          'pt_clock' in text(os.path.join(files_of(app_e), 'build_log.txt')), out[-300:])
    shutil.rmtree(e)

    # a package folder that cannot be written, as under Program Files: nothing changes for the player, whose files are in the app-data folder.
    # The package's folder is named plainly, with a space (TinyCC gets its path in quotes) and with letters outside plain ASCII (TinyCC reads its
    # command line in the system's code page, so it gets a copy of its own files in the build folder instead).
    made = []
    for n, name in enumerate(('RO', 'RO two words', 'RO Привет')):
        ro = os.path.join(folder, name)
        shutil.copytree(base, ro)
        app_n, env_n = appdata(folder, 'appdata%d' % n)
        made += [ro, app_n]
        denied = deny(ro, user)
        try:
            g4 = os.path.join(folder, 'run4_%d' % n)
            rc, out = launcher(ro, '--headless', '--rom', rom_path, *game_args(g4), env=env_n)
            other = files_of(app_n)
            log = text(os.path.join(other, 'launcher_log.txt'))
            check("a package folder that cannot be written, named %r (%s): the game is built in the app-data folder and plays, the package's folder unchanged" %
                  (name, 'denied' if denied else 'deny failed'),
                  denied and rc == 0 and os.path.exists(os.path.join(other, 'pc_game.dll')) and set(listing(ro)) == names and
                  len(frames_of(g4)) == 21 and sha(os.path.join(other, 'pc_game.dll')) == rules['tested_dll_sha256'], 'rc %d %s' % (rc, out[-400:]))
            copied = "TinyCC's files were copied into the build folder" in log
            if n == 2:
                check('letters outside plain ASCII on the way to the package: TinyCC got a copy of its own files, and the log says why', copied and
                      'letters outside plain ASCII' in log, log[-600:])
            print('TinyCC for %s: %s' % (ascii(name), 'a copy of its files' if copied else 'its own folder by a relative path'))
            check('TinyCC got its folder in quotes', re.search(r'tcc\.exe" "-B[^"]+" -shared', text(os.path.join(other, 'build_log.txt'))) is not None)
        finally:
            undeny(ro, user)
    # the app-data folder cannot be written: my_copy beside the program, and the log and the window's text say so
    w = os.path.join(folder, 'W')
    shutil.copytree(base, w)
    app_w, env_w = appdata(folder, 'appdata_ro')
    made += [w, app_w]
    denied = deny(app_w, user)
    try:
        g10 = os.path.join(folder, 'run10')
        rc, out = launcher(w, '--headless', '--rom', rom_path, *game_args(g10), env=env_w)
        wm = os.path.normpath(os.path.abspath(os.path.join(w, 'my_copy')))     # as the launcher names it, from its own path
        log = text(os.path.join(wm, 'launcher_log.txt'))
        parts = {'denied': denied, 'rc 0': rc == 0, 'DLL in my_copy': os.path.exists(os.path.join(wm, 'pc_game.dll')), '21 frames': len(frames_of(g10)) == 21,
                 'nothing in the app-data folder': not os.path.exists(files_of(app_w)), 'the log says why': 'the app-data folder cannot be written' in log,
                 'the settings name the save in my_copy': ('save=' + os.path.join(wm, 'paprium.sav')) in text(os.path.join(wm, 'paprium_pc.cfg'))}
        check('an app-data folder that cannot be written (%s): the player\'s files go to my_copy beside the program, and the game plays' %
              ('denied' if denied else 'deny failed'), all(parts.values()), '%s rc %d %s' % ([k for k, v in parts.items() if not v], rc, out[-300:]))
        # neither folder can be written
        w2 = os.path.join(folder, 'W2')
        shutil.copytree(base, w2)
        made.append(w2)
        denied2 = deny(w2, user)
        try:
            rc, out = launcher(w2, '--headless', '--rom', rom_path, env=env_w)
            check('neither the app-data folder nor the program\'s folder can be written: it says so and stops', denied2 and rc == 4 and
                  'no folder can be written' in out, 'rc %d %s' % (rc, out[-300:]))
        finally:
            undeny(w2, user)
    finally:
        undeny(app_w, user)
    for x in [a, b, c, bad, canary] + made:
        shutil.rmtree(x, ignore_errors=True)


# The words the team's decision allows the players' texts to quote as given: the answer on the GPL and on selling, the answer on
# sharing, the phrase for what the program builds, and the answer about the name. Any other span in double quotes of more than five words is
# refused, so this check names no words but these, which the texts carry anyway.
ALLOWED_QUOTES = (
    'gpl would apply to everything but the games rom itself the output your tool makes would still be partially their ip and cannot be sold.',
    'sharing output is forbidden and not allowed just like sharing a rom',
    'a Patch copy of the Legally obtained rom',
    'there is no affiliation and no commercial use.',
)


def quotes_check(readme, notices):
    """Whether READ_ME and NOTICES quote only the allowed words. Returns (ok, detail). Every allowed quote stands in READ_ME, the phrase for what
    is built stands in NOTICES too, and neither text holds another span in double quotes of more than five words. A line break inside a quote
    counts as a space; each text is read on its own, so a stray quote mark in one cannot pair with one in the other."""
    extra, quoted = [], {}
    for name, body in (('READ_ME', readme), ('NOTICES', notices)):
        quoted[name] = [' '.join(q.split()) for q in re.findall(r'"([^"]+)"', body, flags=re.S)]
        extra += ['%s: %s' % (name, q[:80]) for q in quoted[name] if len(q.split()) > 5 and q not in ALLOWED_QUOTES]
    missing = [q[:40] for q in ALLOWED_QUOTES if q not in quoted['READ_ME']]
    if ALLOWED_QUOTES[2] not in quoted['NOTICES']:
        missing.append('NOTICES: ' + ALLOWED_QUOTES[2])
    return not extra and not missing, 'other quotes %s; missing %s' % (extra[:4], missing)


# What READ_ME and NOTICES must say, each as one run of words (line breaks count as spaces): the text, the statement it makes.
PACKAGE_WORDS = (
    ('NOTICES', 'rtl/upstream/pad_io.sv, MegaDrive input implementation, Copyright (c) 2023 Alexey Melnikov',
     'the credit for the pad model the EA 4-Way Play follows'),
    ('NOTICES', 'No Sega BIOS, no TMSS code and no 32X or Mega CD code.', 'no console BIOS, TMSS, 32X or Mega CD code'),
    ('READ_ME', 'This download carries the file little_man_firmware.bin beside Paprium.exe', 'the download carries the firmware file beside Paprium.exe'),
    ('NOTICES', 'licences\\THIRD_PARTY_NOTICES_firmware.md covers little_man_firmware.bin', 'the firmware file\'s notices'),
    ('READ_ME', 'stalls.txt also names your PC\'s sound device, monitor and graphics card', 'what stalls.txt names'),
    ('READ_ME', 'then the first pad of that model plugged in after the pull counts as the one that came back', 'which pad counts as the one that came back'),
    ('READ_ME', 'A pad pulled out for a moment (a cable, a pad that went to sleep) goes back to the player who had it when it comes back',
     'a pad pulled out goes back to its player'),
    ('READ_ME', 'The list only picks the stage, as on the Pocket: its Off is the game\'s own first stage', 'the stage list only picks the stage'),
)


def package_words(readme, notices):
    """The statements of PACKAGE_WORDS that READ_ME or NOTICES lack."""
    texts = {'READ_ME': ' '.join(readme.split()), 'NOTICES': ' '.join(notices.split())}
    return [what for name, words, what in PACKAGE_WORDS if words not in texts[name]]


def package_words_rules(check):
    """package_words on the texts as targets/package holds them: all there; each statement taken out of its text in turn is caught. And the
    firmware file's published hashes, as targets/pc/pc_package.py pins them, in READ_ME, NOTICES and the repository's README.md."""
    readme, notices = pc_package.package_text('READ_ME.txt', VERSION='0'), pc_package.package_text('NOTICES.txt', VERSION='0', FW='0')
    check('package words: the texts in targets/package say each statement', not package_words(readme, notices), str(package_words(readme, notices)))
    pins = pc_package.FIRMWARE_PINS
    texts = {'READ_ME': readme, 'NOTICES': notices, 'README.md': text(os.path.join(REPO, 'README.md'))}
    check('READ_ME, NOTICES and README.md name the firmware file\'s MD5 and SHA-256 as targets/pc/pc_package.py pins them',
          all(pins['md5'] in t and pins['sha256'] in t for t in texts.values()), str([k for k, t in texts.items() if pins['sha256'] not in t]))
    for name, words, what in PACKAGE_WORDS:
        first = words.split()[0]
        r = readme.replace(first, 'X' + first[1:]) if name == 'READ_ME' else readme
        n = notices.replace(first, 'X' + first[1:]) if name == 'NOTICES' else notices
        check('package words: %s without "%s ..." is caught' % (name, first), what in package_words(r, n))


def firmware_zip_rules(check, tmp, config_path):
    """targets/pc/pc_package.py takes little_man_firmware.bin only out of the published zip (firmware_from_zip): made-up zips and made-up pins here,
    so no byte of the real file is needed. Then the package script itself: with no firmware zip and no --no-firmware it stops before it builds
    anything, and a zip that is not the published one stops it too."""
    import contextlib
    folder = os.path.join(tmp, 'fwzip')
    if os.path.exists(folder):
        shutil.rmtree(folder)
    os.makedirs(folder)
    fw = made_up_firmware(41)

    def made_zip(name, members):
        path = os.path.join(folder, name)
        with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
            for n, d in members:
                z.writestr(n, d)
        return path

    def stops(path, pins):
        try:
            pc_package.firmware_from_zip(path, pins)
            return ''
        except SystemExit as x:
            return str(x)
        except Exception as x:          # any other failure is not the refusal a check looks for
            return 'broke: %r' % x
    good = made_zip('good.zip', [('little_man_firmware.bin', fw), ('README.txt', b'made up\n')])
    pins = {'zip_sha256': pc_package.sha256(good), 'md5': hashlib.md5(fw).hexdigest(), 'sha256': hashlib.sha256(fw).hexdigest()}
    check('the package script takes the firmware file out of the zip with the pinned hashes, byte for byte', pc_package.firmware_from_zip(good, pins) == fw)
    other = made_zip('other.zip', [('little_man_firmware.bin', fw), ('README.txt', b'another\n')])
    check('a zip other than the pinned one is refused, though it holds the same file', 'is not the published Little-Man_Firmware_1.0.zip' in stops(other, pins))
    for name, members, words in (('folder.zip', [('fw/little_man_firmware.bin', fw)], 'not once at its top level'),
                                 ('twice.zip', [('little_man_firmware.bin', fw), ('little_man_firmware.bin', fw)], 'not once at its top level'),
                                 ('changed.zip', [('little_man_firmware.bin', fw[:-1] + bytes([fw[-1] ^ 1]))], 'is not the published file')):
        import warnings
        with warnings.catch_warnings():
            warnings.simplefilter('ignore')         # zipfile warns of the doubled name, which is the case here
            path = made_zip(name, members)
        got = stops(path, dict(pins, zip_sha256=pc_package.sha256(path)))
        check('the package script refuses a zip with the firmware file %s' % {'folder.zip': 'in a folder', 'twice.zip': 'twice',
                                                                               'changed.zip': 'with other bytes'}[name], words in got, got)
    check('with the published pins, a made-up zip is refused', 'is not the published Little-Man_Firmware_1.0.zip' in stops(good, None))
    with open(config_path, encoding='utf-8') as f:
        cfg = json.load(f)
    rom = os.path.join(folder, 'made_up.md')
    with open(rom, 'wb') as f:
        f.write(test_pc_machine.made_up_rom())
    with open(rom, 'rb') as f:
        rom_sha = hashlib.sha256(f.read()).hexdigest()
    plain = os.path.join(folder, 'local_no_zip.json')
    with open(plain, 'w', encoding='utf-8') as f:
        json.dump(dict(cfg, rom_path=rom, rom_sha256=rom_sha, firmware_zip=''), f)
    said = io.StringIO()
    with contextlib.redirect_stdout(said):
        try:
            rc = pc_package.main(['--config', plain, '--out', os.path.join(folder, 'pkg_none'), '--version', 't'])
        except (SystemExit, Exception) as x:
            rc = 'stopped another way: %r' % x
    check('the package script stops when it is given no firmware zip and no --no-firmware, before it builds anything', rc == 1 and
          'name the published Little-Man_Firmware_1.0.zip with --firmware-zip' in said.getvalue() and not os.path.exists(os.path.join(folder, 'pkg_none.work')),
          said.getvalue()[-300:])
    try:
        pc_package.main(['--config', plain, '--out', os.path.join(folder, 'pkg_other'), '--version', 't', '--firmware-zip', good])
        stopped = ''
    except SystemExit as x:
        stopped = str(x)
    except Exception as x:
        stopped = 'broke: %r' % x
    check('the package script refuses a firmware zip that is not the published one', 'is not the published Little-Man_Firmware_1.0.zip' in stopped and
          not os.path.exists(os.path.join(folder, 'pkg_other.work')), stopped)
    shutil.rmtree(folder)


def quote_rules(check):
    """quotes_check on made-up texts: the allowed words pass, also broken over lines; another long quote and a missing one are caught."""
    cut = ALLOWED_QUOTES[0].index(' ', 50)
    good = ('Press "Run anyway". As given:\n  "%s\n  %s"\n  "%s"\nIn the words given, "%s".\nAbout the name: "%s"\n' %
            (ALLOWED_QUOTES[0][:cut], ALLOWED_QUOTES[0][cut + 1:], ALLOWED_QUOTES[1], ALLOWED_QUOTES[2], ALLOWED_QUOTES[3]))
    notices = 'In the words given, that output is\n"%s".\n' % ALLOWED_QUOTES[2]
    check('quote check: the allowed words pass, a quote broken over two lines and a short quote too', quotes_check(good, notices)[0],
          quotes_check(good, notices)[1])
    planted = good + 'And also, as given: "this made-up sentence stands for words nobody allowed us to quote".\n'
    check('quote check: a planted quote of more than five other words is caught', not quotes_check(planted, notices)[0])
    check('quote check: the same planted quote in NOTICES is caught', not quotes_check(good, notices + '"%s"\n' % 'one two three four five six')[0])
    check('quote check: an allowed quote left out is caught', not quotes_check(good.replace(ALLOWED_QUOTES[3], 'the name'), notices)[0])
    check('quote check: NOTICES without the phrase for what is built is caught', not quotes_check(good, 'nothing quoted\n')[0])


def package_check_rules(check):
    """tests/check_package.py's rules on made-up bytes only: a made-up reference stands for the ROM."""
    import random
    import check_package
    import check_tree
    rnd = random.Random(20261111)
    for n in (12, 13, 14, 15, 16):
        weak = sum(check_package.weak(rnd.randbytes(n)) for _ in range(2000))
        check('package check: random runs of %d bytes are strong (%d of 2000 called weak)' % (n, weak), weak <= 2)
    tables = {
        'a 16-bit counting table, big-endian': b''.join(v.to_bytes(2, 'big') for v in range(0x1230, 0x1238)),
        'a 16-bit counting table, little-endian, from its second byte': b''.join(v.to_bytes(2, 'little') for v in range(0x0F00, 0x0F48, 9))[1:13],
        'a 32-bit counting table of 8 values': b''.join(v.to_bytes(4, 'little') for v in range(0x10000, 0x10000 + 8 * 0x4321, 0x4321)),
        'a program\'s table of export numbers': b''.join(v.to_bytes(2, 'little') for v in range(40, 48)),
        'zero bytes around three values': bytes(5) + b'\x12\x34\x56' + bytes(5),
    }
    for what, w in tables.items():
        check('package check: %s is weak' % what, check_package.weak(w))
    # The two kinds the earlier rule called weak although the firmware leak guard's 12-byte windows call them strong.
    five = bytes([0x11, 0x5A, 0x23, 0xE7, 0x9C, 0x11, 0x23, 0x5A, 0x9C, 0xE7, 0x23, 0x11, 0x5A, 0x9C, 0xE7, 0x23])
    aabb = b''.join(v.to_bytes(4, 'little') for v in (0x7A3C915E, 0x7A3C915E, 0x1B6DE402, 0x1B6DE402))
    check('package check: a 16-byte run of only 5 byte values, strong in each 12-byte window, is strong', len(set(five)) == 5 and not check_package.weak(five))
    check('package check: an A A B B run of 32-bit values (a counting table of 4 values) is strong', not check_package.weak(aabb))
    check('package check: the earlier rule called both weak, so they test the change', check_package.earlier_rule_blind(five)[0] and
          check_package.earlier_rule_blind(aabb)[1])
    reference = rnd.randbytes(1 << 15)
    refs = {'plain': check_package.Reference(reference), 'swapped': check_package.Reference(check_tree.swapped_pairs(reference))}
    piece = bytes(7) + b'\x05\x91\x3c\xe7\x22'      # strong as a whole, though its first 8 bytes are not
    reference = reference[:5000] + piece + reference[5012:]
    reference = reference[:6000] + five + reference[6016:]
    reference = reference[:7000] + aabb + reference[7016:]
    refs['plain'] = check_package.Reference(reference)
    for at, size in ((9001, 12), (13001, 13), (20001, 16), (6000, 16), (7000, 16)):
        data = bytearray(rnd.randbytes(1 << 12))
        data[1000:1000 + size] = reference[at:at + size]
        found = check_package.scan(bytes(data), refs)
        check('package check: a planted %d-byte run of the reference%s is found whole and strong' % (
              size, ' (5 byte values)' if at == 6000 else ' (A A B B)' if at == 7000 else ''),
              any(form == 'plain' and strong and o <= 1000 and o + n >= 1000 + size for o, n, form, strong, ct in found))
    data = bytearray(rnd.randbytes(1 << 12))
    data[1000:1012] = piece
    found = check_package.scan(bytes(data), refs)
    check('package check: a strong 12-byte run whose first 8 bytes are mostly zero is found whole, grown backwards',
          any(form == 'plain' and strong and o <= 1000 and o + n >= 1012 for o, n, form, strong, ct in found), str(found[:4]))
    text_ = ('u = {%s};\n' % ', '.join('0x%02X' % v for v in reference[24001:24013])).encode()
    check('package check: a list of 12 byte values of the reference in a text is caught', bool(check_package.text_problems('x.c', text_, refs)))
    check('package check: the compiler\'s runtime library rule takes no file outside the compiler\'s folder',
          check_package.toolchain_library(os.path.join(REPO, 'x.a') + '(y.o)') is None)


def process_alive(pid):
    """Whether a process this test started, known by its number, still runs."""
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    k32.OpenProcess.restype = ctypes.c_void_p
    h = k32.OpenProcess(0x1000, False, pid)      # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return False
    code = ctypes.c_ulong(0)
    k32.GetExitCodeProcess(ctypes.c_void_p(h), ctypes.byref(code))
    k32.CloseHandle(ctypes.c_void_p(h))
    return code.value == 259                     # STILL_ACTIVE


def stop_process(pid):
    """Ends one process this test started, known by its number (never one found by its name)."""
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    k32.OpenProcess.restype = ctypes.c_void_p
    h = k32.OpenProcess(0x0001, False, pid)      # PROCESS_TERMINATE
    if h:
        k32.TerminateProcess(ctypes.c_void_p(h), 1)
        k32.CloseHandle(ctypes.c_void_p(h))


def made_up_dlls():
    """Made-up DLLs for the game program's own checks: (what, C source, words of the refusal)."""
    # TinyCC knows __declspec from its own headers (stdio.h brings it), so a header comes first.
    head = ('#include <stdio.h>\ntypedef struct { unsigned pc; void *fn; unsigned entry; } E;\nstatic E map[1];\n'
            '__declspec(dllexport) const E *pt_game_map(void) { return map; }\n')
    full = head + ('__declspec(dllexport) unsigned pt_game_map_n(void) { return 0; }\n__declspec(dllexport) int pt_game_abi(void) { return %d; }\n'
                   '__declspec(dllexport) const char *pt_game_image_check(void) { return "%s"; }\n')
    # The version the program takes now, as runtime/pt_rt.h defines it, so this DLL fails only on its image.
    abi = int(re.search(r'#define PT_GAME_ABI (\d+)', text(os.path.join(REPO, 'runtime', 'pt_rt.h'))).group(1))
    return [('a DLL for another version of the program', full % (99, 'whatever'), 'another version of this program'),
            ('a DLL made from another image', full % (abi, '0000000000000000'), 'another copy of the game'),
            ('a DLL without the exports', head, 'not a game this program built'),
            ('text that is not a DLL at all', head, 'Windows would not load')]


def leftover_tools(pkg):
    """Processes still there that were started from the package's tools folder, by their command line: a process that was started suspended
    and never ran has no executable path to read yet, but it has its command line."""
    r = subprocess.run(['powershell', '-NoProfile', '-Command',
                        "[Console]::OutputEncoding = [Text.Encoding]::UTF8; Get-CimInstance Win32_Process | Where-Object { $_.CommandLine } | "
                        "ForEach-Object { $_.CommandLine }"], capture_output=True, text=True, encoding='utf-8', errors='replace')
    tools = os.path.normcase(os.path.join(pkg, 'tools'))
    return [x.strip()[:200] for x in r.stdout.splitlines() if tools in os.path.normcase(x)]


def main():
    tmp = sys.argv[1] if len(sys.argv) > 1 else tempfile.mkdtemp(prefix='ltest')
    os.makedirs(tmp, exist_ok=True)
    config = os.environ.get('PAPRIUM_PC_CONFIG')
    if not config:
        print('PAPRIUM_PC_CONFIG names no local configuration')
        return 2
    run(tmp, config)
    bad = [n for n, ok in RESULTS if not ok]
    print('%d checks, %d failed' % (len(RESULTS), len(bad)))
    print('RESULT: FAIL: ' + '; '.join(bad) if bad else 'RESULT: PASS')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
