/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Put in front of each firmware file with gcc's -include. The firmware's host board has no MegaWire text view, because its own tests build the MegaWire code on a smaller
 * board of their own. Everything else is that board's.
 */
#ifndef PC_BOARD_IMPL_H
#define PC_BOARD_IMPL_H

#include <stdint.h>

#if FW_MW
extern uint8_t pc_mwtext[];
static inline volatile uint8_t *brd_mwtext(void) { return pc_mwtext; }
#endif

#endif /* PC_BOARD_IMPL_H */
