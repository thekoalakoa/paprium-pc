# Copyright (C) 2026 Marsupial Solutions LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""The Console menu's two lists and the consoles' own checks in the whole program (targets/pc/pc_console.h), as scenes that tests/test_pc_machine.py
runs with its own runners: its whole-program test gives play (a hidden window with a key script, returning the folder, the run and the settings
file after), its console-program test gives run (the made-up program with words of its work RAM logged). Each check goes to check."""

# Console plants for tests/test_pc_machine.py's console test (its CONSOLE_PLANTS takes these in): each must make the named group of
# tests/pc_console_test.c fail. The unit rows, TMSS and the TeraDrive's answer.
PLANTS = [
    ('pc_addons.c', '    { 0x0000u, 0x424Fu, 0x0000u, 0x0000u, 0x0000u, 0x0032u, 0x0031u, 0x3143u },',
     '    { 0x0000u, 0x424Fu, 0x0000u, 0x0000u, 0x0000u, 0x0032u, 0x0031u, 0x3142u },', 'models'),
    ('pc_addons.c', '    { 0x574Fu, 0x0000u, 0x0000u, 0x0000u, 0x424Fu, 0x0032u, 0x0000u, 0x0000u },',
     '    { 0x574Fu, 0x0000u, 0x0000u, 0x0000u, 0x424Fu, 0x0031u, 0x0000u, 0x0000u },', 'models'),
    ('pc_addons.c', '    pc_addons_on = x32 || megacd || tmss_on;', '    pc_addons_on = x32 || megacd;', 'tmss'),
    ('pc_addons.c', '#define TMSS_BYTE       0x01u', '#define TMSS_BYTE       0xFFu', 'tmss'),
    ('pc_addons.c', '    if (size == 1 && a == 0xA14101u)\n        cart_mapped = (int)(v & 1u);', '    if (size == 1 && a == 0xA14101u)\n        cart_mapped = 1;', 'tmss'),
    ('pc_addons.c', '    cart_mapped = 1;\n    pc_addons_on', '    pc_addons_on', 'tmss'),
    ('pc_bus.c', 'return pc_addons_on ? pc_addons_low_read(size, a, v) : v;', 'return v;', 'tmss'),
    ('pc_addons.c', '    pc_vdp_set_teradrive(mach.teradrive);', '', 'teradrive'),
    ('pc_vdp.c', '                td_words[TD_PLACE()] = (uint16_t)v;', '                ;', 'teradrive'),
    ('pc_vdp.c', '        if ((S.regs[1] & 0x80u) && teradrive)', '        if (teradrive && 0)', 'teradrive'),
]


def menu_rules(play, menu_items, check):
    """The hardware rules said in the menu, and on the console when the program starts."""
    d, r, text = play('rules', None, cfg_extra='console=nomad\nmegacd=megacd2\ncpumate=on\n', end=12)
    items = menu_items(r.stdout)

    def look(menu, label, default=''):
        return items.get((menu, label), ('', '', default))[2]
    check('the rules in the menu: a Nomad has no port for a Mega CD, said in a grey line; CPU MATE grey, with its reason',
          'grey' in look('Console', 'Nomad: no port, so no Mega CD') and 'grey' in look('Console', 'CPU MATE, with the Mega CD')
          and 'grey' in look('Console', 'CPU MATE: needs a Mega CD unit') and 'tick' in look('Mega CD units (list)', 'Mega-CD 2 / Sega CD 2')
          and 'the console: Nomad (USA), version register A1. The Nomad has no expansion port, so the Mega-CD 2 / Sega CD 2 is off.' in r.stdout,
          str([k for k in items if k[0] == 'Console']) + r.stdout[-400:])
    d, r, text = play('rules2', None, cfg_extra='console=megajet\nmegacd=laseractive\n', end=12)
    items = menu_items(r.stdout)
    check('the rules in the menu: a LaserActive is the console, with the Console list\'s region', ('Console', 'LaserActive is the console (Japan)') in items
          and 'grey' not in look('Console', 'CPU MATE, with the Mega CD', 'grey')
          and 'the console: LaserActive (Japan), version register 01. The LaserActive is a whole console, so the game finds it (Japan).' in r.stdout,
          str([k for k in items if k[0] == 'Console']) + r.stdout[-400:])


def menu_lists(play, own, check):
    """A choice from each list goes into the player's file, and the menu says when it acts and when a rule changes it."""
    d, r, text = play('console', ['Right'] * 6 + ['Down'] * 4 + ['Return'])
    check('Console, Genesis 3 from its list: console=genesis3 is added', text == own + 'console=genesis3\n', text[-200:])
    check('the menu says when the change acts', 'menu: Console: Genesis 3, from the next Soft Reset (Game menu).' in r.stdout, r.stdout[-300:])
    d, r, text = play('console_old', ['Right'] * 6 + ['Down'] * 3 + ['Return'], cfg_extra='region=us\nconsole=genesis\n')
    check('Console, Genesis 3, with an earlier version\'s region line: it goes back to auto, so the region cannot pick another console',
          text == own + 'region=auto\nconsole=genesis3\n', text[-200:])
    d, r, text = play('megacd', ['Right'] * 5 + ['Down', 'Right'] + ['Down'] * 5 + ['Return'])
    check('Console, Mega CD, LaserActive from its list: megacd=laseractive is added, and the menu says it is the console', text == own + 'megacd=laseractive\n'
          and 'menu: Mega CD: LaserActive, from the next Soft Reset (Game menu). The LaserActive is a whole console, so the game finds it (Japan).' in r.stdout,
          text[-200:] + r.stdout[-300:])
    d, r, text = play('megacd_dropped', ['Right'] * 5 + ['Down', 'Right', 'Down', 'Return'], cfg_extra='console=genesis3\n')
    check('Console, Mega CD on a Genesis 3: megacd=megacd is added, and the menu says the Genesis 3 has no expansion port', text == own + 'console=genesis3\nmegacd=megacd\n'
          and 'menu: Mega CD: Mega-CD / Sega CD, from the next Soft Reset (Game menu). The Genesis 3 has no expansion port, so the Mega-CD / Sega CD is off.' in r.stdout,
          text[-200:] + r.stdout[-300:])


def program_checks(run, lw, ram, check):
    """TMSS's check and the TeraDrive's test in the whole program, on the made-up program: the word at $000120 before, with $A14101 bit 0 clear and
    after (logged at ram + 12, + 14 and + 0x20), and the word read back at $10000 in the 128 KB mode (ram + 0x22)."""
    words = ['--log-word', '%06X' % (ram + 12), '--log-word', '%06X' % (ram + 14), '--log-word', '%06X' % (ram + 0x20), '--log-word', '%06X' % (ram + 0x22)]
    for name, args, tmss, tera in (('t_megadrive', (), 0, 0), ('t_megadrive2', ('--console', 'console=megadrive2'), 1, 0),
                                   ('t_wondermega', ('--console', 'megacd=wondermega'), 1, 0), ('t_teradrive', ('--console', 'console=teradrive'), 0, 1),
                                   ('t_teradrive_cdx', ('--console', 'console=teradrive', '--console', 'megacd=cdx'), 1, 0)):
        rd, rr, rows = run(name, *args, logged=words)
        ok = rr.returncode == 0 and len(rows) == 12
        last = rows[-1] if ok else {}
        before, held, after, back = (lw(last, k) for k in range(4)) if ok else (None,) * 4
        check('the console\'s made-up program, %s: $000120 reads 0101 only while $A14101 bit 0 is clear on a console with TMSS (%d), and the 128 KB test '
              'reads 5A02 only on a TeraDrive (%d)' % (name, tmss, tera),
              ok and before == after and before != 0x0101 and held == (0x0101 if tmss else before) and back == (0x5A02 if tera else 0xA501),
              rr.stdout[-300:] if not ok else str(last))
