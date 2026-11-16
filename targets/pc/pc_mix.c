/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See pc_mix.h. The chain is the Pocket core's, in its order, as target/pocket/core_top.sv and its audio blocks have it:
 *
 *   FM        the 24 slots' sum, 14 bits, shifted up by 2                                 audio_cond
 *   PSG       level + level / 2. The low-pass filter mode is 3, no filter                 audio_cond
 *   sum       FM + PSG, then a saturating doubling                                        audio_cond
 *   DT        added, hard clip to 16 bits                                                 core_top
 *   IIR       third order, at 6,144,000 steps a second per channel                        audio_filters: its clock enable fires on every clock of 12.288 MHz, two channels in turn
 *   sampling  once in 256 of those steps, so 24,000 values a second                       audio_filters: its counter steps by 1 over 512 clocks
 *   DC block  at that same 24,000 a second                                                dc_blocker
 *   output    each value sent twice at 48 kHz                                             sound_i2s
 *
 * Left out, because they only delay: the registers between these blocks.
 *
 * The filter has no need to take every step by hand. Its input holds still between changes, so a run of steps is one multiplication by a table of powers of the filter's own matrix. In
 * doubles, this stays within one unit of the integer form on every sample of the test, tests/pc_mix_test.c. This costs about 2 % of what stepping does.
 *
 * Every source's changes wait in a queue of their own with their time on them, and the mix merges them at the frame's end, when all have run up to that time. The machine's clock is the only
 * clock here. No host time comes in.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc_mix.h"
#include "pc_sound.h"

#define CLK_HZ          53693175u
#define STEP_HZ         6144000u    /* a rate; not a game address */
#define STEPS_A_VALUE   256u        /* 24,000 values a second */
#define MUTE_VALUES     8192u       /* audio_filters holds its output at 0 this many values after a reset, 16,384 at the 48 kHz setting */

/* ---- the IIR's numbers, audio_mixer.sv */
#define IIR_CX          4258969         /* not a game address */
#define IIR_Y0          (-6216759)      /* not a game address */
#define IIR_Y1          6143386         /* not a game address */
#define IIR_Y2          (-2023767)      /* not a game address */
#define JUMP_MAX        128u

static double jump_a[JUMP_MAX + 1u][3][3], jump_b[JUMP_MAX + 1u][3];

static void jump_tables(void)
{
    const double a0 = IIR_Y0 / 2097152.0, a1 = IIR_Y1 / 2097152.0, a2 = IIR_Y2 / 2097152.0;
    const double a[3][3] = {{-a0, 1, 0}, {-a1, 0, 1}, {-a2, 0, 0}};
    const double b[3] = {3.0 / 16.0 - a0, 3.0 / 16.0 - a1, 1.0 / 16.0 - a2};
    unsigned n, i, j, k;
    memset(jump_a, 0, sizeof(jump_a));
    memset(jump_b, 0, sizeof(jump_b));
    for (i = 0; i < 3u; i++)
        jump_a[0][i][i] = 1;
    for (n = 1; n <= JUMP_MAX; n++) {
        for (i = 0; i < 3u; i++) {
            for (j = 0; j < 3u; j++) {
                double s = 0;
                for (k = 0; k < 3u; k++)
                    s += a[i][k] * jump_a[n - 1u][k][j];
                jump_a[n][i][j] = s;
            }
            /* the n-step form: s[n] = A^n s[0] + (A^(n-1) + ... + 1) B x */
            jump_b[n][i] = jump_b[n - 1u][i];
            for (k = 0; k < 3u; k++)
                jump_b[n][i] += jump_a[n - 1u][i][k] * b[k];
        }
    }
}

/* ---- the state of the chain */
static struct {
    int on, no_hold, tables_made;
    void (*device)(const int16_t *pairs, uint32_t n);
    FILE *wav;
    uint64_t wav_pairs;
    /* the inputs */
    int32_t fm[2], dt[2], psg;
    double x[2];        /* what goes into the filter, the 16-bit mix times its input gain */
    double s[2][3];     /* the filter's three states, per channel */
    uint64_t steps;     /* filter steps taken */
    uint64_t next_out;  /* the step of the next value */
    uint64_t values;    /* values made */
    int64_t dc_x1[2], dc_y[2];
    int steps_a_value, dc_shift_x, dc_shift_y;
    uint32_t mute_values;
    /* made and waiting to go out */
    int16_t out[4096][2];
    uint32_t out_n;
    uint64_t pairs_out, forced_runs;
} mx;

#define QUEUE_N 65536u
typedef struct SoundEvent { uint64_t t; int32_t a, b; } SoundEvent;
typedef struct SoundQueue { uint32_t head, tail; SoundEvent e[QUEUE_N]; } SoundQueue;
static SoundQueue q_fm, q_psg, q_dt;

static inline uint64_t steps_at(uint64_t t)
{
    uint64_t q = t / CLK_HZ, r = t % CLK_HZ;
    return q * STEP_HZ + (r * STEP_HZ) / CLK_HZ;
}

/* ---- the file and the device */
static void out_flush(void)
{
    if (!mx.out_n)
        return;
    if (mx.wav) {
        fwrite(mx.out, 4, mx.out_n, mx.wav);
        mx.wav_pairs += mx.out_n;
    }
    if (mx.device)
        mx.device(&mx.out[0][0], mx.out_n);
    mx.pairs_out += mx.out_n;
    mx.out_n = 0;
}

static inline void out_pair(int16_t l, int16_t r)
{
    if (mx.out_n == sizeof(mx.out) / sizeof(mx.out[0]))
        out_flush();
    mx.out[mx.out_n][0] = l;
    mx.out[mx.out_n][1] = r;
    mx.out_n++;
}

/* ---- the DC blocker, dc_blocker.sv, in its 40-bit integers */
static inline int64_t wrap40(int64_t v) { return (int64_t)((uint64_t)v << 24) >> 24; }

static inline int16_t dc_block(int ch, int32_t in)
{
    int64_t x = (int64_t)in << 23;
    int64_t x0 = x - (x >> mx.dc_shift_x);
    int64_t y1 = mx.dc_y[ch] - (mx.dc_y[ch] >> mx.dc_shift_y);
    int64_t y0 = wrap40(x0 - mx.dc_x1[ch] + y1);
    mx.dc_x1[ch] = x0;
    if (y0 >= (int64_t)1 << 38)
        y0 = ((int64_t)1 << 38) - 1;
    else if (y0 < -((int64_t)1 << 38))
        y0 = -((int64_t)1 << 38);
    mx.dc_y[ch] = y0;
    return (int16_t)(y0 >> 23);
}

/* the filter's output, which is its input plus its first state, clamped to 16 bits */
static inline int32_t filter_output(int ch)
{
    double y = floor((mx.x[ch] + mx.s[ch][0]) / 1048576.0);
    return y > 32767.0 ? 32767 : y < -32768.0 ? -32768 : (int32_t)y;
}

static void make_value(void)
{
    int16_t o[2];
    o[0] = dc_block(0, filter_output(0));
    o[1] = dc_block(1, filter_output(1));
    if (mx.values < mx.mute_values)
        o[0] = o[1] = 0;
    mx.values++;
    out_pair(o[0], o[1]);
    if (!mx.no_hold)
        out_pair(o[0], o[1]);
}

static inline void filter_jump(unsigned n)
{
    int ch;
    const double (*a)[3] = jump_a[n];
    const double *b = jump_b[n];
    for (ch = 0; ch < 2; ch++) {
        double *s = mx.s[ch], x = mx.x[ch];
        double s0 = a[0][0] * s[0] + a[0][1] * s[1] + a[0][2] * s[2] + b[0] * x;
        double s1 = a[1][0] * s[0] + a[1][1] * s[1] + a[1][2] * s[2] + b[1] * x;
        double s2 = a[2][0] * s[0] + a[2][1] * s[1] + a[2][2] * s[2] + b[2] * x;
        s[0] = s0;
        s[1] = s1;
        s[2] = s2;
    }
}

/* The filter runs on with its input as it is, up to the given step, and a value is read from it at every 256th. */
static void filter_to(uint64_t target)
{
    while (mx.steps < target) {
        uint64_t left = target - mx.steps, to_out = mx.next_out - mx.steps;
        unsigned n = (unsigned)(left < to_out ? left : to_out);
        if (n > JUMP_MAX)
            n = JUMP_MAX;
        filter_jump(n);
        mx.steps += n;
        if (mx.steps == mx.next_out) {
            make_value();
            mx.next_out += (uint64_t)mx.steps_a_value;
        }
    }
}

/* ---- the sum in front of the filter, audio_cond.sv and core_top.sv */
static void inputs_changed(void)
{
    int ch;
    uint32_t psg = (uint32_t)mx.psg & 0xFFFFu;
    uint32_t psg_amp = (psg + (psg >> 1)) & 0xFFFFu;
    for (ch = 0; ch < 2; ch++) {
        uint32_t fm = ((uint32_t)mx.fm[ch] << 2) & 0xFFFFu;
        uint32_t al = (fm + psg_amp) & 0xFFFFu;
        int32_t pre, mix;
        if ((al >> 15 & 1u) != (al >> 14 & 1u))
            pre = al >> 15 ? -32768 : 32767;
        else
            pre = (int16_t)(al << 1);
        mix = pre + mx.dt[ch];
        mix = mix > 32767 ? 32767 : mix < -32768 ? -32768 : mix;
        mx.x[ch] = (double)(((int64_t)mix * IIR_CX) >> 20);
    }
}

/* ---- the merge */
static inline const SoundEvent *queue_head(const SoundQueue *q) { return q->head == q->tail ? NULL : &q->e[q->head & (QUEUE_N - 1u)]; }

static void run_to(uint64_t t)
{
    for (;;) {
        const SoundEvent *f = queue_head(&q_fm), *p = queue_head(&q_psg), *d = queue_head(&q_dt), *e = NULL;
        SoundQueue *q = NULL;
        if (f && f->t <= t) { e = f; q = &q_fm; }
        if (p && p->t <= t && (!e || p->t < e->t)) { e = p; q = &q_psg; }
        if (d && d->t <= t && (!e || d->t < e->t)) { e = d; q = &q_dt; }
        if (!e)
            break;
        filter_to(steps_at(e->t));
        if (q == &q_fm) {
            mx.fm[0] = e->a;
            mx.fm[1] = e->b;
        } else if (q == &q_psg) {
            mx.psg = e->a;
        } else {
            mx.dt[0] = e->a;
            mx.dt[1] = e->b;
        }
        q->head++;
        inputs_changed();
    }
    filter_to(steps_at(t));
}

/* A queue holds many frames' worth of the most any source can make. Should one fill anyway, what it holds goes through at once, in the order of the times in hand. */
static inline void queue_put(SoundQueue *q, uint64_t t, int32_t a, int32_t b)
{
    SoundEvent *e;
    if (q->tail - q->head == QUEUE_N) {
        mx.forced_runs++;
        run_to(q->e[(q->tail - 1u) & (QUEUE_N - 1u)].t);
    }
    e = &q->e[q->tail & (QUEUE_N - 1u)];
    e->t = t;
    e->a = a;
    e->b = b;
    q->tail++;
}

/* ---- the calls */
static void start(void)
{
    if (mx.on)
        return;
    mx.on = 1;
    if (!mx.tables_made) {
        jump_tables();
        mx.tables_made = 1;
    }
    mx.steps_a_value = STEPS_A_VALUE;
    mx.dc_shift_x = 10;
    mx.dc_shift_y = 9;
    mx.mute_values = MUTE_VALUES;
    mx.next_out = mx.steps_a_value;
    inputs_changed();
}

int pc_mix_on(void) { return mx.on; }

void pc_mix_to_device(void (*push)(const int16_t *pairs, uint32_t n))
{
    start();
    mx.device = push;
}

void pc_mix_no_hold(void)
{
    start();
    mx.no_hold = 1;
    mx.steps_a_value = STEPS_A_VALUE / 2u;
    mx.dc_shift_x = 11;
    mx.dc_shift_y = 10;
    mx.mute_values = 2u * MUTE_VALUES;
    mx.next_out = mx.steps_a_value;
}

void pc_sound_fm(uint64_t t, int32_t left, int32_t right)
{
    if (mx.on)
        queue_put(&q_fm, t, left, right);
}

void pc_sound_psg(uint64_t t, int32_t level)
{
    if (mx.on)
        queue_put(&q_psg, t, level, 0);
}

void pc_mix_dt(uint64_t t, int32_t left, int32_t right)
{
    if (mx.on)
        queue_put(&q_dt, t, left, right);
}

void pc_sound_frame_end(uint64_t t)
{
    if (!mx.on)
        return;
    run_to(t);
    out_flush();
}

/* ---- the WAV file, plain PCM. Its sizes go in when it closes. */
static void wav_header(FILE *f, uint64_t pairs)
{
    uint32_t data = pairs > 0x3FFFFFFFu ? 0xFFFFFFFCu : (uint32_t)pairs * 4u;
    uint8_t h[44];
    uint32_t rate = 48000u, bytes = 192000u;    /* bytes a second; not a game address */
    uint32_t riff = data + 36u;
    memcpy(h, "RIFF", 4);
    memcpy(h + 4, &riff, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    h[16] = 16; h[17] = h[18] = h[19] = 0;
    h[20] = 1; h[21] = 0;       /* plain PCM */
    h[22] = 2; h[23] = 0;       /* stereo */
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &bytes, 4);
    h[32] = 4; h[33] = 0;       /* bytes a pair */
    h[34] = 16; h[35] = 0;      /* bits a sample */
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data, 4);
    fwrite(h, 1, sizeof(h), f);
}

void pc_mix_close(void)
{
    out_flush();
    if (mx.wav) {
        fseek(mx.wav, 0, SEEK_SET);
        wav_header(mx.wav, mx.wav_pairs);
        fclose(mx.wav);
        mx.wav = NULL;
    }
}

void pc_sound_file_open(const char *path)
{
    mx.wav = fopen(path, "wb");
    if (!mx.wav) {
        printf("no sound file at %s\n", path);
        fflush(stdout);
        return;
    }
    wav_header(mx.wav, 0);
    atexit(pc_mix_close);
    start();
}

void pc_mix_report(void)
{
    if (!mx.on)
        return;
    printf("mix: %llu values, %llu pairs out, %s a value, queues full %llu times\n", (unsigned long long)mx.values, (unsigned long long)mx.pairs_out,
           mx.no_hold ? "one pair" : "two pairs", (unsigned long long)mx.forced_runs);
}
