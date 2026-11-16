/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A test route: a small program, built outside this repository and loaded with --route, that plays the game for a run. It sees work RAM at each frame's end and answers with
 * the pads for the next frame and, on test routes, changes to work RAM. The layout is the lockstep rig's, so the same route programs run on both. Addresses a route uses are
 * its own business and stay in its own source.
 */
#ifndef PC_ROUTE_H
#define PC_ROUTE_H

#include <stdint.h>

#define PC_ROUTE_POKES_MAX 256

typedef struct PcRoutePoke {
    uint32_t address;           /* a work RAM address */
    uint32_t size;              /* 1 or 2 */
    uint32_t value;
} PcRoutePoke;

typedef struct PcRouteIO {
    /* in */
    int frame;                  /* the frame about to run */
    const uint8_t *ram;         /* 64 KB of work RAM, 16-bit host words: byte address XOR 1 */
    const uint8_t *rom;         /* the program image, 16-bit host words */
    uint32_t rom_size;
    /* out */
    uint32_t pad[2];            /* buttons held this frame, in the rig's bits, see PC_ROUTE_* */
    int n_pokes;
    PcRoutePoke pokes[PC_ROUTE_POKES_MAX];
    int shot;                   /* keep a picture of this frame */
    int end;                    /* this frame is the last */
    char note[240];             /* a line for the run's route log */
} PcRouteIO;

/* The pad bits a route answers with, and a pad script's masks too. */
enum {
    PC_ROUTE_B = 1 << 0, PC_ROUTE_A = 1 << 1, PC_ROUTE_MODE = 1 << 2, PC_ROUTE_START = 1 << 3, PC_ROUTE_UP = 1 << 4, PC_ROUTE_DOWN = 1 << 5,
    PC_ROUTE_LEFT = 1 << 6, PC_ROUTE_RIGHT = 1 << 7, PC_ROUTE_C = 1 << 8, PC_ROUTE_Y = 1 << 9, PC_ROUTE_X = 1 << 10, PC_ROUTE_Z = 1 << 11
};

typedef int (*PcRouteInitFn)(const char *argument);
typedef void (*PcRouteFrameFn)(PcRouteIO *io);
/* A route may also export route_pad3: player 3's pad for the frame route_frame has just answered, in the same bits. A route that has it plugs player 3
   in, for the EA 4-Way Play or the arcade stick's EXT socket (pc_io.h). The rig's routes have no third pad. */
typedef uint32_t (*PcRoutePad3Fn)(void);

#endif /* PC_ROUTE_H */
