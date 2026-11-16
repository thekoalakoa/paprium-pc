<!-- Copyright (C) 2026 Marsupial Solutions LLC
     SPDX-License-Identifier: GPL-3.0-or-later -->
# Tools this project uses

None of these is copied into this repository. The tool's own code is GPL-3.0-or-later.

| Tool | Licence | Used for | Part of the tool's output? |
|---|---|---|---|
| Python 3.12 | PSF-2.0 | Runs the translator | No |
| NumPy | BSD-3-Clause | The pointer scan in discovery | No |
| GCC, from MSYS2 mingw-w64 | GPL-3.0-or-later with the GCC Runtime Library Exception; the mingw-w64 runtime has its own permissive terms | Compiles the generated C and the runtime | Only as the compiled code, under the runtime exception |
| Capstone | BSD-3-Clause | Tests only: checks our decoder | No |
| Unicorn 2 | GPL-2.0 (from QEMU) | Tests: the reference for the small test programs. `tool/price.py`: runs the RV32 replay build to count native instructions and model caches. A separate engine, driven from Python | No |
| Genesis Plus GX | Its own terms: no commercial use, source with any redistribution. Its 68000 core comes from Musashi 3.3 by Karl Stenerud, free for any non-commercial purpose with credit | The lockstep rig's reference machine, on our own PC only. A patched copy in the output folder, linked only into the rig programs there. The rig parts that build against it are not in this repository, see `rig/README.md`. Never shipped | No |
| xPack RISC-V GCC, with newlib | GPL-3.0-or-later; newlib has its own permissive terms | The size of the generated code on a 32-bit RISC target, and the replay build `tool/price.py` runs, on our own PC | No |
| Pillow | MIT-CMU | Viewing a run's pictures in local tools outside this repository | No |
| FX68K, by Jorge Cwik | GPL-3.0-or-later | The form fuzz's referee on single instructions, in simulation on our own PC, in a bench that is not in this repository (see `rig/README.md`); the bench copied it unchanged. Also the bench that measured the 68000's times, see below | No |
| Verilator | LGPL-3.0-only or Artistic-2.0 | Runs FX68K for that bench, on our own PC | No |
| chips-test, by Andre Weissflog, at commit 14f423dd, with ZEXALL by Frank D. Cringle (changes by J.G. Harston) and the FUSE test data | chips-test itself: MIT. ZEXALL: GPL-2.0-or-later, by the notice in its source. The FUSE test data comes from the Fuse emulator project, GPL-2.0-or-later, and the copied files carry no notice of their own | Checks the Z80 core on our own PC. Not in this repository or the program | No |
| WavPack 4.0 tiny decoder, BSD-3-Clause, as the firmware tree carries it | BSD-3-Clause | `rig/fw`: the firmware's own sound-bank unpack, when the firmware answers as the cart, on our own PC | No |

## What the PC program links

The PC program under `targets/pc` is built from the translated game, our machine model and renderer, and the firmware's source, which comes from a local copy of the firmware tree. None of the parts below is copied into this repository. Their notices go in the program's package with it.

| Part | Licence | What of it the PC program holds |
|---|---|---|
| mega-ppm, by krikzz, <https://github.com/krikzz/mega-ppm>, as the firmware tree carries it | BSD-3-Clause for krikzz's code; the firmware's changes are GPL-3.0-or-later | The firmware's command bodies in `interim/s1/bodies_s1.c`, and the headers they include: `interim/s1/appmain.h` and `legacy/026/mame.h`. The module `legacy/026/mame.c` is left out, see below |
| WavPack 4.0 tiny decoder, Conifer Software, as the firmware tree carries it | BSD-3-Clause | `core/wavpack`, except `wvbank.c` and `wvbank.h`, which are the firmware's own: it unpacks the game's sound bank once |
| SDL2, <https://www.libsdl.org> | zlib | The window, the game controllers and the audio device, as `SDL2.dll` beside the program. Not copied into this repository |
| SDL_GameControllerDB, SDL's community controller database, <https://github.com/mdqinc/SDL_GameControllerDB>, at commit c0f11d40 | zlib | `gamecontrollerdb.txt` beside the program, as published: the program reads its lines for Windows to know pads SDL does not know itself (`targets/pc/pc_input.c`). Not copied into this repository: `pc_build.py` pins its text and its licence by hash, and the local configuration's `controller_db` names where they are |
| `chips/z80.h` from chips, by Andre Weissflog (floooh), <https://github.com/floooh/chips>, at commit 9e88298c | zlib | The Z80 the sound driver runs on, compiled into `targets/pc/pc_z80.c`. Not copied into this repository: `pc_build.py` pins its text and its licence by hash, and the local configuration's `sound_cores` names where it is |
| Nuked-OPN2, by Alexey Khokholov (nukeykt), <https://github.com/nukeykt/Nuked-OPN2>, at commit 335747d7 | LGPL-2.1-or-later | The YM2612 model, `ym3438.c`, compiled from its author's source and linked. Pinned and found the same way |
| Nuked-PSG, by Alexey Khokholov (nukeykt), <https://github.com/nukeykt/Nuked-PSG>, at commit d15a168c | GPL-2.0-or-later | The PSG model, `ympsg.c`, compiled from its author's source and linked. Pinned and found the same way |

Nuked-MD, by nukeykt, GPL-2.0-or-later, is an exact model of the video chip that this project's other trees carry. The PC program links none of it. The renderer's colour output levels and the chip's status and interrupt rules were read from it as facts about the chip, and so was the PSG's integer level table in `targets/pc/pc_psg.c`. The machine's timing points (where its interrupts and status bits change, its access slots, its DMA rates) were measured by running it in simulation with scripted bus traffic. No code of it is in this repository or in the program.

The instruction times in `tool/cycles68k.py` come from the published tables and were checked by running FX68K in simulation. The divide times in `runtime/pt_rt.c` were measured that way, with made-up operands: they are facts about the chip, and no code of FX68K is in this repository or in the program. That a divide's time follows its quotient bit by bit was first worked out and published by Jorge Cwik. The two routines here were written from our own measurements and hold no text of his.

The pads in the EA 4-Way Play's sockets answer the game as the Pocket core's pad model does: its file `rtl/upstream/pad_io.sv`, MegaDrive input implementation, Copyright (c) 2023 Alexey Melnikov, GPL-2.0-or-later, with our team's changes under GPL-3.0-or-later. The PC program's own code (`targets/pc/pc_io.c`) follows that model; none of its text is in this repository or in the program. `NOTICES.txt` gives the same credit.

## The picture filters' shaders

These are the only other projects' files copied into this repository: three shaders the program draws its CRT looks with, in `targets/pc/shaders`,
exactly as their authors published them in libretro's glsl-shaders collection at commit a35069864b4b. `targets/pc/pc_build.py` pins each by its
SHA-256 (`SHADER_PINS`) and builds their text into the program; `tests/check_tree.py` lets them keep their authors' notices in place of our header
lines. `targets/pc/shaders/README.md` says which others were looked at and why they were left out.

| Part | Licence | What of it the PC program holds |
|---|---|---|
| `crt-hyllian.glsl`, Hyllian's CRT Shader, by Hyllian, with colour masks by hunterk | MIT; hunterk's masks public domain | The looks `scanlines` and `pvm` |
| `crt-lottes.glsl`, by Timothy Lottes | Public domain | The look `crt` |
| `crt-geom.glsl` (CRT-interlaced), by cgwg, Themaister and DOLLS | GPL-2.0-or-later | The look `curved` |
| librashader, <https://github.com/SnowflakePowered/librashader> | MPL-2.0, or GPL-3.0 at the user's choice | Nothing: it is not built in or carried. A player who puts `librashader.dll` beside the program can run their own RetroArch `.slangp` preset through it. `targets/pc/pc_filter.c` declares the few calls it uses, as its C header (MIT) lays them out |

**No reference emulator in the PC program.** Genesis Plus GX and its Musashi core stay in the lockstep rig, as a black box. Nothing under `targets/pc` includes anything from `rig/`, `tests/check_tree.py` holds that, and `tests/check_pc_link.py` checks the finished program. That is why the PC program leaves out the firmware's `legacy/026/mame.c`: it holds old decoders, one of them written after that emulator. The firmware's Pocket image drops them at link time, but a PC link keeps all it is given. `targets/pc/pc_fw_vars.c` defines the only two names the rest of the firmware takes from that module.

## What goes in the program's package

The program's folder carries these, whatever else it holds. `pc_build.py` puts the first five rows' files beside the program, and the next two when the
local configuration names `controller_db`.

| File | What it is | Where it comes from |
|---|---|---|
| `pc_port.exe` | The program. It holds the translated game, so it is never in this repository or a download | The build |
| `SDL2.dll` | SDL2 2.30.9: the window, the game controllers and the audio device | SDL2's files, as `sdl_dir` names them |
| `Z80_LICENSE.txt` | The zlib licence of chips, for `z80.h` | `LICENSE` of chips at commit 9e88298c |
| `NUKED_OPN2_LICENSE.txt` | The GNU Lesser General Public License 2.1, for Nuked-OPN2 | `LICENSE` of Nuked-OPN2 at commit 335747d7 |
| `NUKED_PSG_LICENSE.txt` | The GNU General Public License 2, for Nuked-PSG | `LICENSE` of Nuked-PSG at commit d15a168c |
| `gamecontrollerdb.txt` | SDL's community controller database, unchanged | `gamecontrollerdb.txt` of SDL_GameControllerDB at commit c0f11d40 |
| `GAMECONTROLLERDB_LICENSE.txt` | Its zlib licence | `LICENSE` of SDL_GameControllerDB at commit c0f11d40 |
| `SDL2_LICENSE.txt` | SDL2's zlib licence | `LICENSE.txt` in SDL2's files |
| `COPYING_GPL3.txt` | The licence of this project's own code | `LICENSE` in this repository |
| `THIRD_PARTY_NOTICES_firmware.md` | The firmware's own notices, with mega-ppm's and WavPack's licence texts | The firmware tree |
| `MINGW_RUNTIME_LICENSE.txt` | The terms of the C runtime compiled into the program | mingw-w64's files |
| `NOTICES.txt` | The notices below, and the terms of the program's output | Written with the package |

## The players' package

`targets/pc/pc_package.py` makes the folder a player downloads. It holds no translated game: the launcher builds the game on the player's PC from
the player's own ROM, with the tools below, each taken from its publisher's archive after a check of that archive's SHA-256 (`TOOL_PINS` in that
script). Python and NumPy are copied from their archives; TinyCC is built from its source archive by `targets/pc/pc_tinycc.py`, so the compiler
and the source in the package correspond exactly. Its notices, licence texts and sources come from `targets/package/` and the archives themselves.
`tests/check_package.py` checks the finished folder.

| Tool | Licence | Used for | In the package |
|---|---|---|---|
| Python 3.12.10, the embeddable package for 64-bit Windows | PSF-2.0, with its bundled parts' notices in its `LICENSE.txt` | Runs the translator on the player's PC | `tools/python`, as published, with a `._pth` file of ours |
| NumPy 2.5.3, the wheel for that Python | BSD-3-Clause, with its bundled libraries' licences in its `dist-info` | The translator's pointer scan | `tools/python/site`, less its tests, f2py, packaging hooks, documents and type stubs |
| TinyCC 0.9.27, built by `targets/pc/pc_tinycc.py` from its source archive with gcc (the steps of the archive's `win32/build-tcc.bat` for 64-bit Windows) | LGPL-2.1; its runtime library `lib/libtcc1.c` GPL-2.0-or-later with the linking exception written in that file (the GPL's text as `licences/COPYING_GPL2.txt`), `lib/bcheck.c` LGPL-2.0-or-later; its include folder's mingw-w64 and mingw headers public domain, `include/tcc/tcc_libm.h` (musl) MIT | Compiles the translated game into `pc_game.dll` on the player's PC | `tools/tcc`: `tcc.exe`, `libtcc.dll`, `include`, `lib` (`libtcc1-64.a` and four `.def` files), without its 32-bit compiler, examples and documents; its full source, `tcc-0.9.27.tar.bz2`, and `pc_tinycc.py` in `source/` |
| The three sound cores above | as above | Compiled into `pc_port.exe` | their pinned source files in `source/sound_cores` |
| The three picture filter shaders above | as above | Built into `pc_port.exe` | the pinned files in `source/shaders` |
| SDL_GameControllerDB, as above | zlib | Read by `pc_port.exe` | `gamecontrollerdb.txt` beside it, unchanged; its licence in `licences/GAMECONTROLLERDB_LICENSE.txt` |

The package also holds the mingw-w64 runtime's notices and GCC's runtime library exception, from MSYS2's files, for the runtime code gcc puts in
the three programs and in TinyCC's `tcc.exe` and `libtcc.dll`. NumPy's wheel carries Microsoft's C++ runtime library (`msvcp140`) as published,
and TinyCC's include folder holds mingw-w64's and mingw's public-domain headers and one header from musl under the MIT licence; `NOTICES.txt`
names both.

These three paragraphs go in `NOTICES.txt`, under the other projects' code the program holds:

chips z80.h, by Andre Weissflog (floooh): the Z80 the game's sound driver runs on, compiled into pc_port.exe from its author's file at commit 9e88298c. zlib licence, see Z80_LICENSE.txt.

Nuked-OPN2, by Alexey Khokholov (nukeykt): the YM2612 model, compiled into pc_port.exe from its author's source at commit 335747d7. GNU Lesser General Public License, version 2.1 or (at your option) any later version, see NUKED_OPN2_LICENSE.txt.

Nuked-PSG, by Alexey Khokholov (nukeykt): the PSG model, compiled into pc_port.exe from its author's source at commit d15a168c. GNU General Public License, version 2 or (at your option) any later version, see NUKED_PSG_LICENSE.txt.

## Little-Man Firmware 1.0 in the players' package

The players' package carries `little_man_firmware.bin` beside `Paprium.exe`: Little-Man Firmware 1.0, our firmware for the Paprium cartridge's chip, byte for byte the public file of the Paprium core 0.3.0's release for the Analogue Pocket (MD5 `f8cda67209d0e4d35beb077ae88fb16b`, SHA-256 `6fb90204322c6a5596bfb4d3a743696cbdced5744cf8d4b9f09aa9fc322873e2`). `pc_port.exe` reads CPU MATE's brain from it. GPL-3.0-or-later; its source is the `firmware/` folder of <https://github.com/thekoalakoa/paprium-pocket> at tag `v0.3.0`. The other projects' parts in it are NEORV32's linker script, adapted (BSD-3-Clause), newlib's `memcpy` and `memset` (BSD-2-Clause), mega-ppm (BSD-3-Clause for krikzz's code) and WavPack 4.0's tiny decoder (BSD); their notices and licence texts are in `licences/THIRD_PARTY_NOTICES_firmware.md`, which travels with it. `targets/pc/pc_package.py` takes the file only from that release's `Little-Man_Firmware_1.0.zip` after the zip's and the file's hashes matched the published ones (`FIRMWARE_PINS`). It is never in this repository.
