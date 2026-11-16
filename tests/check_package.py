# Copyright (C) 2026 Marsupial Solutions LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks a players' package (targets/pc/pc_package.py) for pieces of the game.

    python -I -B tests/check_package.py --config LOCAL.json --package FOLDER [--maps FOLDER] [--json OUT] [--firmware required|absent|either]

Every file of the package is searched for runs of bytes equal to the ROM, as the ROM's file holds them and with each 16-bit pair swapped, the
order a little-endian host keeps it in:
  - raw windows of 8 bytes at every offset of every file; each found window is grown, forwards and backwards, to the whole run that is still
    equal, and the runs are counted at 8, 10, 12 and 16 bytes and more. In our files every window with at least 3 different byte values (once
    the zero bytes at its ends are left out) starts a run, which leaves no strong run of 12 bytes unseen (measured on 36,000 strong windows of the
    ROM); in third-party files, which cannot fail, only windows that are strong themselves start one, to keep the scan quick.
  - Each run is called weak or strong as the firmware leak guard judges the data in a program (its tools/leakguard.py, strong_bytes, on
    windows of data_window_bytes = 12, with the numbers of its leakguard.json): every window of 12 bytes of the run is judged by itself (a run
    shorter than 12 bytes is one window), and the run is strong when any window is. A window is strong when, with the zero bytes at either end
    left out, it has at least 5 different byte values and the steps from byte to byte take at least 3 values. We add two exceptions of our
    own, stated here, for counting tables, which are in every program (a program's table of export numbers, the straight parts of generated
    curves): a window that, read as 16-bit values in either byte order and from either starting byte, has at least 4 values whose steps take
    at most 2 values is not strong; and a whole run of 32 bytes or more that, read as 32-bit values the same way, has at least 8 such values
    (the guard's own count of values for a table of wide values) is weak.
    A run is also marked when one of its 16-byte windows passes tests/check_tree.py's own rule (worth_looking_up).
  - in every text file, all the forms tests/check_tree.py looks for at 16 bytes (hexadecimal text, lists of numbers, string escapes), and, as
    the firmware leak guard does for byte values, lists of 12 byte values or more and hexadecimal text of 12 bytes or more, each looked up as
    above. Lists of 16-bit and 32-bit values stay at check_tree's 8 and 4 values, the guard's own 8 values.
The files fall in two kinds. Third-party files byte-equal to a member of their publisher's archive (Python, NumPy, the TinyCC source and the
headers and .def files the TinyCC build copies from it), to SDL2.dll, or to the sound cores', the picture filters' or the controller
database's pinned files: their counts are reported, and they cannot fail, as they were made before and without the game. Little-Man Firmware
1.0's file is counted the same way, in its one place only (see below). Everything else is ours, TinyCC's tcc.exe, libtcc.dll and runtime library
too, as we build them from TinyCC's source. In our programs (OUR_PROGRAMS) each strong run of 12 bytes or more is named through the linker map (from
--maps), and passes only in two cases: inside a table the firmware's tools/leak_allow.json allows, checked by its values' hash, with zero bytes
beside it (the team's decisions, written in that file); or inside what the linker took from the compiler's own runtime
libraries (the map names the library and its member), when the run's bytes, less the zero bytes at its ends, are found in that library's file as
it is on this PC: C runtime data made before and without the game. Any other is a failure. Our programs must also carry no symbol table (the
team's decision). In our other files any strong run of 12 bytes or more, and any text form, is a failure. A positive control plants ROM
windows of 16 and of 12 bytes, plain and swapped, in a made-up buffer, and 16-byte ROM runs of the two kinds this check's earlier rule called
weak (5 byte values only, and 32-bit counting tables of 4 values such as A A B B), and a made-up table as text in 16 and in 12 byte values, and
must find each.
CPU MATE's brain is in a package only inside Little-Man Firmware 1.0's own file: little_man_firmware.bin beside Paprium.exe, at the package's top
level, 256 KB, with exactly the MD5 and the SHA-256 the Paprium core 0.3.0's release published for it (targets/pc/pc_package.py, FIRMWARE_PINS).
The game program reads the brain from it when a player turns CPU MATE on (targets/pc/pc_brain.h). That one file is the release's own, byte for
byte, and it passed the firmware's own leak check in memory order when that release was made: here its runs are counted and reported, as a
third-party file's are, and cannot fail, and the rules below pass over it. With --firmware required the package must hold it. Any other file
that holds the brain is refused:
  - by name: little_man_firmware*.bin anywhere else, or beside Paprium.exe when it is not that published file (another build, a cut, doubled or
    changed copy), and brain.bin, cf.bin or rodata.bin, the brain's three parts as the firmware's build takes them;
  - by its first word: Little-Man Firmware's "FAFR", as the file stores it (each 16-bit pair swapped) or in the order the cart's MCU reads it;
  - by its bytes, when the local configuration names a brain reference (brain_reference: a little_man_firmware.bin, read as pc_brain.c reads it,
    or a folder of the three parts): any run of 16 bytes or more equal to a part, as the part is and with each 16-bit pair swapped, that holds a
    strong window (as above), in any file. A positive control plants such runs in made-up bytes and must find each. The reference must be the
    brain of Little-Man Firmware 1.0 by the SHA-256 pins targets/pc/pc_brain.c holds, or the check fails: a wrong file there would let the real
    brain through unseen. Only the tests' own made-up firmware files are taken without the pins, when their configuration says
    brain_reference_made_up; and only they are taken as the published file by other hashes, when their configuration names those hashes in
    firmware_pins_made_up (the report then says so).
This check does not look inside archives, so an archive (zip, 7z, RAR, gzip, xz, bzip2 or cabinet, known by its first bytes) is refused unless it
is one of the pinned third-party files above: whatever it holds could not be seen.
--firmware: required, the package must hold the published firmware file beside Paprium.exe (a release); absent, it must hold none (a package
made with --no-firmware); either, the default, takes both. The firmware file's two hashes are printed beside the published ones either way.
No byte of the ROM or of the brain is printed: offsets, lengths, counts and names only. Exit 0 with RESULT: PASS, or 1.
"""
import argparse
import bisect
import hashlib
import json
import os
import random
import re
import struct
import sys
import tarfile
import zipfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, REPO)
sys.path.insert(0, HERE)

import check_tree  # noqa: E402
from targets.pc import pc_build, pc_package  # noqa: E402

TEXT_EXT = {'.py', '.txt', '.md', '.json', '.h', '.c', '.def', '.cfg', '._pth', '.manifest', '.html', '.htm', '.css', '.rst', '.toml', '.cmake',
            '.in', '.pxd', '.cfg'}
ROM_EXT = {'.bin', '.gen', '.smd', '.rom', '.sav', '.srm'}     # .md is a game file's name too, but every licence folder has one: the size rule covers it
# Our programs, with their linker maps' names in the --maps folder. TinyCC's two programs are built by us from its source archive
# (targets/pc/pc_tinycc.py), so they are ours here too.
OUR_PROGRAMS = {'Paprium.exe': 'Paprium.exe.map', 'pc_port.exe': 'pc_port.exe.map', 'tools/pc_boot_image.exe': 'pc_boot_image.exe.map',
                'tools/tcc/tcc.exe': 'tcc.exe.map', 'tools/tcc/libtcc.dll': 'libtcc.dll.map'}


TOOLCHAIN = os.path.dirname(os.path.dirname(pc_build.GCC))    # the compiler's own folder, whose lib folders hold its runtime libraries
TEXT_BYTES = 12                                                # the firmware leak guard's values_u8: a list of byte values is looked up from 12


WINDOW = 12             # the firmware leak guard's data window: leakguard.json's data_window_bytes
MIN_VALUES = 5          # its min_distinct_bytes_u8, the number for a window of 12 bytes
MIN_STEPS = 3           # its min_distinct_steps


def counting_table(w, size, least):
    """w read as values of size bytes, either byte order, from any starting byte: at least `least` values whose steps take at most 2 values."""
    mask = (1 << (8 * size)) - 1
    for order in ('big', 'little'):
        for start in range(size):
            vals = [int.from_bytes(w[i:i + size], order) for i in range(start, len(w) - size + 1, size)]
            if len(vals) >= least and len({(vals[i + 1] - vals[i]) & mask for i in range(len(vals) - 1)}) <= 2:
                return True
    return False


def strong_window(w):
    """One window of at most 12 bytes, by the firmware leak guard's strong_bytes rule with its 12-byte numbers: once the zero bytes at either end
    are left out, at least 5 different byte values and at least 3 different steps from byte to byte. And not a 16-bit counting table (our
    exception, see the top of this file): at least 4 16-bit values whose steps take at most 2 values."""
    s = w.strip(b'\0')
    if len(set(s)) < MIN_VALUES or len({s[i + 1] - s[i] for i in range(len(s) - 1)}) < MIN_STEPS:
        return False
    return not counting_table(w, 2, 4)


def weak(w):
    """A run that says nothing about where it came from: no window of 12 bytes in it is strong (strong_window; a run shorter than 12 bytes is
    one window), or the whole run is a 32-bit counting table of at least 8 values, the guard's own count of values for a table of wide
    values."""
    if len(w) >= 32 and counting_table(w, 4, 8):
        return True
    if len(w) <= WINDOW:
        return not strong_window(w)
    return not any(strong_window(w[i:i + WINDOW]) for i in range(len(w) - WINDOW + 1))


def seed(w, light):
    """Whether an 8-byte window equal to the ROM starts a run: in our files (light) any with 3 byte values or more, else only a strong one."""
    return len(set(w.strip(b'\0'))) >= 3 if light else not weak(w)


def keys8(buf):
    a = np.frombuffer(buf, dtype=np.uint8)
    n = len(a) - 7
    if n <= 0:
        return np.zeros(0, dtype=np.uint64)
    k = np.zeros(n, dtype=np.uint64)
    for j in range(8):
        k |= a[j:j + n].astype(np.uint64) << np.uint64(56 - 8 * j)
    return k


class Reference:
    """One form of the ROM, with its 8-byte keys sorted, so a window's places in it are found by a binary search."""

    def __init__(self, data):
        self.data = data
        k = keys8(data)
        self.order = np.argsort(k, kind='stable')
        self.keys = k[self.order]

    def runs(self, data, light=True):
        """The runs of 8 bytes or more of data equal to this form that hold an 8-byte window that starts a run (seed()), each grown forwards
        and backwards to its whole length: (offset, length); a run inside another is dropped. And how many 8-byte windows found did not start
        one."""
        k = keys8(data)
        if not len(k):
            return [], 0
        lo = np.searchsorted(self.keys, k, 'left')
        hi = np.searchsorted(self.keys, k, 'right')
        hits = np.nonzero(hi > lo)[0].tolist()
        out, covered, weak_windows = [], -1, 0
        for o in hits:
            if o < covered:
                continue
            if not seed(data[o:o + 8], light):
                weak_windows += 1
                continue
            start, end = o, o + 8
            for i in range(lo[o], min(hi[o], lo[o] + 32)):
                p = int(self.order[i])
                n = 8
                while o + n < len(data) and p + n < len(self.data) and data[o + n] == self.data[p + n] and n < 4096:
                    n += 1
                b = 0
                while o - b > 0 and p - b > 0 and data[o - b - 1] == self.data[p - b - 1] and b < 4096:
                    b += 1
                if n + b > end - start:
                    start, end = o - b, o + n
            out.append((start, end - start))
            covered = end - 7
        out.sort(key=lambda r: (r[0], -r[1]))
        kept = []
        for s, n in out:
            if kept and s + n <= kept[-1][0] + kept[-1][1]:
                continue
            kept.append((s, n))
        return kept, weak_windows


def check_tree_window(run):
    return any(check_tree.worth_looking_up(run[i:i + 16]) for i in range(len(run) - 15))


def scan(data, refs, weak_counts=None, light=True):
    """The runs in one file: a list of (offset, length, form, strong, check_tree's rule). weak_counts, a dict, gets the windows that started no
    run, by form. light: our file (see seed())."""
    found = []
    for form, ref in refs.items():
        runs, weak_windows = ref.runs(data, light)
        if weak_counts is not None:
            weak_counts['weak_window_8_' + form] = weak_windows
        for o, n in runs:
            run = data[o:o + n]
            found.append((o, n, form, not weak(run), n >= 16 and check_tree_window(run)))
    return found


def counts(found, weak_counts):
    c = {k: v for k, v in weak_counts.items() if v}
    for o, n, form, strong, ct in found:
        for k in (8, 10, 12, 16):
            if n >= k:
                key = '%s_%d+_%s' % ('strong' if strong else 'weak', k, form)
                c[key] = c.get(key, 0) + 1
        if ct:
            c['check_tree_16_' + form] = c.get('check_tree_16_' + form, 0) + 1
    return c


# ---- our programs: name a run by the linker map, and allow only the firmware's allowed tables
class Program:
    def __init__(self, data, map_path):
        pe = struct.unpack_from('<I', data, 0x3C)[0]
        nsec = struct.unpack_from('<H', data, pe + 6)[0]
        opt = struct.unpack_from('<H', data, pe + 20)[0]
        self.base = struct.unpack_from('<Q', data, pe + 24 + 24)[0]
        self.symbol_records = struct.unpack_from('<I', data, pe + 16)[0] if struct.unpack_from('<I', data, pe + 12)[0] else 0
        self.sections = []
        for i in range(nsec):
            s = pe + 24 + opt + 40 * i
            name = data[s:s + 8].rstrip(b'\0').decode('ascii', 'replace')
            vsize, va, rsize, rptr = struct.unpack_from('<IIII', data, s + 8)
            self.sections.append((rptr, rptr + rsize, va, name))
        self.symbols = []
        self.inputs = []        # what the linker put where: (first address, end address, the object's path as the map names it)
        if map_path and os.path.exists(map_path):
            obj, pending = '', None
            for line in open(map_path, errors='replace'):
                line = line.rstrip('\r\n')
                # An input section on one line: ' .rdata  0xADDRESS  0xSIZE  path'; a long section name puts the rest on the next line.
                m = re.match(r'^ (\S+)\s+0x([0-9a-fA-F]{16})\s+0x([0-9a-fA-F]+)\s+(\S.*)$', line)
                if not m and pending:
                    m2 = re.match(r'^\s+0x([0-9a-fA-F]{16})\s+0x([0-9a-fA-F]+)\s+(\S.*)$', line)
                    if m2:
                        m = (pending, m2.group(1), m2.group(2), m2.group(3))
                elif m:
                    m = m.groups()
                pending = None
                if m:
                    address, size, path = int(m[1], 16), int(m[2], 16), m[3].strip()
                    obj = path.replace('\\', '/').split('/')[-1]
                    self.symbols.append((address, '[object %s]' % obj))
                    if size:
                        self.inputs.append((address, address + size, path))
                    continue
                m = re.match(r'^ (\S+)\s*$', line)
                if m and not m.group(1).startswith('*'):
                    pending = m.group(1)
                    continue
                m = re.match(r'^\s+0x([0-9a-fA-F]{16})\s+(\S+)\s*$', line)
                if m and not m.group(2).startswith('0x'):
                    self.symbols.append((int(m.group(1), 16), '%s (%s)' % (m.group(2), obj)))
        self.symbols.sort()
        self.addresses = [a for a, _ in self.symbols]
        self.inputs.sort()
        self.input_starts = [a for a, _, _ in self.inputs]

    def input_at(self, offset):
        """The object path the linker took the byte at this file offset from, or None."""
        a, _ = self.address(offset)
        if a is None:
            return None
        i = bisect.bisect_right(self.input_starts, a) - 1
        if i >= 0 and self.inputs[i][0] <= a < self.inputs[i][1]:
            return self.inputs[i][2]
        return None

    def address(self, offset):
        for lo, hi, va, name in self.sections:
            if lo <= offset < hi:
                return self.base + va + (offset - lo), name
        return None, 'headers'

    def offset(self, address):
        for lo, hi, va, name in self.sections:
            if self.base + va <= address < self.base + va + (hi - lo):
                return lo + address - self.base - va
        return None

    def name(self, offset):
        a, section = self.address(offset)
        if a is None or not self.symbols:
            return section, '?'
        i = bisect.bisect_right(self.addresses, a) - 1
        return section, self.symbols[i][1] if i >= 0 else '?'

    def symbol_address(self, symbol):
        for a, name in self.symbols:
            if name.split(' ')[0] == symbol:
                return a
        return None


def allowed_objects(program, data, allow):
    """The byte ranges of the allowed tables in one program: (start, end) file offsets, each checked against its entry's count and values hash."""
    out = []
    for e in allow:
        a = program.symbol_address(e['symbol'])
        if a is None:
            continue
        off = program.offset(a)
        size = {'u8': 1, 'u16': 2, 'u32': 4}[e['elem']] * e['count']
        if off is None or hashlib.sha256(data[off:off + size]).hexdigest() != e['values_sha256']:
            continue
        out.append((off, off + size))
    return out


def inside_allowed(o, n, data, ranges):
    if not ranges:
        return False
    touches = any(lo < o + n and o < hi for lo, hi in ranges)
    return touches and all(any(lo <= i < hi for lo, hi in ranges) or data[i] == 0 for i in range(o, o + n))


_library_bytes = {}


def toolchain_library(path):
    """The library file of a map's 'LIBRARY.a(MEMBER.o)' when the library is one of the compiler's own (under TOOLCHAIN), or None."""
    m = re.match(r'^(.*\.a)\((.*)\)$', path or '')
    if not m:
        return None
    lib = os.path.normcase(os.path.realpath(m.group(1)))
    root = os.path.normcase(os.path.realpath(TOOLCHAIN))
    try:
        if os.path.commonpath([lib, root]) != root:
            return None
    except ValueError:
        return None
    return lib if os.path.isfile(lib) else None


def from_toolchain(program, o, n, data):
    """Whether a run is the compiler's runtime library data: every byte of it that is not zero lies in what the linker took from one of the
    compiler's own libraries, and the run, less the zero bytes at its ends, is found in that library's file. Returns the member's name or None."""
    libs = {}
    for i in range(o, o + n):
        if data[i] == 0:
            continue
        path = program.input_at(i)
        lib = toolchain_library(path)
        if not lib:
            return None
        libs[lib] = path
    piece = data[o:o + n].strip(b'\0')
    for lib, path in libs.items():
        if lib not in _library_bytes:
            with open(lib, 'rb') as f:
                _library_bytes[lib] = f.read()
        if piece and piece in _library_bytes[lib]:
            return path.replace('\\', '/').split('/')[-1]
    return None


# ---- third-party files: byte-equal to a member of a pinned archive, or to another pinned file
def third_party_hashes(cfg):
    known = {}
    tools = cfg.get('player_tools') or {}
    for key, (name, want, _) in pc_package.TOOL_PINS.items():
        path = tools.get(key)
        if not path or not os.path.exists(path):
            continue
        known[pc_package.sha256(path)] = name
        if path.endswith(('.zip', '.whl')):
            with zipfile.ZipFile(path) as z:
                for info in z.infolist():
                    if not info.filename.endswith('/'):
                        known[hashlib.sha256(z.read(info)).hexdigest()] = '%s: %s' % (name, info.filename)
        elif path.endswith('.tar.bz2'):
            with tarfile.open(path, 'r:bz2') as t:
                for m in t.getmembers():
                    if m.isfile():
                        known[hashlib.sha256(t.extractfile(m).read()).hexdigest()] = '%s: %s' % (name, m.name)
    sdl = cfg.get('sdl_dir')
    if sdl:
        for p in (pc_build.sdl_dll(sdl), pc_package.sdl_licence(sdl)):
            known[pc_package.sha256(p)] = 'SDL2: ' + os.path.basename(p)
    for name, files in pc_build.SOUND_CORE_PINS.items():
        for rel, want in files.items():
            known[want] = 'sound core %s: %s' % (name, rel)
    for name, want in pc_build.SHADER_PINS.items():
        known[want] = 'picture filter shader ' + name
    for rel, want in pc_build.CONTROLLER_DB_PINS.items():
        known[want] = 'SDL_GameControllerDB: ' + rel
    lic = pc_package.MSYS_LICENCES
    for rel in ('crt/COPYING.MinGW-w64-runtime.txt', 'gcc-libs/COPYING.RUNTIME'):
        p = os.path.join(lic, *rel.split('/'))
        if os.path.exists(p):
            known[pc_package.sha256(p)] = 'MSYS2 licence text ' + rel
    return known


def text_runs(data):
    """The byte runs a text holds as lists of 12 byte values or more, or as hexadecimal text of 12 bytes or more (check_tree's own forms take
    16). check_tree's number reader is used, so a number reads the same in both checks."""
    for m in re.finditer(rb'[0-9a-fA-F]{%d,}' % (2 * TEXT_BYTES), data):
        t = m.group(0)
        for s in (t, t[1:]):
            yield bytes.fromhex(s[:len(s) & ~1].decode())
    for m in re.finditer(rb'(?:[0-9a-fA-F]{2}[\s,:_-]+){%d,}[0-9a-fA-F]{2}' % (TEXT_BYTES - 1), data):
        yield bytes.fromhex(re.sub(rb'[^0-9a-fA-F]', b'', m.group(0)).decode())
    for text in (data, check_tree._COMMENT.sub(b' ', data)):
        yield from check_tree._runs_of(check_tree._numbers(text), lambda v: v[0] <= 0xFF, TEXT_BYTES, bytes)


def text_problems(name, data, refs):
    """Strong runs of 12 bytes or more, equal to the ROM, in the short text forms of text_runs()."""
    out = []
    for run in text_runs(data):
        for o, n, form, strong, ct in scan(run, refs):
            if strong and n >= TEXT_BYTES:
                out.append('%s: a list of byte values or hexadecimal text holds %d bytes equal to the ROM (%s)' % (name, n, form))
                return out
    return out


def earlier_rule_blind(w):
    """What this check's earlier rule called weak although the guard's 12-byte windows call it strong: a run of more than 12 bytes with only
    5 byte values (the earlier rule asked 6 of the whole run), or a run that reads as a 32-bit counting table of only 4 to 7 values, such as
    A A B B. Used only to pick the positive control's runs of those two kinds."""
    s = w.strip(b'\0')
    five = len(w) > 12 and len(set(s)) == 5
    return five, counting_table(w, 4, 4) and not counting_table(w, 4, 8)


def control(rom, refs):
    """Plants 8 strong ROM windows of 16 bytes and 8 of 12 bytes, each plain and swapped, in made-up bytes, and a made-up table as text of 16
    and of 12 byte values; each must be found. A window is picked only for being strong as a whole, so the scan's own seeding is tested.
    Two more kinds are planted, plain and swapped, 4 of each, from the ROM: runs of 16 bytes with only 5 byte values, and runs of 16 bytes that
    read as 32-bit counting tables of 4 values (A A B B and the like). The earlier rule of this check called both weak; each must be found
    strong. Returns (raw found, text found, how many of the two kinds were planted)."""
    rnd = random.Random(5)
    picks = []
    for size in (16, 12):
        n = 0
        while n < 8:
            p = rnd.randrange(0, len(rom) - size) & ~1
            w = rom[p:p + size]
            if not weak(w) and not weak(check_tree.swapped_pairs(w)):
                picks.append((p, size))
                n += 1
    kinds = {'five': [], 'u32': []}
    tries = 0
    while (len(kinds['five']) < 4 or len(kinds['u32']) < 4) and tries < 1 << 21:
        tries += 1
        p = rnd.randrange(0, len(rom) - 16) & ~1
        w = rom[p:p + 16]
        five, u32 = earlier_rule_blind(w)
        if (five or u32) and not weak(w) and not weak(check_tree.swapped_pairs(w)):
            k = 'five' if five else 'u32'
            if len(kinds[k]) < 4:
                kinds[k].append((p, 16))
    picks += kinds['five'] + kinds['u32']
    buf = bytearray(rnd.getrandbits(8) for _ in range(1 << 17))
    places = []
    for i, (p, size) in enumerate(picks):
        w = rom[p:p + size]
        for form, piece in (('plain', w), ('swapped', check_tree.swapped_pairs(w))):
            at = 2048 * (2 * i + (form == 'swapped')) + 1000
            buf[at:at + size] = piece
            places.append((at, size, form))
    found = scan(bytes(buf), refs)
    raw_ok = all(any(f == form and strong and o <= at and o + n >= at + size for o, n, f, strong, ct in found) for at, size, form in places)
    raw_ok = raw_ok and len(kinds['five']) == 4 and len(kinds['u32']) == 4
    p16, p12 = picks[0][0], picks[8][0]
    text = ('/* made up */\nstatic const unsigned char t[] = {%s};\n' % ', '.join('0x%02X' % b for b in rom[p16:p16 + 16])).encode()
    text12 = ('/* made up */\nstatic const unsigned char u[] = {%s};\n' % ', '.join('0x%02X' % b for b in rom[p12:p12 + 12])).encode()
    text_ok = bool(check_tree.rom_problems({'control.c': text}, rom)) and bool(text_problems('control12.c', text12, refs))
    return raw_ok, text_ok, {'5-value runs': len(kinds['five']), '32-bit counting runs': len(kinds['u32'])}


# ---- CPU MATE's brain (see the top of this file)
BRAIN_NAMES = ('brain.bin', 'cf.bin', 'rodata.bin')
FIRMWARE_FIRST_WORDS = (b'FAFR', b'AFRF')        # as the cart's MCU reads it, and as the file stores it
# The first bytes of the archive kinds this check does not look inside.
ARCHIVE_FIRST_BYTES = ((b'PK\x03\x04', 'zip'), (b'PK\x05\x06', 'zip'), (b'PK\x07\x08', 'zip'), (b'7z\xbc\xaf\x27\x1c', '7z'), (b'Rar!\x1a\x07', 'RAR'),
                       (b'\x1f\x8b', 'gzip'), (b'\xfd7zXZ\x00', 'xz'), (b'MSCF', 'cabinet'))


def archive_kind(data):
    """The kind of archive a file is, by its first bytes, or None."""
    for first, kind in ARCHIVE_FIRST_BYTES:
        if data.startswith(first):
            return kind
    if data[:3] == b'BZh' and data[3:4].isdigit() and data[4:10] == b'1AY&SY':
        return 'bzip2'
    return None
FIRMWARE_BYTES = 0x40000                          # Little-Man Firmware's file, 256 KB; not a game address
BRAIN_RUN = 16


def brain_parts(path):
    """The brain's three parts from a brain reference: a folder of brain.bin, cf.bin and rodata.bin, or a little_man_firmware.bin, read as
    targets/pc/pc_brain.c reads it (each 16-bit pair of the file swapped back; the code at 0x5360-0xD2EB of the MCU's instruction memory, which the
    file holds from its byte 0x80; the helpers and the tables of its work RAM from 0x2C00 and 0x4110, which the file holds from its byte
    0x16080, a place in the file, not a game address)."""
    if os.path.isdir(path):
        parts = []
        for name in BRAIN_NAMES:
            with open(os.path.join(path, name), 'rb') as f:
                parts.append(f.read())
        return parts
    with open(path, 'rb') as f:
        disk = f.read()
    mcu = check_tree.swapped_pairs(disk)
    if len(disk) != FIRMWARE_BYTES or mcu[:4] != b'FAFR':
        raise SystemExit('the brain reference %s is not a Little-Man Firmware file' % path)
    work = 0x16080 - 0x2C00                                     # where the file holds the MCU's work RAM byte 0; not a game address
    return [mcu[0x80 + 0x5360:0x80 + 0xD2EC], mcu[work + 0x2C00:work + 0x2C00 + 5328], mcu[work + 0x4110:work + 0x4110 + 1336]]


def brain_pins_match(parts):
    """Whether the parts are the brain of Little-Man Firmware 1.0, by the SHA-256 pins targets/pc/pc_brain.c holds."""
    with open(os.path.join(REPO, 'targets', 'pc', 'pc_brain.c'), encoding='utf-8') as f:
        pins = re.findall(r'PIN_(?:CODE|HELPERS|TABLES) = "([0-9a-f]{64})"', f.read())
    return len(pins) == 3 and [hashlib.sha256(p).hexdigest() for p in parts] == pins


def brain_references(parts):
    """The brain's parts as one reference, each part apart from the next by bytes no part is compared across, as it is and swapped."""
    gap = b'\xA5\x5A' * 8
    whole = gap.join(parts)
    return {'brain': Reference(whole), 'brain swapped': Reference(check_tree.swapped_pairs(whole))}


def brain_runs(data, refs):
    """Runs of BRAIN_RUN bytes or more equal to the brain that hold a strong window: (offset, length, form)."""
    return [(o, n, form) for o, n, form, strong, ct in scan(data, refs) if strong and n >= BRAIN_RUN]


def firmware_hashes(data):
    return hashlib.md5(data).hexdigest(), hashlib.sha256(data).hexdigest()


def is_published_firmware(rel, data, pins=None):
    """Whether this file is Little-Man Firmware 1.0's file as its release published it, in its one place: little_man_firmware.bin at the package's top
    level, beside Paprium.exe, with the published length, MD5 and SHA-256 (pc_package.FIRMWARE_PINS; pins, (MD5, SHA-256), for a test's made-up
    file only). Any other file, or this one anywhere else, is not."""
    md5, sha = pins or (pc_package.FIRMWARE_PINS['md5'], pc_package.FIRMWARE_PINS['sha256'])
    return rel == pc_package.FIRMWARE_FILE and len(data) == pc_package.FIRMWARE_BYTES and firmware_hashes(data) == (md5, sha)


def brain_control(parts, refs):
    """Plants 8 strong windows of 16 bytes of each part, as they are and swapped, in made-up bytes; each must be found."""
    rnd = random.Random(7)
    buf = bytearray(rnd.getrandbits(8) for _ in range(1 << 16))
    places, i = [], 0
    for part in parts:
        picked = tries = 0
        while picked < 8 and len(part) > BRAIN_RUN and tries < 1 << 17:
            tries += 1
            p = rnd.randrange(0, len(part) - BRAIN_RUN) & ~1
            w = part[p:p + BRAIN_RUN]
            if weak(w) or weak(check_tree.swapped_pairs(w)):
                continue
            for form, piece in (('brain', w), ('brain swapped', check_tree.swapped_pairs(w))):
                at = 512 * i + 100
                buf[at:at + BRAIN_RUN] = piece
                places.append((at, form))
                i += 1
            picked += 1
    found = brain_runs(bytes(buf), refs)
    return len(places) == 48 and all(any(f == form and o <= at and o + n >= at + BRAIN_RUN for o, n, f in found) for at, form in places)


def main(argv=None):
    p = argparse.ArgumentParser()
    p.add_argument('--config', required=True)
    p.add_argument('--package', required=True)
    p.add_argument('--maps', default='')
    p.add_argument('--json')
    p.add_argument('--firmware', choices=('required', 'absent', 'either'), default='either')
    args = p.parse_args(argv)
    with open(args.config, encoding='utf-8') as f:
        cfg = json.load(f)
    made_up_pins = tuple(cfg['firmware_pins_made_up']) if cfg.get('firmware_pins_made_up') else None
    fw_pins = made_up_pins or (pc_package.FIRMWARE_PINS['md5'], pc_package.FIRMWARE_PINS['sha256'])
    with open(cfg['rom_path'], 'rb') as f:
        rom = f.read()
    refs = {'plain': Reference(rom), 'swapped': Reference(check_tree.swapped_pairs(rom))}
    allow = []
    fw = cfg.get('fw_source')
    if fw and os.path.exists(os.path.join(fw, 'tools', 'leak_allow.json')):
        with open(os.path.join(fw, 'tools', 'leak_allow.json'), encoding='utf-8') as f:
            allow = json.load(f)['entries']
    known = third_party_hashes(cfg)
    problems, report = [], {'files': {}, 'third_party_files': 0, 'published_firmware_files': 0, 'our_files': 0}
    raw_ok, text_ok, kinds = control(rom, refs)
    report['control'] = {'raw windows found': raw_ok, 'text form found': text_ok, 'planted runs the earlier rule missed': kinds}
    if not (raw_ok and text_ok):
        problems.append('the positive control was not found: raw %s, text %s' % (raw_ok, text_ok))
    brain_refs = None
    if cfg.get('brain_reference'):
        parts = brain_parts(cfg['brain_reference'])
        brain_refs = brain_references(parts)
        brain_ok = brain_control(parts, brain_refs)
        pins = brain_pins_match(parts)
        made_up = bool(cfg.get('brain_reference_made_up'))
        report['brain'] = {'reference': 'named', 'parts bytes': [len(x) for x in parts], 'Little-Man Firmware 1.0 brain by its pins': pins,
                           'control found': brain_ok}
        if made_up:
            report['brain']['reference'] = 'named: a test\'s made-up firmware file, taken without the pins'
        if not brain_ok:
            problems.append('the brain\'s positive control was not found')
        if not pins and not made_up:
            problems.append('the brain reference %s is not the brain of Little-Man Firmware 1.0 by the pins in targets/pc/pc_brain.c, so the brain players '
                            'have was not looked for' % cfg['brain_reference'])
    else:
        report['brain'] = {'reference': 'none named: the brain was looked for by its file names and its first word only'}
    pkg = os.path.abspath(args.package)
    report['firmware'] = {'published MD5': fw_pins[0], 'published SHA-256': fw_pins[1], 'beside Paprium.exe': None}
    if made_up_pins:
        report['firmware']['pins'] = 'a test\'s made-up hashes (firmware_pins_made_up), not the published ones'
    for root, dirs, files in os.walk(pkg):
        for name in sorted(files):
            path = os.path.join(root, name)
            rel = os.path.relpath(path, pkg).replace('\\', '/')
            with open(path, 'rb') as f:
                data = f.read()
            digest = hashlib.sha256(data).hexdigest()
            entry = {'bytes': len(data)}
            if rel == pc_package.FIRMWARE_FILE:
                md5, sha = firmware_hashes(data)
                report['firmware']['beside Paprium.exe'] = {'bytes': len(data), 'MD5': md5, 'SHA-256': sha}
            published = is_published_firmware(rel, data, fw_pins)
            if published:
                report['firmware']['equal to the published file'] = True
            if rel.split('/')[0] == 'my_copy':
                problems.append('%s: a player\'s folder is in the package' % rel)
            if len(data) == len(rom) or (os.path.splitext(name)[1].lower() in ROM_EXT and not published):
                problems.append('%s: the size or the name of a game file' % rel)
            if digest == hashlib.sha256(rom).hexdigest():
                problems.append('%s: IS the ROM' % rel)
            if rel == pc_package.FIRMWARE_FILE and not published:
                problems.append('%s: a firmware file beside Paprium.exe that is not Little-Man Firmware 1.0 as published (%d bytes, MD5 %s, SHA-256 %s)'
                                % (rel, len(data), md5, sha))
            if not published and (name.lower().startswith('little_man_firmware') or name.lower() in BRAIN_NAMES):
                problems.append('%s: a CPU MATE brain file, by its name' % rel)
            if not published and data[:4] in FIRMWARE_FIRST_WORDS:
                problems.append('%s: a Little-Man Firmware file, by its first word' % rel)
            # This check does not look inside archives, so the only archives a package may hold are the pinned third-party ones (Python's, TinyCC's
            # source), byte-equal to their publishers' files. Any other archive is refused, whatever it holds.
            if digest not in known and not published and archive_kind(data):
                problems.append('%s: a %s archive that is not one of the pinned third-party files; this check does not look inside archives'
                                % (rel, archive_kind(data)))
            if brain_refs and not published:
                found_brain = brain_runs(data, brain_refs)
                entry['brain_runs_16_plus'] = len(found_brain)
                for o, n, form in found_brain[:3]:
                    problems.append('%s: %d bytes of CPU MATE\'s brain (%s) at offset %d' % (rel, n, form, o))
            weak_counts = {}
            ours = digest not in known and not published
            found = scan(data, refs, weak_counts, light=ours)
            entry['runs'] = counts(found, weak_counts)
            if published:
                entry['kind'] = 'published firmware: Little-Man Firmware 1.0, %s as its release published it' % pc_package.FIRMWARE_FILE
                report['published_firmware_files'] += 1
            elif not ours:
                entry['kind'] = 'third party: ' + known[digest]
                report['third_party_files'] += 1
            else:
                entry['kind'] = 'ours'
                report['our_files'] += 1
                if rel in OUR_PROGRAMS:
                    prog = Program(data, os.path.join(args.maps, OUR_PROGRAMS[rel]) if args.maps else '')
                    # The team's decision: the programs we ship carry no symbol table.
                    entry['symbol table'] = prog.symbol_records
                    if prog.symbol_records:
                        problems.append('%s: still holds a symbol table of %d records' % (rel, prog.symbol_records))
                    ranges = allowed_objects(prog, data, allow)
                    strong = []
                    for o, n, form, is_strong, ct in found:
                        if not is_strong or n < 12:
                            continue
                        section, symbol = prog.name(o)
                        ok = inside_allowed(o, n, data, ranges)
                        library = None if ok else from_toolchain(prog, o, n, data)
                        where = {'in a table': sum(1 for i in range(o, o + n) if any(lo <= i < hi for lo, hi in ranges)),
                                 'zero outside': sum(1 for i in range(o, o + n) if data[i] == 0 and not any(lo <= i < hi for lo, hi in ranges))}
                        strong.append({'offset': o, 'bytes': n, 'form': form, 'section': section, 'symbol': symbol, 'allowed table': ok,
                                       'compiler runtime library': library, 'where': where})
                        if not ok and not library:
                            problems.append('%s: a strong run of %d bytes equal to the ROM (%s) at offset %d, %s %s' % (rel, n, form, o, section, symbol))
                    entry['strong_12_plus'] = strong
                    entry['allowed_tables_checked'] = len(ranges)
                    entry['map_inputs'] = len(prog.inputs)
                else:
                    for o, n, form, is_strong, ct in found:
                        if is_strong and n >= 12:
                            problems.append('%s: a strong run of %d bytes equal to the ROM (%s) at offset %d' % (rel, n, form, o))
                if os.path.splitext(name)[1].lower() in TEXT_EXT or name.endswith('._pth'):
                    for x in check_tree.rom_problems({rel: data}, rom) + text_problems(rel, data, refs):
                        problems.append(x)
                # The ROM's hash is rule data in exactly two places: the rules file, and the game program, which checks the game file against it.
                if cfg.get('rom_sha256') and cfg['rom_sha256'].lower().encode() in data.lower():
                    expected = rel in ('tools/rules.json', pc_build.GAME_EXE)
                    entry['names the ROM hash'] = 'yes, as rule data' if expected else 'NOT EXPECTED'
                    if not expected:
                        problems.append('%s: names the ROM hash' % rel)
            report['files'][rel] = entry
    if args.firmware == 'required' and not report['firmware'].get('equal to the published file'):
        problems.append('no %s as published beside Paprium.exe, and this package must hold it (--firmware required)' % pc_package.FIRMWARE_FILE)
    if args.firmware == 'absent' and report['firmware']['beside Paprium.exe']:
        problems.append('%s beside Paprium.exe, and this package must hold none (--firmware absent)' % pc_package.FIRMWARE_FILE)
    totals = {}
    for e in report['files'].values():
        kind = 'third_party' if e['kind'].startswith('third') else 'firmware' if e['kind'].startswith('published') else 'ours'
        for k, v in e['runs'].items():
            totals['%s %s' % (kind, k)] = totals.get('%s %s' % (kind, k), 0) + v
    report['totals'] = totals
    report['problems'] = problems
    if args.json:
        with open(args.json, 'w', encoding='utf-8') as f:
            json.dump(report, f, indent=1)
    print('%d files: %d third party as published, %d ours, %d Little-Man Firmware 1.0 as published' % (
        len(report['files']), report['third_party_files'], report['our_files'], report['published_firmware_files']))
    fw = report['firmware']
    if fw['beside Paprium.exe']:
        print('%s beside Paprium.exe: %d bytes, MD5 %s, SHA-256 %s; published: MD5 %s, SHA-256 %s: %s%s' % (
            pc_package.FIRMWARE_FILE, fw['beside Paprium.exe']['bytes'], fw['beside Paprium.exe']['MD5'], fw['beside Paprium.exe']['SHA-256'],
            fw['published MD5'], fw['published SHA-256'], 'equal' if fw.get('equal to the published file') else 'NOT EQUAL',
            ' (a test\'s made-up hashes)' if made_up_pins else ''))
        for k in sorted(totals):
            if k.startswith('firmware'):
                print('  %s: %d (counted only)' % (k, totals[k]))
    else:
        print('no %s beside Paprium.exe' % pc_package.FIRMWARE_FILE)
    print('positive control: raw windows %s, text form %s; planted runs the earlier rule called weak: %s' % (raw_ok, text_ok, kinds))
    print('CPU MATE\'s brain: %s' % ', '.join('%s %s' % (k, v) for k, v in report['brain'].items()))
    for k in sorted(totals):
        if k.startswith('ours'):
            print('  %s: %d' % (k, totals[k]))
    for rel in OUR_PROGRAMS:
        e = report['files'].get(rel)
        if e:
            print('%s: strong runs of 12+ bytes: %d (%d in allowed tables, %d in the compiler\'s runtime libraries); %d inputs in its linker map'
                  % (rel, len(e.get('strong_12_plus', [])), sum(1 for x in e.get('strong_12_plus', []) if x['allowed table']),
                     sum(1 for x in e.get('strong_12_plus', []) if x['compiler runtime library']), e.get('map_inputs', 0)))
            for x in e.get('strong_12_plus', []):
                verdict = ('allowed' if x['allowed table'] else 'the compiler\'s runtime library %s, byte-equal to its file' % x['compiler runtime library']
                           if x['compiler runtime library'] else 'NOT ALLOWED')
                print('    %d bytes %s at %d, %s %s: %s (%d bytes in an allowed table, %d zero bytes beside it)' % (
                    x['bytes'], x['form'], x['offset'], x['section'], x['symbol'], verdict, x['where']['in a table'], x['where']['zero outside']))
    for x in problems:
        print(x)
    print('RESULT: FAIL' if problems else 'RESULT: PASS')
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
