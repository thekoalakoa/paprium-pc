/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The PC port's one shared interface: what the machine model hands the renderer for one picture.
 *
 * THE PC PORT
 *   The game's translated 68000 program runs on this PC against a machine model of our own: no reference emulator is linked, and nothing
 *   under targets/pc includes anything from rig/. The firmware answers as the cart. The picture goes to a window, playable with keyboard and
 *   pads. The sound comes from the game's own driver on a Z80, the FM chip, the PSG and the cart's synth, through the Pocket core's output chain: see pc_sound.h.
 *
 * TWO PARTS THAT BUILD ALONE
 *   The machine part runs the program and produces a PcFrame for each picture.
 *   The renderer part turns a PcFrame into a PcPicture. It is a plain function of its input: no clock, no window, no global machine state.
 *   This header is all they share. A change here is agreed by both parts.
 *
 *   The machine part owns:
 *     tool/cycles68k.py        new: 68000 instruction times from the published tables
 *     tool/emit_c.py           each block charges its time; taken branches, shifts and multiplies charge what they add; a device access in an interrupt handler says how
 *                              far into its block it comes
 *     tool/regions.py          a straight block longer than a set number of instructions gets a safe point inside it
 *     ptool.py                 commands to list the firmware's boot page changes and to build the PC program
 *     runtime/pt_rt.h          the speed build's block macro: the clock and the deadline; a fetch from the cart RAM when the low page no longer holds what was translated
 *     runtime/pt_port.h        the clock, the deadline, the tick and the restart
 *     runtime/pt_rt.c          an interrupt's entry charges its time; the dispatcher sends the low page to the interpreter when it must
 *     runtime/pt_fallback.c    the interpreter charges time too, and stays in the low page while that page runs from the cart RAM
 *     targets/pc/pc_main.c     start-up, the run loop, headless mode for tests
 *     targets/pc/pc_clock.c/.h the master clock, lines and frames from the counted time, pacing at the frame boundary
 *     targets/pc/pc_vdp.c/.h   the video chip's ports, registers, memories, write queue, DMA, counters, status; fills PcFrame
 *     targets/pc/pc_bus.c      pt_port_read and pt_port_write: which device answers which address
 *     targets/pc/pc_io.c/.h    the I/O chip: version register, three and six button pads on the game's own read sequence
 *     targets/pc/pc_z80.c/.h   the Z80 and its bus, with its bus request, its reset and its RAM as the 68000 sees them; pc_ym.c and pc_psg.c are the two sound chips behind it
 *     targets/pc/pc_cart.c/.h  the firmware on the cart's bus, in its own fiber; the hold and the restart the firmware's MegaWire screen asks for; pc_synth.c is the cart's synth behind it
 *     targets/pc/pc_board_impl.h, pc_fiber.h  what the firmware's host board needs from this program
 *     targets/pc/pc_firmware.py, pc_fw_vars.c  which firmware files the program links, and the two variables that stand in for the old module left out (in place)
 *     targets/pc/pc_window.c/.h the window and the present, through SDL2
 *     targets/pc/pc_input.c/.h keyboard and game controllers, through SDL2
 *     targets/pc/pc_mix.c/.h   the sound's mix, as the Pocket core's output chain, and the sound file
 *     targets/pc/pc_audio.c/.h the audio device, through SDL2, behind a one-way ring
 *     targets/pc/pc_stall.c/.h the freeze log, which no part of the machine reads
 *     targets/pc/pc_config.c/.h the local configuration file; the check of the ROM's hash
 *     targets/pc/pc_save.c/.h  the cart's battery save as a file beside the program
 *     targets/pc/pc_pace.h     when the run loop gives up vsync
 *     targets/pc/pc_route.h    test routes: the same layout as the lockstep rig's route programs, with no include from rig/
 *     targets/pc/pc_nowindow.c stands in for the window and the keys in a program built without SDL, as the machine's own tests build it
 *     rig/pc_shadow.c          feeds the rig's bus writes to pc_vdp.c and compares the memories with the reference machine's, frame by frame
 *     tests/pc_machine_test.c  the machine part's tests, on made-up programs
 *     tests/test_all.py, tests/check_tree.py, tests/check_pc_link.py, THIRD_PARTY.md, targets/README.md, config.example.json, README.md: its additions
 *
 *   The renderer part owns:
 *     targets/pc/pr_render.c   pr_render below: walks the lines, applies the writes, composes, makes colours
 *     targets/pc/pr_planes.c   planes A and B, the window, scrolling in all its modes
 *     targets/pc/pr_sprites.c  the chip's sprite walk with its limits, and the walk with none
 *     targets/pc/pr_local.h    what those three share
 *     rig/pc_framedump.c       records PcFrame files and the reference machine's pictures from the rig, for pr_compare; the files stay outside the repository
 *     tests/pr_render_test.c   the renderer's tests, on made-up scenes
 *     tests/pr_compare.c       compares pr_render with recorded frames and layer files named on its command line; they stay outside the repository
 *
 *   The rig's two files link pieces of targets/pc into the rig. That direction is allowed; nothing under targets/pc may point back at rig/.
 *   Neither part puts a game address in a source file. If a target ever truly needs one it goes in one clearly named rule data file.
 *
 * WHAT A FRAME IS
 *   start holds the chip's memories and registers at the moment its V counter becomes 0, which is the start of the blanking before line 0.
 *   writes holds everything whose time falls from then until its V counter becomes 224, each with that time. Writes timed in the vertical blank are
 *   not listed: they are in the next frame's start. A write is filed by its time and not by how far the machine has stepped its lines, because the
 *   machine holds its line counter at a raised interrupt until the interrupt is taken, while the program goes on.
 *   A frame is not limited to the game's own start-up: the firmware's MegaWire screen holds and restarts the whole console, and a frame made during
 *   that hold is a blank chip, with no writes.
 *
 * WHEN A WRITE TAKES EFFECT
 *   The machine reports time, the renderer decides what that time means for each kind of fetch, because that is drawing knowledge. vline is the
 *   chip's V counter at the time of the write. hclock is master clocks since that V counter value began, 0 to 3,419. The V counter steps during
 *   the horizontal blank, so a line's blank comes before its picture.
 *
 *   A write made inside an interrupt handler carries the time it would have had on the real machine, however late the translated program really ran
 *   the handler, because that program takes interrupts only at safe points and before a write to the chip. The handler's effective start is the later of two times: when the chip
 *   raised the interrupt, plus half an average instruction; and the effective end of the handler before it, which can hold a new one off because
 *   the new one is masked until the old one returns. Each write's time is that start, plus the interrupt's entry, plus the clocks from the handler's
 *   real start up to that very access (not to the end of its block), plus any time it waited for the chip's write queue.
 *
 *   So a data port write's time is when it entered the queue, not when it landed. The renderer works out the landing, against the chip's access slots. A word of a DMA has no program
 *   time of its own, so it carries the slot where the machine puts it, and the renderer looks for a slot from there as for any other write. That places it up to a slot late. Only a
 *   DMA during the picture would show that, and the game does one only on leaving its CRT calibration screens.
 *
 *   So times can run backwards in the list where a late handler's writes follow a main program write. The renderer applies the writes in order of
 *   time, keeping the list's order among equal times. Its own copy of the chip's state then equals the machine's at the end of the frame, as long as
 *   no two writes in the frame hit one place out of order, which the game's main program does not do during the picture after start-up.
 */
#ifndef PC_FRAME_H
#define PC_FRAME_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PC_VRAM_BYTES   0x10000u
#define PC_CRAM_WORDS   64u
#define PC_VSRAM_WORDS  40u
#define PC_VDP_REGS     24u
#define PC_SAT_ENTRIES  80u         /* the chip's own copy of the sprite table holds this many entries */
#define PC_LINE_CLOCKS  3420u       /* master clocks in a line */
#define PC_PICTURE_W    320u
#define PC_PICTURE_H    240u        /* room for a 240 line mode; the game uses 224 */

/* The chip's state. vram[a] is the byte at VRAM address a, so a word at even address a is vram[a] << 8 | vram[a + 1]. */
typedef struct PcVdpState {
    uint8_t  vram[PC_VRAM_BYTES];
    uint16_t cram[PC_CRAM_WORDS];   /* colour n: 0000 BBB0 GGG0 RRR0 */
    uint16_t vsram[PC_VSRAM_WORDS]; /* 11 bits each */
    uint8_t  regs[PC_VDP_REGS];     /* the registers as written */
    uint8_t  sat_cache[PC_SAT_ENTRIES][4];  /* the chip's own copy of bytes 0-3 of each sprite entry: Y high, Y low, size, link */
} PcVdpState;

enum {
    PC_WRITE_REG,       /* addr = register number, value = low 8 bits */
    PC_WRITE_CRAM,      /* addr = colour number 0-63, value = 16 bits as they reach the chip */
    PC_WRITE_VSRAM,     /* addr = entry number 0-39 */
    PC_WRITE_VRAM       /* addr = the even byte address; value = the word as it lands there, already swapped if the 68000's address was odd */
};
enum {
    PC_WF_IN_HINT = 1   /* made inside an H-int handler; its time is the one explained above */
};

typedef struct PcWrite {
    uint16_t vline;     /* the V counter's line, 0-261 */
    uint16_t hclock;    /* master clocks since that line began, 0-3419 */
    uint8_t  kind;      /* PC_WRITE_* */
    uint8_t  flags;     /* PC_WF_* */
    uint16_t addr;
    uint16_t value;
} PcWrite;

#define PC_FRAME_WRITES_MAX 65536u

typedef struct PcFrame {
    uint32_t number;        /* frames since power-on */
    uint32_t n_writes;
    uint32_t dropped;       /* writes that did not fit; a test fails when this is not 0 */
    PcVdpState start;
    PcWrite  writes[PC_FRAME_WRITES_MAX];
} PcFrame;

/* One write applied to a state. Both parts use this, so the machine's memories and the renderer's copy follow one rule. A VRAM write inside the
   sprite table also updates the chip's own copy, for the entry's first four bytes, as the chip does. Which table: register 5, with its low
   bit ignored in the 320 pixel mode, and 80 entries there or 64 in the 256 pixel mode. */
static inline void pc_state_apply(PcVdpState *s, const PcWrite *w)
{
    switch (w->kind) {
    case PC_WRITE_REG:
        if (w->addr < PC_VDP_REGS)
            s->regs[w->addr] = (uint8_t)w->value;
        break;
    case PC_WRITE_CRAM:
        s->cram[w->addr & 63u] = (uint16_t)(w->value & 0x0EEEu);
        break;
    case PC_WRITE_VSRAM:
        if (w->addr < PC_VSRAM_WORDS)
            s->vsram[w->addr] = (uint16_t)(w->value & 0x07FFu);
        break;
    case PC_WRITE_VRAM: {
        unsigned a = w->addr, h40 = s->regs[12] & 1u;
        unsigned sat = (unsigned)(s->regs[5] & (h40 ? 0x7Eu : 0x7Fu)) << 9;
        unsigned entries = h40 ? 80u : 64u;
        s->vram[a & 0xFFFEu] = (uint8_t)(w->value >> 8);
        s->vram[(a & 0xFFFEu) + 1u] = (uint8_t)w->value;
        if ((a & 0xFFFEu) >= sat && (a & 0xFFFEu) < sat + entries * 8u) {
            unsigned off = (a & 0xFFFEu) - sat;
            if ((off & 7u) < 4u) {
                s->sat_cache[off >> 3][off & 7u] = (uint8_t)(w->value >> 8);
                s->sat_cache[off >> 3][(off & 7u) + 1u] = (uint8_t)w->value;
            }
        }
        break;
    }
    default:
        break;
    }
}

/* ---- the renderer's side */

typedef enum PrLimits {
    PR_LIMITS_CART = 0, /* the chip's rules: 20 sprites and 40 cells a line in the 320 pixel mode, the last sprite part drawn. The default */
    PR_LIMITS_NONE = 1  /* the same table with no limit on a line, walked in VRAM itself. A switch the player can flip while the game runs */
} PrLimits;

typedef struct PrOptions {
    PrLimits limits;
    int      want_layer;    /* 1: also fill PcPicture.sprite_layer, for tests */
} PrOptions;

typedef struct PcPicture {
    uint16_t width;         /* 320 or 256 */
    uint16_t height;        /* 224 */
    uint32_t rgb[PC_PICTURE_H][PC_PICTURE_W];   /* 0x00RRGGBB, the levels of the chip's colour output on the Pocket core */
    uint8_t  sprite_layer[PC_PICTURE_H][PC_PICTURE_W];  /* priority, palette and colour bits of the sprite line buffer: pppcccc, 0 where none */
    uint32_t lines_over;    /* lines where the chip's rules cut something; counted in both modes */
} PcPicture;

/* Draws one picture. frame is not changed. */
void pr_render(const PcFrame *frame, const PrOptions *options, PcPicture *out);

/* ---- a frame in a file, for tests. Both parts read and write this. Game-derived when it comes from a real run: such files stay outside the repository. */

#define PC_FRAME_FILE_MAGIC 0x31524650u

static inline int pc_frame_write(FILE *f, const PcFrame *frame)
{
    uint32_t head[4];
    head[0] = PC_FRAME_FILE_MAGIC;
    head[1] = frame->number;
    head[2] = frame->n_writes;
    head[3] = frame->dropped;
    if (fwrite(head, sizeof(head), 1, f) != 1) return -1;
    if (fwrite(&frame->start, sizeof(frame->start), 1, f) != 1) return -1;
    if (frame->n_writes && fwrite(frame->writes, sizeof(PcWrite), frame->n_writes, f) != frame->n_writes) return -1;
    return 0;
}

/* Returns 0 on a frame, 1 at the end of the file, -1 on a bad file. */
static inline int pc_frame_read(FILE *f, PcFrame *frame)
{
    uint32_t head[4];
    if (fread(head, sizeof(head), 1, f) != 1) return 1;
    if (head[0] != PC_FRAME_FILE_MAGIC || head[2] > PC_FRAME_WRITES_MAX) return -1;
    frame->number = head[1];
    frame->n_writes = head[2];
    frame->dropped = head[3];
    if (fread(&frame->start, sizeof(frame->start), 1, f) != 1) return -1;
    if (frame->n_writes && fread(frame->writes, sizeof(PcWrite), frame->n_writes, f) != frame->n_writes) return -1;
    return 0;
}

#endif /* PC_FRAME_H */
