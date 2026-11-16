# Copyright (C) 2026 Marsupial Solutions LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks the repository itself. The repository holds our tool's source and tests, and nothing from the game.

Always:
  - every file except the licence text carries the copyright and licence lines, but for the picture filters' shaders in targets/pc/shaders, kept exactly as
    their authors wrote them: each carries its author's own notice instead (see THIRD_PARTY_TEXT)
  - no file is larger than a source file should be, and none has a ROM or dump extension
  - nothing under targets/pc includes or imports anything from rig/ or names a reference emulator
  - no number that is new since the base commit (see base_commit), in any file, looks like a game address (see looks_like_game_address), but the
    addresses the patch source names (patches/patch_source.json: its hook sites, its caves and its words). Those are ours by design, as a patch has
    to say where it goes, and each is checked against the player's ROM by a hash when the game is translated (tool/patchsrc.py)

With the local configuration (PAPRIUM_PC_CONFIG or --config), also:
  - no file holds a word or phrase of the list the configuration's tree_words names: a text file outside the repository, one on each line (a line
    starting with # is a note), of words that do not belong in this repository, such as names it must not credit. The list stays outside, so this
    file does not hold it
  - no tracked file holds a piece of the ROM of 16 bytes or more, in any of the forms a source file could hold it: raw bytes at any offset, hexadecimal
    text, a list of byte, word or long values in C or Python, in hexadecimal or decimal, or as string escapes. Each is compared with the ROM as the file
    has it and with each 16-bit pair swapped, which is the order a little-endian host keeps it in.
  - no tracked file names the ROM's hash

Exit 0 or 1, with RESULT: PASS or RESULT: FAIL.
"""
import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
MAX_SIZE = 200 * 1024
BANNED_EXT = {'.md_rom', '.bin', '.gen', '.smd', '.rom', '.srm', '.sav', '.dll', '.exe', '.o', '.obj'}
NO_HEADER = {'LICENSE'}
# Other projects' files kept exactly as their authors wrote them, pinned by hash where they are built (targets/pc/pc_build.py, SHADER_PINS). Each carries its author's
# notice in place of our header lines; every other rule here holds for them as for ours.
THIRD_PARTY_TEXT = re.compile(r'^targets/pc/shaders/[^/]+\.glsl$')
AUTHOR_NOTICE = re.compile(rb'(?i)copyright|public domain')


def base_commit(cfg=None):
    """The commit the address rule counts from: a number a file already held there was looked at then, and a number that is new since must pass
    the rule. The local configuration may name it (base_commit), when this history holds it; else it is this history's first commit, whose tree
    passed this check before that commit was made, as a history that starts from a copy of the tree does."""
    want = (cfg or {}).get('base_commit')
    if want:
        r = subprocess.run(['git', '-C', REPO, 'rev-parse', '--verify', '--quiet', want + '^{commit}'], capture_output=True, text=True)
        if not r.returncode:
            return want
    r = subprocess.run(['git', '-C', REPO, 'rev-list', '--max-parents=0', 'HEAD'], capture_output=True, text=True)
    roots = r.stdout.split()
    return roots[-1] if roots else 'HEAD'


def tree_words(cfg=None):
    """The words the configuration's tree_words file lists, in lower case; none without one."""
    path = (cfg or {}).get('tree_words')
    if not path:
        return []
    with open(path, encoding='utf-8') as f:
        return [x.strip().lower() for x in f if x.strip() and not x.lstrip().startswith('#')]

# ---- what a program under targets/pc may not contain.
# The PC port links no reference emulator: the lockstep rig uses one as a black box, and a target stays clear of it. So a target includes nothing from
# rig/ and names nothing of an emulator's. The firmware's old module and its cart header are barred too: they carry code the PC program must not link.
TARGET_PREFIX = 'targets/pc/'
TARGET_BAD_INCLUDE = re.compile(rb'#\s*include\s*[<"]([^>"]*)[>"]')
TARGET_BAD_IMPORT = re.compile(rb'^\s*(?:from|import)\s+rig\b', re.M)
TARGET_BAD_PATHS = [b'rig/', b'cart_hw/', b'input_hw/', b'gpgx/', b'legacy/']
TARGET_BAD_HEADERS = {b'ls_core.h', b'fw_cart.h', b'route_api.h', b'shared.h', b'm68k.h', b'm68kcpu.h', b'm68kconf.h', b'vdp_ctrl.h', b'vdp_render.h',
                      b'genesis.h', b'system.h', b'mem68k.h', b'memz80.h', b'io_ctrl.h', b'z80.h', b'cpuhook.h', b'libretro.h', b'paprium.h', b'mame.h',
                      b'ym2612.h', b'ym3438.h', b'sn76489.h', b'blip_buf.h'}
# The three sound chips' cores come from their authors, pinned by hash in targets/pc/pc_build.py, and only the one file that wraps each may include it, by exactly this path.
TARGET_PINNED_INCLUDES = {('targets/pc/pc_z80.c', b'chips/z80.h'), ('targets/pc/pc_ym.c', b'ym3438.h'), ('targets/pc/pc_psg.c', b'ympsg.h')}
TARGET_BAD_WORDS = [b'genesis plus', b'gpgx', b'musashi', b'm68ki_', b'cpu_hook', b'lock_fw_', b'lock_irq']

# ---- what looks like a game address.
# A line that means a size or some other plain number says so with the words below. Such lines are listed in the report, so a reader can see them.
NOT_ADDRESS = b'not a game address'
PROCESSOR_NAME = 68000      # in decimal this is the processor's name, and it is in every comment
_SUFFIX =rb'(?:[uU]?[lL]{0,2}|[lL]{1,2}[uU])'
_NOT_WORD_BEFORE = rb'(?<![0-9a-zA-Z_])'
_NOT_WORD_AFTER = rb'(?![0-9a-zA-Z_])'
ADDRESS_FORMS = [
    (re.compile(_NOT_WORD_BEFORE + rb'0[xX]([0-9a-fA-F]{5,8})' + _SUFFIX + _NOT_WORD_AFTER), 16),       # C and Python
    (re.compile(rb'\$([0-9a-fA-F]{5,8})' + _NOT_WORD_AFTER), 16),                                       # 68000 assembler text
    (re.compile(_NOT_WORD_BEFORE + rb'((?=[0-9A-F]*[A-F])[0-9A-F]{6})' + _NOT_WORD_AFTER), 16),         # bare, as our notes write them
    (re.compile(rb'(?<![0-9a-zA-Z_.,$])([0-9]{5,8})' + _SUFFIX + rb'(?![0-9a-zA-Z_]|[.,][0-9])'), 10),  # decimal; 1,189,381 is three small numbers
]


def looks_like_game_address(v):
    """The game's code and data sit in the file's range; its variables in work RAM. A machine model has to name the device space and the edges of work
    RAM and of the file, so those pass: only an address inside a range is suspect."""
    if 0x10000 < v < 0x400000:
        return True
    if 0x400000 <= v < 0xA00000:
        return v & 0xFFFF != 0
    if 0xE00000 <= v <= 0xFFFFFF:
        return (v & 0xFFFF) not in (0x0000, 0xFFFF, 0xFFFE, 0xFFFC)
    return False


def _address_values(line):
    for form, base in ADDRESS_FORMS:
        for m in form.finditer(line):
            v = int(m.group(1), base)
            if looks_like_game_address(v) and not (base == 10 and v == PROCESSOR_NAME):
                yield m.group(0).decode('latin-1'), v


_patch_source_addresses = None


def patch_source_addresses():
    """The addresses the repository's patch source names: its hooks' sites, its caves and its words."""
    global _patch_source_addresses
    if _patch_source_addresses is None:
        found = set()
        path = os.path.join(REPO, 'patches', 'patch_source.json')
        if os.path.exists(path):
            with open(path, encoding='utf-8') as f:
                src = json.load(f)
            value = lambda v: v if isinstance(v, int) else int(str(v), 16)
            found |= set(value(h['site']) for h in src.get('hooks', []))
            found |= set(value(c['base']) for c in src.get('caves', []))
            found |= set(value(w['address']) for w in src.get('words', []))
        _patch_source_addresses = frozenset(found)
    return _patch_source_addresses


def address_problems(name, data, old_values=frozenset(), marked=None):
    """Numbers in one file that look like game addresses. old_values: what the file held at the base commit. marked: a list that receives the lines that excuse themselves.
    The patch source's own addresses pass (see the head of this file)."""
    problems = []
    for number, line in enumerate(data.splitlines(), 1):
        found = [x for x in _address_values(line) if x[1] not in old_values and x[1] not in patch_source_addresses()]
        if not found:
            continue
        if NOT_ADDRESS in line.lower():
            if marked is not None:
                marked.append('%s:%d: %s' % (name, number, ', '.join(x[0] for x in found)))
            continue
        for text, _ in found:
            problems.append('%s:%d: the constant %s looks like a game address' % (name, number, text))
    return problems


def old_address_values(name, base):
    """The values at the base commit (base_commit) that the address rule would flag in this file; empty for a file that is new since then."""
    r = subprocess.run(['git', '-C', REPO, 'show', '%s:%s' % (base, name)], capture_output=True)
    if r.returncode:
        return frozenset()
    return frozenset(v for line in r.stdout.splitlines() for _, v in _address_values(line))


def header_problems(name, data):
    """Our copyright and licence lines at the head of one file; for another project's file kept exactly as its author wrote it (THIRD_PARTY_TEXT), its author's
    notice instead; nothing for the licence text itself."""
    if THIRD_PARTY_TEXT.match(name.replace('\\', '/')):
        if not AUTHOR_NOTICE.search(data[:1500]):
            return ['%s: another project\'s file with no notice of its author at its head' % name]
        return []
    if os.path.basename(name) in NO_HEADER:
        return []
    if b'Copyright (C) 2026 Marsupial Solutions LLC' not in data[:400] or b'GPL-3.0-or-later' not in data[:400]:
        return ['%s: copyright or licence line missing' % name]
    return []


def target_problems(name, data):
    """What is wrong with one file under targets/pc. name is its path in the repository, with forward slashes."""
    problems = []
    low = data.lower()
    for m in TARGET_BAD_INCLUDE.finditer(data):
        path = m.group(1).replace(b'\\', b'/')
        if (name, path) in TARGET_PINNED_INCLUDES:
            continue
        if any(p in path for p in TARGET_BAD_PATHS) or path.split(b'/')[-1] in TARGET_BAD_HEADERS:
            problems.append('%s: includes %s, which a target must not' % (name, path.decode('latin-1')))
    if name.endswith('.py') and TARGET_BAD_IMPORT.search(data):
        problems.append('%s: imports from rig/, which a target must not' % name)
    for w in TARGET_BAD_WORDS:
        if w in low:
            problems.append('%s: names %s, which a target must not' % (name, w.decode('latin-1')))
    return problems + address_problems(name, data)


# ---- pieces of the ROM. Everything a file could hold is turned into runs of bytes, then every 16-byte window of every run is looked up in one set while one
# pass walks the ROM, once as it is and once with each 16-bit pair swapped. So the cost is one walk, whatever the number of files.
WINDOW = 16
_COMMENT = re.compile(rb'/\*.*?\*/|//[^\n]*|#[^\n]*', re.S)
_NUMBER = re.compile(rb'\\[xX]([0-9a-fA-F]{2})'                                                         # a string escape in hexadecimal
                     rb'|\\([0-7]{3})'                                                                  # a string escape in octal
                     rb'|' + _NOT_WORD_BEFORE + rb'0[xX]([0-9a-fA-F]{1,8})' + _SUFFIX + _NOT_WORD_AFTER +
                     rb'|\$([0-9a-fA-F]{2,8})' + _NOT_WORD_AFTER +                                      # 68000 assembler text
                     rb'|(?<![0-9a-zA-Z_.$])([0-9]{1,10})' + _SUFFIX + _NOT_WORD_AFTER)                 # decimal
_HEX_TEXT = re.compile(rb'[0-9a-fA-F]{%d,}' % (WINDOW * 2))
_HEX_APART = [re.compile(rb'(?:%s[\s,:_-]+){%d,}%s' % (rb'[0-9a-fA-F]{%d}' % n, count, rb'[0-9a-fA-F]{%d}' % n)) for n, count in ((2, 15), (4, 7), (8, 3))]


def _numbers(data):
    """The numbers in a text, in order: (value, 1 when it is written or sized as a long)."""
    out = []
    for m in _NUMBER.finditer(data):
        if m.group(1) is not None:
            out.append((int(m.group(1), 16), 0))
        elif m.group(2) is not None:
            out.append((int(m.group(2), 8) & 0xFF, 0))
        else:
            digits = m.group(3) or m.group(4)
            if digits is not None:
                out.append((int(digits, 16), len(digits) > 4))
            else:
                v = int(m.group(5))
                out.append((v, v > 0xFFFF))
    return out


def _runs_of(values, fits, least, encode):
    run = []
    for v in values + [None]:
        if v is not None and fits(v):
            run.append(v[0])
            continue
        if len(run) >= least:
            yield encode(run)
        run = []


def byte_runs(data):
    """Every run of bytes a file holds, as (form, bytes)."""
    yield 'raw bytes', data
    for m in _HEX_TEXT.finditer(data):
        t = m.group(0)
        yield 'hexadecimal text', bytes.fromhex(t[:len(t) & ~1].decode())
        if len(t) & 1 or len(t) >= WINDOW * 2 + 1:      # the same digits one place along, in case the text starts in the middle of a byte
            t = t[1:]
            yield 'hexadecimal text', bytes.fromhex(t[:len(t) & ~1].decode())
    for form in _HEX_APART:
        for m in form.finditer(data):
            t = re.sub(rb'[^0-9a-fA-F]', b'', m.group(0))
            yield 'hexadecimal text with spaces', bytes.fromhex(t.decode())
    # The numbers as the file has them, then with its comments out, so a remark between the rows of a table does not hide the table.
    for text in (data, _COMMENT.sub(b' ', data)):
        values = _numbers(text)
        yield from (('a list of byte values', r) for r in _runs_of(values, lambda v: v[0] <= 0xFF, WINDOW, bytes))
        yield from (('a list of 16-bit values', r) for r in _runs_of(values, lambda v: v[0] <= 0xFFFF and not v[1], WINDOW // 2,
                                                                     lambda run: b''.join(x.to_bytes(2, 'big') for x in run)))
        for order in ('big', 'little'):
            yield from (('a list of 32-bit values', r) for r in _runs_of(values, lambda v: v[1] and v[0] <= 0xFFFFFFFF, WINDOW // 4,
                                                                         lambda run: b''.join(x.to_bytes(4, order) for x in run)))


def worth_looking_up(w):
    """A window with hardly any variety, or a plain ramp of bytes or of words, says nothing: fill bytes and counting tables are in every program."""
    if len(set(w)) <= 4:
        return False
    for step in (1, 2):
        if len({(w[i + step] - w[i]) & 0xFF for i in range(len(w) - step)}) <= 2:
            return False
    return True


def swapped_pairs(rom):
    out = bytearray(rom)
    n = len(rom) & ~1
    out[0:n:2] = rom[1:n:2]
    out[1:n:2] = rom[0:n:2]
    return bytes(out)


def rom_problems(files, rom):
    """files: {name: bytes}. What each file holds of the ROM, one line for each file and form."""
    wanted = set()
    candidates = []
    for name, data in files.items():
        for form, run in byte_runs(data):
            candidates.append((name, form, run))
            for i in range(len(run) - WINDOW + 1):
                w = run[i:i + WINDOW]
                if w not in wanted and worth_looking_up(w):
                    wanted.add(w)
    found = {}
    if wanted:
        for image, order in ((rom, 'in the order of the file'), (swapped_pairs(rom), 'with each 16-bit pair swapped')):
            for i in range(len(image) - WINDOW + 1):
                w = image[i:i + WINDOW]
                if w in wanted and w not in found:
                    found[w] = (i, order)
    problems = []
    seen = set()
    for name, form, run in candidates:
        if (name, form) in seen:
            continue
        for i in range(len(run) - WINDOW + 1):
            w = run[i:i + WINDOW]
            if w in found:
                problems.append('%s: %s hold %d bytes that are in the ROM, %s' % (name, form, WINDOW, found[w][1]))
                seen.add((name, form))
                break
    return problems


def tracked():
    r = subprocess.run(['git', '-C', REPO, 'ls-files', '--cached', '--others', '--exclude-standard'],
                       capture_output=True, text=True)
    return [x for x in r.stdout.splitlines() if x]


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--config')
    args = p.parse_args()
    problems = []
    marked = []
    files = tracked()
    rom = None
    want = None
    cfg = {}
    path = args.config or os.environ.get('PAPRIUM_PC_CONFIG')
    if path and os.path.exists(path):
        with open(path, encoding='utf-8') as f:
            cfg = json.load(f)
        want = cfg.get('rom_sha256')
        if cfg.get('rom_path') and os.path.exists(cfg['rom_path']):
            with open(cfg['rom_path'], 'rb') as f:
                rom = f.read()
    base = base_commit(cfg)
    words = tree_words(cfg)
    contents = {}
    for name in files:
        full = os.path.join(REPO, name)
        ext = os.path.splitext(name)[1].lower()
        size = os.path.getsize(full)
        if size > MAX_SIZE and os.path.basename(name) not in NO_HEADER:
            problems.append('%s: %d bytes is too large for a source file' % (name, size))
        if ext in BANNED_EXT:
            problems.append('%s: this kind of file does not belong here' % name)
        with open(full, 'rb') as f:
            data = f.read()
        contents[name] = data
        problems += header_problems(name, data)
        low = data.lower()
        for w in words:
            if w.encode() in low:
                problems.append('%s: holds a word that does not belong in this repository' % name)
        if want and want.lower().encode() in low:
            problems.append('%s: names the ROM hash' % name)
        forward = name.replace('\\', '/')
        if forward.startswith(TARGET_PREFIX):
            problems += target_problems(forward, data)
        if os.path.basename(name) not in NO_HEADER:
            problems += address_problems(forward, data, old_address_values(forward, base), marked)
    if rom is not None:
        problems += rom_problems({n: d for n, d in contents.items() if os.path.basename(n) not in NO_HEADER}, rom)
    print('%d files checked%s; the address rule counts from %s; %d words looked for' % (
        len(files), ', against the ROM' if rom is not None else '', 'the configured base commit' if base == cfg.get('base_commit') else
        'this history\'s first commit', len(words)))
    for x in marked:
        print('marked as not a game address: ' + x)
    for x in sorted(set(problems)):
        print(x)
    print('RESULT: FAIL' if problems else 'RESULT: PASS')
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
