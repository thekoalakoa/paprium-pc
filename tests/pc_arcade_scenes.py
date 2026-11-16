# Copyright (C) 2026 Marsupial Solutions LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""The Pocket core's menu entries the PC took in its round after the consoles: Arcade Mode, Arcade Coins, Attract Only, 3-Player Mode with a third
player, Console Reset and Aspect Ratio. As scenes that tests/test_pc_machine.py runs with its own runners (its whole-program test gives play: a hidden
window with a key script, returning the folder, the run and the settings file after), and the ports' own unit test (tests/pc_ports_test.c) with
the planted faults it must catch. Each check goes to check."""
import os
import shutil
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
PC = os.path.join(os.path.dirname(HERE), 'targets', 'pc')
RUNTIME = os.path.join(os.path.dirname(HERE), 'runtime')

# Planted faults: file, text, what replaces it, the group of tests/pc_ports_test.c that must fail.
PLANTS = [
    ('pc_scum.c', '    p->tap = a->three_players && a->mode == PC_ARCADE_OFF;', '    p->tap = a->three_players;', 'ports'),
    ('pc_scum.c', '(a->coins == PC_COINS_2_1 ? PC_STICK_TWO_COINS : 0u)', '(a->coins == PC_COINS_2_1 ? PC_STICK_INF_CREDITS : 0u)', 'ports'),
    ('pc_io.c', '    case 0: return STICK_ID | (ext_there() ? STICK_EXT_PAD : 0u);', '    case 0: return STICK_ID;', 'ext'),
    ('pc_io.c', '        b &= ~(uint32_t)(PC_PAD_MODE | PC_PAD_X | PC_PAD_Y | PC_PAD_Z);', '        b &= ~(uint32_t)0;', 'ext'),
    ('pc_io.c', '        if (bit && !stick.ack_before && stick.coins)', '        if (bit && stick.coins)', 'coins'),
    ('pc_io.c', '        if (ports.stick_coin && (buttons & PC_PAD_MODE)', '        if ((buttons & PC_PAD_MODE)', 'stick'),
    ('pc_io.c', '        return (data & 0x80u) | TAP_ID;', '        return (data & 0x80u) | (TAP_ID & ~0x40u);', 'tap'),
    ('pc_io.c', '    if (!(shown & 3u))\n        tap.n = (shown >> 4) & 7u;', '    tap.n = (shown >> 4) & 7u;', 'tap'),
    ('pc_io.c', '        p->count = (p->count + 1u) & 3u;', '        p->count = p->count + 1u;', 'count'),
    ('pc_io.c', '    if (!p->driven && !p->th && t >= p->free_from + TAP_PAD_FLOAT)', '    if (0)', 'float'),
    ('pc_scum.c', 'static void at_switch_point(void) { stage_word_set(served_stage(&asked)); }', 'static void at_switch_point(void) { }', 'stage'),
]


def _build_unit(folder, sources, gcc, gcc_env):
    exe = os.path.join(folder, 'pc_ports_test.exe')
    r = subprocess.run([gcc, '-std=gnu11', '-O1', '-Wall', '-Werror', '-DPT_PC_BUILD', '-I', sources, '-I', RUNTIME, '-o', exe,
                        os.path.join(HERE, 'pc_ports_test.c'), os.path.join(sources, 'pc_io.c'), os.path.join(sources, 'pc_scum.c')],
                       capture_output=True, text=True, env=gcc_env)
    return exe if r.returncode == 0 else None, r.stdout + r.stderr


def run_unit(tmp, gcc, gcc_env, check):
    """tests/pc_ports_test.c on the program's own pc_io.c and pc_scum.c, then once on each planted fault, which must fail its group."""
    folder = os.path.join(tmp, 'ports_unit')
    os.makedirs(folder, exist_ok=True)
    exe, out = _build_unit(folder, PC, gcc, gcc_env)
    check('the ports\' unit test builds', exe is not None, out[-400:])
    if not exe:
        return
    r = subprocess.run([exe], capture_output=True, text=True, timeout=120)
    check('the ports\' unit test passes: the menu values on the ports as the Pocket core puts them, its Grand Stick III, its EA 4-Way Play and its pads',
          r.returncode == 0 and r.stdout.rstrip().endswith(' 0 failed'), r.stdout[-600:])
    for n, (name, text, planted, group) in enumerate(PLANTS):
        with open(os.path.join(PC, name), encoding='utf-8') as f:
            source = f.read()
        check('plant in %s for the group %s: its text is there once' % (name, group), source.count(text) == 1)
        pf = os.path.join(folder, 'plant%d' % n)
        os.makedirs(pf, exist_ok=True)
        for other in ('pc_io.c', 'pc_scum.c', 'pc_io.h', 'pc_scum.h', 'pc_config.h', 'pc_console.h', 'pc_vdp.h', 'pc_frame.h'):
            with open(os.path.join(PC, other), encoding='utf-8') as f:
                body = f.read()
            if other == name:
                body = body.replace(text, planted)
            with open(os.path.join(pf, other), 'w', encoding='utf-8', newline='\n') as f:
                f.write(body)
        exe2, out2 = _build_unit(pf, pf, gcc, gcc_env)
        if not exe2:
            check('plant in %s (%s): builds' % (name, group), False, out2[-300:])
            continue
        r2 = subprocess.run([exe2], capture_output=True, text=True, timeout=120)
        check('plant in %s: caught by the group %s' % (name, group), r2.returncode != 0 and ('group %s: FAIL' % group) in r2.stdout, r2.stdout[-300:])


def menu(play, own, menu_items, check):
    """The Pocket core's entries in the menus, in its order with its names, and the lines they write."""
    d, r, text = play('arcade_names', None, end=12)
    items = menu_items(r.stdout)
    scum = [k[1] for k in items if k[0] == 'Scum!']
    check('Scum!: the Pocket core\'s entries in its order, Arcade Mode, Arcade Coins, Attract Only, the stage, 6-Button for three players, 3-Player Mode',
          scum == ['Health: no damage', 'Pills: a Blu pill is not spent', 'Lives: a death is not spent', 'Arcade Mode: Off', 'Arcade Coins: 1 Coin 1 Credit',
                   'Attract Only', 'Arcade stage: off', 'Coins, Attract Only, stage: with On or Multiplayer', '6-Button pad, player 1', '6-Button pad, player 2',
                   '6-Button pad, player 3', '3-Player Mode', 'Soft Reset, for these'], str(scum))
    check('Arcade Mode and Arcade Coins: the Pocket core\'s lists, Off and 1 Coin 1 Credit ticked',
          [k[1] for k in items if k[0] == 'Arcade Mode (list)'] == ['Off', 'On', 'Multiplayer', 'Stick Only']
          and [k[1] for k in items if k[0] == 'Arcade Coins (list)'] == ['1 Coin 1 Credit', '2 Coins 1 Credit', 'Inf. Credits (0012)']
          and 'tick' in items.get(('Arcade Mode (list)', 'Off'), ('', '', ''))[2] and 'tick' in items.get(('Arcade Coins (list)', '1 Coin 1 Credit'), ('', '', ''))[2]
          and 'grey' in items.get(('Scum!', 'Coins, Attract Only, stage: with On or Multiplayer'), ('', '', ''))[2])
    check('Game: Console Reset beside Soft Reset', [k[1] for k in items if k[0] == 'Game'] == ['Pause', 'Soft Reset', 'Console Reset', 'Quit'])
    video = [k[1] for k in items if k[0] == 'Video']
    check('Video: Aspect Ratio, the Pocket core\'s Original and Corrected, then 4:3 (ticked, the default) and square pixels, and the shape key',
          video[8:13] == ['Aspect Ratio: Original', 'Aspect Ratio: Corrected', 'Aspect Ratio: 4:3', 'Aspect Ratio: Square pixels', 'Next aspect ratio']
          and 'tick' in items.get(('Video', 'Aspect Ratio: 4:3'), ('', '', ''))[2] and items.get(('Video', 'Next aspect ratio'), ('',))[0] == 'F2', str(video))
    # Each entry writes its own line, after the player's lines.
    for name, presses, line, said in (
            ('arcade_multi', ['Right'] * 4 + ['Down'] * 3 + ['Right', 'Down', 'Down', 'Return'], 'arcade_mode=multiplayer\n', 'menu: Arcade Mode: Multiplayer, from'),
            ('arcade_stick', ['Right'] * 4 + ['Down'] * 3 + ['Right'] + ['Down'] * 3 + ['Return'], 'arcade_mode=stick_only\n', 'menu: Arcade Mode: Stick Only, from'),
            ('coins_inf', ['Right'] * 4 + ['Down'] * 4 + ['Right', 'Down', 'Down', 'Return'], 'arcade_coins=infinite\n', 'menu: Arcade Coins: Inf. Credits (0012)'),
            ('attract', ['Right'] * 4 + ['Down'] * 5 + ['Return'], 'attract_only=on\n', 'menu: Attract Only on, from'),
            ('three', ['Right'] * 4 + ['Down'] * 11 + ['Return'], 'three_players=on\n', 'menu: 3-Player Mode on, from'),
            ('pad3', ['Right'] * 4 + ['Down'] * 10 + ['Return'], 'pad3=three\n', 'menu: Player 3: a three-button pad.'),
            ('shape', ['Right'] + ['Down'] * 8 + ['Return'], 'display=original\n', 'menu: Aspect Ratio: Original')):
        d, r, text = play(name, presses)
        check('Scum! / Video, %s: %s is added, and the menu says so' % (name, line.strip()), text == own + line and said in r.stdout, text[-200:] + r.stdout[-300:])
    # A file of an earlier version: an arcade_stage line alone was the arcade mode at that stage, so the menu shows Arcade Mode On, and a stage chosen now
    # writes arcade_mode beside it, so the file keeps meaning what it meant.
    d, r, text = play('arcade_old', ['Right'] * 4 + ['Down'] * 6 + ['Right', 'Down', 'Return'], cfg_extra='arcade_stage=6\n')
    items = menu_items(r.stdout)
    check('an earlier version\'s arcade_stage=6: Arcade Mode shows On; a stage chosen writes arcade_mode=on with it', text == own + 'arcade_stage=7\narcade_mode=on\n'
          and ('Scum!', 'Arcade Mode: On') in items and ('Scum!', 'Arcade stage: Ice Factory') in items, text[-200:] + str([k for k in items if k[0] == 'Scum!']))
    d, r, text = play('arcade_back', None, cfg_extra='arcade_mode=multiplayer\narcade_coins=2coins\nattract_only=on\nthree_players=on\ndisplay=corrected\n', end=12)
    items = menu_items(r.stdout)
    check('the lines read back: the menu ticks Multiplayer, 2 Coins 1 Credit, Attract Only, 3-Player Mode and Corrected, and says where player 3 is',
          'tick' in items.get(('Arcade Mode (list)', 'Multiplayer'), ('', '', ''))[2] and 'tick' in items.get(('Arcade Coins (list)', '2 Coins 1 Credit'), ('', '', ''))[2]
          and 'tick' in items.get(('Scum!', 'Attract Only'), ('', '', ''))[2] and 'tick' in items.get(('Scum!', '3-Player Mode'), ('', '', ''))[2]
          and 'tick' in items.get(('Video', 'Aspect Ratio: Corrected'), ('', '', ''))[2] and ('Scum!', 'Player 3: in the stick\'s EXT socket') in items
          and 'Scum!: the Grand Stick III in port 1, its switches 2D' in r.stdout, str([k for k in items if k[0] == 'Scum!']) + r.stdout[-300:])
    for name, (ok, detail) in stage_note(play, own, menu_items).items():
        check('Scum!, the stage list on ' + name, ok, detail)
    for name, (ok, detail) in unit_article(play, own, menu_items).items():
        check('the console line names ' + name, ok, detail)
    d, r, text = play('console_reset', ['Down', 'Down', 'Return'], end=30)
    check('Game, Console Reset: the console\'s reset button, and the game goes on (on this made-up program too)', r.returncode == 0 and 'Console Reset at frame' in r.stdout
          and 'menu: Console Reset: the console\'s reset button.' in r.stdout and 'RESULT: PASS' in r.stdout, r.stdout[-400:])


STAGE_NOTE_ON = 'Stage Off = first stage; arcade off: Arcade Mode'
STAGE_NOTE_OFF = 'Coins, Attract Only, stage: with On or Multiplayer'


def stage_note(play, own, menu_items):
    """The stage list only picks the stage, as on the Pocket core; Arcade Mode is its own entry. While the arcade mode is on, the grey line under the
    stage says that the list's Off is the game's own first stage and where the arcade mode is turned off; otherwise it says when Arcade Coins, Attract
    Only and the stage act. On an earlier version's file (an arcade_stage line and no arcade_mode line, so Arcade Mode On), Off chosen in the list
    writes arcade_stage=off and keeps arcade_mode=on. Returns {scene: (ok, detail)}."""
    found = {}
    for mode, extra, note in (('off', '', STAGE_NOTE_OFF), ('on', 'arcade_mode=on\n', STAGE_NOTE_ON), ('multiplayer', 'arcade_mode=multiplayer\n', STAGE_NOTE_ON),
                              ('stick_only', 'arcade_mode=stick_only\n', STAGE_NOTE_OFF)):
        d, r, text = play('stage_note_' + mode, None, cfg_extra=extra, end=12)
        items = menu_items(r.stdout)
        scum = [k[1] for k in items if k[0] == 'Scum!']
        found['Arcade Mode %s: the grey line under the stage is "%s"' % (mode, note)] = (
            r.returncode == 0 and scum[7:8] == [note] and 'grey' in items.get(('Scum!', note), ('', '', ''))[2], str(scum))
    d, r, text = play('stage_note_old_off', ['Right'] * 4 + ['Down'] * 6 + ['Right'] + ['Up'] * 24 + ['Return'], cfg_extra='arcade_stage=24\n')
    items = menu_items(r.stdout)
    found['an earlier version\'s arcade_stage=24, Off chosen in the list: arcade_stage=off with arcade_mode=on, Arcade Mode still On, and the first stage told'] = (
        r.returncode == 0 and text == own + 'arcade_stage=off\narcade_mode=on\n' and ('Scum!', 'Arcade Mode: On') in items
        and ('Scum!', 'Arcade stage: off') in items and ('Scum!', STAGE_NOTE_ON) in items
        and 'menu: Arcade stage: off, the game\'s own first stage.' in r.stdout, text[-200:] + r.stdout[-400:])
    return found


def unit_article(play, own, menu_items):
    """The console's line at the start names a separate Mega CD unit with its article: an Unknown CD, a Mega-CD / Sega CD. Returns {scene: (ok, detail)}."""
    found = {}
    for unit, words in (('unknown', ', an Unknown CD'), ('megacd', ', a Mega-CD / Sega CD'), ('megacd2', ', a Mega-CD 2 / Sega CD 2')):
        d, r, text = play('unit_' + unit, None, cfg_extra='megacd=%s\n' % unit, end=12)
        line = [x for x in r.stdout.splitlines() if x.startswith('the console: ')]
        found['megacd=%s as "%s"' % (unit, words[2:])] = (r.returncode == 0 and len(line) == 1 and (line[0].endswith(words) or (words + '. ') in line[0]),
                                                          str(line))
    return found


SCENES = {'stage_note': stage_note, 'unit_article': unit_article}

# Planted faults in pc_main.c for the scenes above: the text, what replaces it, the scene function, the scene that must be lost.
MENU_PLANTS = [
    ('        if (a.mode == PC_ARCADE_ON || a.mode == PC_ARCADE_MULTIPLAYER)\n            pc_menu_add_item(m, "Stage Off',
     '        if (0)\n            pc_menu_add_item(m, "Stage Off', 'stage_note', 'Arcade Mode on: the grey line under the stage is "%s"' % STAGE_NOTE_ON),
    ('        settings->arcade_mode = pc_config_arcade_mode(settings);\n        settings->arcade_stage = arg;',
     '        settings->arcade_mode = arg ? pc_config_arcade_mode(settings) : 0;\n        settings->arcade_stage = arg;', 'stage_note',
     'an earlier version\'s arcade_stage=24, Off chosen in the list: arcade_stage=off with arcade_mode=on, Arcade Mode still On, and the first stage told'),
    ('(mach.unit == PC_MEGACD_UNKNOWN ? ", an " : ", a ")', '", a "', 'unit_article', 'megacd=unknown as "an Unknown CD"'),
]


def menu_plants(exe, sdl, folder, own, env, menu_script, menu_items, pc_build, check):
    """Each planted fault of MENU_PLANTS in pc_main.c, on a copy of the made-up program exe linked with it (its objects in exe + '.rsp'): each must lose
    its scene. menu_script and menu_items are tests/test_pc_machine.py's helpers; pc_build its module."""
    with open(exe + '.rsp', encoding='utf-8') as f:
        objects = [line.strip().strip('"') for line in f if line.strip()]
    with open(os.path.join(PC, 'pc_main.c'), newline='') as f:
        text = f.read()
    for n, (a, b, scenes, scene) in enumerate(MENU_PLANTS):
        check('plant %d in pc_main.c (%s): its text is there once' % (n, scene), text.count(a) == 1)
        where = os.path.join(folder, 'stage_plant%d' % n)
        os.makedirs(where, exist_ok=True)
        src = os.path.join(where, 'pc_main.c')
        with open(src, 'w', newline='') as f:
            f.write(text.replace(a, b))
        job = pc_build.machine_jobs(where, '-O1', ('pc_main',))[0]
        obj = pc_build.compile_jobs([(src, os.path.join(where, 'pc_main.o'), job[2], [])], 1)[0]
        planted = os.path.join(where, 'pc_port.exe')
        pc_build.link([obj if os.path.basename(o) == 'pc_main.o' else o for o in objects], planted, pc_build.sdl_link_args(sdl) + ['-lwinmm'])
        shutil.copy2(pc_build.sdl_dll(sdl), os.path.join(where, 'SDL2.dll'))

        def play(name, presses, cfg_extra='', end=40):
            d = os.path.join(where, name)
            os.makedirs(d, exist_ok=True)
            cfg = os.path.join(d, 'play.cfg')
            with open(cfg, 'w', encoding='ascii', newline='') as f:
                f.write(own + cfg_extra)
            keys = os.path.join(d, 'keys.txt')
            if presses is None:
                with open(keys, 'w', encoding='ascii') as f:
                    f.write('%d down Escape\n%d up Escape\n' % (end, end + 1))
            else:
                menu_script(keys, presses, end=end)
            r = subprocess.run([planted, '--hidden-window', '--config', cfg, '--run-dir', d, '--key-script', keys, '--render', '--menu-dump'],
                               capture_output=True, text=True, env=env, timeout=180)
            with open(cfg, encoding='ascii', newline='') as f:
                return d, r, f.read()
        check('plant %d in pc_main.c (%s): caught' % (n, scene), not SCENES[scenes](play, own, menu_items)[scene][0])
