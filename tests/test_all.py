# Copyright (C) 2026 Marsupial Solutions LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""The repository's tests. They use only test programs assembled here and never need the game.

Run: python -I -B tests/test_all.py [scratch folder]
Ends with RESULT: PASS or RESULT: FAIL: <names>. Exit 0 or 1.
"""
import hashlib
import json
import os
import random
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import harness  # noqa: E402
from harness import REPO, EXIT_PC  # noqa: E402
import asm68  # noqa: E402
from asm68 import Asm, a, absl, absw, d, disp, idx, imm, ind, post, pre  # noqa: E402
from tool import dec68k, discover, gate, image, patchsrc, ramcode  # noqa: E402

RESULTS = []


def check(name, ok, detail=''):
    RESULTS.append((name, ok))
    print('%s %s %s' % ('ok  ' if ok else 'FAIL', name, detail))


# ---- the gate
def test_gate(tmp):
    rom = bytes(range(256)) * 16
    path = os.path.join(tmp, 'g.bin')
    with open(path, 'wb') as f:
        f.write(rom)
    good = {'rom_path': path, 'rom_sha256': hashlib.sha256(rom).hexdigest(), 'out_dir': os.path.join(tmp, 'gate_out')}
    check('gate accepts the right ROM', gate.check_rom(good) == rom)
    bad = dict(good, rom_sha256='0' * 64)
    try:
        gate.check_rom(bad)
        check('gate refuses a wrong ROM', False)
    except gate.GateRefused as x:
        check('gate refuses a wrong ROM', str(x) == gate.REFUSAL)
    try:
        gate.check_rom(dict(good, rom_sha256=''))
        check('gate refuses a missing hash', False)
    except gate.GateRefused:
        check('gate refuses a missing hash', True)
    try:
        gate.out_dir(dict(good, out_dir=os.path.join(REPO, 'out')))
        check('output inside the repository is refused', False)
    except gate.GateRefused:
        check('output inside the repository is refused', True)
    cfg = os.path.join(tmp, 'bad.json')
    with open(cfg, 'w') as f:
        json.dump(dict(bad, out_dir=os.path.join(tmp, 'never')), f)
    r = subprocess.run([sys.executable, '-I', '-B', os.path.join(REPO, 'ptool.py'), 'translate', '--config', cfg],
                       capture_output=True, text=True)
    check('the command line stops at the gate and writes nothing',
          r.returncode == 3 and not os.path.exists(os.path.join(tmp, 'never')), r.stdout.strip())


# ---- the decoder against capstone
def test_decoder():
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_M68K, capstone.CS_MODE_BIG_ENDIAN | capstone.CS_MODE_M68K_000)
    ext = bytes([0x00, 0x04, 0x00, 0x08, 0x00, 0x0C, 0x00, 0x10])
    seen = disagree = 0
    samples = []
    for op in range(65536):
        code = bytes([op >> 8, op & 255]) + ext

        def r16(x, code=code):
            return (code[x] << 8) | code[x + 1]

        try:
            I = dec68k.decode(r16, 0)
        except dec68k.DecodeError:
            continue
        got = list(md.disasm(code, 0, 1))
        if not got:
            continue
        seen += 1
        name = got[0].mnemonic.split('.')[0]
        want = I.op
        if I.op == 'bcc':
            want = 'b' + I.cc
        elif I.op == 'dbcc':
            want = 'dbra' if I.cc == 'f' else 'db' + I.cc
        elif I.op == 'scc':
            want = 's' + I.cc
        if I.op == 'btst' and I.src[0] == 'd' and I.dst[0] == 'imm':
            continue                        # capstone reads this form's immediate as a long; the word form is right
        if I.op == 'sbcd' and I.src[0] == 'pre':
            continue                        # capstone gives the memory form a second word; it has one
        if name != want or got[0].size != I.n:
            disagree += 1
            if len(samples) < 8:
                samples.append('%04X mine %s/%d capstone %s/%d' % (op, want, I.n, name, got[0].size))
    check('decoder agrees with capstone on mnemonic and length', disagree == 0 and seen > 40000,
          '%d opcodes, %d disagree %s' % (seen, disagree, samples))


# ---- discovery on a small program of ours
def test_discovery():
    s = Asm(0x200)
    s.lea_label('copy_src', 1)              # a copy loop into RAM
    s.words(0x207C)                         # movea.l #imm,a0
    s.long(0xFF5000)
    s.words(0x4241)                         # clr.w d1
    s.label('copy')
    s.words(0x7000)                         # moveq #0,d0
    s.words(0x3001)                         # move.w d1,d0
    s.words(0xD080)                         # add.l d0,d0
    s.words(0x31B1, 0x0800, 0x0800)         # move.w (a1,d0.l),(a0,d0.l)
    s.words(0x5241)                         # addq.w #1,d1
    s.immop('cmpi', 2, 7, d(1))
    s.branch('ls', 'copy')
    s.jsr(absl(0xFF5000))
    s.words(0x45F9)                         # lea $const.l,a2
    s.dc_long_label('via_register')
    s.jsr(ind(2))
    s.jmp_label('switcher')
    s.label('copy_src')
    s.moveq(5, 3)
    s.rts()
    s.words(0, 0, 0, 0, 0, 0)
    s.label('via_register')
    s.link(6, 0)
    s.unlk(6)
    s.rts()
    s.label('switcher')
    s.words(0x303B, 0x0806)                 # move.w table(pc,d0.l),d0
    s.words(0x4EFB, 0x0002)                 # jmp table(pc,d0.w)
    s.label('table')
    s.words(6, 10)
    s.moveq(1, 4)
    s.jmp_label('end')
    s.moveq(2, 4)
    s.label('end')
    s.jmp(absl(EXIT_PC))
    rom = asm68.image(s)
    disc = discover.Discovery(image.Image(rom)).run()
    r = disc.report()
    check('discovery finds the vector table', disc.vector_tables == [0])
    check('discovery finds the RAM copy', len(disc.copies) == 1 and disc.copies[0]['ram'] == 0xFF5000
          and disc.copies[0]['length'] == 16, str(disc.copies))
    check('discovery decodes the RAM routine at its RAM address', 0xFF5000 in disc.ins)
    check('discovery parses the switch table', r['switches'] == 1 and r['switch_targets'] == 2)
    check('discovery resolves a constant-loaded jsr', r['indirect_constant'] == 1 and r['indirect_unresolved'] == 0,
          str(disc.const_target))


# ---- the patch source
def test_patch_source():
    s = Asm(0x200)
    s.moveq(1, 0)
    s.moveq(2, 1)
    s.moveq(3, 2)
    s.moveq(4, 3)
    s.jmp(absl(EXIT_PC))
    rom = bytearray(asm68.image(s))
    img = image.Image(bytes(rom))
    check('an empty patch source changes nothing', patchsrc.apply(img, {'format': 1, 'hooks': [], 'caves': []}) == []
          and bytes(img.bytes) == bytes(rom))
    src = {'format': 1, 'caves': [{'name': 'c', 'base': 0x800, 'hex': '7A07'}],
           'hooks': [{'id': 'h', 'site': 0x200, 'kind': 'jump_cave', 'cave': 'c',
                      'window': {'offset': 0, 'length': 64, 'sha256': img.window_sha256(0x200, 64)}}]}
    done = patchsrc.apply(img, src)
    check('a hook jumps to its cave and the cave keeps the displaced code', done == ['h'] and img.r16(0x200) == 0x4EF9
          and img.r16(0x800) == 0x7A07 and img.r16(0x802) == 0x7001)
    try:
        patchsrc.apply(image.Image(bytes(rom)), dict(src, hooks=[dict(src['hooks'][0], window={'offset': 0, 'length': 8, 'sha256': ''})]))
        check('a short identity window is refused', False)
    except patchsrc.PatchError:
        check('a short identity window is refused', True)
    with open(os.path.join(REPO, 'patches', 'patch_source.json'), encoding='utf-8') as f:
        real = json.load(f)
    check_repository_switches(real)


# ---- switches: patches the player turns on and off while the game runs (patch source format 2)
# The Pocket core's Scum! table as it stands in its 0.3.0 release: each switch, its bit (the core's menu values), and each hook as
# (kind, site, length). Our own addresses; the game's bytes at them are checked through hashes only.
SCUM_SWITCHES = {'health': 1, 'pills': 2, 'lives': 4}
SCUM_HOOKS = {'health_compare': ('call', 0x02FAC4, 6), 'health_throws': ('skip', 0x030E8A, 4), 'pills': ('skip', 0x01A9FA, 4),
              'lives': ('skip', 0x02A4EE, 2), 'switch_point': ('switch_point', 0x0B238C, 0)}
# The health cave's own code (players' slots get zero damage), as the Pocket serves it at 0x0BF000 before the instructions it moves there.
SCUM_HEALTH_CAVE = ('200A 0280 00FF FFFF 0480 00FF 8710 6516 E088 5280 0C80 0000 00FF 620A B039 00FF 9903 6402 7600')


def check_repository_switches(real):
    """The repository's patch source: the Pocket's Scum! switches, as our own addresses and words, with every game byte read from the player's ROM."""
    hooks = {h['id']: h for h in real.get('hooks', [])}
    check('the repository patch source is format 2 with the three cheat switches at the Pocket menu\'s bits',
          real.get('format') == 2 and {s['name']: s['bit'] for s in real.get('switches', [])} == SCUM_SWITCHES, str(real.get('switches')))
    got = {k: (h['kind'], patchsrc.addr(h['site']), int(h.get('length', 0))) for k, h in hooks.items()}
    check('its hooks are the Pocket 0.3.0 table\'s sites, kinds and lengths', got == SCUM_HOOKS, str(got))
    check('each hook names its switch, and the switch point none',
          all(hooks[k].get('switch') == ('health' if k.startswith('health') else k) for k in hooks if k != 'switch_point')
          and 'switch' not in hooks.get('switch_point', {}))
    check('each hook checks the player\'s ROM through a SHA-256 over at least 64 bytes',
          all(h['window']['length'] >= 64 and len(h['window']['sha256']) == 64 for h in hooks.values()))
    caves = {c['name']: c for c in real.get('caves', [])}
    code = caves.get('health', {}).get('code', [])
    ours = ' '.join(x for x in code if isinstance(x, str)).upper()
    check('the health cave is at 0x0BF000: our code, the hook\'s displaced instructions read from the ROM, then rts',
          list(caves) == ['health'] and patchsrc.addr(caves['health']['base']) == 0x0BF000 and len(code) == 3
          and code[1] == {'displaced': 'health_compare'} and ours == SCUM_HEALTH_CAVE + ' 4E75', str(code))
    words = [x for x in ours.split()]
    check('the patch source holds no word that is not ours: 20 cave words and no other code', len(words) == 20, str(len(words)))
    named = {w['name']: patchsrc.addr(w['address']) for w in real.get('words', [])}
    with open(os.path.join(REPO, 'targets', 'pc', 'pc_scum.c'), encoding='utf-8') as f:
        stage = [line.split()[2] for line in f if line.startswith('#define STAGE_WORD ')]
    check('the program\'s arcade stage word is the one the patch source names and checks', list(named) == ['arcade_stage'] and stage
          and int(stage[0].rstrip('uU'), 16) == named['arcade_stage'], '%s %s' % (named, stage))


def _switch_program():
    """A made-up program with two switched sites and a switch point, run three rounds.

    The call site is the Pocket health cave's shape: a health word compared with a damage, the cave giving zero damage. The skip site is a
    decrement whose flags the next instruction reads: with the switch on it reads the flags from before the site, so a translation that
    did not keep them whole is caught."""
    s = Asm(0x400)
    s.lea(absl(0xFFFFFC), 7)                # the reference's model keeps the stack pointer of the vector table only in some modes
    s.moveq(2, 7)                           # three rounds
    s.lea(absl(0xFF0000), 2)                # an object at the start of work RAM: its health word at +$88
    s.move(2, imm(100), disp(2, 0x88))
    s.lea(disp(2, 0x1000), 3)               # a log, one entry a round, 4 KB above it
    s.moveq(9, 4)
    s.label('round')
    s.jsr_label('point')
    s.moveq(5, 3)                           # the damage; also clears the carry, which the cave's own compare must replace
    s.label('site_call')
    s.move(2, disp(2, 0x88), d(0))
    s.arith('cmp', 2, d(0), 3)
    s.branch('cs', 'hurt')
    s.clr(2, disp(2, 0x88))
    s.branch('bra', 'after')
    s.label('hurt')
    s.arith('sub', 2, d(3), 0)
    s.move(2, d(0), disp(2, 0x88))
    s.label('after')
    s.moveq(0, 5)
    s.tst(2, d(5))                          # Z set
    s.label('site_skip')
    s.subq(1, 1, d(4))                      # 9 -> 8: Z clear, unless the switch skips it
    s.scc('eq', d(6))
    s.move(1, d(6), post(3))
    s.move(1, d(4), post(3))
    s.dbra(7, 'round')
    s.jmp(absl(EXIT_PC))
    s.label('point')
    s.move(4, d(0), pre(7))
    s.label('site_point')
    s.move(4, d(1), pre(7))
    s.move(4, post(7), d(1))
    s.move(4, post(7), d(0))
    s.rts()
    rom = bytearray(asm68.image(s))
    return rom, s.labels


def _switch_source(rom, labels, cave_base=0x800):
    img = image.Image(bytes(rom))
    win = lambda site: {'offset': -16, 'length': 64, 'sha256': img.window_sha256(site - 16, 64)}
    return {'format': 2, 'switches': [{'name': 'zero', 'bit': 1}, {'name': 'keep', 'bit': 4}],
            'hooks': [{'id': 'c', 'switch': 'zero', 'kind': 'call', 'site': '0x%X' % labels['site_call'], 'length': 6, 'cave': 'z', 'window': win(labels['site_call'])},
                      {'id': 's', 'switch': 'keep', 'kind': 'skip', 'site': labels['site_skip'], 'length': 2, 'window': win(labels['site_skip'])},
                      {'id': 'p', 'kind': 'switch_point', 'site': labels['site_point'], 'window': win(labels['site_point'])}],
            'caves': [{'name': 'z', 'base': '0x%X' % cave_base, 'code': ['7600', {'displaced': 'c'}, '4E75']}]}


def _switch_translate(rom, gen, src, drop_hook_uses=False, drop_ret_all=False):
    """ptool.do_translate's steps on a made-up ROM, with two negative controls that each undo one of the flag rules."""
    img = image.Image(bytes(rom))
    patchsrc.apply(img, src)
    d = discover.Discovery(img).run()
    before = set(d.ins)
    patchsrc.write_switch_caves(d)
    blocks, regs = harness.regions.build(d)
    plan = patchsrc.switch_plan(img)
    uses = {} if drop_hook_uses else {pc: harness.flagsopt.ALL for pc, h in plan['hooks'].items() if 'mask' in h}
    harness.flagsopt.analyse(blocks, regs, hook_uses=uses, ret_all=set() if drop_ret_all else set(c['base'] for c in plan['caves']))
    prog = harness.emit_c.Program(d, blocks, regs, hooks=plan['hooks'], hook_uses={} if drop_hook_uses else None)
    files = prog.write(gen)
    return prog, files, before, img, blocks


def test_switches(tmp):
    rom, labels = _switch_program()
    src = _switch_source(rom, labels)
    rom_path = os.path.join(tmp, 'switch.bin')
    with open(rom_path, 'wb') as f:
        f.write(rom)
    # What the Pocket's substitution would serve: the patched words at the sites and the cave in its space.
    patched = {}
    for mask in (0, 1, 4, 5):
        p = bytearray(rom)
        if mask & 1:
            p[labels['site_call']:labels['site_call'] + 6] = bytes.fromhex('4EB900000800')
            p[0x800:0x80A] = bytes.fromhex('7600') + rom[labels['site_call']:labels['site_call'] + 6] + bytes.fromhex('4E75')
        if mask & 4:
            p[labels['site_skip']:labels['site_skip'] + 2] = bytes.fromhex('4E71')
        patched[mask] = harness.run_unicorn(bytes(p))
    check('the made-up program: each switch changes the outcome on the reference',
          len(set(patched[m]['ram'][0:0x100] + patched[m]['ram'][0x1000:0x1010] for m in patched)) == 4)

    gen = os.path.join(tmp, 'gen_switches')
    prog, files, before, img, blocks = _switch_translate(rom, gen, src)
    plain = harness.translate(bytes(rom), os.path.join(tmp, 'gen_switches_plain'))[0]
    own = lambda bl: sorted((pc, len(b.insns)) for pc, b in bl.items() if not 0x800 <= pc < 0x80A)
    check('discovery and the cut of the program are the same with the switches as without them', own(blocks) == own(plain.blocks)
          and set(before) == set(plain.disc.ins), '%d / %d blocks' % (len(own(blocks)), len(own(plain.blocks))))
    check('the cave is translated, and the image check leaves it out', 0x800 in blocks and patchsrc.image_check_bytes(img) == bytes(rom))
    text = ''.join(open(f).read() for f in files)
    check('the generated code tests each switch at its site and calls the switch point', 'PT_SWITCH(0x1u)' in text and 'PT_SWITCH(0x4u)' in text
          and text.count('pt_switch_point();') == 1)
    exe = os.path.join(gen, 'host.exe')
    harness.build(files, gen, exe)
    for mask in (0, 1, 4, 5):
        mine = run_switched(exe, rom_path, gen, mask)
        problems = compare_states('switches %X' % mask, mine, patched[mask])
        check('switches %X: the translated program equals the reference with the Pocket\'s words served' % mask, not problems, '; '.join(problems[:4]))
    mine = run_switched(exe, rom_path, gen, 5, interp=True)
    check('the fallback runs no switch\'s way: with every switch asked for it plays the program as it is',
          not compare_states('fallback', mine, patched[0]))
    for label, kw in (('the skip site\'s flags not kept whole', {'drop_hook_uses': True}), ('the cave\'s flags not stored at its return', {'drop_ret_all': True})):
        g = os.path.join(tmp, 'gen_switches_' + label.split()[1].replace('\'', ''))
        prog2, files2 = _switch_translate(rom, g, src, **kw)[:2]
        e = os.path.join(g, 'host.exe')
        harness.build(files2, g, e)
        bad = [m for m in (1, 4) if compare_states('planted', run_switched(e, rom_path, g, m), patched[m])]
        check('a planted fault is caught: %s' % label, bool(bad))
    try:
        patchsrc.apply(image.Image(bytes(rom)), dict(src, caves=[dict(src['caves'][0], base='0x%X' % labels['site_call'])]))
        check('a cave over code that is not free is refused', False)
    except patchsrc.PatchError:
        check('a cave over code that is not free is refused', True)
    try:
        patchsrc.apply(image.Image(bytes(rom)), dict(src, hooks=[dict(src['hooks'][1], length=3)] + src['hooks'][2:]))
        check('a hook whose length ends inside an instruction is refused', False)
    except patchsrc.PatchError:
        check('a hook whose length ends inside an instruction is refused', True)


def empty_patch_source(folder):
    """A patch source with no patch, named as the repository's, for a made-up ROM: the repository's own patches are checked against the real game's bytes,
    which a made-up ROM does not hold, and a patch that does not fit stops the translation (tool/patchsrc.py)."""
    path = os.path.join(folder, 'empty_patches', 'patch_source.json')
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        json.dump({'format': 2, 'name': 'patch source', 'switches': [], 'hooks': [], 'caves': [], 'words': []}, f)
    return path


def run_switched(exe, rom_path, cwd, mask, interp=False):
    ram_path = os.path.join(cwd, 'ram_%X%s.bin' % (mask, 'i' if interp else ''))
    cmd = [exe, rom_path, '%X' % EXIT_PC, ram_path] + (['interp'] if interp else []) + ['switches=%X' % mask]
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=cwd, env=harness.gcc_env())
    state = {'rc': r.returncode, 'stderr': r.stderr}
    for line in r.stdout.splitlines():
        k, v = line.split(None, 1)
        state[k] = v
    if os.path.exists(ram_path):
        with open(ram_path, 'rb') as f:
            state['ram'] = f.read()
    return state


# ---- whole pipeline, against Unicorn
def forms_program(seed):
    """Random register values, every instruction form we lift, and the status register captured after each."""
    rng = random.Random(seed)
    s = Asm(0x400)
    data = 0xFF4000

    def rnd32():
        return rng.choice([rng.getrandbits(32), rng.getrandbits(8), 0, 0xFFFFFFFF, 0x80000000, 0x7FFFFFFF, 0x8000, 0xFFFF])

    def setup():
        for n in range(7):
            s.move(4, imm(rnd32()), d(n))
        for n in (0, 1, 2):
            s.move(4, imm(data + rng.randrange(0, 0x100) * 2), a(n))
        s.move_to_ccr(imm(rng.getrandbits(4)))

    def capture():
        s.move_from_sr(d(7))
        s.immop('andi', 2, 0xFFEF, d(7))    # X is not kept, so it is left out of the comparison
        s.move(2, d(7), pre(5))

    s.move(4, imm(0xFF8000), a(5))
    s.move(4, imm(0xFF9000), a(7))
    forms = [
        lambda: s.move(1, d(1), ind(0)),
        lambda: s.move(2, d(1), post(0)),
        lambda: s.move(4, d(1), pre(1)),
        lambda: s.move(2, ind(0), disp(1, 6)),
        lambda: s.move(1, idx(0, 2, 'd', 3 & 0, 2), d(4)) if False else s.move(1, disp(0, 3), d(4)),
        lambda: s.moveq(rng.randrange(-128, 127), 3),
        lambda: s.move(2, imm(rng.getrandbits(16)), d(3)),
        lambda: s.clr(1, d(2)), lambda: s.clr(2, d(2)), lambda: s.clr(4, ind(1)),
        lambda: s.tst(1, d(2)), lambda: s.tst(2, d(2)), lambda: s.tst(4, d(2)),
        lambda: s.arith('add', 1, d(1), 2), lambda: s.arith('add', 2, d(1), 2), lambda: s.arith('add', 4, d(1), 2),
        lambda: s.arith('sub', 1, d(1), 2), lambda: s.arith('sub', 2, d(1), 2), lambda: s.arith('sub', 4, d(1), 2),
        lambda: s.arith('cmp', 1, d(1), 2), lambda: s.arith('cmp', 2, d(1), 2), lambda: s.arith('cmp', 4, d(1), 2),
        lambda: s.arith('and', 2, d(1), 2), lambda: s.arith('or', 4, d(1), 2),
        lambda: s.arith_to_mem('add', 2, 1, ind(0)), lambda: s.arith_to_mem('sub', 1, 1, disp(1, 2)),
        lambda: s.arith_to_mem('eor', 4, 1, d(2)),
        lambda: s.immop('addi', 2, rng.getrandbits(16), d(2)), lambda: s.immop('subi', 1, rng.getrandbits(8), d(2)),
        lambda: s.immop('cmpi', 4, rng.getrandbits(32), d(2)), lambda: s.immop('andi', 2, rng.getrandbits(16), d(2)),
        lambda: s.immop('ori', 1, rng.getrandbits(8), d(2)), lambda: s.immop('eori', 4, rng.getrandbits(32), d(2)),
        lambda: s.addq(2, rng.randrange(1, 9), d(2)), lambda: s.subq(4, rng.randrange(1, 9), d(2)),
        lambda: s.addq(4, 3, a(1)), lambda: s.subq(2, 2, a(1)),
        lambda: s.adda(2, d(2), 1) if False else s.adda(4, imm(4), 1), lambda: s.suba(2, imm(2), 1),
        lambda: s.cmpa(2, d(2), 1), lambda: s.cmpa(4, d(2), 1),
        lambda: s.neg(1, d(2)), lambda: s.neg(4, d(2)), lambda: s.not_(2, d(2)), lambda: s.not_(4, ind(0)),
        lambda: s.ext(2, 2), lambda: s.ext(4, 2), lambda: s.swap(2),
        lambda: s.mulu(d(1), 2), lambda: s.muls(d(1), 2),
        # No overflow here: on overflow the 68000 leaves N undefined, and references differ.
        lambda: (s.move(2, imm(rng.getrandbits(16) | 1), d(1)), s.immop('andi', 4, 0xFFFF, d(2)), s.divu(d(1), 2)),
        lambda: (s.move(2, imm(rng.getrandbits(16) | 1), d(1)), s.immop('andi', 4, 0x7FFF, d(2)), s.divs(d(1), 2)),
        lambda: s.bit('btst', imm(rng.randrange(32)), d(2)), lambda: s.bit('bset', imm(rng.randrange(32)), d(2)),
        lambda: s.bit('bclr', d(1), d(2)), lambda: s.bit('bchg', d(1), ind(0)), lambda: s.bit('btst', imm(3), ind(0)),
        lambda: s.scc(rng.choice(list(asm68.CC)), d(2)), lambda: s.scc(rng.choice(list(asm68.CC)), ind(0)),
        lambda: s.exg_dd(1, 2), lambda: s.exg_aa(0, 1), lambda: s.exg_da(1, 1),
        lambda: s.lea(disp(0, 16), 1), lambda: s.lea(absl(data), 2), lambda: s.pea(disp(0, 4)),
        lambda: s.link(6, -8), lambda: (s.link(6, 0), s.unlk(6)),
        lambda: s.movem_to_mem(4, 0x000F, ind(0)), lambda: s.movem_to_mem(2, 0x0303, disp(0, 8)),
        lambda: s.movem_to_mem(4, 0xE0E0 >> 5, pre(1)), lambda: s.movem_to_regs(4, 0x0006, post(0)),
        lambda: s.movem_to_regs(2, 0x0006, ind(0)),
    ]
    for kind in ('asl', 'asr', 'lsl', 'lsr', 'rol', 'ror'):
        for sz in (1, 2, 4):
            forms.append(lambda kind=kind, sz=sz: s.shift(kind, sz, imm(rng.randrange(1, 9)), 2))
            forms.append(lambda kind=kind, sz=sz: s.shift(kind, sz, d(1), 2))
    for form in forms:
        setup()
        form()
        capture()
    # control flow: all condition codes, a loop, calls
    for cc in asm68.CC:
        if cc in ('t', 'f'):
            continue
        setup()
        s.arith('cmp', 4, d(1), 2)
        s.branch(cc, 'skip_' + cc)
        s.moveq(1, 6)
        s.label('skip_' + cc)
        capture()
    s.moveq(9, 4)
    s.label('loop')
    s.addq(2, 1, d(6))
    s.dbra(4, 'loop')
    s.jsr_label('fn')
    s.move(2, d(6), pre(5))
    s.lea_label('fn2', 3)
    s.jsr(ind(3))
    s.move(2, d(6), pre(5))
    s.jmp(absl(EXIT_PC))
    s.label('fn')
    s.addq(2, 3, d(6))
    s.rts()
    s.label('fn2')
    s.link(6, -4)
    s.addq(2, 5, d(6))
    s.unlk(6)
    s.rts()
    return asm68.image(s)


def compare_states(name, mine, ref, flags_in_ram=True):
    problems = []
    if mine.get('rc') != 0:
        problems.append('exit %s %s' % (mine.get('rc'), mine.get('stderr', '')[:200]))
    if mine.get('pc') != ref['pc']:
        problems.append('pc %s %s' % (mine.get('pc'), ref['pc']))
    for k in ['d%d' % i for i in range(7)] + ['a%d' % i for i in range(8)]:
        if mine.get(k) != ref[k]:
            problems.append('%s %s %s' % (k, mine.get(k), ref[k]))
    mram, rram = mine.get('ram', b''), ref['ram']
    if mram != rram:
        where = [i for i in range(min(len(mram), len(rram))) if mram[i] != rram[i]]
        problems.append('%d RAM bytes differ, first at %04X: mine %02X ref %02X' % (
            len(where), where[0] if where else 0, mram[where[0]] if where else 0, rram[where[0]] if where else 0))
    return problems


def test_pipeline(tmp):
    seeds = (1, 2, 3)
    for seed in seeds:
        rom = forms_program(seed)
        rom_path = os.path.join(tmp, 'forms%d.bin' % seed)
        with open(rom_path, 'wb') as f:
            f.write(rom)
        ref = harness.run_unicorn(rom)
        for label, every, extra in (('blocks', False, ()), ('every instruction', True, ()),
                                    ('portable bytes', False, ('-DPT_RAM_BYTES',))):
            gen = os.path.join(tmp, 'gen_%d_%s' % (seed, label.replace(' ', '_')))
            prog, files = harness.translate(rom, gen, every_insn=every)
            nl = prog.stats()['blocks_not_lifted']
            exe = os.path.join(gen, 'host.exe')
            harness.build(files, gen, exe, extra)
            mine = harness.run_native(exe, rom_path, os.path.join(gen, 'ram.bin'), gen)
            problems = compare_states(label, mine, ref)
            check('forms seed %d, %s, against Unicorn' % (seed, label), not problems and nl == 0,
                  '; '.join(problems[:6]) + (' not lifted %s' % prog.stats()['unlifted_by_mnemonic'] if nl else ''))
            if label == 'blocks':
                mine = harness.run_native(exe, rom_path, os.path.join(gen, 'ram_i.bin'), gen, interp=True)
                problems = compare_states('interpreter', mine, ref)
                check('forms seed %d, fallback interpreter, against Unicorn' % seed, not problems, '; '.join(problems[:6]))
    # Negative control: a planted flag bug must be caught by the same comparison.
    rom = forms_program(1)
    rom_path = os.path.join(tmp, 'forms1.bin')
    ref = harness.run_unicorn(rom)
    gen = os.path.join(tmp, 'gen_planted')
    prog, files = harness.translate(rom, gen, plant_flag_bug=True)
    exe = os.path.join(gen, 'host.exe')
    harness.build(files, gen, exe)
    mine = harness.run_native(exe, rom_path, os.path.join(gen, 'ram.bin'), gen)
    check('a planted flag bug is caught', bool(compare_states('planted', mine, ref)))


def test_command_line(tmp):
    rom = forms_program(4)
    path = os.path.join(tmp, 'cli.bin')
    with open(path, 'wb') as f:
        f.write(rom)
    out = os.path.join(tmp, 'cli_out')
    cfg = os.path.join(tmp, 'cli.json')
    with open(cfg, 'w') as f:
        json.dump({'rom_path': path, 'rom_sha256': hashlib.sha256(rom).hexdigest(), 'out_dir': out, 'patch_source': empty_patch_source(tmp)}, f)
    r = subprocess.run([sys.executable, '-I', '-B', os.path.join(REPO, 'ptool.py'), 'translate', '--config', cfg],
                       capture_output=True, text=True)
    made = sorted(os.listdir(out)) if os.path.isdir(out) else []
    check('the command line translates a known image into the output folder',
          r.returncode == 0 and 'gen' in made and 'discover.json' in made and 'translate.json' in made, str(made))


def test_fallback_log(tmp):
    s = Asm(0x400)
    s.move(4, imm(0x500 + 2), a(0))       # the middle of a block nothing else names
    s.jmp(ind(0))
    rom = bytearray(asm68.image(s))
    rom[0x500:0x506] = bytes.fromhex('70014EF9007F')
    rom[0x506:0x508] = bytes.fromhex('0000')
    path = os.path.join(tmp, 'miss.bin')
    with open(path, 'wb') as f:
        f.write(rom)
    gen = os.path.join(tmp, 'gen_miss')
    prog, files = harness.translate(bytes(rom), gen)
    exe = os.path.join(gen, 'host.exe')
    harness.build(files, gen, exe)
    mine = harness.run_native(exe, path, os.path.join(gen, 'ram.bin'), gen)
    log = os.path.join(gen, 'fallback.log')
    text = open(log).read() if os.path.exists(log) else ''
    check('a discovery miss is logged and runs in the fallback', mine['rc'] == 0 and '000502' in text, text.strip())

    # An instruction that reads X stops the fallback: X is not kept, so it refuses rather than guess.
    rom[0x502:0x504] = bytes.fromhex('D380')    # addx.l d0,d1
    path = os.path.join(tmp, 'missx.bin')
    with open(path, 'wb') as f:
        f.write(rom)
    gen = os.path.join(tmp, 'gen_missx')
    prog, files = harness.translate(bytes(rom), gen)
    exe = os.path.join(gen, 'host.exe')
    harness.build(files, gen, exe)
    mine = harness.run_native(exe, path, os.path.join(gen, 'ram.bin'), gen)
    check('the fallback stops on an instruction that reads X', mine['rc'] == 4 and 'reads X' in mine.get('stderr', ''),
          mine.get('stderr', '')[:200])


# ---- the lockstep core
def test_rig(tmp):
    rt = os.path.join(REPO, 'runtime')
    rig = os.path.join(REPO, 'rig')
    exe = os.path.join(tmp, 'rig_unit.exe')
    cmd = [harness.GCC, '-O1', '-w', '-std=gnu11', '-DPT_LOCKSTEP', '-I', rt, '-I', rig, os.path.join(HERE, 'rig_unit.c'),
           os.path.join(rig, 'ls_core.c'), os.path.join(rt, 'pt_rt.c'), os.path.join(rt, 'pt_fallback.c'),
           os.path.join(rt, 'pt_port_null.c'), '-o', exe]
    r = subprocess.run(cmd, capture_output=True, text=True, env=harness.gcc_env())
    if r.returncode:
        check('rig unit test builds', False, r.stderr[:2000])
        return
    r = subprocess.run([exe, tmp], capture_output=True, text=True, cwd=tmp, env=harness.gcc_env())
    out = r.stdout
    check('rig unit test', r.returncode == 0, out.strip().splitlines()[-1] if out.strip() else r.stderr[:500])
    for line in out.splitlines():
        if line.startswith('case FAIL'):
            check(line, False)
    report = os.path.join(tmp, 'diff_0001.txt')
    check('a difference writes a report with its trail', os.path.exists(report) and 'trail' in open(report).read())


# ---- the measuring tools, on small made-up input
def test_tools(tmp):
    from tool import coverage, waitloops
    table = bytearray(32)
    table[1] = 0b101        # addresses 0x10 and 0x14
    table[2] = 0b10000000   # address 0x2E
    check('coverage reads its bit table', coverage.bits(bytes(table)) == {0x10, 0x14, 0x2E})
    gen = os.path.join(tmp, 'waitgen')
    os.makedirs(gen, exist_ok=True)
    with open(os.path.join(gen, 'gen_000.c'), 'w', newline='\n') as f:
        f.write('\n'.join([
            'u32 f_000100(PtCtx *c, u32 e) {',
            'B_000100: ;',
            '  PT_BLOCK(0x000100u, 3, 0, 1);',
            '  t0 = R8(A(0));',
            '  if (!fz) { goto B_000100; }',
            '  goto B_000110;',
            'B_000110: ;',
            '  PT_BLOCK(0x000110u, 2, 0, 1);',
            '  W16(A(1), t0);',
            '  if (!fz) { goto B_000110; }',
            '  goto B_000120;',
            'B_000120: ;',
            '  PT_BLOCK(0x000120u, 2, 0, 1);',
            '  if ((D(0) & 0xFFFFu) != 0xFFFFu) { goto B_000120; }',
            '  return 0x000130u;',
            '}', '']))
    check('a wait loop has no store and no counter', waitloops.find_loops(gen) == {0x100: 3})


# ---- the form fuzz. It links the reference in the local output folder, so it runs only with the local configuration, and only where its
# program's sources are: a public copy of this repository leaves them out, as they build only against that reference (see rig/README.md).
def test_form_fuzz(tmp, config):
    from tool import formfuzz
    if not os.path.exists(os.path.join(REPO, 'rig', 'fuzz', 'build_fuzz.py')):
        print('skip the form fuzz: its program\'s sources are not in this copy of the repository')
        return
    folder = os.path.join(tmp, 'ff')
    r = subprocess.run([sys.executable, '-I', '-B', os.path.join(REPO, 'tool', 'formfuzz.py'), 'prepare', '--all-opcodes', '--limit', '3000', '--out', folder,
                        '--plant-auto'], capture_output=True, text=True)
    check('the form fuzz prepares slots with no program behind them', r.returncode == 0, r.stderr[-300:])
    build = [sys.executable, '-I', '-B', os.path.join(REPO, 'rig', 'fuzz', 'build_fuzz.py'), '--config', config, '--folder', folder, '--jobs', os.environ.get('PC_TEST_JOBS', '2')]
    r = subprocess.run(build, capture_output=True, text=True)
    if 'RESULT: PASS' not in r.stdout:
        check('the form fuzz builds', False, r.stdout[-500:])
        return
    exe = os.path.join(folder, 'fuzz.exe')
    planted = json.load(open(os.path.join(folder, 'plant.json')))
    r = subprocess.run([exe, '.', '--cases', '32'], capture_output=True, text=True, cwd=folder, errors='replace')
    kinds = {line.split()[2] for line in r.stdout.splitlines() if line.startswith('DIFFERENCE')}
    check('the form fuzz catches three planted lines', len(planted) == 3 and bool(kinds) and r.returncode == 1, str(sorted(kinds)))
    r = subprocess.run([sys.executable, '-I', '-B', os.path.join(REPO, 'tool', 'formfuzz.py'), 'prepare', '--all-opcodes', '--limit', '3000', '--out', folder],
                       capture_output=True, text=True)
    r = subprocess.run(build, capture_output=True, text=True)
    check('the form fuzz rebuilds without the planted lines', 'RESULT: PASS' in r.stdout)
    for mode, name in (('T', 'translated code'), ('I', 'fallback interpreter')):
        r = subprocess.run([exe, '.', '--cases', '32', '--mode', mode], capture_output=True, text=True, cwd=folder, errors='replace')
        check('form fuzz, %s, 3000 random encodings' % name, r.returncode == 0, ' '.join(r.stdout.splitlines()[-1:])[:200])
    r = subprocess.run(build + ['--define', 'PT_RAM_BYTES', '--name', 'fuzz_be'], capture_output=True, text=True)
    exe = os.path.join(folder, 'fuzz_be.exe')
    r = subprocess.run([exe, '.', '--cases', '32'], capture_output=True, text=True, cwd=folder, errors='replace')
    check('form fuzz, big-endian byte layout, 3000 random encodings', r.returncode == 0, ' '.join(r.stdout.splitlines()[-1:])[:200])


# ---- the firmware as the cart, on a made-up ROM. It needs a copy of the firmware tree, so it runs only when the local
# configuration names one.
def test_fw_cart(tmp, config):
    import concurrent.futures
    import importlib.util
    with open(config, encoding='utf-8') as f:
        src = json.load(f).get('fw_source')
    if not src or not os.path.isdir(src):
        print('skip the firmware cart: the local configuration names no firmware tree')
        return
    spec = importlib.util.spec_from_file_location('build_rig', os.path.join(REPO, 'rig', 'build_rig.py'))
    build_rig = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(build_rig)
    folder = os.path.join(tmp, 'fwc')
    os.makedirs(folder, exist_ok=True)
    jobs = build_rig.firmware_jobs(src, folder, '-O1')
    with concurrent.futures.ThreadPoolExecutor(int(os.environ.get('PC_TEST_JOBS', '2'))) as pool:
        errors = [e for e in pool.map(lambda j: build_rig.compile_one(*j), jobs) if e]
    if errors:
        check('the firmware cart builds', False, errors[0][:500])
        return
    exe = os.path.join(folder, 'fw_cart_test.exe')
    r = subprocess.run([harness.GCC, '-o', exe, os.path.join(HERE, 'fw_cart_test.c')] + jobs[0][2] + [j[1] for j in jobs] + ['-lm'],
                       capture_output=True, text=True)
    if r.returncode:
        check('the firmware cart test links', False, r.stderr[:500])
        return
    r = subprocess.run([exe], capture_output=True, text=True, cwd=folder, errors='replace')
    check('the firmware answers as the cart on a made-up ROM', r.returncode == 0, ' '.join(r.stdout.splitlines()[-2:])[:300])
    for line in r.stdout.splitlines():
        if line.startswith('FAIL'):
            check(line, False)
    test_fw_without_old_module(src, folder, jobs)


def test_fw_without_old_module(src, folder, jobs):
    """The PC program's firmware list: the same test program, with the firmware's old module out and the two variables from targets/pc in."""
    import check_pc_link
    firmware = check_pc_link.load_firmware_list()
    names = firmware.sources(src)
    check('the PC firmware list leaves out what it says',
          not [n for n in names if n.startswith(('legacy/', 'buddy/', 'board/pocket/'))] and 'core/trace/fatal.c' not in names and 'interim/s1/bodies_s1.c' in names)
    objs = [j[1] for j in jobs if 'fw_legacy_' not in os.path.basename(j[1])]
    check('the rig\'s firmware list held the old module, and this one does not', len(objs) == len(jobs) - 1)
    vars_obj = os.path.join(folder, 'pc_fw_vars.o')
    r = subprocess.run([harness.GCC, '-c', '-Wall', '-Wextra', '-Werror', '-std=gnu11', '-O1', os.path.join(REPO, 'targets', 'pc', 'pc_fw_vars.c'), '-o', vars_obj],
                       capture_output=True, text=True, env=harness.gcc_env())
    check('the two variables compile', r.returncode == 0, r.stderr[:300])
    exe = os.path.join(folder, 'fw_cart_test_pc.exe')
    r = subprocess.run([harness.GCC, '-o', exe, os.path.join(HERE, 'fw_cart_test.c')] + jobs[0][2] + objs + [vars_obj, '-lm'], capture_output=True, text=True, env=harness.gcc_env())
    if r.returncode:
        check('the firmware links without its old module', False, r.stderr[:500])
        return
    r = subprocess.run([exe], capture_output=True, text=True, cwd=folder, errors='replace')
    check('the firmware answers as the cart without its old module', r.returncode == 0, ' '.join(r.stdout.splitlines()[-2:])[:300])
    problems = check_pc_link.object_problems(objs) + check_pc_link.symbol_problems(check_pc_link.program_symbols(exe)) + \
        check_pc_link.source_problems(src, names + firmware.wavpack_sources())
    check('the link check passes that program', not problems, str(problems[:5]))
    # The control: the rig's own list, old module in, must fail all three looks.
    old = os.path.join(folder, 'fw_cart_test.exe')
    check('the link check sees the old module on a link line', bool(check_pc_link.object_problems([j[1] for j in jobs])))
    check('the link check sees the old module\'s decoders in a program', bool(check_pc_link.symbol_problems(check_pc_link.program_symbols(old))))
    check('the link check sees the old module\'s source line', bool(check_pc_link.source_problems(src, ['legacy/026/mame.c'])))


def test_pc_link_rules(tmp):
    """The link check on made-up programs and link lines."""
    import check_pc_link
    folder = os.path.join(tmp, 'pclink')
    os.makedirs(folder, exist_ok=True)
    src = os.path.join(folder, 'x.c')
    programs = {'clean': b'int pc_vdp_step(void) { return 1; }\nint main(void) { return pc_vdp_step() - 1; }\n',
                'decoder': b'void ppm_unpack_scal(void) {}\nint main(void) { return 0; }\n',
                'emulator': b'void m68k_run(void) {}\nint main(void) { return 0; }\n'}
    seen = {}
    for name, text in programs.items():
        with open(src, 'wb') as f:
            f.write(text)
        exe = os.path.join(folder, name + '.exe')
        r = subprocess.run([harness.GCC, '-O0', src, '-o', exe], capture_output=True, text=True, env=harness.gcc_env())
        check('link rules: made-up program %s builds' % name, r.returncode == 0, r.stderr[:300])
        seen[name] = check_pc_link.symbol_problems(check_pc_link.program_symbols(exe))
    check('link rules: a clean program passes', not seen['clean'], str(seen['clean']))
    check('link rules: the old module\'s decoder is caught', bool(seen['decoder']))
    check('link rules: a reference machine\'s name is caught', bool(seen['emulator']))
    for bad in ('out/fw_legacy_026_mame.o', 'C:/x/rig/ls_core.o', 'C:/x/gpgx/core/vdp_ctrl.o', 'legacy/026/mame.o'):
        check('link rules: caught ' + bad, bool(check_pc_link.object_problems([bad])))
    good = ['out/pc_vdp.o', 'out/fw_interim_s1_bodies_s1.o', 'out/fw_cart_x.o']
    check('link rules: plain objects pass', not check_pc_link.object_problems(good), str(check_pc_link.object_problems(good)))
    # The pinned cores: an FM model's names pass only from its pinned object, and a reference machine's write entries never do.
    src2 = os.path.join(folder, 'core.c')
    with open(src2, 'wb') as f:
        f.write(b'void OPN2_Clock(void) {}\n')
    pinned, fake = os.path.join(folder, 'ym3438.o'), os.path.join(folder, 'fake_core.o')
    for obj in (pinned, fake):
        r = subprocess.run([harness.GCC, '-c', src2, '-o', obj], capture_output=True, text=True, env=harness.gcc_env())
        check('link rules: made-up core builds', r.returncode == 0, r.stderr[:300])
    allowed, problems = check_pc_link.pinned_allowed([pinned])
    check('link rules: the pinned object may define its names', 'OPN2_Clock' in allowed and not problems, str((allowed, problems)))
    allowed, problems = check_pc_link.pinned_allowed([pinned, fake])
    check('link rules: any other object may not', bool(problems), str((allowed, problems)))
    check('link rules: with no pinned object the name is caught', bool(check_pc_link.symbol_problems('0000 T OPN2_Clock\n', check_pc_link.pinned_allowed([fake])[0])))
    check('link rules: a name the pinned object is not allowed is caught', bool(check_pc_link.symbol_problems('0000 T OPN2_Other\n', frozenset(['OPN2_Clock']))))
    for name in ('fm_write', 'psg_write'):
        check('link rules: a reference machine\'s entry %s is caught' % name, bool(check_pc_link.symbol_problems('0000 T %s\n' % name)))


def test_target_rules():
    """The tree check's rules for targets/pc catch what they are for, and pass a plain file."""
    import check_tree
    ok = check_tree.target_problems('targets/pc/x.c', b'#include <stdint.h>\n#include "pc_frame.h"\nstatic unsigned a = 0xC00004u, b = 0x10000u, c = 0xFF0000u, d = 0x400000u;\n')
    check('target rules: a plain file passes', not ok, str(ok))
    for bad in (b'#include "../../rig/ls_core.h"\n', b'#include "fw_cart.h"\n', b'#include "vdp_ctrl.h"\n', b'/* from Genesis Plus GX */\n',
                b'#include "cart_hw/paprium.h"\n', b'#include "paprium.h"\n', b'#include "../../legacy/026/mame.h"\n',
                b'unsigned a = 0x012345;\n', b'unsigned a = 0x0ABCDEu;\n', b'unsigned a = 0x0ABCDEU;\n', b'unsigned a = 0x0ABCDEUL;\n',  # not a game address: made up for this test
                b'unsigned a = 0x0ABCDEull;\n', b'unsigned a = 0x0ABCDEL;\n', b'unsigned a = 703710;\n', b'unsigned a = 703710u;\n',  # not a game address: made up for this test
                b'unsigned a = 0xFF1234u;\n', b'/* see 0ABCDE */\n', b'move.l $0ABCDE,d0\n', b'unsigned a = 0x512345;\n'):  # not a game address: made up for this test
        check('target rules: caught ' + bad.decode().strip(), bool(check_tree.target_problems('targets/pc/x.c', bad)))
    check('target rules: a python file may not import from rig', bool(check_tree.target_problems('targets/pc/x.py', b'from rig import build_rig\n')))
    # Our header lines on our files; another project's shader in targets/pc/shaders carries its author's notice instead, and nowhere else is that enough.
    ours = b'/* Copyright (C) 2026 Marsupial Solutions LLC\n * SPDX-License-Identifier: GPL-3.0-or-later\n */\n'
    check('header rule: our file with our lines passes', not check_tree.header_problems('targets/pc/x.c', ours))
    check('header rule: our file without them is caught', bool(check_tree.header_problems('targets/pc/x.c', b'/* x */\n')))
    check('header rule: a shader with its author\'s notice passes', not check_tree.header_problems('targets/pc/shaders/x.glsl', b'/*\n  A Shader\n  Copyright (C) 2011 Someone\n*/\n'))
    check('header rule: a public domain shader passes', not check_tree.header_problems('targets/pc/shaders/x.glsl', b'// PUBLIC DOMAIN SHADER by Someone\n'))
    check('header rule: a shader with no notice is caught', bool(check_tree.header_problems('targets/pc/shaders/x.glsl', b'void main() {}\n')))
    check('header rule: an author\'s notice is not enough outside the shaders folder', bool(check_tree.header_problems('targets/pc/x.glsl', b'// Copyright (C) 2011 Someone\n')))
    check('header rule: nor for another kind of file in it', bool(check_tree.header_problems('targets/pc/shaders/x.c', b'// Copyright (C) 2011 Someone\n')))
    # The pinned cores: each may be included by its one wrapper, by its exact path, and no other way.
    check('target rules: the Z80 core in its wrapper passes', not check_tree.target_problems('targets/pc/pc_z80.c', b'#include "chips/z80.h"\n'))
    check('target rules: the FM core in its wrapper passes', not check_tree.target_problems('targets/pc/pc_ym.c', b'#include "ym3438.h"\n'))
    for name, text in (('targets/pc/x.c', b'#include "chips/z80.h"\n'), ('targets/pc/x.c', b'#include "ym3438.h"\n'), ('targets/pc/pc_z80.c', b'#include "z80.h"\n'), ('targets/pc/pc_ym.c', b'#include "sound/ym3438.h"\n'), ('targets/pc/pc_z80.c', b'#include "ym2612.h"\n')):
        check('target rules: caught %s in %s' % (text.decode().strip(), name), bool(check_tree.target_problems(name, text)))
    check('target rules: a marked number passes', not check_tree.target_problems('targets/pc/x.c', b'#define N 0x20000u /* not a game address: a size */\n'))  # not a game address: made up for this test
    marked = []
    plain = b'unsigned a = 0x0ABCDE;\n'  # not a game address: made up for this test
    check('address rule: a new file anywhere is held to it', bool(check_tree.address_problems('rig/new.c', plain)))
    check('address rule: a number the file already held passes', not check_tree.address_problems('rig/old.c', plain, frozenset([0x0ABCDE])))  # not a game address: made up for this test
    check('address rule: a new number in an old file is caught', bool(check_tree.address_problems('rig/old.c', plain, frozenset([0x0ABCDF]))))  # not a game address: made up for this test
    check_tree.address_problems('tests/new.c', b'#define N 0x20000u /* not a game address */\n', frozenset(), marked)  # not a game address: made up for this test
    check('address rule: a line that excuses itself is listed for the reader', len(marked) == 1, str(marked))
    check('address rule: large plain numbers written in groups pass', not check_tree.address_problems('tests/new.c', b'1,189,381 frames\n'))
    check('address rule: the name of the processor passes', not check_tree.address_problems('tests/new.c', b'the 68000 reads it\n'))


def test_tree_config(tmp):
    """The tree check's two rules that come from the local configuration: its word list (tree_words), which stays outside the repository, and the
    commit the address rule counts from (base_commit), which falls back to this history's first commit when the configuration names none or one
    this history does not hold. A made-up word list that names a line every file holds makes the whole check fail."""
    import check_tree
    words = os.path.join(tmp, 'tree_words.txt')
    with open(words, 'w', encoding='utf-8') as f:
        f.write('# made up for this test\nMade-Up Word\n\n  copyright (c) 2026 marsupial solutions llc  \n')
    got = check_tree.tree_words({'tree_words': words})
    check('tree check: the word list is read from the file the configuration names, notes and blank lines left out',
          got == ['made-up word', 'copyright (c) 2026 marsupial solutions llc'], str(got))
    check('tree check: no word list without one', check_tree.tree_words({}) == [] and check_tree.tree_words(None) == [])
    roots = subprocess.run(['git', '-C', REPO, 'rev-list', '--max-parents=0', 'HEAD'], capture_output=True, text=True).stdout.split()
    head = subprocess.run(['git', '-C', REPO, 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
    check('tree check: with no base commit named, the address rule counts from this history\'s first commit',
          bool(roots) and check_tree.base_commit({}) == roots[-1])
    check('tree check: a base commit this history holds is used', check_tree.base_commit({'base_commit': head}) == head)
    check('tree check: a base commit this history does not hold falls back to its first commit',
          check_tree.base_commit({'base_commit': 'f' * 40}) == roots[-1])
    cfg = os.path.join(tmp, 'tree_words.json')
    with open(cfg, 'w', encoding='utf-8') as f:
        json.dump({'tree_words': words, 'base_commit': head}, f)
    env = dict(os.environ)
    env.pop('PAPRIUM_PC_CONFIG', None)
    r = subprocess.run([sys.executable, '-I', '-B', os.path.join(HERE, 'check_tree.py'), '--config', cfg], capture_output=True, text=True, env=env)
    check('tree check: a word of the list in the files fails the check, named as a word that does not belong',
          r.returncode == 1 and 'holds a word that does not belong in this repository' in r.stdout and '2 words looked for' in r.stdout, r.stdout[-300:])


def test_rom_scan():
    """The scan for pieces of the ROM, on a fixture made up here from a random generator: no game byte is involved."""
    import random
    import check_tree
    fixture = random.Random(20261104).randbytes(1 << 15) + bytes(range(64)) + bytes(64) + b''.join(v.to_bytes(2, 'big') for v in range(32))
    head = b'/* header */\n'
    piece = fixture[0x1234:0x1234 + 48]
    pairs = bytes(piece[i ^ 1] for i in range(len(piece)))
    words = [piece[i] << 8 | piece[i + 1] for i in range(0, len(piece), 2)]
    longs = [words[i] << 16 | words[i + 1] for i in range(0, len(words), 2)]
    cases = {
        'raw bytes': piece,
        'exactly 16 raw bytes at an odd offset': b'abc' + piece[:16] + b'z',
        'raw bytes, pairs swapped': pairs,
        'exactly 16 raw bytes, pairs swapped, at an odd offset': b'a' + pairs[:16] + b'z',
        'hexadecimal text': b'x = "' + piece.hex().encode() + b'";\n',
        'hexadecimal text in capitals': b'x = "' + piece.hex().upper().encode() + b'";\n',
        'hexadecimal text, pairs swapped': b'x = "' + pairs.hex().encode() + b'";\n',
        'hexadecimal text with spaces': b'x = "' + piece.hex(' ').encode() + b'";\n',
        'hexadecimal text in words': b'x = "' + piece.hex(' ', 2).encode() + b'";\n',
        'C byte array': b'const unsigned char x[] = {' + b', '.join(b'0x%02X' % v for v in piece) + b'};\n',
        'C byte array with suffixes': b'const unsigned x[] = {' + b', '.join(b'0x%02Xu' % v for v in piece) + b'};\n',
        'C byte array, comments between the rows': b'char x[] = {\n' + b''.join(b'/* %d */ %d,\n' % (i, v) for i, v in enumerate(piece)) + b'};\n',
        'C word array': b'const unsigned short x[] = {' + b', '.join(b'0x%04X' % v for v in words) + b'};\n',
        'C word array with capital suffixes': b'const unsigned x[] = {' + b', '.join(b'0x%04XU' % v for v in words) + b'};\n',
        'C word array in the host order': b'const unsigned short x[] = {' + b', '.join(b'0x%04X' % (((v & 255) << 8) | (v >> 8)) for v in words) + b'};\n',
        'C long array': b'const unsigned x[] = {' + b', '.join(b'0x%08XUL' % v for v in longs) + b'};\n',
        'C long array in the host order': b'const unsigned x[] = {' + b', '.join(b'0x%08X' % int.from_bytes(v.to_bytes(4, 'big'), 'little') for v in longs) + b'};\n',
        'decimal list': b'x = [' + b', '.join(b'%d' % v for v in piece) + b']\n',
        'decimal list with no spaces': b'x = [' + b','.join(b'%d' % v for v in piece) + b']\n',
        'decimal word list': b'x = [' + b', '.join(b'%d' % v for v in words) + b']\n',
        'decimal word list, in the host order': b'x = [' + b', '.join(b'%d' % (((v & 255) << 8) | (v >> 8)) for v in words) + b']\n',
        'decimal long list': b'x = [' + b', '.join(b'%d' % v for v in longs) + b']\n',
        'python escapes': b"x = b'" + b''.join(b'\\x%02x' % v for v in piece) + b"'\n",
        'python escapes, pairs swapped': b"x = b'" + b''.join(b'\\x%02x' % v for v in pairs) + b"'\n",
        'octal escapes': b'char x[] = "' + b''.join(b'\\%03o' % v for v in piece) + b'";\n',
        'assembler bytes': b'  dc.b ' + b','.join(b'$%02X' % v for v in piece) + b'\n',
        'assembler words': b'  dc.w ' + b','.join(b'$%04X' % v for v in words) + b'\n',
        'calls with one value each': b''.join(b'  put(0x%02X);\n' % v for v in piece),
    }
    for name, text in cases.items():
        found = check_tree.rom_problems({'tests/x': head + text}, fixture)
        check('ROM scan: caught ' + name, bool(found), name)
    quiet = {
        'a plain text': b'A plain sentence that is long enough to hold many windows and is not in the fixture at all.\n',
        '15 raw bytes': piece[:15],
        '15 values in a list': b'x = [' + b', '.join(b'%d' % v for v in piece[:15]) + b']\n',
        '7 words in a list': b'x = [' + b', '.join(b'0x%04X' % v for v in words[:7]) + b']\n',
        'a ramp that the fixture also holds': b'x = [' + b', '.join(b'%d' % v for v in range(64)) + b']\n',
        'a ramp of words that the fixture holds as bytes': b'x = [' + b', '.join(b'%d' % (v << 8 | v + 1) for v in range(0, 60, 2)) + b']\n',
        'a ramp of small words': b'x = [' + b', '.join(b'%d' % v for v in range(32)) + b']\n',
        'zeros that the fixture also holds': bytes(64),
    }
    for name, text in quiet.items():
        found = check_tree.rom_problems({'tests/x': head + text}, fixture)
        check('ROM scan: quiet on ' + name, not found, str(found))
    # Two files at once, each reported, and the swap helper with an odd length.
    found = check_tree.rom_problems({'tests/a': piece, 'tests/b': b'clean\n', 'tests/c': pairs}, fixture)
    check('ROM scan: reports each file', sorted(x.split(':')[0] for x in found) == ['tests/a', 'tests/c'], str(found))
    check('ROM scan: the swap keeps the last odd byte', check_tree.swapped_pairs(b'abc') == b'bac')


def test_pc_frame(tmp):
    """The PC port's shared interface alone: it compiles without warnings and its one rule for a write does what its comments say."""
    exe = os.path.join(tmp, 'pc_frame_test.exe')
    inc = os.path.join(REPO, 'targets', 'pc')
    r = subprocess.run([harness.GCC, '-Wall', '-Wextra', '-Werror', '-std=gnu11', '-O1', '-I', inc, os.path.join(HERE, 'pc_frame_test.c'), '-o', exe],
                       capture_output=True, text=True, env=harness.gcc_env())
    check('the PC frame interface compiles without warnings', r.returncode == 0, r.stderr[:500])
    if r.returncode:
        return
    r = subprocess.run([exe, os.path.join(tmp, 'pc_frame_test.bin')], capture_output=True, text=True, env=harness.gcc_env())
    check('the PC frame interface on made-up data', r.returncode == 0, ' '.join(r.stdout.splitlines()[-2:])[:300])


import test_pc_machine  # noqa: E402
import pc_arcade_scenes  # noqa: E402
import test_launcher  # noqa: E402


def main():
    tmp = sys.argv[1] if len(sys.argv) > 1 else tempfile.mkdtemp(prefix='ptest')
    os.makedirs(tmp, exist_ok=True)
    print('scratch folder', tmp)
    test_target_rules()
    test_tree_config(tmp)
    test_rom_scan()
    test_pc_link_rules(tmp)
    test_pc_frame(tmp)
    test_gate(tmp)
    test_decoder()
    test_discovery()
    test_patch_source()
    test_switches(tmp)
    test_pipeline(tmp)
    test_command_line(tmp)
    test_fallback_log(tmp)
    test_rig(tmp)
    test_tools(tmp)
    test_pc_machine.run(tmp, os.environ.get('PAPRIUM_PC_CONFIG'), check)
    pc_arcade_scenes.run_unit(tmp, harness.GCC, harness.gcc_env(), check)
    if os.environ.get('PAPRIUM_PC_CONFIG'):
        test_form_fuzz(tmp, os.environ['PAPRIUM_PC_CONFIG'])
        test_fw_cart(tmp, os.environ['PAPRIUM_PC_CONFIG'])
        test_launcher.run(tmp, os.environ['PAPRIUM_PC_CONFIG'], check)
    else:
        print('skip the form fuzz: PAPRIUM_PC_CONFIG names no local configuration')
    r = subprocess.run([sys.executable, '-I', '-B', os.path.join(HERE, 'check_tree.py')], capture_output=True, text=True)
    print(r.stdout.strip())
    check('tree check', r.returncode == 0)
    bad = [n for n, ok in RESULTS if not ok]
    print('%d checks, %d failed' % (len(RESULTS), len(bad)))
    if bad:
        print('RESULT: FAIL: ' + '; '.join(bad))
        return 1
    print('RESULT: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
