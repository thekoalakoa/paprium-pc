<!-- Copyright (C) 2026 Marsupial Solutions LLC
     SPDX-License-Identifier: GPL-3.0-or-later -->
# paprium-pc

Paprium on a Windows PC, from your own copy of the game.

Paprium on PC is a program that turns your own, legally obtained copy of the Paprium game file (the ROM) into a game that
runs on Windows. It carries no game. This repository holds the tool that translates the game's 68000 program into portable
C, a runtime for that C, a model of the console and the cart written by our team from our own study of how the cart
behaves, the PC program built on them, and the launcher players start. The game itself comes only from the player's own
copy, on the player's own PC.

About the name Paprium, as given: "there is no affiliation and no commercial use."

## For players

Download `Paprium_PC_0.1.0.zip` from this repository's
[v0.1.0 release](https://github.com/thekoalakoa/paprium-pc/releases/tag/v0.1.0), not the repository itself.

**What you need**

- 64-bit Windows 10 (version 1903 or later) or Windows 11. It has been tried on one Windows 11 PC so far.
- Your own copy of the Paprium game file, legally obtained. The download does not come with it. The program knows one
  version of the game file, the one we tested, and refuses any other with a plain message.
- About 62 MB of disk space for the unzipped folder, and about 104 MB free on the drive of your app-data folder for the
  first start, which copies your game file there and builds the game (about 30 MB of that stays).
- No Python, no compiler and nothing from this repository: the download carries what it needs.

**How to play**

1. Unzip the whole download into a folder with a short path: your Desktop, your Documents or a games folder.
2. Start `Paprium.exe`. The program is not signed, so Windows may show "Windows protected your PC": click "More info",
   then "Run anyway". When Windows 11's Smart App Control is on, it blocks programs that are not signed and offers no
   "Run anyway", so the program cannot start there.
3. Drop your game file on the window, or click "Choose the game file".
4. The first time, the launcher checks the file and builds the game from it on your PC, in about 10 to 20 seconds. Every
   later start goes straight to the game.

The download's `READ_ME.txt` says how to play: the keys, the menu bar, the controls screen, the picture filters, the
console settings, CPU MATE, three players, where the program keeps your files and how to remove everything.

The launcher keeps your copy of the game file and the game it builds from it in your own app-data folder,
`%LOCALAPPDATA%\Paprium PC`, not in the folder you unzipped; only when that folder cannot be written does it keep them in
`my_copy`, beside `Paprium.exe`. They are yours alone: never share them and never sell them.

## CPU MATE and Little-Man Firmware

CPU MATE is the game's computer partner. Little-Man Firmware 1.0, our firmware for the Paprium cartridge's chip, carries
a reverse-engineered emulation of the game's CPU partner, in the file `little_man_firmware.bin`. The release download carries that file beside `Paprium.exe`, where
the program looks for it first, so CPU MATE works with no extra download: pick a Mega CD unit the game knows in the
Console menu, tick CPU MATE and use Soft Reset (`READ_ME.txt` says the rest).

It is the same public file as in `Little-Man_Firmware_1.0.zip` of the
[Paprium for Analogue Pocket 0.3.0 release](https://github.com/thekoalakoa/paprium-pocket/releases/tag/v0.3.0), byte for byte:

- `little_man_firmware.bin`: md5 `f8cda67209d0e4d35beb077ae88fb16b`, SHA-256
  `6fb90204322c6a5596bfb4d3a743696cbdced5744cf8d4b9f09aa9fc322873e2`

Little-Man Firmware is free software under the GNU General Public License, version 3 or later. Its source is in the
`firmware/` folder of [paprium-pocket](https://github.com/thekoalakoa/paprium-pocket) at tag `v0.3.0`, and the firmware
code built into this program comes from that same folder. The firmware's own third-party notices travel with it in the
download's `licences\THIRD_PARTY_NOTICES_firmware.md`. The PC program only reads the firmware file: it takes the brain
from it, checks it, and keeps it in memory.

`little_man_firmware.bin` is only in the release download. This repository never holds it.

Little-Man Firmware (trademark pending) is Marsupial Solutions LLC's name for this firmware.

## The terms

What the launcher builds on your PC from your game file is, in the words given, "a Patch copy of the Legally obtained rom".
The terms, as given, word for word:

- "gpl would apply to everything but the games rom itself the output your tool makes would still be partially their ip and
  cannot be sold."
- "sharing output is forbidden and not allowed just like sharing a rom"

## Licence

Our code is free software under the GNU General Public License, version 3 or (at your option) any later version: see
`LICENSE`. The game is not ours and is not under this licence. `THIRD_PARTY.md` lists the other projects' code and tools
this uses, with their licences, and the players' download carries every notice in its `NOTICES.txt`.

Copyright (C) 2026 Marsupial Solutions LLC.

## What is here

| Part | Where | What |
|---|---|---|
| Gate | `tool/gate.py` | SHA-256 of the whole ROM against the local configuration. On a mismatch nothing is written. |
| Image | `tool/image.py` | An in-memory copy of the ROM. Never written out. |
| Patch | `tool/patchsrc.py`, `patches/` | Applies the patch source to the copy. The patch source holds the Pocket core's Scum! cheats as switches: the translated code keeps the game's own way and the patched way at each site, and takes the patched way while the player has the switch on, with no new translation. |
| Discover | `tool/discover.py`, `tool/ramcode.py` | Finds the game's code in the player's ROM at run time. |
| Cut | `tool/regions.py` | Regions (C functions) and basic blocks, safe points. |
| Flags | `tool/flagsopt.py` | Which flags something can read. |
| Lift | `tool/lift.py`, `tool/dec68k.py` | 68000 to our intermediate form. |
| Back end | `tool/emit_c.py` | Portable C. |
| Runtime | `runtime/` | Memory, flags, dispatcher, interrupt entry, and a fallback interpreter. |
| PC target | `targets/pc/` | The machine model, the renderer, the sound, the window, the menu bar, the controls screen, the launcher and the package script. See `targets/README.md`. |
| Rig | `rig/` | The lockstep comparison core and the firmware as the cart. See `rig/README.md` for what is not here. |
| Tests | `tests/` | Every test uses small programs and made-up ROMs it makes itself. |

Generated code, address lists, logs and anything else the tools make from a player's ROM never enter this repository:
they write them to an output folder named in a local configuration file, which is never committed. Neither does
`little_man_firmware.bin`.

## Building, for developers

You need Python 3.12 with NumPy, gcc from MSYS2's mingw-w64, SDL2's development files, a copy of the firmware's source tree
(the `firmware/` folder of paprium-pocket at tag `v0.3.0`), the three sound chips' cores from their authors (see
`THIRD_PARTY.md`; `targets/pc/pc_build.py` pins each by hash), and your own ROM. Copy `config.example.json` outside the
repository and fill it in.

    python -I -B tests/test_all.py [scratch folder]     # the tests; needs gcc on PATH, capstone and unicorn in Python
    python -I -B ptool.py gate --config <local.json>
    python -I -B ptool.py translate --config <local.json>
    python -I -B ptool.py pc-build --config <local.json>                # the PC program, see targets/README.md
    python -I -B targets/pc/pc_package.py --config <local.json> --out FOLDER --version TEXT   # the players' package, see targets/README.md
    python -I -B tests/test_launcher.py [scratch folder]                # the package and its launcher on made-up ROMs (PAPRIUM_PC_CONFIG)
    python -I -B tests/check_package.py --config <local.json> --package FOLDER   # a package searched for pieces of the ROM

The PC program `pc-build` makes holds the translated game, so it is for your own use only and is never shared.

Tools that read a run's record:

    python -I -B tool/runstats.py <translation folder> <run folder>           # the run's counts
    python -I -B tool/coverage.py <translation folder> <run folders>          # what ran, over several runs
    python -I -B tool/waitloops.py <translation folder> <run folders>         # how much of a run was spinning
    python -I -B tool/sizes.py --gen <gen folder> --out <folder> --target ... # code size in bytes, by kind
    python -I -B tool/price.py <RV32 replay ELF> <run folder> --from-frame F --to-frame T   # native instructions and cache misses
    python -I -B tool/secondhost.py <RV32 replay ELF> <run folder>            # a whole run on RV32IM, end state compared
    python -I -B tool/formfuzz.py prepare --all-opcodes --out <folder>        # each distinct encoding in a slot, for the form fuzz

`tests/check_tree.py --config <local.json>` scans the repository against the ROM for leaked bytes, in every form a source
file could hold them. Run it before every commit. `tests/check_pc_link.py` checks what the PC program really links.

## How it was made

The release notes of [v0.1.0](https://github.com/thekoalakoa/paprium-pc/releases/tag/v0.1.0) say how we did it.

Made by Marsupial Solutions LLC: bJam, Freya, Isa, Spec and Koalakoa.

Copyright (C) 2026 Marsupial Solutions LLC. GPL-3.0-or-later.
