<!-- Copyright (C) 2026 Marsupial Solutions LLC
     SPDX-License-Identifier: GPL-3.0-or-later -->
# The picture filters' shaders

These three files are other people's work, kept here exactly as their authors published them in libretro's glsl-shaders collection
(<https://github.com/libretro/glsl-shaders>, commit a35069864b4bf30f8c8e740d57206543fb7e745e, under `crt/shaders`). Each carries its own notice
at its head. `targets/pc/pc_build.py` pins each file's SHA-256 (`SHADER_PINS`) and builds the program only from that exact text, which it turns
into C text for `targets/pc/pc_filter.c`. The program sets the shaders' own parameters for each look; it changes nothing in the files.

| File | Author | Licence | Looks that use it |
|---|---|---|---|
| `crt-hyllian.glsl` | Hyllian, 2011-2020; its colour masks by hunterk | MIT; hunterk's masks public domain | `scanlines`, `pvm` |
| `crt-lottes.glsl` | Timothy Lottes | Public domain | `crt` |
| `crt-geom.glsl` | cgwg, Themaister and DOLLS, 2010-2012 | GPL-2.0-or-later | `curved` |

Each was chosen after reading the licence at the head of the file itself: each lets a GPL-3.0-or-later program hold it. Others in the same
collection, and in libretro's slang-shaders collection, were left out for these reasons:

- The Sony Megatron Colour Video Monitor (`hdr/shaders` in slang-shaders), which imitates Sony PVM and BVM monitors: its files carry no licence
  and no copyright notice at all, so nothing gives us leave to copy them. A player may still run it as their own preset (see below).
- crt-guest-advanced and crt-royale: GPL-2.0-or-later, so their licences allow it, but each is ten or more passes with picture textures,
  which needs a full preset runner. They run as a player's own preset.
- crt-easymode and crt-aperture: their notice says only "License: GPL", with no version. A program may then choose any version, so they could
  be used; they were not needed for the looks chosen.
- zfast_crt and crt-pi: GPL-2.0-or-later; their looks are close to the ones kept.
- crt-yo6 (a Sony Trinitron KV-M1420B): no licence in its files.
- crt-potato's BVM preset: GPL-3.0-or-later, but its mask is a picture file.

## A player's own preset

A player can name a RetroArch slang preset in `paprium_pc.cfg` (`filter=` and the `.slangp` file's path). The program runs it through
librashader (<https://github.com/SnowflakePowered/librashader>, Mozilla Public License 2.0, or GPL 3 at the user's choice), which it does not
carry: `librashader.dll` has to sit beside `pc_port.exe`. The program looks at the preset file first (it names its shaders, and each file it
names is there), then loads the library by its full path. Anything that goes wrong leaves the picture as with off, with a plain message.
