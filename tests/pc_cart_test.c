/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The firmware answering as the cart in the PC program, on a made-up ROM with nothing from the game: the bus rules of targets/pc/pc_cart.c, a post served through the firmware's own
 * dispatcher, the board timer following the machine's clock, the synth's schedule and its rules that the game never reaches, and the hold the firmware's MegaWire screen asks for. Built by tests/test_all.py when
 * the local configuration names a firmware tree.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "disp.h"
#include "pc_cart.h"
#include "pc_clock.h"
#include "pc_fiber.h"
#include "pc_synth.h"
#include "pc_vdp.h"
#include "chip_tables.h"

/* What pc_cart.c wants of the machine. */
PcCounts pc_counts;
static int stop_requests;
void pc_tick_stop_request(void) { stop_requests++; }
void pc_z80_run_to(uint64_t t) { (void)t; }
void pc_sound_log_post(uint64_t t, uint32_t address, uint32_t word) { (void)t; (void)address; (void)word; }

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

#define ROM_BYTES 0x800000u
static uint8_t file_bytes[ROM_BYTES];
static uint8_t rom_words[ROM_BYTES];

static unsigned file_word(unsigned a) { return (unsigned)file_bytes[a] << 8 | file_bytes[a + 1]; }

/* The sound memory's map and two of its free places, which the game cannot see, and the synth's 5 ms */
#define CR_BGM          0x152100u   /* not a game address */
#define CR_DACL         0x170000u   /* not a game address */
#define CR_DACR         0x170F00u   /* not a game address */
#define CR_CURVE        0x198000u   /* not a game address */
#define CR_FREE_A       0x300000u   /* not a game address */
#define CR_FREE_B       0x290000u   /* not a game address */
#define TIMEOUT_MCLK    268465u     /* not a game address */

/* ---- the firmware's two writes that act on the schedule, as its board calls leave them */
static void config_write(uint32_t v)
{
    hb.sy_config = v;
    hb.cfg_log[hb.cfg_n++ & 63u] = v;
    hb_config_apply(v);
}

static void arm_write(uint32_t v)
{
    hb.sy_arm = v;
    hb.arm_log[hb.arm_n++ & 63u] = v;
}

static unsigned blocks_seen, last_idx_seen;
static void on_block(uint32_t idx, int ym)
{
    (void)ym;
    blocks_seen++;
    last_idx_seen = idx;
}

/* ---- single blocks on made-up state, for the rules the golden vectors do not reach. Every number here comes from the logic's text. */
typedef struct { uint32_t pos, len, loop, base; uint16_t step; int16_t declick; uint16_t gain_l, gain_r; int16_t last_l, last_r, level; uint16_t cnt; uint8_t state, bb; uint16_t mute; uint8_t ar, hold, dr, sl, sus, d2r, rr, compand; } TestVoice;
typedef struct { uint32_t ptr, base, pos, len; uint8_t format, rate; uint16_t id; uint16_t gain_l, gain_r; uint8_t loop, filt; int16_t y; uint16_t delay; } TestSfx;

static TestVoice *test_voice(unsigned v) { return (TestVoice *)(void *)(hb.sy_win + 0x400u + 0x40u * v); }
static TestSfx *test_sfx(unsigned c) { return (TestSfx *)(void *)(hb.sy_win + 0x800u + 0x40u * c); }
static void put_global(uint32_t off, uint32_t v) { memcpy(hb.sy_win + off, &v, 4); }
static uint32_t get_global(uint32_t off) { uint32_t v; memcpy(&v, hb.sy_win + off, 4); return v; }
static uint16_t dac_l(uint32_t i) { return hb.cram[CR_DACL / 2u + i]; }
static uint16_t dac_r(uint32_t i) { return hb.cram[CR_DACR / 2u + i]; }

static void clean_synth(void)
{
    static uint8_t keep[0x4Cu];
    memcpy(keep, hb.sy_win, sizeof(keep));
    memset(hb.sy_win, 0, sizeof(hb.sy_win));
    memcpy(hb.sy_win, keep, sizeof(keep));
    put_global(0x008u, 0);
    put_global(0x00Cu, 0);
    put_global(0x010u, 0);
    put_global(0x014u, 0);
    put_global(0x018u, 0);
    put_global(0x01Cu, 0);
    put_global(0x020u, 0);
    put_global(0x038u, 0);
    hb.sy_config &= ~8u;
}

static void block_rules(void)
{
    uint32_t i;
    uint8_t *cram = (uint8_t *)(void *)hb.cram;

    /* The envelope, state by state, in the order of paprium_synth_env.sv. */
    {
        TestVoice *k = test_voice(3);
        clean_synth();
        put_global(0x008u, 3u);                         /* SEQON, PLAYING */
        k->state = 6; k->level = 100; k->rr = 30; k->cnt = 9;
        pc_synth_block(0, 1);
        CHECK(k->state == 6 && k->level == 70 && k->cnt == 9);      /* the soft release, which no track reaches */
        k->level = 20;
        pc_synth_block(0, 1);
        CHECK(k->state == 0 && k->level == 0 && k->cnt == 0);       /* and its end */
        k->state = 7; k->level = 100;
        pc_synth_block(0, 1);
        CHECK(k->state == 7 && k->level == 18);                     /* the fast fade: (100 - 64) / 2 */
        k->level = 64;
        pc_synth_block(0, 1);
        CHECK(k->state == 0 && k->level == 0);
        k->state = 7; k->level = -10;
        pc_synth_block(0, 1);
        CHECK(k->state == 0 && k->level == 0);                      /* a negative level ends it, by (level - 63) / 2 */
        k->state = 1; k->ar = 0; k->level = 5; k->cnt = 7;
        pc_synth_block(0, 1);
        CHECK(k->state == 1 && k->level == 0x5FF && k->cnt == 7);   /* AR 0 tops at once and stays in state 1 */
        k->ar = 0xFF; k->level = 5;
        pc_synth_block(0, 1);
        CHECK(k->state == 2 && k->level == 0x5FF && k->cnt == 0);
        k->state = 8; k->level = 77;
        pc_synth_block(0, 1);
        CHECK(k->state == 8 && k->level == 77);                     /* above 7 does nothing */
        k->state = 0;
        put_global(0x008u, 0u);                                     /* stopped: 8 at a time above 7, then 1 */
        k->level = 9;
        pc_synth_block(0, 1);
        CHECK(k->level == 1);
        pc_synth_block(0, 1);
        CHECK(k->level == 0);
        pc_synth_block(0, 1);
        CHECK(k->level == 0);
        put_global(0x008u, 2u);                                     /* playing with the sequencer off: left alone */
        k->level = 9;
        k->state = 7;
        pc_synth_block(0, 1);
        CHECK(k->level == 9 && k->state == 7);
    }

    /* The YM output stage's clamp, which the game's mix never reaches: the sum of the two lanes is held to +-0x1FFE, and each filter state halves toward zero. */
    {
        TestVoice *k = test_voice(0);
        clean_synth();
        put_global(0x008u, 3u);
        k->state = 2; k->level = 0x5FF; k->len = 60000u; k->hold = 255;
        k->step = 0x100u;
        k->base = CR_FREE_A;
        memset(cram + CR_FREE_A, 0x7F, 4096u);
        k->gain_l = k->gain_r = 0xFFFFu;
        put_global(0x00Cu, 0xFFFFFFFFu);
        put_global(0x010u, 0xFFFFu << 16 | 0xFFFFu);
        put_global(0x008u, 3u | 32u);
        pc_synth_block(0, 1);
        /* The bus carries a sum far past the clamp. The states go 0 -> 0x1FFE / 2 -> ... toward 0x1FFE, and the curve's index is that state. */
        CHECK(dac_l(0) == chip_out_curve(8192u + 0x1FFEu / 2u) >> 4);
        CHECK(dac_l(1) == chip_out_curve(8192u + 0x1FFEu / 2u) >> 4);
        CHECK(dac_l(2) == chip_out_curve(8192u + (0x1FFEu / 2u + 0x1FFEu) / 2u) >> 4);
        /* The curve is flat that far out, so the clamp shows in the two states themselves: each settles at (state + 0x1FFE) / 2 */
        CHECK((int16_t)get_global(0x038u) == 8189 && (int16_t)(get_global(0x038u) >> 16) == 8189);
        k->state = 0;
    }

    /* The curve's index on the DT path: past the table it is clamped to the ends with DTWIN 0, and reads the window with DTWIN 1. */
    {
        TestVoice *k = test_voice(0);
        clean_synth();
        put_global(0x008u, 3u);
        k->state = 2; k->level = 0x5FF; k->len = 60000u; k->hold = 255; k->step = 0x100u; k->base = CR_FREE_A;
        memset(cram + CR_FREE_A, 0x7F, 4096u);
        k->gain_l = 0xFFFFu;
        k->gain_r = 0xFFFEu;
        put_global(0x00Cu, 0xFFFFFFFFu);
        put_global(0x010u, 0xFFFFu << 16 | 0xFFFFu);
        put_global(0x008u, 3u | 32u);
        pc_synth_block(320, 0);
        CHECK(dac_l(320) == chip_out_curve(16383u));
        /* the sample's 0x7F at the gain the chain gives, 0xF9FA: 31,746 on the bus, halved at the output, so the index is 15,873 */
        hb.sy_config |= 8u;
        hb.cram[(CR_CURVE + 2u * 15873u) / 2u] = 0x0ABCu;
        k->state = 2;
        pc_synth_block(320, 0);
        CHECK(dac_l(320) == 0x0ABCu);
        hb.sy_config &= ~8u;
        k->state = 0;
    }

    /* The declick: a value above 160 ramps from 160, and an odd negative one too. */
    {
        TestVoice *k = test_voice(1);
        int16_t last = 800;
        clean_synth();
        put_global(0x008u, 3u);
        k->state = 2; k->level = 0x5FF; k->len = 60000u; k->hold = 255; k->step = 0x100u; k->base = CR_FREE_A;
        memset(cram + CR_FREE_A, 0, 4096u);
        k->gain_l = k->gain_r = 0;              /* no sample part: the ramp alone reaches the bus, and the bus reaches dac_buf at half on DT */
        k->last_l = last;
        k->last_r = -last;
        k->declick = 1000;
        pc_synth_block(0, 0);
        CHECK(k->declick == 0);
        CHECK(k->last_l == 0 && k->last_r == 0);
        /* frame 0: 160 x 800 / 8 = 16,000, halved at the output: the curve's input 8,000 is past its knee, so look further down: frame 150 has k = 10, 1,000, halved to 500 */
        CHECK(dac_l(150) == chip_out_curve(8192u + 500u));
        CHECK(dac_r(150) == chip_out_curve(8192u - 500u));
        CHECK(dac_l(159) == chip_out_curve(8192u + 50u));
        k->last_l = last;
        k->last_r = 0;
        k->declick = -1;
        pc_synth_block(0, 0);
        CHECK(k->declick == 0);
        CHECK(dac_l(150) == chip_out_curve(8192u + 500u));
        k->last_l = last;
        k->declick = 8;
        pc_synth_block(0, 0);
        CHECK(dac_l(7) == chip_out_curve(8192u + 50u));
        CHECK(dac_l(8) == chip_out_curve(8192u));
        k->state = 0;
    }

    /* The held boost: with LEVELNZ 0 the voice uses what the slot last held, the SFX section's ring address when its prefill ran. The chain's arithmetic from the logic's text. */
    {
        TestVoice *k = test_voice(2);
        uint32_t wm = 0x0100u, wavemaster = 0x0100u, gain = 0x0200u, t, t1, t2, m, a, g;
        clean_synth();
        k->state = 2; k->level = 600; k->len = 60000u; k->hold = 255; k->step = 0x100u; k->base = CR_FREE_A;
        memset(cram + CR_FREE_A, 0x40, 4096u);
        k->gain_l = k->gain_r = (uint16_t)gain;
        put_global(0x00Cu, wm | wm << 16);
        put_global(0x010u, wavemaster << 16 | 0x0030u);    /* BOOST 0x30, which the slot takes only with LEVELNZ */
        put_global(0x008u, 3u | 4u | 16u);                 /* SFXON and SFXECHO, LEVELNZ off: the DT path's SFX prefill stores the ring address */
        pc_synth_block(0, 0);
        /* the first block's voice used the old slot, and the SFX section stored the ring. The second block's voice uses the ring's address. */
        k->last_l = k->last_r = 0;
        pc_synth_block(0, 0);
        t = chip_level_T[k->level / 6];
        t1 = (wm * (0xC01E5800u + 256u)) >> 8;
        t2 = (wavemaster * t1) >> 8;
        m = t2 > 0xFFFFu ? 0xFFFFu : t2;
        a = ((gain * t) >> 8) & 0x1FFu;
        g = (m * a) >> 8;
        g = ((g > 0xFFFFu ? 0xFFFFu : g) * 6u) & 0xFFFFu;
        CHECK(k->last_l == (int16_t)((0x40 * (int32_t)g) / 256));
        k->state = 0;
    }

    /* SFX: filter codes 6 and 7 are no filter; a RATE outside 1-5 decodes nothing and fetches nothing; DELAY only counts; a sound's end with and without its loop. */
    {
        TestSfx *c = test_sfx(5);
        static const uint32_t filters[3] = {6u, 7u, 0u};
        unsigned f;
        clean_synth();
        put_global(0x008u, 4u);        /* SFXON */
        put_global(0x014u, 0x0100u | 0x0100u << 16);
        put_global(0x018u, 0x0100u);
        for (i = 0; i < 64u; i++)
            hb.cram[CR_FREE_B / 2u + i] = 0x8000u | 0x0080u;
        for (f = 0; f < 3u; f++) {
            c->id = 9; c->delay = 0; c->ptr = CR_FREE_B; c->base = CR_FREE_B; c->pos = 0; c->len = 1000u; c->format = 1; c->rate = 1;
            c->gain_l = c->gain_r = 0x0100u; c->loop = 0; c->filt = (uint8_t)filters[f]; c->y = 50;
            pc_synth_block(0, 1);
            CHECK(c->y == chip_sfx8_expand[0x80]);
            CHECK(c->ptr == CR_FREE_B + 2u * 40u);     /* 80 samples, two a word at rate 1 */
            CHECK(c->pos == 80u);
        }
        c->rate = 6; c->filt = 0; c->pos = 0; c->ptr = CR_FREE_B; c->y = 50;
        pc_synth_block(0, 1);
        CHECK(c->y == 0 && c->ptr == CR_FREE_B && c->pos == 80u);
        c->rate = 1; c->delay = 3; c->pos = 0;
        pc_synth_block(0, 1);
        CHECK(c->delay == 2 && c->pos == 0 && c->id == 9);
        c->delay = 0; c->pos = 960u; c->len = 1000u; c->ptr = CR_FREE_B; c->loop = 0; c->id = 9;
        pc_synth_block(0, 1);
        CHECK(c->id == 0 && c->pos == 1000u && c->ptr == CR_FREE_B + 2u * 20u);    /* 40 samples, then the end */
        c->id = 9; c->pos = 960u; c->ptr = CR_FREE_B + 8u; c->loop = 1;
        pc_synth_block(0, 1);
        CHECK(c->id == 9 && c->pos == 0 && c->ptr == CR_FREE_B);                   /* looped: back to its start */
        c->id = 9; c->pos = 1000u; c->ptr = CR_FREE_B + 8u; c->loop = 0; c->y = 7;
        pc_synth_block(0, 1);
        CHECK(c->id == 0 && c->pos == 1000u && c->ptr == CR_FREE_B + 8u && c->y == 7);
        put_global(0x008u, 0u);        /* SFXON off: delays do not count, and the ring index stays */
        c->id = 9; c->delay = 4;
        {
            uint32_t echos = get_global(0x020u);
            pc_synth_block(0, 1);
            CHECK(c->delay == 4 && get_global(0x020u) == echos);
        }
        c->id = 0;
    }

    /* The echo rings. The index wraps past 0xFF and the block goes to the ring in any mode; the prefill reads DELAY blocks back with the gain, toward zero. */
    {
        uint32_t a;
        clean_synth();
        put_global(0x01Cu, 0xFFu << 24);
        hb.cram[CR_BGM / 2u] = 0xBEEFu;                  /* block 0, which the wrap must write */
        hb.cram[(CR_BGM + 160u * 256u) / 2u] = 0x1234u;  /* the first word past the ring, which it must not */
        pc_synth_block(0, 1);
        CHECK(get_global(0x01Cu) >> 24 == 0u);
        CHECK(hb.cram[CR_BGM / 2u] == 0u);
        CHECK(hb.cram[(CR_BGM + 160u * 256u) / 2u] == 0x1234u);
        a = CR_BGM + 160u * 0x7Cu;
        hb.cram[a / 2u] = (uint16_t)-1000;
        hb.cram[a / 2u + 1u] = 500;
        put_global(0x008u, 8u);                             /* BGMECHO, playing off, so the bus carries the prefill alone */
        put_global(0x01Cu, 0x80u << 24 | 0x04u << 16 | 0x0080u);   /* index 0x80, delay 4, gain 0x80 */
        pc_synth_block(0, 0);
        /* (0x80 x -1000) / 256 = -500, the pair four times at frames 0-3, halved at the output on DT */
        CHECK(dac_l(0) == chip_out_curve(8192u - 250u) && dac_l(3) == chip_out_curve(8192u - 250u));
        CHECK(dac_r(0) == chip_out_curve(8192u + 125u));
        CHECK(get_global(0x01Cu) >> 24 == 0x81u);
        CHECK((int16_t)hb.cram[(CR_BGM + 160u * 0x81u) / 2u] == -500);
    }

    /* The decision, on the YM path's and DT path's words, in the two halves. */
    {
        uint32_t blk = 10, fp = 0, cur = 0, fill = 1, cnt = 0xFF, idx = 0;
        int inc, mix;
        /* DT: a change of half with fewer than NBLK done counts as incomplete, and restarts the count */
        cnt = 3; cur = 0; fill = 1;
        mix = pc_synth_decide(0, 960, 6, &blk, &fp, &cur, &fill, &cnt, 1000, &idx, &inc);
        CHECK(mix && inc && cur == 1 && fill == 0 && cnt == 1 && idx == 0 && blk == 11);
        mix = pc_synth_decide(0, 960, 6, &blk, &fp, &cur, &fill, &cnt, 1100, &idx, &inc);
        CHECK(mix && !inc && cnt == 2 && idx == 160 && blk == 12);
        cnt = 6;
        mix = pc_synth_decide(0, 960, 6, &blk, &fp, &cur, &fill, &cnt, 1200, &idx, &inc);
        CHECK(!mix && !inc && cnt == 6 && blk == 12);
        mix = pc_synth_decide(0, 960, 6, &blk, &fp, &cur, &fill, &cnt, 5, &idx, &inc);
        CHECK(mix && !inc && cur == 0 && fill == 1 && cnt == 1 && idx == 960);
        /* YM: the old frame's blocks go on past an armed tick; only then the new frame, at the buffer's start */
        fp = 1; cnt = 3; fill = 1; cur = 0; blk = 20;
        mix = pc_synth_decide(1, 800, 5, &blk, &fp, &cur, &fill, &cnt, 0, &idx, &inc);
        CHECK(mix && fp == 1 && cnt == 4 && idx == 800 + 480 && blk == 21);
        cnt = 5;
        mix = pc_synth_decide(1, 800, 5, &blk, &fp, &cur, &fill, &cnt, 0, &idx, &inc);
        CHECK(mix && fp == 0 && fill == 0 && cur == 0 && cnt == 1 && idx == 0 && blk == 22);
        fp = 0; cnt = 5;
        mix = pc_synth_decide(1, 800, 5, &blk, &fp, &cur, &fill, &cnt, 0, &idx, &inc);
        CHECK(!mix && cnt == 5 && blk == 22);
        /* HALF 0 only counts */
        cnt = 0; cur = 0; fill = 0;
        mix = pc_synth_decide(0, 0, 6, &blk, &fp, &cur, &fill, &cnt, 0, &idx, &inc);
        CHECK(!mix && cnt == 1);
    }

    clean_synth();
}

int main(void)
{
    static PcCartConfig cfg;
    unsigned a;
    uint64_t t = 0;
    pc_fiber_main_init();
    for (a = 0; a < ROM_BYTES; a += 2) {
        unsigned w = (a >> 1) * 40503u & 0xFFFFu;
        file_bytes[a] = (uint8_t)(w >> 8);
        file_bytes[a + 1] = (uint8_t)w;
        rom_words[a] = (uint8_t)w;          /* 16-bit host words, as the runtime keeps a ROM */
        rom_words[a + 1] = (uint8_t)(w >> 8);
    }
    cfg.rom_bytes = file_bytes;
    cfg.rom_size = ROM_BYTES;
    cfg.rom_words = rom_words;
    pc_cart_boot(&cfg);
    pc_synth_block_seen = on_block;
    CHECK(!pc_cart_holding());

    /* cart RAM: the boot page from the ROM, whole words, a byte write on both halves */
    CHECK(pc_cart_r16(0x0200, t) == file_word(0x0200));
    CHECK(pc_cart_r8(0x0201, t) == (file_word(0x0200) & 0xFF));
    pc_cart_w16(0x0300, 0x1234, t);
    CHECK(pc_cart_r16(0x0300, t) == 0x1234);
    pc_cart_w8(0x0301, 0x5A, t);
    CHECK(pc_cart_r16(0x0300, t) == 0x5A5A);
    {
        uint32_t bytes;
        const uint8_t *low = pc_cart_low(&bytes);
        CHECK(bytes == 0x1FE0u);
        CHECK(low[0x0300] == 0x5A);
    }

    /* above the RAM: the FM view, the plain ROM, writes go nowhere */
    CHECK(pc_cart_r16(0x2000, t) == (file_word(0x2000) ^ 0xA8A2u));
    CHECK(pc_cart_r8(0x7FFE, t) == ((file_word(0x7FFE) ^ 0xA8A2u) >> 8));
    CHECK(pc_cart_r16(0x8000, t) == file_word(0x8000));
    CHECK(pc_cart_r16(0xC000, t) == file_word(0xC000));
    pc_cart_w16(0x8000, 0xFFFF, t);
    CHECK(pc_cart_r16(0x8000, t) == file_word(0x8000));

    /* a post the firmware does not know: refused and finished by its own dispatcher, as time passes */
    pc_cart_w16(0x1FEA, 0x8E00, t);
    CHECK(pc_cart_r16(0x1FEA, t) == 0x8E00);
    t += 1000 * 100;
    CHECK(pc_cart_r16(0x1FEA, t) == 0);
    CHECK(hb.arr == 1 && hb.done == 1);
    CHECK(pc_cart_r16(0x1FE4, t) == 4);     /* busy at rest, no model bits */

    /* the window is a read queue: any address in it, past the end too, and busy once the stream is out */
    *(uint16_t *)(void *)(hb.workspace + 0x100) = 0xAAAA;
    *(uint16_t *)(void *)(hb.workspace + 0x102) = 0xBBBB;
    *(uint16_t *)(void *)(hb.workspace + 0x104) = 0xCCCC;
    hb.window_en = 1;
    hb.sptr = 0x100;
    hb.remaining = 2;
    hb.stream_valid = 1;
    CHECK(pc_cart_r16(0x1FE4, t) == 0);
    CHECK(pc_cart_r16(0xC000, t) == 0xAAAA);
    CHECK(pc_cart_r16(0xD234, t) == 0xBBBB);
    CHECK(pc_cart_r16(0x1FE4, t) == 4);
    CHECK(pc_cart_r16(0xC000, t) == 0xCCCC);
    CHECK(hb.sptr == 0x106);

    /* the mini view: the file everywhere, no window */
    hb.mini_view = 1;
    CHECK(pc_cart_r16(0xC000, t) == file_word(0xC000));
    CHECK(pc_cart_r16(0x2000, t) == file_word(0x2000));
    hb.mini_view = 0;
    hb.window_en = 0;

    /* frames: the board timer and the VS count follow the caller's clock; the housekeeping timer runs */
    {
        unsigned vs0 = hb.vs, taken0 = hb.taken[1];
        int i;
        for (i = 0; i < 10; i++) {
            t += PC_MCLK_FRAME;
            pc_cart_run_to(t);
        }
        CHECK(hb.vs - vs0 == 10);
        CHECK(hb.taken[1] > taken0);
        CHECK((uint32_t)(hb.now - (uint32_t)t) < 64u);
    }
    /* the first VS comes where the firmware's notes say, 34 lines and a bit into a frame's start: its count moves once a frame after that */

    /* The synth's schedule, on the Pocket's rules. CONFIG and ARM go in as the firmware's board calls leave them. */
    {
        unsigned b0, b1, b2, blk_before, i;
        uint32_t window_word;
        /* Instants run from the reset, in any mode, and BUSY's fall shows in CAUSE.BLK. With no CONFIG they mix nothing. */
        CHECK(BRD_SYNTH_BLKCNT(hb.sy_status) == 0);
        hb.no_irq = 1;
        hb.cause &= ~BRD_CAUSE_BLK;
        t += PC_MCLK_LINE * 40u;
        pc_cart_run_to(t);
        CHECK(hb.cause & BRD_CAUSE_BLK);
        CHECK(BRD_SYNTH_BLKCNT(hb.sy_status) == 0);
        hb.no_irq = 0;

        /* YM: 5 blocks a frame, only once the tick has armed a frame */
        config_write(2u | 1u << 16);
        t += PC_MCLK_FRAME;
        pc_cart_run_to(t);
        CHECK(BRD_SYNTH_BLKCNT(hb.sy_status) == 0);     /* nothing armed, CNT is 0xFF */
        CHECK((hb.sy_sched & 0xFFu) == 0xFFu);
        CHECK(hb.sy_sched >> 16 == 800u && (hb.sy_sched >> 8 & 15u) == 5u);
        CHECK(hb.sy_status & BRD_SYNTH_FILL);
        for (i = 0; i < 3u; i++) {
            arm_write(1u);
            t += PC_MCLK_FRAME;
            pc_cart_run_to(t);
        }
        b0 = BRD_SYNTH_BLKCNT(hb.sy_status);
        CHECK(b0 == 15u);       /* 5 a frame, and an unarmed frame after them stops at its count */
        CHECK(blocks_seen == 15u);
        CHECK(last_idx_seen == 800u + 160u * 4u || last_idx_seen == 160u * 4u);
        /* each block moves the BGM ring on by one */
        memcpy(&window_word, hb.sy_win + 0x1Cu, 4);
        CHECK(window_word >> 24 == 15u);

        /* DT: 6 blocks to a half of 960 samples, 300 a second, and the ring position follows TIM8 */
        config_write(1u | 1u << 16);
        t += PC_MCLK_FRAME;
        pc_cart_run_to(t);
        blk_before = BRD_SYNTH_BLKCNT(hb.sy_status);
        t += 53693175u;
        pc_cart_run_to(t);
        b1 = BRD_SYNTH_BLKCNT(hb.sy_status);
        CHECK(b1 - blk_before >= 299u && b1 - blk_before <= 301u);

        /* the hold-off: an instant after a post waits for DONE, so a firmware that cannot serve keeps it back until TIMEOUT */
        hb.no_irq = 1;
        pc_cart_w16(0x1FEA, 0x8E00, t);
        b1 = BRD_SYNTH_BLKCNT(hb.sy_status);
        t += PC_MCLK_LINE * 70u;                /* 4.5 ms: two instants on, and not yet the first one's TIMEOUT */
        pc_cart_run_to(t);
        b2 = BRD_SYNTH_BLKCNT(hb.sy_status);
        CHECK(BRD_SYNTH_QDEPTH(hb.sy_status) >= 2u);
        CHECK(b2 == b1);
        t += TIMEOUT_MCLK * 3u;                      /* three TIMEOUTs on, what waited has gone through, counted */
        pc_cart_run_to(t);
        CHECK(BRD_SYNTH_BLKCNT(hb.sy_status) != b2);
        CHECK(hb.sy_diag >> 8 & 0xFFu);
        hb.no_irq = 0;
        hb.sy_diag = 0;
        t += PC_MCLK_FRAME;
        pc_cart_run_to(t);

        /* back to mode 0: no block */
        config_write(0u);
        t += PC_MCLK_FRAME;
        pc_cart_run_to(t);
        b1 = BRD_SYNTH_BLKCNT(hb.sy_status);
        t += PC_MCLK_FRAME;
        pc_cart_run_to(t);
        CHECK(BRD_SYNTH_BLKCNT(hb.sy_status) == b1);
    }

    /* What the game's vectors never reach, by hand: each rule below is written out from the logic. */
    block_rules();

    /* the hold: the firmware's board call reaches the machine, and asks for a stop */
    {
        int before = stop_requests;
        t += PC_MCLK_FRAME;
        pc_cart_run_to(t);
        hb.hold = 1;                        /* as brd_md_hold(1) leaves it */
        t += PC_MCLK_LINE;
        pc_cart_run_to(t);
        CHECK(pc_cart_holding());
        CHECK(stop_requests == before + 1);
        /* a write the turn's own hold caught goes nowhere */
        pc_cart_w16(0x0400, 0x7777, t);
        CHECK(pc_cart_r16(0x0400, t) != 0x7777);
        hb.hold = 0;
        t += PC_MCLK_LINE;
        pc_cart_run_to(t);
        CHECK(!pc_cart_holding());
        CHECK(stop_requests == before + 1);
    }

    /* The firmware's own MegaWire screen. The game's boot opens this post's gate; here it is opened by hand. The firmware waits about 74 ms, holds the console, puts its page in the
       cart RAM, waits about 300 ms and lets go. */
    {
        uint32_t bytes;
        const uint8_t *low = pc_cart_low(&bytes);
        uint64_t posted, rise = 0, fall = 0;
        int before = stop_requests, i;
        uint16_t first = hb.ram[2];
        disp_g.mw_ready = 1u;
        t += PC_MCLK_LINE;
        pc_cart_run_to(t);
        posted = t;
        pc_cart_w16(0x1FEA, 0xA858, t);
        for (i = 0; i < 12000 && !fall; i++) {
            t += PC_MCLK_LINE;
            pc_cart_run_to(t);
            if (!rise && pc_cart_holding())
                rise = t;
            if (rise && !pc_cart_holding())
                fall = t;
        }
        printf("the hold rose %llu ms after the post and lasted %llu ms\n", (unsigned long long)((rise - posted) / 53693u), (unsigned long long)((fall - rise) / 53693u));
        CHECK(rise != 0 && fall != 0);
        CHECK(stop_requests == before + 1);
        CHECK(rise - posted > 60u * 53693u && rise - posted < 200u * 53693u);
        CHECK(fall - rise > 250u * 53693u && fall - rise < 450u * 53693u);
        CHECK(hb.ram[2] != first || *(const uint16_t *)(const void *)(low + 4) != 0u);  /* the page's vector for the restart is there */
        CHECK(*(const uint16_t *)(const void *)(low + 6) >= 0x200u);
    }

    printf("%d checks, %d failed\n", checks, fails);
    printf(fails ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    fflush(stdout);
    exit(fails ? 1 : 0);
}
