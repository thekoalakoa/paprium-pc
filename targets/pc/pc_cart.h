/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The cart as the firmware answers it, on this machine's clock.
 */
#ifndef PC_CART_H
#define PC_CART_H

#include <stdint.h>

typedef struct PcCartConfig {
    uint32_t options;           /* the board's option word: 0 = the default console, MegaWire off */
    uint32_t step;              /* master clocks a board call costs the firmware; 0 = the host board's own figure */
    const uint8_t *save;        /* battery save bytes in the firmware's own layout, or NULL for a blank one */
    uint32_t save_size;
    const uint8_t *rom_bytes;   /* the file, in file byte order */
    uint32_t rom_size;
    uint8_t *rom_words;         /* the program as the runtime keeps it, 16-bit host words; the firmware's boot work lands in it */
    int no_bank;                /* 1: leave the wave bank empty instead of unpacking it */
} PcCartConfig;

/* Power-on: the firmware boots as far as releasing the 68000, then waits. */
void pc_cart_boot(const PcCartConfig *config);

/* The bus. t is the machine's clock in master clocks: the firmware first runs up to it. */
uint32_t pc_cart_r8(uint32_t address, uint64_t t);
uint32_t pc_cart_r16(uint32_t address, uint64_t t);
void pc_cart_w8(uint32_t address, uint32_t value, uint64_t t);
void pc_cart_w16(uint32_t address, uint32_t value, uint64_t t);

void pc_cart_run_to(uint64_t t);            /* the firmware runs up to t, with no access */
/* The console's reset button was pressed at t: the chip's console-reset interrupt is raised, and its firmware writes the warm vector page into cart RAM
   before it acknowledges it. pc_cart_console_reset_pending says it has not acknowledged it yet. */
void pc_cart_console_reset(uint64_t t);
int pc_cart_console_reset_pending(void);
int pc_cart_holding(void);                  /* the firmware holds the console: the 68000, the video chip, the I/O chip and the Z80 are off */
const uint8_t *pc_cart_low(uint32_t *ram_bytes);    /* the cart RAM as 16-bit host words */

/* A word of the page the 68000 fetches from at boot: the cart RAM after the firmware's work, zero over the register page, then the file. */
void pc_cart_copy_boot_page(uint8_t *low_page, uint32_t bytes);

/* What it did, for the logs. */
void pc_cart_frame_line(void *file, uint32_t frame);
void pc_cart_report(void);
uint32_t pc_cart_fatal_count(void);
uint32_t pc_cart_fatal_code(void);
uint64_t pc_cart_host_ticks(void);
extern volatile uint8_t pc_cart_in_turn;    /* not 0 while a firmware turn runs, for the freeze log */
uint32_t pc_cart_hash_low(void);
void pc_cart_save_copy(uint8_t *out, uint32_t bytes);
void pc_cart_save_test_change(void);        /* a test: one byte of the cart's save changes, as when the game saves */

#endif /* PC_CART_H */
