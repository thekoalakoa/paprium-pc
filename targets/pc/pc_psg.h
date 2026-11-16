/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The PSG, the sound chip inside the video chip. Both processors can write it. It runs on the machine's master clock, behind the Z80, and its level goes to pc_sound_psg. pc_psg.c says where the
 * chip's model comes from.
 */
#ifndef PC_PSG_H
#define PC_PSG_H

#include <stdint.h>

void pc_psg_power(uint64_t t);                  /* power-on: the time starts at t. The Z80's reset does not reach this chip */
void pc_psg_run_to(uint64_t t);
void pc_psg_write(uint32_t value, uint64_t t);

#endif /* PC_PSG_H */
