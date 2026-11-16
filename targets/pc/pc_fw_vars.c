/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Two variables the firmware's command bodies expect, for a build that leaves out the firmware's old module legacy/026/mame.c.
 *
 * The cart chip's image takes only these two variables from that module: its linker drops the rest. The PC link keeps everything it is given, so
 * the module would bring its old decoders into the program, one of them written after another project's emulator. The PC program must not carry
 * those, and nothing calls them, so the module stays out and the two names live here. tests/check_pc_link.py checks both halves.
 */
#include <stdint.h>

uint32_t ppm_sptr_shadow = 0;               /* the last stream pointer the firmware wrote: the board's register cannot be read back */
uint32_t snap_ptr_while_list_pending = 0;   /* a counter for debug output: how often that pointer moved while a sprite list waited */
