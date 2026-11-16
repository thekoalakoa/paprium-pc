/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The rig's cart when the firmware answers: what the driver and the reference machine see of fw_cart.c.
 */
#ifndef FW_CART_H
#define FW_CART_H

#include <stdint.h>

typedef struct FwConfig {
    uint32_t options;           /* the board's option word (BRD_OPT_*): 0 = the default console, MegaWire off */
    uint32_t step;              /* master clocks a board call costs the firmware; 0 = the host board's own figure */
    const uint8_t *save;        /* battery save bytes to find at power-on, or NULL for a blank one */
    uint32_t save_size;
    const uint8_t *rom_bytes;   /* the file, in file byte order */
    uint32_t rom_size;
    int no_bank;                /* 1: leave the wave bank empty instead of unpacking it at C6 */
    void *(*current_fiber)(void);   /* a bus handler can run on either of the rig's fibers: an interrupt's vector
                                   read comes from the translated side's turn */
    const char *report_dir;
} FwConfig;

void fwc_configure(const FwConfig *c);

/* The reference machine's side. t is the caller's own clock in master clocks since the frame began. */
void fwc_boot(uint8_t *rom_words, uint32_t size);   /* power-on; the ROM as the machine keeps it, 16-bit host words */
uint8_t *fwc_low(void);                             /* the 64 KB the machine fetches code from */
unsigned int fwc_r8(unsigned int a, int t);
unsigned int fwc_r16(unsigned int a, int t);
void fwc_w8(unsigned int a, unsigned int v, int t);
void fwc_w16(unsigned int a, unsigned int v, int t);

/* The driver's side. */
void fwc_frame_end(unsigned int frame_mclk, int t); /* the frame is over: t is the clock after it was rewound */
void fwc_frame_line(void *file, int frame);         /* one line per frame for the run's fw_frames.txt */
void fwc_report(void);                              /* what the firmware did, for the run's log */

#endif /* FW_CART_H */
