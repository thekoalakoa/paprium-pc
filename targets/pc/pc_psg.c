/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The PSG. The chip's logic is Nuked-PSG by nukeykt, GPL-2.0-or-later, built from its author's source at the commit pc_build.py pins. It is not in this repository. This file clocks it, one chip clock
 * for each 15 master clocks, and turns its four channel volumes into one integer level on the table the Pocket core's video chip uses, so no float is in the path.
 *
 * The model works its output out only when asked, and asking on every clock costs more than the chip itself. A channel's volume out depends only on the inputs in the key below, so the output is asked
 * for only when one of them moved.
 */
#include "ympsg.h"

#include "pc_psg.h"
#include "pc_sound.h"

#define PSG_MCLK 15u

/* Attenuation 0-15 to level, as the Pocket core's video chip has it. */
static const uint16_t level_table[16] = {1200, 926, 746, 582, 458, 348, 274, 208, 158, 115, 86, 61, 40, 22, 10, 0};

static ympsg_t psg;
static uint64_t psg_time;
static uint32_t last_key;
static int32_t last_level;

static inline uint32_t output_key(void)
{
    return (uint32_t)psg.volume[0] | (uint32_t)psg.volume[1] << 4 | (uint32_t)psg.volume[2] << 8 | (uint32_t)psg.volume[3] << 12 | (uint32_t)psg.sign << 16 | (uint32_t)(psg.noise_sign_l & 1u) << 24 |
           (uint32_t)(psg.test & 1u) << 25;
}

static int32_t output_level(void)
{
    uint32_t out = YMPSG_Read(&psg);    /* the four volumes out, a digit each */
    return (int32_t)level_table[out >> 12] + level_table[(out >> 8) & 15u] + level_table[(out >> 4) & 15u] + level_table[out & 15u];
}

void pc_psg_power(uint64_t t)
{
    uint32_t i;
    YMPSG_Init(&psg);
    /* The chip's reset works through its slow internal steps, 32 clocks each. Held long enough to finish and let go here, so nothing of it is left when the first write comes. */
    YMPSG_SetIC(&psg, 1);
    for (i = 0; i < 256u; i++)
        YMPSG_Clock(&psg);
    YMPSG_SetIC(&psg, 0);
    for (i = 0; i < 256u; i++)
        YMPSG_Clock(&psg);
    psg_time = t;
    last_key = output_key();
    i = (uint32_t)output_level();
    if ((int32_t)i != last_level)
        pc_sound_psg(t, (int32_t)i);    /* the console's reset silences it, and the mix must hear that */
    last_level = (int32_t)i;
}

static inline void one_clock(void)
{
    uint32_t key;
    YMPSG_Clock(&psg);
    psg_time += PSG_MCLK;
    key = output_key();
    if (key != last_key) {
        int32_t level = output_level();
        last_key = key;
        if (level != last_level) {
            last_level = level;
            pc_sound_psg(psg_time, level);
        }
    }
}

void pc_psg_run_to(uint64_t t)
{
    while (psg_time + PSG_MCLK <= t)
        one_clock();
}

/* The chip takes two clocks over a write. A translated 68000 block's writes all carry the block's time, so two can come with no time between them. The earlier one gets its two clocks first, and the
   chip's time runs a clock or two ahead until the machine's catches up. */
void pc_psg_write(uint32_t value, uint64_t t)
{
    pc_psg_run_to(t);
    while (psg.write_flag || psg.write_flag_l)
        one_clock();
    YMPSG_Write(&psg, (uint8_t)value);
}
