/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The YM2612. The chip's model is Nuked-OPN2 by nukeykt, LGPL-2.1-or-later, built from its author's source at the commit pc_build.py pins. It is not in this repository. This file only clocks it
 * and adds up its output.
 *
 * One chip clock is 42 master clocks. The chip puts out one value a clock, with each FM channel in its own time slots, 24 slots to a sample. The Pocket core's audio_cond adds up the 24 slots of the
 * real chip's output, times 3, and latches the sum after slot 23. The model's output is already that output times 3, so the sum is plain addition here. A bench of the Pocket's chip beside the model on
 * the same pins gives the place in the model's own count where that sum ends.
 */
#include "ym3438.h"

#include "pc_sound.h"
#include "pc_ym.h"

#define YM_MCLK 42u
#define SUM_ENDS_AT_CYCLE 3u    /* the model's slot counter, just after the clock that finishes a sum */

static ym3438_t ym;
static uint64_t ym_time;
static int32_t sum_left, sum_right;

void pc_ym_power(uint64_t t)
{
    OPN2_SetChipType(ym3438_mode_ym2612);
    OPN2_Reset(&ym);
    ym_time = t;
    sum_left = sum_right = 0;
}

void pc_ym_run_to(uint64_t t)
{
    while (ym_time + YM_MCLK <= t) {
        Bit16s out[2];
        OPN2_Clock(&ym, out);
        ym_time += YM_MCLK;
        sum_left += out[0];
        sum_right += out[1];
        if (ym.cycles == SUM_ENDS_AT_CYCLE) {
            pc_sound_fm(ym_time, sum_left, sum_right);
            sum_left = sum_right = 0;
        }
    }
}

void pc_ym_reset(uint64_t t)
{
    pc_ym_run_to(t);
    OPN2_Reset(&ym);
    sum_left = sum_right = 0;
}

void pc_ym_write(uint32_t port, uint32_t value, uint64_t t)
{
    pc_ym_run_to(t);
    OPN2_Write(&ym, port & 3u, (Bit8u)value);
}

uint32_t pc_ym_read(uint32_t port, uint64_t t)
{
    pc_ym_run_to(t);
    return OPN2_Read(&ym, port & 3u);
}
