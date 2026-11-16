/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The sound's two halves as one: the real machine, firmware, synth and mix, a made-up ROM and made-up Z80 programs, and the sound file read back. A Z80 program turns a PSG tone on, the next turns it off and
 * plays an FM note, then the Z80's reset silences the FM chip, then a wave voice plays on the cart's own DAC path. Each source must reach the file in its own stretch of time, and only there. Nothing here says
 * how anything sounds: it says that a source is connected. Built by tests/test_pc_machine.py with each connection broken in turn.
 *
 * Usage: pc_sound_join_test FILE.wav
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "pt_rt.h"
#include "pc_cart.h"
#include "pc_clock.h"
#include "pc_fiber.h"
#include "pc_io.h"
#include "pc_mix.h"
#include "pc_sound.h"
#include "pc_vdp.h"
#include "pc_z80.h"

/* What the machine wants of a program. */
PtCtx pc_ctx;
void pt_watch_hit(int index) { (void)index; }
int pt_interp_only;
u32 pt_fallback(PtCtx *c, u32 pc) { (void)c; (void)pc; return 0xFFFFFFFFu; }
void pc_machine_frame_start(uint32_t frame) { (void)frame; }
void pc_machine_frame_end(uint32_t frame) { (void)frame; }

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

#define ROM_BYTES 0x800000u
#define FRAME_MCLK 896040u      /* a rate; not a game address */
#define CLK_HZ 53693175u
#define PAIRS_HZ 48000u
#define HEARD 100               /* more than any rounding: this much away from 0 means the source got through */

static uint8_t file_bytes[ROM_BYTES];
static uint8_t rom_words[ROM_BYTES];

/* LD A,n; LD (7F11h),A three times: a PSG tone on channel 0 at full level. Then it stays. */
static const uint8_t psg_on[] = {0x3E, 0x8E, 0x32, 0x11, 0x7F, 0x3E, 0x0F, 0x32, 0x11, 0x7F, 0x3E, 0x90, 0x32, 0x11, 0x7F, 0x18, 0xFE};

/* PSG channel 0 off. Then pairs of register and value to the FM chip's first port from the table at 0100h, with a pause after each write as a real driver has. */
static const uint8_t fm_on[] = {
    0x3E, 0x9F, 0x32, 0x11, 0x7F,       /* LD A,9Fh; LD (7F11h),A */
    0x21, 0x00, 0x01,                   /* LD HL,0100h */
    0x06, 12,                           /* LD B,12 */
    0x7E, 0x32, 0x00, 0x40, 0x23,       /* loop: LD A,(HL); LD (4000h),A; INC HL */
    0x0E, 40, 0x0D, 0x20, 0xFD,         /* LD C,40; DEC C; JR NZ,-3 */
    0x7E, 0x32, 0x01, 0x40, 0x23,       /* LD A,(HL); LD (4001h),A; INC HL */
    0x0E, 40, 0x0D, 0x20, 0xFD,         /* LD C,40; DEC C; JR NZ,-3 */
    0x10, 0xEA,                         /* DJNZ loop */
    0x18, 0xFE                          /* JR $ */
};
/* One operator of channel 1, on at full level: algorithm, both sides, multiple, level, attack, decays, release, pitch, the key. */
static const uint8_t fm_table[] = {0xB0, 0x07, 0xB4, 0xC0, 0x30, 0x01, 0x40, 0x00, 0x50, 0x1F, 0x60, 0x00, 0x70, 0x00, 0x80, 0x0F, 0x90, 0x00, 0xA4, 0x22, 0xA0, 0x69, 0x28, 0xF0};

static const uint8_t idle[] = {0x18, 0xFE};

/* The 68000's part: holds the Z80 in reset, loads a program, lets it go. */
static void load_z80(const uint8_t *p, uint32_t n, const uint8_t *table, uint32_t table_n)
{
    uint32_t a;
    pt_port_write(1, 0xA11200u, 0u);
    pt_port_write(1, 0xA11100u, 1u);
    for (a = 0; a < n; a++)
        pt_port_write(1, 0xA00000u + a, p[a]);
    for (a = 0; a < table_n; a++)
        pt_port_write(1, 0xA00100u + a, table[a]);
    pt_port_write(1, 0xA11100u, 0u);
    pt_port_write(1, 0xA11200u, 1u);
}

static void run_frames(unsigned n)
{
    while (n--) {
        uint64_t now;
        pc_clock_stall(FRAME_MCLK);
        now = pc_clock_now();
        pc_z80_run_to(now);
        pc_cart_run_to(now);
        pc_sound_frame_end(now);
    }
}

static int16_t (*pairs)[2];
static uint32_t pairs_n;

/* The largest output between two times of the machine's clock. */
static int largest(uint64_t from, uint64_t to)
{
    uint32_t i, a = (uint32_t)(from * PAIRS_HZ / CLK_HZ), b = (uint32_t)(to * PAIRS_HZ / CLK_HZ);
    int m = 0;
    if (b > pairs_n)
        b = pairs_n;
    for (i = a; i < b; i++) {
        int l = abs(pairs[i][0]), r = abs(pairs[i][1]);
        if (l > m) m = l;
        if (r > m) m = r;
    }
    return m;
}

/* A square wave of 8-bit samples in sound memory that the game cannot see, for one wave voice. Offsets in that memory: not game addresses. */
#define CR_FREE 0x300000u   /* not a game address */
typedef struct { uint32_t pos, len, loop, base; uint16_t step; int16_t declick; uint16_t gain_l, gain_r; int16_t last_l, last_r, level; uint16_t cnt; uint8_t state, bb; uint16_t mute; uint8_t ar, hold, dr, sl, sus, d2r, rr, compand; } JoinVoice;

static void put_global(uint32_t off, uint32_t v) { memcpy(hb.sy_win + off, &v, 4); }

static void dt_voice_on(void)
{
    JoinVoice *k = (JoinVoice *)(void *)(hb.sy_win + 0x400u);
    uint8_t *cram = (uint8_t *)(void *)hb.cram;
    uint32_t i;
    for (i = 0; i < 0x10000u; i++)
        cram[CR_FREE + i] = (i >> 5) & 1u ? 0x40u : 0xC0u;
    memset(k, 0, sizeof(*k));
    k->state = 2;
    k->level = 0x5FF;
    k->len = 0xFFFFu;
    k->loop = 0;
    k->hold = 255;
    k->step = 0x100u;
    k->base = CR_FREE;
    k->gain_l = k->gain_r = 0x100u;
    put_global(0x00Cu, 0x01000100u);    /* the wave master, each side */
    put_global(0x010u, 0x01000000u);    /* the wave boost */
    put_global(0x008u, 3u | 32u);       /* the sequencer on, playing, level not zero */
    hb.sy_config = 1u;                  /* the DT path, as the firmware's configuration writes it */
    hb.cfg_log[hb.cfg_n++ & 63u] = 1u;
    hb_config_apply(1u);
}

int main(int argc, char **argv)
{
    static PcCartConfig cfg;
    uint32_t a;
    uint64_t t0, t_psg, t_fm, t_reset, t_dt, t_end;
    FILE *f;
    long size;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc != 2)
        return 2;
    for (a = 0; a < ROM_BYTES; a += 2) {
        uint32_t w = (a >> 1) * 40503u & 0xFFFFu;
        file_bytes[a] = (uint8_t)(w >> 8);
        file_bytes[a + 1] = (uint8_t)w;
        rom_words[a] = (uint8_t)w;
        rom_words[a + 1] = (uint8_t)(w >> 8);
    }
    pt_rom = rom_words;
    pt_rom_size = ROM_BYTES;
    pt_rom_bus_end = ROM_BYTES;
    pt_low_device_end = 0x10000u;
    pc_ctx.imask = 7;
    cfg.rom_bytes = file_bytes;
    cfg.rom_size = ROM_BYTES;
    cfg.rom_words = rom_words;
    cfg.no_bank = 1;
    pc_sound_file_open(argv[1]);
    pc_fiber_main_init();
    pc_clock_reset();
    pc_tick_init();
    pc_vdp_reset();
    pc_io_reset();
    pc_z80_reset();
    pc_cart_boot(&cfg);
    load_z80(idle, sizeof(idle), NULL, 0);

    run_frames(40);             /* past the output's hold at the start */
    t0 = pc_clock_now();
    run_frames(10);
    load_z80(psg_on, sizeof(psg_on), NULL, 0);
    t_psg = pc_clock_now();
    run_frames(10);
    load_z80(fm_on, sizeof(fm_on), fm_table, sizeof(fm_table));
    t_fm = pc_clock_now();
    run_frames(10);
    load_z80(idle, sizeof(idle), NULL, 0);     /* the reset line silences the FM chip */
    t_reset = pc_clock_now();
    run_frames(30);
    dt_voice_on();
    t_dt = pc_clock_now();
    run_frames(10);
    t_end = pc_clock_now();
    pc_mix_close();

    f = fopen(argv[1], "rb");
    CHECK(f != NULL);
    if (!f)
        return 1;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 44, SEEK_SET);
    pairs_n = (uint32_t)(size - 44) / 4u;
    pairs = (int16_t (*)[2])malloc(pairs_n * 4u);
    CHECK(fread(pairs, 4, pairs_n, f) == pairs_n);
    fclose(f);

    printf("largest output: before anything %d, PSG %d, FM %d, after the reset %d, DT %d\n", largest(t0, t_psg), largest(t_psg + FRAME_MCLK, t_fm), largest(t_fm + 5u * FRAME_MCLK, t_reset),
           largest(t_reset + 25u * FRAME_MCLK, t_dt), largest(t_dt + 3u * FRAME_MCLK, t_end));
    CHECK(largest(t0, t_psg) < HEARD);
    CHECK(largest(t_psg + FRAME_MCLK, t_fm) >= HEARD);
    CHECK(largest(t_fm + 5u * FRAME_MCLK, t_reset) >= HEARD);
    CHECK(largest(t_reset + 25u * FRAME_MCLK, t_dt) < HEARD);
    CHECK(largest(t_dt + 3u * FRAME_MCLK, t_end) >= HEARD);

    printf("%d checks, %d failed\n", checks, fails);
    printf(fails ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    exit(fails ? 1 : 0);
}
