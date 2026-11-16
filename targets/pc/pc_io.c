/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The I/O chip. Each register sits at an odd address; the even byte below it answers the same, and a word read gets the byte on both halves. The game checks that last
 * point in places, and a mismatch there sends it astray.
 *
 * A pad's lines depend on TH, an output of the console. A six button pad counts the rises of TH, and forgets the count after about 1.5 ms with no change:
 *    TH high, count under 3   ? 1 C B R L D U
 *    TH low,  count under 2   ? 0 S A 0 0 D U
 *    TH low,  count 2         ? 0 S A 0 0 0 0     this is how the game knows a six button pad
 *    TH high, count 3         ? 1 C B M X Y Z
 *    TH low,  count 3         ? 0 S A 1 1 1 1
 * Lines read 0 when the button is held. Bits set as outputs read back what was written.
 *
 * THE GRAND STICK III (PcIoPorts.stick), as the Pocket core's rtl/PAPRIUM/paprium_gs3.sv answers in its 0.3.0 release. Its phase is the number of TH
 * edges since its frame started; a frame starts after STICK_TIMEOUT with no TH edge. It drives D5-D0:
 *    phase 0   TH low    its ID, 0x14: a stick; D1 set when a pad is in its EXT socket
 *    phase 1   TH high   its buttons C B R L D U        (0 = held)
 *    phase 2   TH low    its buttons Mode X Y Z Start A
 *    phase 3   TH high   the EXT socket's pad: C B R L D U, all 1 with nothing there
 *    phase 4   TH low    the EXT socket's pad: Mode X Y Z Start A (a three button pad never holds the first four)
 *    phase 5   TH high   status A: 0x02, and D3 set while a coin waits
 *    phase 6   TH low    status B: 0x3F
 *    phase 7   TH high   the switches, 1 = on: ARCADE on D5, INF. CREDITS D4, DEMO D3, TWO COINS D2, NO HIT FX D1, 3 PLAYERS D0
 *    then all 1
 * The rise into phase 9 carries the game's coin acknowledge on D0-D3, which the console drives then. A coin waiting is taken when bit 2 of that
 * acknowledge is 1 and was 0 the frame before: the game holds the acknowledge until its coin routine has run again, so a stick that took a coin on
 * every acknowledge would lose coins. The ID, the status bytes, the timeout and the acknowledge rule are the Pocket core's; nobody has measured a
 * real stick yet. The game swallows the first coin after power-on itself, as it does on the cart. While the stick's Mode button is its coin slot
 * (stick_coin), each press of it puts in one coin, up to 7 waiting, and the stick's own Mode bit reads as not held; otherwise Mode is a button.
 *
 * THE EA 4-WAY PLAY (PcIoPorts.tap), as the Pocket core's rtl/PAPRIUM/paprium_pads.sv answers. The console writes a socket's number on port 2: the
 * adapter takes bits 6-4 of what port 2 shows whenever its bits 1-0 show 0 (a pin the console does not drive shows 1). Port 1 then carries that
 * socket's pad, and only that pad sees the console's TH; numbers 4 to 7 answer the adapter's own ID, 0x7C on every line of port 1. Socket A is
 * player 1, B player 2, C player 3, and D is always empty; an empty socket, or player 3's when no pad is plugged in, reads as nothing (all 1). Port 2
 * reads back what the console drives, 1 elsewhere. Each socket's pad is the Pocket core's pad model (rtl/upstream/pad_io.sv as it ships there): its
 * count of TH rises is two bits, so a fourth rise starts it again at 0; it forgets the count after 81,200 master clocks with no edge; it comes out of
 * a reset with the count at 3; and a TH line no longer driven (its socket not chosen, or TH set as an input) rises to 1 by itself 1,471 master clocks
 * later, which counts as a rise.
 *
 * The two plain ports keep the program's own pad model above, unchanged.
 */
#include <string.h>

#include "pc_io.h"
#include "pc_vdp.h"

#define PAD_TIMEOUT_MCLK 80539u     /* 1.5 ms of master clocks; not a game address */
#define STICK_TIMEOUT 81200u        /* 1.512 ms of master clocks, the Pocket core's stick timeout; not a game address */
#define STICK_ID 0x14u
#define STICK_EXT_PAD 0x02u         /* in its ID: a pad in the EXT socket */
#define STICK_STATUS_A 0x02u
#define STICK_STATUS_B 0x3Fu
#define STICK_PRE 15                /* the phase before a frame: TH high since the last timeout */
#define STICK_COINS_MAX 7
#define TAP_ID 0x7Cu                /* the adapter's answer on port 1 to the numbers 4 to 7 */
#define TAP_SOCKETS 4
#define TAP_PAD_TIMEOUT 81200u      /* the Pocket core's pad: 11,600 x 7 master clocks; not a game address */
#define TAP_PAD_FLOAT 1471u         /* the Pocket core's pad: a TH line no longer driven reads 1 after 210 x 7 master clocks, one more to its register */

enum { R_VERSION, R_DATA1, R_DATA2, R_DATA3, R_CTRL1, R_CTRL2, R_CTRL3, R_TX1, R_RX1, R_SC1, R_TX2, R_RX2, R_SC2, R_TX3, R_RX3, R_SC3 };

typedef struct Pad {
    uint32_t buttons;
    int three;
    int th;             /* the TH line as the pad sees it */
    int rises;
    uint64_t last_edge;
} Pad;

static uint8_t regs[16];
static Pad pads[PC_IO_PADS];
static int present3;            /* player 3's pad is plugged in */
static uint8_t version = PC_CONSOLE_VERSION;     /* the console's version register: pc_addons.c sets it for the console the game is to find */
static PcIoPorts wanted, ports;     /* what pc_io_set_ports asked for, and what the last reset took */
static FILE *io_log;
static uint64_t io_log_t;       /* the time of the last access the log has, so its lines keep the order the accesses came in */

/* The stick's state. */
static struct {
    int phase, th;
    uint64_t last_edge;
    unsigned coins, ack_before;
    uint32_t buttons;
} stick;

/* A socket's pad in the adapter: the Pocket core's pad model. */
typedef struct TapPad {
    int th;             /* TH as the pad's own register holds it */
    unsigned count;     /* its count of TH rises, two bits */
    uint64_t last_edge; /* its last TH edge, which starts its timeout again */
    int driven;         /* the console drives its TH line now */
    uint64_t free_from; /* when its TH line stopped being driven */
} TapPad;

static struct {
    unsigned n;         /* the socket number taken from port 2 */
    TapPad pad[3];      /* sockets A, B and C; D is always empty */
} tap;

static void log_pads(uint64_t t)
{
    if (io_log)
        fprintf(io_log, "%llu P %03X %03X %03X %d %d%d%d\n", (unsigned long long)t, (unsigned)pads[0].buttons, (unsigned)pads[1].buttons, (unsigned)pads[2].buttons,
                present3, pads[0].three, pads[1].three, pads[2].three);
}

void pc_io_log(FILE *file)
{
    io_log = file;
    if (io_log) {
        fprintf(io_log, "# pc_io log: T W port data ctrl | T R port value | T P pad1 pad2 pad3 present3 three123 | T X stick switches coin ext tap\n");
        fprintf(io_log, "0 X %d %02X %d %d %d\n", ports.stick, ports.switches, ports.stick_coin, ports.stick_ext, ports.tap);
        log_pads(0);
    }
}

/* Pad changes come at a frame's end. A frame with no access to the ports between two of them would otherwise give both the same time. */
void pc_io_log_time(uint64_t t)
{
    if (io_log && t > io_log_t)
        io_log_t = t;
}

void pc_io_set_ports(const PcIoPorts *p)
{
    wanted = *p;
    wanted.switches &= 0x3Fu;
    if (wanted.stick)
        wanted.tap = 0;
}

void pc_io_ports(PcIoPorts *now) { *now = ports; }
int pc_io_stick(void) { return ports.stick; }

/* ---- the stick */

/* The phase as it stands at t: after the timeout, a new frame (TH low) or the time before one (TH high). */
static int stick_phase(uint64_t t)
{
    if (t - stick.last_edge >= STICK_TIMEOUT)
        return stick.th ? STICK_PRE : 0;
    return stick.phase;
}

/* A TH edge the console made by a write: data and ctrl as they are after it. */
static void stick_edge(int th, uint64_t t, uint8_t data, uint8_t ctrl)
{
    int now = stick_phase(t);
    if (th && now == 8 && (ctrl & 0x0Fu) == 0x0Fu) {
        /* The rise into phase 9 with D0-D3 driven by the console: the coin acknowledge. */
        unsigned bit = (data >> 2) & 1u;
        if (bit && !stick.ack_before && stick.coins)
            stick.coins--;
        stick.ack_before = bit;
    }
    stick.phase = now == STICK_PRE ? (th ? STICK_PRE : 0) : now == 14 ? 14 : now + 1;
    stick.th = th;
    stick.last_edge = t;
}

/* Player 3's pad in the EXT socket, in the stick's order of bits (the PC_PAD_* order): a three button pad never holds Mode, X, Y or Z. */
static int ext_there(void) { return ports.stick_ext && present3; }

static uint32_t ext_buttons(void)
{
    uint32_t b = pads[2].buttons;
    if (pads[2].three)
        b &= ~(uint32_t)(PC_PAD_MODE | PC_PAD_X | PC_PAD_Y | PC_PAD_Z);
    return b;
}

static uint32_t stick_lines(uint64_t t)
{
    uint32_t b = stick.buttons;
    if (ports.stick_coin)
        b &= ~(uint32_t)PC_PAD_MODE;        /* Mode is the coin slot */
    switch (stick_phase(t)) {
    case 0: return STICK_ID | (ext_there() ? STICK_EXT_PAD : 0u);
    case 1: return ~b & 0x3Fu;
    case 2: return ~(b >> 6) & 0x3Fu;
    case 3: return ext_there() ? ~ext_buttons() & 0x3Fu : 0x3Fu;
    case 4: return ext_there() ? ~(ext_buttons() >> 6) & 0x3Fu : 0x3Fu;
    case 5: return STICK_STATUS_A | (stick.coins ? 0x08u : 0u);
    case 6: return STICK_STATUS_B;
    case 7: return ports.switches;
    default: return 0x3Fu;
    }
}

/* ---- the adapter */

static int tap_pad_three(int s) { return pads[s].three; }

/* A TH edge the pad sees at time t: its count of rises, which it forgot after its timeout, and its timeout started again. */
static void tap_pad_edge(int s, int th, uint64_t t)
{
    TapPad *p = &tap.pad[s];
    if (tap_pad_three(s) || t - p->last_edge > TAP_PAD_TIMEOUT)
        p->count = 0;
    if (th && !tap_pad_three(s))
        p->count = (p->count + 1u) & 3u;
    p->th = th;
    p->last_edge = t;
}

/* Up to t: a line left undriven while low rises by itself. */
static void tap_pad_catch_up(int s, uint64_t t)
{
    TapPad *p = &tap.pad[s];
    if (!p->driven && !p->th && t >= p->free_from + TAP_PAD_FLOAT)
        tap_pad_edge(s, 1, p->free_from + TAP_PAD_FLOAT);
}

/* Which socket's pad sees the console's TH now, and at what level: after any write to port 1 or port 2. */
static void tap_drive(uint64_t t)
{
    int s, th_out = !(regs[R_CTRL1] & 0x40u) ? -1 : (regs[R_DATA1] & 0x40u) != 0;
    for (s = 0; s < 3; s++) {
        TapPad *p = &tap.pad[s];
        tap_pad_catch_up(s, t);
        if ((unsigned)s == tap.n && th_out >= 0) {
            if (th_out != p->th)
                tap_pad_edge(s, th_out, t);
            p->driven = 1;
        } else if (p->driven) {
            p->driven = 0;
            p->free_from = t;
        }
    }
}

/* Port 2 as the adapter sees it, after a write: it takes the socket's number when bits 1-0 show 0. */
static void tap_select(uint64_t t)
{
    uint32_t shown = (~(uint32_t)regs[R_CTRL2] | regs[R_DATA2]) & 0x7Fu;
    if (!(shown & 3u))
        tap.n = (shown >> 4) & 7u;
    tap_drive(t);
}

static uint32_t tap_pad_lines(int s, uint64_t t)
{
    TapPad *p = &tap.pad[s];
    uint32_t b = pads[s].buttons, up = 0;
    unsigned count;
#define HELD(mask, bit) do { if (!(b & (mask))) up |= 1u << (bit); } while (0)
    tap_pad_catch_up(s, t);
    count = tap_pad_three(s) || t - p->last_edge > TAP_PAD_TIMEOUT ? 0u : p->count;
    if (p->th) {
        if (count == 3) {
            HELD(PC_PAD_Z, 0); HELD(PC_PAD_Y, 1); HELD(PC_PAD_X, 2); HELD(PC_PAD_MODE, 3);
        } else {
            HELD(PC_PAD_UP, 0); HELD(PC_PAD_DOWN, 1); HELD(PC_PAD_LEFT, 2); HELD(PC_PAD_RIGHT, 3);
        }
        HELD(PC_PAD_B, 4); HELD(PC_PAD_C, 5);
    } else {
        if (count == 3) {
            up |= 0xFu;
        } else if (count != 2) {
            HELD(PC_PAD_UP, 0); HELD(PC_PAD_DOWN, 1);
        }
        HELD(PC_PAD_A, 4); HELD(PC_PAD_START, 5);
    }
#undef HELD
    return up;
}

/* What port 1 shows through the adapter: the chosen socket's pad, or the adapter's ID. */
static uint32_t tap_port1(uint64_t t)
{
    uint32_t ctrl = regs[R_CTRL1], data = regs[R_DATA1], lines;
    if (tap.n >= TAP_SOCKETS)
        return (data & 0x80u) | TAP_ID;
    lines = tap.n < 3 && (tap.n < 2 || present3) ? tap_pad_lines((int)tap.n, t) : 0x3Fu;
    return (data & 0x80u) | (data & ctrl & 0x7Fu) | ((lines | 0x40u) & ~ctrl & 0x7Fu);
}

/* ---- the chip */

void pc_io_reset(void)
{
    int i;
    uint32_t buttons[PC_IO_PADS], three[PC_IO_PADS];
    for (i = 0; i < PC_IO_PADS; i++) {
        buttons[i] = pads[i].buttons;
        three[i] = (uint32_t)pads[i].three;
    }
    memset(regs, 0, sizeof(regs));
    memset(pads, 0, sizeof(pads));
    ports = wanted;
    /* The stick comes out of reset before a frame, with no coin waiting. */
    stick.phase = STICK_PRE;
    stick.th = 1;
    stick.last_edge = 0;
    stick.coins = 0;
    stick.ack_before = 0;
    stick.buttons = buttons[0];
    /* The adapter on socket A, each socket's pad with TH high and its count at 3, long timed out, its line not driven. */
    memset(&tap, 0, sizeof(tap));
    for (i = 0; i < 3; i++) {
        tap.pad[i].th = 1;
        tap.pad[i].count = 3;
        tap.pad[i].last_edge = 0;
    }
    regs[R_VERSION] = version;
    regs[R_TX1] = regs[R_TX2] = regs[R_TX3] = 0xFFu;
    for (i = 0; i < PC_IO_PADS; i++) {
        pads[i].buttons = buttons[i];
        pads[i].three = (int)three[i];
        pads[i].th = 1;
    }
    if (io_log)
        fprintf(io_log, "0 X %d %02X %d %d %d\n", ports.stick, ports.switches, ports.stick_coin, ports.stick_ext, ports.tap);
}

void pc_io_set_version(uint32_t value)
{
    version = (uint8_t)value;
    regs[R_VERSION] = version;
}

void pc_io_set_pad(int pad, uint32_t buttons)
{
    if (pad < 0 || pad >= PC_IO_PADS)
        return;
    if (pad == 0 && ports.stick) {
        /* A press of Mode puts in a coin, while Mode is the coin slot. */
        if (ports.stick_coin && (buttons & PC_PAD_MODE) && !(stick.buttons & PC_PAD_MODE) && stick.coins < STICK_COINS_MAX)
            stick.coins++;
        stick.buttons = buttons;
    }
    if (io_log && pads[pad].buttons != buttons) {
        pads[pad].buttons = buttons;
        log_pads(io_log_t);
    }
    pads[pad].buttons = buttons;
}

void pc_io_set_three_button(int pad, int three)
{
    if (pad < 0 || pad >= PC_IO_PADS)
        return;
    if (io_log && pads[pad].three != three) {
        pads[pad].three = three;
        log_pads(io_log_t);
    }
    pads[pad].three = three;
}

void pc_io_set_present(int pad, int present)
{
    if (pad != 2)
        return;         /* players 1 and 2 are always there */
    if (io_log && present3 != (present != 0)) {
        present3 = present != 0;
        log_pads(io_log_t);
    }
    present3 = present != 0;
}

static void th_update(int port, uint64_t t)
{
    Pad *p = &pads[port];
    uint8_t ctrl = regs[R_CTRL1 + port], data = regs[R_DATA1 + port];
    int th = (ctrl & 0x40u) ? (data & 0x40u) != 0 : 1;      /* an input with nothing driving it floats high */
    if (port == 0 && ports.stick && th != stick.th)
        stick_edge(th, t, data, ctrl);
    if (th != p->th) {
        if (t - p->last_edge > PAD_TIMEOUT_MCLK)
            p->rises = 0;
        if (th)
            p->rises++;
        p->th = th;
        p->last_edge = t;
    }
}

/* What the pad drives on its six lines, 1 for a line not pulled low. */
static uint32_t pad_lines(int port, uint64_t t)
{
    Pad *p = &pads[port];
    uint32_t b = p->buttons, up = 0;
#define HELD(mask, bit) do { if (!(b & (mask))) up |= 1u << (bit); } while (0)
    if (t - p->last_edge > PAD_TIMEOUT_MCLK)
        p->rises = 0;
    if (p->th) {
        if (!p->three && p->rises == 3) {
            HELD(PC_PAD_Z, 0); HELD(PC_PAD_Y, 1); HELD(PC_PAD_X, 2); HELD(PC_PAD_MODE, 3);
        } else {
            HELD(PC_PAD_UP, 0); HELD(PC_PAD_DOWN, 1); HELD(PC_PAD_LEFT, 2); HELD(PC_PAD_RIGHT, 3);
        }
        HELD(PC_PAD_B, 4); HELD(PC_PAD_C, 5);
    } else {
        if (!p->three && p->rises == 2) {
            /* the low four lines all low */
        } else if (!p->three && p->rises == 3) {
            up |= 0xFu;
        } else {
            HELD(PC_PAD_UP, 0); HELD(PC_PAD_DOWN, 1);
        }
        HELD(PC_PAD_A, 4); HELD(PC_PAD_START, 5);
    }
#undef HELD
    return up;
}

static uint32_t reg_read(uint32_t index, uint64_t t)
{
    if (ports.tap && index == R_DATA1)
        return tap_port1(t);
    if (ports.tap && index == R_DATA2) {
        uint32_t ctrl = regs[R_CTRL2], data = regs[index];
        return (data & 0x80u) | (data & ctrl & 0x7Fu) | (0x7Fu & ~ctrl);     /* the adapter's port 2: what the console drives, 1 elsewhere */
    }
    if (index == R_DATA1 || index == R_DATA2) {
        int port = (int)index - R_DATA1;
        uint32_t ctrl = regs[R_CTRL1 + port], data = regs[index];
        uint32_t lines = (port == 0 && ports.stick ? stick_lines(t) : pad_lines(port, t)) | (pads[port].th ? 0x40u : 0u);
        return (data & 0x80u) | (data & ctrl & 0x7Fu) | (lines & ~ctrl & 0x7Fu);
    }
    if (index == R_DATA3) {
        uint32_t ctrl = regs[R_CTRL3], data = regs[index];
        return (data & 0x80u) | (data & ctrl & 0x7Fu) | (0x7Fu & ~ctrl);
    }
    return regs[index];
}

uint32_t pc_io_read(int size, uint32_t a, uint64_t t)
{
    uint32_t index = (a >> 1) & 15u, v = reg_read(index, t);
    pc_counts.io_reads++;
    if (io_log && (index == R_DATA1 || index == R_DATA2)) {
        fprintf(io_log, "%llu R %u %02X\n", (unsigned long long)t, (unsigned)(index - R_DATA1 + 1u), (unsigned)v);
        io_log_t = t;
    }
    if (size == 1)
        return v;
    v |= v << 8;
    return size == 2 ? v : v << 16 | v;
}

void pc_io_write(int size, uint32_t a, uint32_t v, uint64_t t)
{
    uint32_t index = (a >> 1) & 15u;
    pc_counts.io_writes++;
    if (index == R_VERSION || index > R_CTRL3)
        return;
    regs[index] = (uint8_t)v;       /* a word write's low byte; a byte write's only byte */
    (void)size;
    if (io_log && (index == R_DATA1 || index == R_CTRL1 || index == R_DATA2 || index == R_CTRL2)) {
        unsigned port = index == R_DATA1 || index == R_CTRL1 ? 1u : 2u;
        fprintf(io_log, "%llu W %u %02X %02X\n", (unsigned long long)t, port, (unsigned)regs[R_DATA1 + port - 1u], (unsigned)regs[R_CTRL1 + port - 1u]);
        io_log_t = t;
    }
    if (ports.tap) {
        if (index == R_DATA2 || index == R_CTRL2)
            tap_select(t);
        else if (index == R_DATA1 || index == R_CTRL1)
            tap_drive(t);
        return;
    }
    if (index == R_DATA1 || index == R_CTRL1)
        th_update(0, t);
    else if (index == R_DATA2 || index == R_CTRL2)
        th_update(1, t);
}
