<!-- Copyright (C) 2026 Marsupial Solutions LLC
     SPDX-License-Identifier: GPL-3.0-or-later -->
# The lockstep rig

The translated program was checked by running it beside a reference 68000, block by block, on whole runs of the game. This
folder holds the parts of that rig that build and run from this repository alone.

## The comparison core

`ls_core.c` compares a reference 68000 with the translated program, block by block. It knows the reference only through the
`LsRef` interface in `ls_core.h`.

- The reference runs each block first and is the only side with a machine behind it. Its device reads and all its writes go
  in a queue.
- The translated block then runs. Its device reads pop the queue, so both sides get the same answers by construction. Its
  writes are compared with the queue, in order.
- At the next block entry the program counter, all registers, the interrupt mask and the flags live there must match. At each
  frame end the whole work RAM must match.

A difference writes `diff_NNNN.txt` to the run folder: the kind, the block, the counters, both sides' registers, the last 64
blocks and what was left in the queue. `tests/rig_unit.c` tests it on small made-up programs, with no machine behind it.

## The firmware as the cart

`fw/fw_cart.c` stands on the cart's bus with the firmware's host build (its core and `board/host/s1_board.c`, from a copy of the
firmware tree named in the local configuration, no CPU MATE brain). It does what the real cart does in logic: the FM view, the
window as a FIFO, the byte write rule. The sprite decoder is the host board's own model. The synth is a stand-in that keeps its
block schedule and mixes nothing. Time: the firmware runs in its own fiber on the board timer, and each cart access first runs it up
to the caller's clock. `tests/fw_cart_test.c` tests it on a made-up ROM. `build_rig.py`'s `firmware_jobs` is the list of what it
compiles.

## Not in this repository

The reference machine was a patched copy of Genesis Plus GX, with its 68000 core from Musashi. Their terms forbid commercial use,
which does not fit a repository under the GPL. So the parts that build or run only with that copy are not published here: the
script that patches it, the rig's driver, its glue and its route interface, the form fuzz's program and its FX68K referee bench,
and the two files that ran the PC program's video chip model beside it. `build_rig.py`'s
`reference` and `lockstep` modes link those parts and do not work from this repository; its `replay` mode and its firmware list do.
`tool/formfuzz.py` prepares the form fuzz's slots, and summarises its results, without them.

Nothing from the reference machine is in the PC program: `tests/check_tree.py` holds that nothing under `targets/pc` includes
anything from `rig/` or names a reference emulator, and `tests/check_pc_link.py` checks the finished program.
