/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The I/O chip: the version register and two pads, three or six button, read the way the public documents describe, with the TH line and its count.
 *
 * What the two ports hold follows the Pocket core's 0.3.0 release (its menu's Arcade Mode, Arcade Coins, Attract Only and 3-Player Mode), and pc_io.c
 * answers as its controller ports do:
 *   - two pads, player 1 on port 1 and player 2 on port 2: the game as it always was;
 *   - in place of the pad in port 1, a Grand Stick III: Paprium's own arcade stick, which the game finds at start and reads with its own code every
 *     frame, its six switches and its coin slot too; player 3's pad can sit in the stick's EXT socket;
 *   - an EA 4-Way Play on the two ports: the multi-player adapter the game also supports, with players 1, 2 and 3 in its sockets A, B and C.
 */
#ifndef PC_IO_H
#define PC_IO_H

#include <stdint.h>
#include <stdio.h>

enum {
    PC_PAD_UP = 1 << 0, PC_PAD_DOWN = 1 << 1, PC_PAD_LEFT = 1 << 2, PC_PAD_RIGHT = 1 << 3,
    PC_PAD_B = 1 << 4, PC_PAD_C = 1 << 5, PC_PAD_A = 1 << 6, PC_PAD_START = 1 << 7,
    PC_PAD_Z = 1 << 8, PC_PAD_Y = 1 << 9, PC_PAD_X = 1 << 10, PC_PAD_MODE = 1 << 11
};

/* The players' pads: 0 and 1 are players 1 and 2, on the ports; 2 is player 3, who reaches the game only through the stick's EXT socket or the
   EA 4-Way Play. */
#define PC_IO_PADS 3

/* The console's version register at power-on: Japan, NTSC, no CD add-on, version nibble 0. It is the Pocket core's default console. paprium_pc.cfg's console
   lines change it (pc_console.h). */
#define PC_CONSOLE_VERSION 0x20u

/* The Grand Stick III's switches, 1 = on, as the stick sends its switch byte: ARCADE in bit 5 down to 3 PLAYERS in bit 0, the bits the game tests. */
enum { PC_STICK_3_PLAYERS = 1, PC_STICK_NO_HIT_FX = 2, PC_STICK_TWO_COINS = 4, PC_STICK_DEMO = 8, PC_STICK_INF_CREDITS = 16, PC_STICK_ARCADE = 32 };

/* What the ports hold. All zero: the two pads. */
typedef struct PcIoPorts {
    int stick;              /* a Grand Stick III in port 1, with player 1's buttons */
    unsigned switches;      /* its switch byte, PC_STICK_* */
    int stick_coin;         /* its Mode button is the coin slot (each press puts in a coin); 0: Mode is the stick's own button */
    int stick_ext;          /* player 3's pad is in its EXT socket */
    int tap;                /* an EA 4-Way Play on both ports (never together with the stick) */
} PcIoPorts;

void pc_io_reset(void);
void pc_io_set_version(uint32_t value);                 /* the version register from now on, also after a reset */
/* What the ports hold, from the next reset of the I/O chip on: the game looks for the stick and the adapter only when it starts. */
void pc_io_set_ports(const PcIoPorts *ports);
void pc_io_ports(PcIoPorts *now);                       /* what they hold now, as the last reset took it */
int pc_io_stick(void);
void pc_io_set_pad(int pad, uint32_t buttons);          /* PC_PAD_* held now, pad 0 to 2 */
void pc_io_set_three_button(int pad, int three);
/* Whether player 3's pad is plugged in: in the stick's EXT socket or in the adapter's socket C. An empty socket answers as nothing plugged in. */
void pc_io_set_present(int pad, int present);
uint32_t pc_io_read(int size, uint32_t address, uint64_t t);
void pc_io_write(int size, uint32_t address, uint32_t value, uint64_t t);

/* A test's log of the ports: every write and read of the two ports' data and control registers with its time in master clocks, and every change of the
   pads and of what the ports hold, one line each. NULL stops it. See pc_io.c for the lines. */
void pc_io_log(FILE *file);
void pc_io_log_time(uint64_t t);     /* the time of the pad changes that follow (the frame's end), when later than the log's last access */

#endif /* PC_IO_H */
