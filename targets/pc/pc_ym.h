/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The YM2612, the FM chip the Z80 drives. It runs on the machine's master clock, behind the Z80, and its sound goes to pc_sound_fm. pc_ym.c says where the chip's model comes from.
 */
#ifndef PC_YM_H
#define PC_YM_H

#include <stdint.h>

void pc_ym_power(uint64_t t);                           /* power-on: the time starts at t */
void pc_ym_reset(uint64_t t);                           /* the Z80's reset line is also this chip's: runs up to t, then resets */
void pc_ym_run_to(uint64_t t);                          /* runs up to t, sending each finished sample to the mix */
void pc_ym_write(uint32_t port, uint32_t value, uint64_t t);    /* port 0-3, at time t */
uint32_t pc_ym_read(uint32_t port, uint64_t t);

#endif /* PC_YM_H */
