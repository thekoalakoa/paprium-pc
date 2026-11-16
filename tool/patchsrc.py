# Copyright (C) 2026 Marsupial Solutions LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Layer 1 of the formula: the one patch source, applied at translate time.

The patch source is a JSON file. It holds only our own hook sites and our own cave code. It never
holds bytes of the game: when a hook displaces game instructions, they are read from the player's
ROM here, and identity is checked with a hash over a window of at least 64 bytes.

Addresses may be written as numbers or as hexadecimal text ("0x02FAC4").

Fields (format 1):
  hooks: list of
    id        a name for logs
    site      the program address of the hook
    window    {"offset": bytes relative to site, "length": at least 64, "sha256": expected hash}
    kind      "jump_cave"  put a jump to the named cave at the site; the cave's code runs first, then the
                           displaced instructions, then control returns to the instruction after them
    cave      the cave's name
  caves: list of
    name      the cave's name
    base      the program address where the cave is placed, in a range the game does not use
    hex       our own 68000 code as hexadecimal text

Format 2 adds switches: patches that the player turns on and off while the game runs, with no new
translation. The site keeps the game's own code. The translated code at the site holds both ways, the
game's own and the patched one, and takes the patched way while the switch's bit is set in the
runtime's pt_switches (runtime/pt_rt.h). Both ways are the same for everything after them.
  switches: list of
    name      the switch's name
    bit       its bit in pt_switches
    about     what it does, in a sentence
  hooks, as above, and these kinds, each with "switch" naming its switch:
    "skip"    "length" bytes of whole instructions at the site are skipped: the switch's way runs on
              after them with the flags as they were before them, as no-operation words would leave them
    "call"    "length" bytes of whole instructions at the site are left out, and the named cave is
              called as a subroutine instead: the return address is the instruction after them. The
              cave holds the displaced instructions where its code says so
  and one kind with no switch:
    "switch_point"  the instruction at the site first calls the runtime's pt_switch_point, where
              pt_switches takes the value the program has asked for. A switch therefore changes only
              there, never between a site and its cave
  words: list of data words of the image that the program itself changes (the arcade stage word, set at the game's start); the
  translation does nothing with them, but the source names them so each is checked against the player's ROM like a hook
    name      the word's name
    address   its address
    window    as a hook's, relative to the address
  caves of switches: list of
    name      the cave's name
    base      where it is placed: a range that holds only zero bytes in the image, which the game never reads
    code      a list, in order, of our own 68000 code as hexadecimal text, and {"displaced": hook id}, which
              stands for that hook's displaced instructions, which are read from the player's ROM here

A switched cave is written into the image only for the translation (write_switch_caves), after discovery
has seen the game as it is, so the game's own code is cut and translated as before. The program never
holds it in its own copy of the ROM: image_check_hash leaves the caves out, so the image check between
the program and the translation still holds.

A file with empty lists is valid and changes nothing.
"""
import json

from . import dec68k

FORMATS = (1, 2)
JMP_ABS_L = 0x4EF9
SWITCH_KINDS = ('skip', 'call')
# Instructions that a switch may skip or move into a cave: no control transfer, and nothing relative to the
# program counter, as the cave runs them at another address.
NOT_DISPLACEABLE = {'bra', 'bcc', 'bsr', 'dbcc', 'jsr', 'jmp', 'rts', 'rte', 'rtr', 'stop', 'illegal', 'trap', 'link', 'unlk'}


class PatchError(Exception):
    pass


def addr(v):
    """An address as a number: a number, or hexadecimal text."""
    if isinstance(v, int):
        return v
    return int(str(v), 16)


def load(path):
    with open(path, encoding='utf-8') as f:
        src = json.load(f)
    if src.get('format') not in FORMATS:
        raise PatchError('Unknown patch source format.')
    return src


def _displaced(img, site, need):
    """Decode whole instructions at the site until they cover 'need' bytes. Their bytes come from the image."""
    p = site
    while p < site + need:
        p += dec68k.decode(img.r16, p).n
    return bytes(img.bytes[site:p])


def _whole(img, site, length, hook_id):
    """The instructions at the site that cover exactly length bytes, each one a plain instruction."""
    p, out = site, []
    while p < site + length:
        I = dec68k.decode(img.r16, p)
        if I.op in NOT_DISPLACEABLE or any(e and e[0] in ('pcd', 'pci') for e in (I.src, I.dst)):
            raise PatchError('Hook %s: the instruction at %06X cannot be moved or skipped.' % (hook_id, p))
        out.append(I)
        p += I.n
    if p != site + length:
        raise PatchError('Hook %s: %d bytes at the site do not end on an instruction.' % (hook_id, length))
    return out


def _check_window(img, hook):
    w = hook['window']
    if w['length'] < 64:
        raise PatchError('Hook %s: the identity window must be at least 64 bytes.' % hook['id'])
    if img.window_sha256(addr(hook['site']) + w['offset'], w['length']) != w['sha256'].lower():
        raise PatchError('Hook %s: the code at the site is not what the patch expects.' % hook['id'])


def _cave_words(code):
    """A cave's code list, as a list of parts: bytes of our own code, or the id of a hook whose displaced instructions go there."""
    parts = []
    for item in code:
        if isinstance(item, dict):
            parts.append(item['displaced'])
        else:
            parts.append(bytes.fromhex(item.replace(' ', '')))
    return parts


def apply(img, src):
    """Apply the patch source to the in-memory image. Returns the list of applied hook ids.

    Format 1 hooks change the image. Switched hooks and switch points leave the site as it is: they are checked here and
    kept in img.switch_plan for the translation (see switch_plan's fields there)."""
    caves = {c['name']: c for c in src.get('caves', [])}
    switches = {s['name']: s for s in src.get('switches', [])}
    bits = [s['bit'] for s in switches.values()]
    if len(set(bits)) != len(bits) or any(b <= 0 or b & (b - 1) or b > 0x80000000 for b in bits):
        raise PatchError('Each switch needs a bit of its own.')
    plan = {'hooks': {}, 'caves': [], 'switches': {n: s['bit'] for n, s in switches.items()}, 'words': {}}
    applied = []
    for word in src.get('words', []):
        _check_window(img, dict(word, id=word['name'], site=word['address']))
        plan['words'][word['name']] = addr(word['address'])
    for hook in src.get('hooks', []):
        _check_window(img, hook)
        site = addr(hook['site'])
        kind = hook['kind']
        if kind == 'jump_cave' and 'switch' not in hook:
            cave = caves[hook['cave']]
            code = bytes.fromhex(cave['hex'])
            moved = _displaced(img, site, 6)
            back = site + len(moved)
            tail = bytes([JMP_ABS_L >> 8, JMP_ABS_L & 0xFF]) + back.to_bytes(4, 'big')
            img.patch(addr(cave['base']), code + moved + tail, 'cave ' + cave['name'])
            jump = bytes([JMP_ABS_L >> 8, JMP_ABS_L & 0xFF]) + addr(cave['base']).to_bytes(4, 'big')
            img.patch(site, jump + bytes.fromhex('4E71') * ((len(moved) - 6) // 2), 'hook ' + hook['id'])
        elif kind == 'switch_point' and 'switch' not in hook:
            dec68k.decode(img.r16, site)
            plan['hooks'][site] = {'id': hook['id'], 'kind': kind}
        elif kind in SWITCH_KINDS and hook.get('switch') in switches:
            if site in plan['hooks']:
                raise PatchError('Hook %s: another hook has the same site.' % hook['id'])
            length = int(hook['length'])
            _whole(img, site, length, hook['id'])
            entry = {'id': hook['id'], 'kind': kind, 'mask': switches[hook['switch']]['bit'], 'resume': site + length,
                     'displaced': bytes(img.bytes[site:site + length])}
            if kind == 'call':
                if hook.get('cave') not in caves or 'code' not in caves[hook['cave']]:
                    raise PatchError('Hook %s: no switch cave called %s.' % (hook['id'], hook.get('cave')))
                entry['cave'] = addr(caves[hook['cave']]['base'])
            plan['hooks'][site] = entry
        else:
            raise PatchError('Hook %s: kind %s is not known, or its switch is not.' % (hook['id'], kind))
        applied.append(hook['id'])
    # The switches' caves: our code with the displaced instructions put in, checked against the image's free space.
    by_id = {h['id']: h for h in plan['hooks'].values()}
    taken = []
    for cave in src.get('caves', []):
        if 'code' not in cave:
            continue
        data = bytearray()
        for part in _cave_words(cave['code']):
            if isinstance(part, str):
                if part not in by_id or 'displaced' not in by_id[part]:
                    raise PatchError('Cave %s: no switched hook called %s.' % (cave['name'], part))
                data += by_id[part]['displaced']
            else:
                data += part
        base = addr(cave['base'])
        if base & 1 or len(data) & 1 or base + len(data) > img.size:
            raise PatchError('Cave %s: not a whole number of words inside the image.' % cave['name'])
        if any(img.bytes[base:base + len(data)]):
            raise PatchError('Cave %s: the space at %06X is not free in this image.' % (cave['name'], base))
        for b, n in taken:
            if base < b + n and b < base + len(data):
                raise PatchError('Cave %s: overlaps another cave.' % cave['name'])
        for site in plan['hooks']:
            if base <= site < base + len(data):
                raise PatchError('Cave %s: holds a hook site.' % cave['name'])
        taken.append((base, len(data)))
        plan['caves'].append({'name': cave['name'], 'base': base, 'data': bytes(data)})
    for h in plan['hooks'].values():
        h.pop('displaced', None)
    img.switch_plan = plan
    return applied


def switch_plan(img):
    """What apply left for the translation: {'hooks': {site: hook}, 'caves': [...], 'switches': {name: bit}}, or an empty plan.

    A hook is {'id', 'kind'} and, for a switched one, 'mask' (its bit), 'resume' (the address after the instructions it leaves
    out) and, for a call, 'cave' (the cave's address)."""
    return getattr(img, 'switch_plan', None) or {'hooks': {}, 'caves': [], 'switches': {}, 'words': {}}


def write_switch_caves(disc):
    """After discovery: writes the switches' caves into the image and lets discovery walk them, each one a function head.
    Returns the list of (base, length) written."""
    plan = switch_plan(disc.img)
    written = []
    for cave in plan['caves']:
        disc.img.patch(cave['base'], cave['data'], 'switch cave ' + cave['name'])
        disc.func_heads.add(cave['base'])
        disc.work.append(cave['base'])
        disc.walk()
        if cave['base'] not in disc.ins:
            raise PatchError('Cave %s: its code does not decode.' % cave['name'])
        written.append((cave['base'], len(cave['data'])))
    disc.img.switch_caves = written
    return written


def image_check_bytes(img):
    """The image as the program holds it at run time: the switches' caves left out (their space is zero in the ROM)."""
    data = bytearray(img.bytes)
    for base, n in getattr(img, 'switch_caves', ()):
        data[base:base + n] = bytes(n)
    return bytes(data)
