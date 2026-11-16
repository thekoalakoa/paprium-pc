/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * First on the include path when the firmware is built for the rig. The firmware's host board has no MegaWire text
 * view, because its own tests build the MegaWire code on a smaller board of their own. Everything else is that board's.
 */
#ifndef RIG_FW_BOARD_IMPL_H
#define RIG_FW_BOARD_IMPL_H

#include_next "board_impl.h"

#if FW_MW
extern uint8_t rig_fw_mwtext[];
static inline volatile uint8_t *brd_mwtext(void) { return rig_fw_mwtext; }
#endif

#endif /* RIG_FW_BOARD_IMPL_H */
