# Copyright (C) 2026 Marsupial Solutions LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Which of the firmware's source files the PC program compiles and links.

The list starts from the firmware's own manifest for its RELEASE image, on its x86 board, and leaves out by name:
  - board/pocket/: what only the Pocket has
  - buddy/: the CPU MATE glue, which runs the brain on the cart's MCU. This program does that part itself (pc_addons.c, pc_brain.c), and no brain is in
    it: with CPU MATE on, the brain is read at start from Little-Man Firmware's file, which the players' download carries (pc_brain.h)
  - core/trace/fatal.c: the fatal halt, which targets/pc/pc_cart.c replaces
  - legacy/: the firmware's old module legacy/026/mame.c. The Pocket's linker keeps two variables from it and drops the rest, but a PC link keeps all it
    is given, and that module holds old decoders written after another project's emulator. targets/pc/pc_fw_vars.c defines the two variables instead.
    tests/check_pc_link.py checks the link line and the finished program.
"""
import json
import os

LEFT_OUT_FOLDERS = ('board/pocket/', 'buddy/', 'legacy/')
LEFT_OUT_FILES = ('core/trace/fatal.c',)
# The wave bank's decoder, built apart because it wants signed chars by its own rule.
WAVPACK = ('bits', 'metadata', 'unpack', 'words', 'wputils', 'wvbank')


def sources(firmware_tree):
    """The firmware's source files for this program, as paths inside its tree, in the manifest's order."""
    with open(os.path.join(firmware_tree, 'fwbuild.json'), encoding='utf-8') as f:
        s1 = json.load(f)['s1']
    keep = []
    for name in list(s1['pocket']['sources']) + list(s1['pocket']['variant_sources']['s5release']):
        if name.startswith(LEFT_OUT_FOLDERS) or name in LEFT_OUT_FILES or name in keep:
            continue
        keep.append(name)
    keep.append('board/host/s1_board.c')
    return keep


def wavpack_sources():
    return ['core/wavpack/%s.c' % name for name in WAVPACK]
