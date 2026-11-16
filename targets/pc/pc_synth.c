/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See pc_synth.h. A copy in C of the Pocket core's synth, block for block: the rule of each part below is the rule of its namesake in rtl/PAPRIUM/paprium_synth_*.sv. Where that logic is
 * trimmed to save gates, this file writes the plain arithmetic the logic is proved equal to.
 *
 * TIME. The logic counts tenths of a sample, +480,000 per clock, one tenth each 53,693,175. Here the same count comes out of exact arithmetic on the firmware's time, so it cannot drift:
 * TIM8, the DAC trigger, every 10 tenths, 48,000 Hz, and TIM6, the block instant, every 1,001 tenths, 479.52 Hz. A TIM8 that falls on a TIM6 counts before it. Blocks take no time, so BUSY
 * never reads 1 to the firmware, whose every read comes after this file has run up to that moment. RUN stays 0 for the same reason.
 *
 * WHAT THE LOGIC HAS AND THIS DOES NOT: the Pocket's menu freeze (this program's pause stops the whole machine), the bus RAM's and CRAM's timing, and DIAG.WBUSY and DIAG.UNDERRUN, which
 * need time to happen.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "chip_tables.h"

#include "pc_mix.h"
#include "pc_synth.h"

#define CLK_HZ          53693175u
#define TENTHS_HZ       480000u     /* a rate; not a game address */
#define TIM8_TENTHS     10u
#define TIM6_TENTHS     1001u
#define TIMEOUT_RESET   (CLK_HZ / 200u)     /* 5 ms, the register's reset value */
#define QUEUE_N         8u                  /* the TIM6 queue, MCU_INTERFACE 3.10.5 */
#define DMA_ENTRIES     1920u               /* dac_buf_L and dac_buf_R each */
#define STAMPS_N        32u                 /* the arrivals whose DT position is kept */

/* ---- the window, as MCU_INTERFACE 3.10.1, 3.10.3 and 3.10.4 lay it out */
#define R_CONFIG        0x004u
#define R_FLAGS         0x008u
#define R_WMASTER       0x00Cu
#define R_WBOOST        0x010u
#define R_SMASTER       0x014u
#define R_SMCFG         0x018u
#define R_ECHOB         0x01Cu
#define R_ECHOS         0x020u
#define R_YMFILT        0x038u
#define R_DTOUT         0x03Cu
#define R_TIMEOUT       0x044u
#define FLAG_SEQON      0x01u
#define FLAG_PLAYING    0x02u
#define FLAG_SFXON      0x04u
#define FLAG_BGMECHO    0x08u
#define FLAG_SFXECHO    0x10u
#define FLAG_LEVELNZ    0x20u

typedef struct SyVoice {
    uint32_t pos, len, loop, base;
    uint16_t step;
    int16_t declick;
    uint16_t gain_l, gain_r;
    int16_t last_l, last_r;
    int16_t env_level;
    uint16_t env_cnt;
    uint8_t env_state, bb;
    uint16_t mute;
    uint8_t ar, hold, dr, sl;       /* the rate word that states 1-3 use */
    uint8_t sus, d2r, rr, compand;  /* the rate word that the other states use */
    uint8_t reserved[0x14];
} SyVoice;

typedef struct SySfx {
    uint32_t ptr, base, pos, len;
    uint8_t format, rate;
    uint16_t id;
    uint16_t gain_l, gain_r;
    uint8_t loop, filt;
    int16_t y;
    uint16_t delay;
    uint8_t reserved[0x22];
} SySfx;

_Static_assert(sizeof(SyVoice) == 0x40 && sizeof(SySfx) == 0x40, "the window's layout");
_Static_assert(offsetof(hb_state, sy_win) % 4u == 0, "the window sits on a word");

#define VOICE(v)        ((SyVoice *)(void *)(hb.sy_win + BRD_SY_VOICE(v)))
#define SFX_CH(c)       ((SySfx *)(void *)(hb.sy_win + BRD_SY_SFX(c)))
#define GLOBAL(off)     (*(uint32_t *)(void *)(hb.sy_win + (off)))

/* ---- the sound memory's map, MCU_INTERFACE 3.8. Offsets in that memory, which the game cannot see: not game addresses */
#define CR_BGMRING      0x152100u   /* not a game address */
#define CR_SFXRING      0x15C100u   /* not a game address */
#define CR_DACBUF_L     0x170000u   /* not a game address */
#define CR_DACBUF_R     0x170F00u   /* not a game address */
#define CR_CURVE_CTR    0x198000u   /* not a game address */
#define CR_MASK         0x3FFFFFu   /* not a game address */

#define CRAM8()         ((uint8_t *)(void *)hb.cram)
static inline uint16_t cram_word(uint32_t a) { return hb.cram[(a & CR_MASK) >> 1]; }
static inline void cram_put(uint32_t a, uint16_t v) { hb.cram[(a & CR_MASK) >> 1] = v; }

/* ---- the block's two buses */
static int16_t bgm_bus[160][2], sfx_bus[80][2];

/* ---- B3, the envelope. The voice's fields are stored only where the chip stores them. */
static void envelope_voice(SyVoice *k)
{
    int32_t lev = k->env_level;
    uint32_t cnt = k->env_cnt;
    uint32_t st = k->env_state, m6;
    int32_t s, half;
    if (st > 7u)
        return;
    switch (st) {
    case 0:
        k->env_level = 0;
        return;
    case 1:
        s = (int16_t)(lev + k->ar);
        if (k->ar != 0u && k->ar != 0xFFu && s <= 0x5FE) {
            k->env_level = (int16_t)s;
            return;
        }
        k->env_level = 0x5FF;
        if (k->ar == 0u)
            return;
        k->env_cnt = 0;
        k->env_state = 2;
        return;
    case 2:
    case 4:
        m6 = (uint32_t)(st == 2u ? k->hold : k->sus) * 6u;
        cnt = (cnt + 1u) & 0xFFFFu;
        k->env_cnt = (uint16_t)cnt;
        if (cnt >= m6)
            k->env_state = (uint8_t)(st + 1u);
        return;
    case 3:
        m6 = (uint32_t)k->sl * 6u;
        s = (int16_t)(lev - k->dr);
        if (s <= (int32_t)m6) {
            k->env_level = (int16_t)m6;
            k->env_cnt = 0;
            k->env_state = 4;
        } else {
            k->env_level = (int16_t)s;
        }
        return;
    case 5:
    case 6:
        s = (int16_t)(lev - (st == 5u ? k->d2r : k->rr));
        if (s <= 0) {
            k->env_level = 0;
            k->env_cnt = 0;
            k->env_state = 0;
        } else {
            k->env_level = (int16_t)s;
        }
        return;
    default:    /* 7, the fast fade: level / 2 - 0x20, toward zero */
        half = (lev + (lev < 0 ? -63 : -64)) >> 1;
        half = (int16_t)half;
        if (half <= 0) {
            k->env_level = 0;
            k->env_state = 0;
        } else {
            k->env_level = (int16_t)half;
        }
        return;
    }
}

static void envelope(uint32_t flags)
{
    uint32_t v;
    if (!(flags & FLAG_PLAYING)) {
        /* stopped: each voice's level steps down every block while it is not 0 */
        for (v = 0; v < 16u; v++) {
            SyVoice *k = VOICE(v);
            if (k->env_level != 0)
                k->env_level = (int16_t)(k->env_level - (k->env_level > 7 ? 8 : 1));
        }
    } else if (flags & FLAG_SEQON) {
        for (v = 0; v < 16u; v++)
            envelope_voice(VOICE(v));
    }
}

/* ---- B4, the gains. The slot is the chip's held boost: with LEVELNZ 0 a voice uses whatever the slot last held, the voice loop's value or the SFX ring's address that the SFX section
   stores there. The reset value is 0; the chip never clears it. */
#define SLOT_RING       0xC01E5800u
static uint32_t boost_slot;

static uint32_t voice_gain_side(uint32_t master, uint32_t gain, uint32_t t)
{
    uint32_t t1 = (master * (boost_slot + 256u)) >> 8;
    uint32_t t2 = ((uint32_t)(GLOBAL(R_WBOOST) >> 16) * t1) >> 8;
    uint32_t m = t2 > 0xFFFFu ? 0xFFFFu : t2;
    uint32_t u = (gain * t) >> 8;
    uint32_t a = (u > 0xFFFFu ? 0xFFFFu : u) & 0x1FFu;
    uint32_t g = (m * a) >> 8;
    return ((g > 0xFFFFu ? 0xFFFFu : g) * 6u) & 0xFFFFu;
}

static void voice_gains(const SyVoice *k, uint32_t flags, uint32_t *gl, uint32_t *gr)
{
    int32_t level = k->env_level;
    uint32_t t;
    if (flags & FLAG_LEVELNZ) {
        uint32_t boost = GLOBAL(R_WBOOST) & 0xFFFFu;
        boost_slot = k->bb ? boost : boost >> 1;
    }
    if (level < 0)
        t = 0u;
    else if (level >= 3072)
        t = 510u;
    else
        t = chip_level_T[level / 6];
    *gl = voice_gain_side(GLOBAL(R_WMASTER) & 0xFFFFu, k->gain_l, t);
    *gr = voice_gain_side(GLOBAL(R_WMASTER) >> 16, k->gain_r, t);
}

/* p / 256 toward zero, kept to 16 bits */
static inline int16_t over256(int64_t p) { return (int16_t)((p + (p < 0 ? 255 : 0)) >> 8); }

/* ---- B5, the wave voices */
static void wave_voice(SyVoice *k, uint32_t flags, int ym)
{
    const uint8_t *cram = CRAM8();
    uint32_t gl, gr, ga, gb, base = k->base & CR_MASK, step = k->step;
    uint32_t pos = k->pos, len = k->len, loop = k->loop, frames;
    uint32_t n_ramp = (uint16_t)k->declick > 160u ? 160u : (uint32_t)(uint16_t)k->declick;
    int one_shot = loop == 0xFFFFFFFFu, slow, mono, ended = 0;
    int16_t last_l = 0, last_r = 0;
    if (k->env_state == 0u || len == 0u || k->env_level == 0 || k->mute != 0u) {
        k->last_l = k->last_r = 0;
        return;
    }
    voice_gains(k, flags, &gl, &gr);
    mono = gl == gr || ym;
    ga = mono ? (gl + gr) >> 1 : gl;
    gb = mono ? (gl + gr) >> 1 : gr;
    /* A block that stays inside the sample has no end test, so one that ends exactly on the sample's end reads one byte past it. The bank's layout keeps that byte the chip's. */
    slow = len < 0x1000000u && len < ((pos + step * 160u) >> 8);
    for (frames = 0; frames < 160u; frames++) {
        int32_t s;
        pos += step;
        if (slow && (pos >> 8) >= len) {
            if (one_shot) {
                ended = 1;
                break;
            }
            pos = loop << 8;
        }
        s = (int8_t)cram[(base + ((pos & 0x3FFFFFFFu) >> 8)) & CR_MASK];
        last_l = over256((int64_t)s * ga);
        last_r = over256((int64_t)s * gb);
        bgm_bus[frames][0] = (int16_t)(bgm_bus[frames][0] + last_l);
        bgm_bus[frames][1] = (int16_t)(bgm_bus[frames][1] + last_r);
    }
    /* the declick ramp, in a pass of its own: k x old last / 8, toward zero */
    if (n_ramp) {
        uint32_t i;
        for (i = 0; i < n_ramp; i++) {
            int64_t ramp = 32 * (int64_t)(n_ramp - i);
            bgm_bus[i][0] = (int16_t)(bgm_bus[i][0] + over256(ramp * k->last_l));
            bgm_bus[i][1] = (int16_t)(bgm_bus[i][1] + over256(ramp * k->last_r));
        }
        k->declick = 0;
    }
    k->pos = pos;
    /* at a one-shot's end stereo keeps the last outputs written, and mono stores 0 */
    k->last_l = ended && mono ? 0 : last_l;
    k->last_r = ended && mono ? 0 : last_r;
    if (ended)
        k->env_state = 0;
}

/* ---- B7, the echo rings. A ring pair is {L, R} s16. The bus holds a ring pair four times over. */
static void bus_prefill(int16_t (*bus)[2], uint32_t pairs, uint32_t ring, uint32_t block_bytes, uint32_t reg)
{
    uint32_t word = GLOBAL(reg), j, i;
    int32_t gain = (int16_t)word;
    uint32_t blk = ((word >> 24) - (word >> 16)) & 0xFFu;
    uint32_t a = ring + blk * block_bytes;
    for (j = 0; j < pairs / 4u; j++, a += 4u) {
        int16_t l = over256((int64_t)gain * (int16_t)cram_word(a));
        int16_t r = over256((int64_t)gain * (int16_t)cram_word(a + 2u));
        for (i = 0; i < 4u; i++) {
            bus[4u * j + i][0] = l;
            bus[4u * j + i][1] = r;
        }
    }
}

static void ring_write(const int16_t (*bus)[2], uint32_t pairs, uint32_t ring, uint32_t block_bytes, uint32_t reg)
{
    uint32_t word = GLOBAL(reg), j;
    uint32_t index = ((word >> 24) + 1u) & 0xFFu;
    uint32_t a = ring + index * block_bytes;
    GLOBAL(reg) = (word & 0x00FFFFFFu) | index << 24;
    for (j = 0; j < pairs / 4u; j++, a += 4u) {
        cram_put(a, (uint16_t)bus[4u * j][0]);
        cram_put(a + 2u, (uint16_t)bus[4u * j][1]);
    }
}

/* ---- B6, the SFX channels */
static inline uint32_t clamp_gain(uint32_t a, uint32_t b)
{
    uint32_t p = a * b;
    return p >> 24 ? 0xFFFFu : (p >> 8) & 0xFFFFu;
}

/* each sample through the channel's filter, a smoothing of five strengths. The logic divides by 3 and 5 by reciprocals, proved exact for every value the filters meet. */
static int16_t sfx_filter(uint32_t filt, int32_t x, int32_t y)
{
    int32_t v;
    uint32_t m;
    switch (filt) {
    case 1:
        v = x + y;
        return (int16_t)((v + (v < 0 ? 1 : 0)) >> 1);
    case 2:
        v = x + 2 * y;
        m = v < 0 ? (uint32_t)-v : (uint32_t)v;
        m = (uint32_t)(((uint64_t)m * 0xAAABu) >> 17) & 0xFFFFu;
        return (int16_t)(v < 0 ? -(int32_t)m : (int32_t)m);
    case 3:
        v = x + 3 * y;
        return (int16_t)((v + (v < 0 ? 3 : 0)) >> 2);
    case 4:
        v = x + 4 * y;
        m = v < 0 ? (uint32_t)-v : (uint32_t)v;
        m = (uint32_t)(((uint64_t)m * 0xCCCDu) >> 18) & 0xFFFFu;
        return (int16_t)(v < 0 ? -(int32_t)m : (int32_t)m);
    case 5:
        v = x + 7 * y;
        return (int16_t)((v + (v < 0 ? 7 : 0)) >> 3);
    default:
        return (int16_t)x;
    }
}

static void sfx_channel(SySfx *c, uint32_t gml, uint32_t gmr, int ym)
{
    uint32_t ptr = c->ptr & CR_MASK, base = c->base & CR_MASK, pos = c->pos, len = c->len;
    uint32_t filt = c->filt & 7u;
    int32_t y = c->y;
    int ended = 0, rewind;
    if (c->delay) {
        c->delay--;
        return;
    }
    if (pos >= len) {
        ended = 1;
    } else {
        uint32_t gl = clamp_gain(gml, c->gain_l), gr = clamp_gain(gmr, c->gain_r);
        uint32_t g = (gl + gr) >> 1;
        int mono = gl == gr || ym;
        uint32_t n = len >= pos + 80u ? 80u : (len - pos) & 0x7Fu;
        uint32_t rate = c->rate, i;
        int fmt8 = c->format == 1u, rate_ok = rate >= 1u && rate <= 5u;
        uint32_t h0 = rate & 7u, h1 = rate == 3u ? 2u : rate & 7u;     /* how long each byte or nibble holds: at rate 3 the odd ones hold one sample less */
        uint32_t word = 0, part = 0, held = 0;
        int need = 1;
        for (i = 0; i < n; i++) {
            int32_t x = 0;
            if (rate_ok) {
                if (need) {
                    word = cram_word(ptr);
                    ptr = (ptr + 2u) & CR_MASK;
                    need = 0;
                    part = 0;
                    held = 0;
                }
                if (fmt8)
                    x = chip_sfx8_expand[part & 1u ? word & 0xFFu : word >> 8];
                else
                    x = chip_sfx4_expand[(word >> (12u - 4u * part)) & 0xFu];
                if (++held >= (part & 1u ? h1 : h0)) {
                    held = 0;
                    if (part == (fmt8 ? 1u : 3u))
                        need = 1;
                    else
                        part++;
                }
            }
            y = sfx_filter(filt, x, y);
            {
                int16_t ol = over256((int64_t)y * (mono ? g : gl));
                int16_t or_ = mono ? ol : over256((int64_t)y * gr);
                sfx_bus[i][0] = (int16_t)(sfx_bus[i][0] + ol);
                sfx_bus[i][1] = (int16_t)(sfx_bus[i][1] + or_);
            }
        }
        pos += n;
        ended = pos >= len;
    }
    rewind = ended && c->loop != 0u;
    c->ptr = rewind ? base : ptr;
    c->pos = rewind ? 0u : pos;
    c->y = (int16_t)y;
    if (ended && c->loop == 0u)
        c->id = 0;
}

static void sfx_channels(int ym)
{
    uint32_t sm = GLOBAL(R_SMASTER), master = GLOBAL(R_SMCFG) & 0xFFFFu, ch;
    uint32_t gml = clamp_gain(sm & 0xFFFFu, master), gmr = clamp_gain(sm >> 16, master);
    for (ch = 0; ch < 8u; ch++) {
        SySfx *c = SFX_CH(ch);
        if (c->id)
            sfx_channel(c, gml, gmr, ym);
    }
}

/* ---- B8, the output stage. The SFX bus runs at 24 kHz, so each SFX frame serves two BGM frames. */
static inline uint32_t curve_at(int32_t x, int window)
{
    if (!window)
        x = x < -8192 ? -8192 : x > 8191 ? 8191 : x;
    return cram_word(CR_CURVE_CTR + 2u * (uint32_t)x);
}

static void output_stage(uint32_t idx, int ym)
{
    int window = (hb.sy_config >> 3) & 1u;
    uint32_t i, h;
    if (ym) {
        uint32_t filters = GLOBAL(R_YMFILT);
        int32_t state[2];
        state[0] = (int16_t)filters;
        state[1] = (int16_t)(filters >> 16);
        for (i = 0; i < 80u; i++) {
            for (h = 0; h < 2u; h++) {
                int32_t x = (int16_t)(bgm_bus[2u * i + h][0] + sfx_bus[i][0]), t;
                x = x < -0x1FFF ? -0x1FFE : x > 0x1FFF ? 0x1FFE : x;
                t = state[h] + x;
                state[h] = (int16_t)((t + (t < 0 ? 1 : 0)) >> 1);
                cram_put(CR_DACBUF_L + 2u * (idx + 2u * i + h), (uint16_t)(curve_at(state[h], window) >> 4));
            }
        }
        GLOBAL(R_YMFILT) = ((uint32_t)state[0] & 0xFFFFu) | (uint32_t)state[1] << 16;
    } else {
        for (i = 0; i < 80u; i++) {
            for (h = 0; h < 2u; h++) {
                int32_t bl = bgm_bus[2u * i + h][0], br = bgm_bus[2u * i + h][1];
                int32_t xl = ((bl + (bl < 0 ? 1 : 0)) >> 1) + sfx_bus[i][0];     /* the BGM lane halved toward zero */
                int32_t xr = ((br + (br < 0 ? 1 : 0)) >> 1) + sfx_bus[i][1];
                cram_put(CR_DACBUF_L + 2u * (idx + 2u * i + h), (uint16_t)curve_at(xl, window));
                cram_put(CR_DACBUF_R + 2u * (idx + 2u * i + h), (uint16_t)curve_at(xr, window));
            }
        }
    }
}

void pc_synth_block(uint32_t idx, int ym)
{
    uint32_t flags = GLOBAL(R_FLAGS), v;
    envelope(flags);

    /* the BGM bus: zero, or the ring's prefill on the DT path with its echo on */
    if (!ym && (flags & FLAG_BGMECHO))
        bus_prefill(bgm_bus, 160u, CR_BGMRING, 160u, R_ECHOB);
    else
        memset(bgm_bus, 0, sizeof(bgm_bus));

    if ((flags & FLAG_SEQON) && (flags & FLAG_PLAYING))
        for (v = 0; v < 16u; v++)
            wave_voice(VOICE(v), flags, ym);

    /* the ring takes the block in every mode, echo on or off */
    ring_write((const int16_t (*)[2])bgm_bus, 160u, CR_BGMRING, 160u, R_ECHOB);

    /* the SFX section: the same again, with the slot taking the ring's address when its prefill runs */
    if ((flags & FLAG_SFXON) && !ym && (flags & FLAG_SFXECHO)) {
        boost_slot = SLOT_RING;
        bus_prefill(sfx_bus, 80u, CR_SFXRING, 80u, R_ECHOS);
    } else {
        memset(sfx_bus, 0, sizeof(sfx_bus));
    }
    if (flags & FLAG_SFXON) {
        sfx_channels(ym);
        ring_write((const int16_t (*)[2])sfx_bus, 80u, CR_SFXRING, 80u, R_ECHOS);
    }

    output_stage(idx, ym);
}

/* ---- B2's decision, paprium_synth_tim6.sv */
int pc_synth_decide(int path, uint32_t half, uint32_t nblk, uint32_t *blk, uint32_t *fp, uint32_t *cur, uint32_t *fill, uint32_t *cnt, uint32_t pos, uint32_t *idx, int *incomplete)
{
    int mix = 0;
    uint32_t k = *cnt;
    *incomplete = 0;
    if (!path) {
        uint32_t c = pos >= half;
        if (*cur != c) {
            if (*cnt < nblk)
                *incomplete = 1;
            *cur = c;
            *fill = pos < half;
            k = 0;
        }
        *cnt = k;
        if (half == 0u) {
            if (k < nblk)
                *cnt = k + 1u;
        } else if (k < nblk) {
            mix = 1;
            *idx = (*fill ? half : 0u) + 160u * k;
            *cnt = k + 1u;
            *blk = (*blk + 1u) & 0xFFFu;
        }
    } else if (*fp && *cnt < nblk) {
        /* the old frame's next block; FPEND stays */
        if (half) {
            mix = 1;
            *idx = (*fill ? half : 0u) + 160u * *cnt;
            *blk = (*blk + 1u) & 0xFFFu;
        }
        *cnt = (*cnt + 1u) & 0xFFu;
    } else {
        if (*fp) {
            /* the new frame starts at dac_buf_L[0] */
            *fp = 0;
            *cur = 0;
            *fill = 0;
            k = 0;
        }
        *cnt = k;
        if (k < nblk) {
            if (half) {
                mix = 1;
                *idx = (*fill ? half : 0u) + 160u * k;
                *blk = (*blk + 1u) & 0xFFFu;
            }
            *cnt = k + 1u;
        }
    }
    return mix;
}

/* ---- B2, the schedule and the queue */
static struct {
    uint64_t t;                 /* what this file has run up to */
    uint64_t tim8_n;            /* TIM8 strobes done */
    uint64_t tim8_t;            /* the next one's time */
    uint64_t tim6_n;            /* TIM6 instants done */
    uint64_t tim6_t;            /* the next one's time */
    uint32_t path, half, nblk, blk, fp, cur, fill, cnt;     /* the scheduler's words */
    uint32_t dt_run, dt_off, dt_hold;                       /* the DT DMA */
    uint32_t stamp[STAMPS_N], stamps_n;
    struct { uint32_t need, pos; uint64_t t; } q[QUEUE_N];
    uint32_t q_head, q_n;
    uint32_t cfg_seen, arm_seen, done_seen;
    int32_t lp[2];              /* the DC blockers of the DT output, 16 fraction bits */
    uint64_t blocks, instants, timeouts_all, dt_samples;
} sy;

static inline uint64_t tenth_time(uint64_t n)      /* the clock of the n-th tenth, 1 up */
{
    uint64_t q = n / TENTHS_HZ, r = n % TENTHS_HZ;
    return q * CLK_HZ + (r * CLK_HZ + TENTHS_HZ - 1u) / TENTHS_HZ;
}

static inline uint64_t strobes_at(uint64_t t)      /* TIM8 strobes up to and including clock t */
{
    uint64_t q = t / CLK_HZ, r = t % CLK_HZ;
    return (q * TENTHS_HZ + (r * TENTHS_HZ) / CLK_HZ) / TIM8_TENTHS;
}

static inline uint32_t dma_sub(uint32_t a, uint32_t b) { return a >= b ? a - b : a + DMA_ENTRIES - b; }

static void status_update(void)
{
    hb.sy_status = sy.blk << 16 | (sy.q_n < 15u ? sy.q_n : 15u) << 5 | sy.cur << 4 | sy.fill << 3 | sy.fp << 2;
    hb.sy_sched = sy.half << 16 | sy.nblk << 8 | sy.cnt;
}

static void diag_add(uint32_t shift)
{
    if (((hb.sy_diag >> shift) & 0xFFu) != 0xFFu)
        hb.sy_diag += 1u << shift;
}

void (*pc_synth_block_seen)(uint32_t idx, int ym);

/* the head instant, while it can be served: what the logic does in a run of clocks with BUSY up. */
static int serve(uint64_t now)
{
    int any = 0;
    uint32_t timeout = GLOBAL(R_TIMEOUT) & 0xFFFFFFu;
    while (sy.q_n) {
        uint32_t i = sy.q_head, pos, idx = 0;
        int done_ok = (int16_t)((uint16_t)hb.done - (uint16_t)sy.q[i].need) >= 0, incomplete;
        if (!done_ok && now - sy.q[i].t < timeout && sy.q_n < QUEUE_N)
            break;
        if (!done_ok) {
            diag_add(8u);
            sy.timeouts_all++;
        }
        pos = sy.dt_run && sy.half ? dma_sub(sy.q[i].pos, sy.dt_off) : 0u;
        sy.q_head = (i + 1u) % QUEUE_N;
        sy.q_n--;
        any = 1;
        if (pc_synth_decide((int)sy.path, sy.half, sy.nblk, &sy.blk, &sy.fp, &sy.cur, &sy.fill, &sy.cnt, pos, &idx, &incomplete)) {
            pc_synth_block(idx, (int)sy.path);
            sy.blocks++;
            if (pc_synth_block_seen)
                pc_synth_block_seen(idx, (int)sy.path);
        }
        if (incomplete)
            diag_add(0u);
    }
    return any;
}

/* a CONFIG write, applied as the logic does: 3.10.2, with a DT restart dated at the arrival it names */
static void apply_config(uint32_t v, uint64_t now)
{
    uint32_t mode = v & 3u, pal = (v >> 2) & 1u, seq = v >> 16;
    uint32_t dist = (hb.arr - seq) & 0xFFFFu, stamp;
    int old = dist > 31u || dist >= sy.stamps_n;
    if (old)
        diag_add(8u);
    stamp = old ? (uint32_t)(strobes_at(now) % DMA_ENTRIES) : sy.stamp[seq % STAMPS_N];
    if (mode == 1u) {
        sy.path = 0;
        sy.half = 960;
        sy.nblk = 6;
        sy.fill = 1;
        sy.cur = 0;
        sy.cnt = 0xFF;
        if (!sy.dt_run)
            sy.dt_off = dma_sub(stamp, sy.dt_hold);
        sy.dt_run = 1;
    } else {
        if (mode == 2u) {
            sy.path = 1;
            sy.half = pal ? 960 : 800;
            sy.nblk = pal ? 6 : 5;
            sy.fill = 1;
            sy.cur = 0;
            sy.cnt = 0xFF;
        } else {
            sy.half = 0;
        }
        if (sy.dt_run) {
            sy.dt_hold = dma_sub(stamp, sy.dt_off);
            if (pc_mix_on())
                pc_mix_dt(now, 0, 0);   /* outside DT the output is 0 */
        }
        sy.dt_run = 0;
    }
}

/* B9, the DT playback: at each TIM8 strobe, the entry the DMA points at, through the coupling capacitor's stand-in and DTOUT */
static inline int32_t dt_output(uint32_t code, int side, uint32_t dtout)
{
    int32_t x_in = (int32_t)code - 2047, x = x_in, y;
    uint32_t dck = (dtout >> 17) & 15u;
    int64_t product;
    if (dck) {
        x = x_in - (int32_t)((sy.lp[side] + 0x8000) >> 16);
        sy.lp[side] += (int32_t)((((int64_t)x_in * 65536) - sy.lp[side]) >> dck);
    }
    product = ((int64_t)x * (dtout & 0xFFFFu)) >> 8;
    if (dtout & 0x10000u)
        product = -product;
    y = product > 32767 ? 32767 : product < -32768 ? -32768 : (int32_t)product;
    return y;
}

static void tim8_strobe(uint64_t t)
{
    if (sy.dt_run && pc_mix_on()) {
        /* n strobes are done, so this one is number n + 1, at DMA count n */
        uint32_t i = dma_sub((uint32_t)(sy.tim8_n % DMA_ENTRIES), sy.dt_off);
        uint32_t dtout = GLOBAL(R_DTOUT);
        int32_t l = dt_output(cram_word(CR_DACBUF_L + 2u * i) & 0xFFFu, 0, dtout);
        int32_t r = dt_output(cram_word(CR_DACBUF_R + 2u * i) & 0xFFFu, 1, dtout);
        pc_mix_dt(t, l, r);
        sy.dt_samples++;
    }
    sy.tim8_n++;
    sy.tim8_t = tenth_time((sy.tim8_n + 1u) * TIM8_TENTHS);
}

static void tim6_instant(uint64_t t)
{
    uint32_t k = (sy.q_head + sy.q_n) % QUEUE_N;
    sy.q[k].need = hb.arr;
    sy.q[k].pos = (uint32_t)(sy.tim8_n % DMA_ENTRIES);
    sy.q[k].t = t;
    sy.q_n++;
    sy.instants++;
    sy.tim6_n++;
    sy.tim6_t = tenth_time((sy.tim6_n + 1u) * TIM6_TENTHS);
}

void pc_synth_step(uint32_t now32)
{
    int32_t moved = (int32_t)(now32 - (uint32_t)sy.t);
    uint64_t now = moved > 0 ? sy.t + (uint32_t)moved : sy.t;
    int any = 0, changed = 0;

    /* the firmware's two writes that act on the schedule. Every firmware store comes after a board call, so each is taken here before the time it follows moves on. */
    while (sy.cfg_seen != hb.cfg_n) {
        apply_config(hb.cfg_log[sy.cfg_seen & 63u], sy.t);
        sy.cfg_seen++;
        changed = 1;
    }
    while (sy.arm_seen != hb.arm_n) {
        sy.fp = hb.arm_log[sy.arm_seen & 63u] & 1u;
        sy.arm_seen++;
        changed = 1;
    }
    if (sy.done_seen != hb.done) {
        sy.done_seen = hb.done;
        if (sy.q_n)
            any |= serve(sy.t);
    }

    while (sy.tim8_t <= now || sy.tim6_t <= now) {
        if (sy.tim8_t <= sy.tim6_t) {       /* a TIM8 that falls on a TIM6 counts before it */
            uint64_t t = sy.tim8_t;
            if (sy.q_n)
                any |= serve(t);
            tim8_strobe(t);
        } else {
            uint64_t t = sy.tim6_t;
            if (sy.q_n)
                any |= serve(t);
            tim6_instant(t);
            any |= serve(t);
        }
        changed = 1;
    }
    if (sy.q_n)
        any |= serve(now);
    sy.t = now;

    if (any)
        hb.cause |= BRD_CAUSE_BLK;      /* BUSY fell. The bit is a level: one or many falls since the firmware looked read the same */
    if (any || changed)
        status_update();
}

void pc_synth_arrival(uint64_t t)
{
    sy.stamp[hb.arr % STAMPS_N] = (uint32_t)(strobes_at(t) % DMA_ENTRIES);
    if (sy.stamps_n < STAMPS_N)
        sy.stamps_n++;
}

uint32_t pc_synth_next_wake(void)
{
    uint64_t next = sy.tim6_t;
    if (sy.q_n) {
        uint64_t limit = sy.q[sy.q_head].t + (GLOBAL(R_TIMEOUT) & 0xFFFFFFu);
        if (limit < next)
            next = limit;
    }
    return (uint32_t)next;
}

/* The output curve, written at every boot on the Pocket. Index 0 is at CR_CURVE_CTR. */
static void curve_write(void)
{
    unsigned i;
    for (i = 0; i < 16384u; i++)
        cram_put(CR_CURVE_CTR - 16384u + 2u * i, chip_out_curve(i));
}

void pc_synth_reset(void)
{
    memset(&sy, 0, sizeof(sy));
    memset(bgm_bus, 0, sizeof(bgm_bus));
    memset(sfx_bus, 0, sizeof(sfx_bus));
    boost_slot = 0;
    sy.tim8_t = tenth_time(TIM8_TENTHS);
    sy.tim6_t = tenth_time(TIM6_TENTHS);
    GLOBAL(R_TIMEOUT) = TIMEOUT_RESET;
    chip_level_init();
    curve_write();
    status_update();
}

void pc_synth_report(void)
{
    printf("synth: %llu instants, %llu blocks, blocks since their count wrapped %u, served on timeout %llu, DT samples %llu, DMA %s, status %08X, sched %08X, diag %08X\n",
           (unsigned long long)sy.instants, (unsigned long long)sy.blocks, (unsigned)sy.blk, (unsigned long long)sy.timeouts_all, (unsigned long long)sy.dt_samples,
           sy.dt_run ? "running" : "held", (unsigned)hb.sy_status, (unsigned)hb.sy_sched, (unsigned)hb.sy_diag);
}
