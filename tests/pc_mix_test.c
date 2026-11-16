/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The mix against the Pocket's chain written out here in its own integers, as the logic steps it, on made-up input. No game byte, no sound device. targets/pc/pc_mix.c is included whole, so this
 * sees its private parts. Built by tests/test_pc_machine.py, which also plants faults in it and wants each one caught.
 */
#include <stdio.h>
#include <stdlib.h>

#include "pc_mix.c"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* ---- what the output calls */
#define CAPTURE_MAX (1u << 21)
static int16_t captured[CAPTURE_MAX][2];
static uint32_t captured_n;

static void capture(const int16_t *pairs, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n && captured_n < CAPTURE_MAX; i++, captured_n++) {
        captured[captured_n][0] = pairs[2u * i];
        captured[captured_n][1] = pairs[2u * i + 1u];
    }
}

static void fresh(void)
{
    memset(&mx, 0, sizeof(mx));
    q_fm.head = q_fm.tail = q_psg.head = q_psg.tail = q_dt.head = q_dt.tail = 0;
    captured_n = 0;
    pc_mix_to_device(capture);
}

/* ---- the logic, word for word */
static int64_t sign40(int64_t v) { return (int64_t)((uint64_t)v << 24) >> 24; }

typedef struct { int64_t t[3]; } RefIir;
static inline int64_t ref_y_mul(int64_t y, int64_t cy)
{
    /* y[36:0] signed, times a signed coefficient, bits 60:21 */
    int64_t y37 = (int64_t)((uint64_t)y << 27) >> 27;
    return (int64_t)(((__int128)y37 * cy) >> 21);
}
static inline int64_t ref_x_mul(int64_t x, unsigned cx)
{
    int64_t m = 0;
    if (cx & 1u) m = sign40(m + (x >> 4));
    if (cx & 2u) m = sign40(m + (x >> 3));
    if (cx & 4u) m = sign40(m + (x >> 2));
    return m;
}
/* one step, audio_filters' iir_filter: y = x + tap0 and the three taps move on. Returns y. */
static inline int64_t ref_step(RefIir *f, int32_t in)
{
    int64_t x = sign40(((__int128)in * IIR_CX) >> 20);
    int64_t y = sign40(x + f->t[0]);
    int64_t t0 = sign40(ref_x_mul(x, 3u) - ref_y_mul(y, IIR_Y0) + f->t[1]);
    int64_t t1 = sign40(ref_x_mul(x, 3u) - ref_y_mul(y, IIR_Y1) + f->t[2]);
    int64_t t2 = sign40(ref_x_mul(x, 1u) - ref_y_mul(y, IIR_Y2));
    f->t[0] = t0;
    f->t[1] = t1;
    f->t[2] = t2;
    return y;
}
static inline int32_t ref_clamp(int64_t y)
{
    int64_t top = y >> 35;     /* y[39:35], signed */
    if (top > 0) return 32767;
    if (top < -1) return -32768;
    return (int32_t)(y >> 20);
}

typedef struct { int64_t x1, y; } RefDc;
static int32_t ref_dc(RefDc *d, int32_t din, int high_rate)
{
    int64_t x = (int64_t)din << 23;
    int64_t x0 = sign40(x - (x >> (high_rate ? 11 : 10)));
    int64_t y1 = sign40(d->y - (d->y >> (high_rate ? 10 : 9)));
    int64_t y0 = sign40(x0 - d->x1 + y1);
    int top = (int)((y0 >> 38) & 3);
    d->x1 = x0;
    if (top == 1)
        d->y = ((int64_t)1 << 38) - 1;      /* {00, 38 ones} */
    else if (top == 2)
        d->y = -((int64_t)1 << 38);         /* {11, 38 zeros} */
    else
        d->y = y0;
    return (int32_t)(int16_t)(d->y >> 23);
}

/* audio_cond's sum and core_top's clip, in plain words */
static int32_t ref_input(int32_t fm, int32_t psg, int32_t dt)
{
    uint32_t md_fm = ((uint32_t)fm & 0x3FFFu) << 2;
    uint32_t amp = ((uint32_t)psg + ((uint32_t)psg >> 1)) & 0xFFFFu;
    uint32_t al = (md_fm + amp) & 0xFFFFu;
    uint32_t b15 = al >> 15 & 1u, b14 = al >> 14 & 1u;
    int32_t pre = b15 != b14 ? (b15 ? -32768 : 32767) : (int16_t)(al << 1);
    int32_t mix = pre + dt;
    return mix > 32767 ? 32767 : mix < -32768 ? -32768 : mix;
}

/* ---- made-up input: the FM, PSG and DT sources as their rates give them */
typedef struct { uint64_t t; int32_t fm[2], psg, dt[2]; int which; } Change;
static uint32_t rng = 12345u;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }     /* a textbook generator; not a game address */

#define CLK ((uint64_t)CLK_HZ)

static void against_the_integers(uint32_t seconds_tenths, int no_hold)
{
    uint64_t end = CLK * seconds_tenths / 10u, t_fm = 1008, t_dt = 1119, t_psg = 3000;
    int32_t fm[2] = {0, 0}, psg = 0, dt[2] = {0, 0};
    int32_t ramp = 0, step_fm = 5;
    RefIir iir[2] = {{{0, 0, 0}}, {{0, 0, 0}}};
    RefDc dc[2] = {{0, 0}, {0, 0}};
    uint64_t n = 0, value = 0, out_at = 0;
    int a_value = no_hold ? 128 : 256, diff_max = 0, filter_diff_max = 0;
    fresh();
    if (no_hold)
        pc_mix_no_hold();
    /* Each source's changes go in together, in time order, and the reference steps through them one filter step at a time. */
    while (n < steps_at(end)) {
        /* the next change of any source */
        uint64_t next = t_fm < t_dt ? t_fm : t_dt;
        int32_t in[2];
        uint64_t stop;
        if (t_psg < next)
            next = t_psg;
        stop = steps_at(next);
        in[0] = ref_input(fm[0], psg, dt[0]);
        in[1] = ref_input(fm[1], psg, dt[1]);
        while (n < stop) {
            int64_t y0 = ref_step(&iir[0], in[0]), y1;
            y1 = ref_step(&iir[1], in[1]);
            n++;
            (void)y0;
            (void)y1;
            if (n % (uint64_t)a_value == 0) {
                /* y as the filter stands at this step */
                int32_t v0 = ref_clamp(sign40((int64_t)(((__int128)in[0] * IIR_CX) >> 20) + iir[0].t[0]));
                int32_t v1 = ref_clamp(sign40((int64_t)(((__int128)in[1] * IIR_CX) >> 20) + iir[1].t[0]));
                int32_t o0 = ref_dc(&dc[0], v0, no_hold), o1 = ref_dc(&dc[1], v1, no_hold);
                if (value < (no_hold ? 16384u : 8192u))
                    o0 = o1 = 0;
                value++;
                /* the same point of the mix's output, once it has made it */
                pc_sound_frame_end(n * CLK / STEP_HZ + 1u);
                {
                    int f0 = abs(filter_output(0) - v0), f1 = abs(filter_output(1) - v1);
                    if (f0 > filter_diff_max) filter_diff_max = f0;
                    if (f1 > filter_diff_max) filter_diff_max = f1;
                }
                while (out_at < captured_n) {
                    int e0 = abs((int)captured[out_at][0] - o0), e1 = abs((int)captured[out_at][1] - o1);
                    int e = e0 > e1 ? e0 : e1;
                    if (e > diff_max)
                        diff_max = e;
                    out_at++;
                    break;
                }
                if (!no_hold)
                    out_at++;   /* its second copy, looked at below */
            }
        }
        /* the changes at this time, which reach the mix too */
        if (t_fm == next) {
            ramp += step_fm;
            if (ramp > 2000 || ramp < -2000)
                step_fm = -step_fm;
            fm[0] = (ramp * 2 + (int32_t)(rnd() % 64u)) & 0x3FFF;
            fm[1] = (-ramp * 3) & 0x3FFF;
            pc_sound_fm(next, fm[0], fm[1]);
            t_fm += 1008;
        }
        if (t_dt == next) {
            dt[0] = (int32_t)(rnd() % 2000u) - 1000;
            dt[1] = (int32_t)(rnd() % 40000u) - 20000;
            pc_mix_dt(next, dt[0], dt[1]);
            t_dt = (t_dt / 1119u + 1u) * 1119u;
        }
        if (t_psg == next) {
            psg = (int32_t)(rnd() % 4800u);
            pc_sound_psg(next, psg);
            t_psg += 1500u + rnd() % 30000u;
        }
    }
    pc_sound_frame_end(end);
    printf("largest difference from the integers over %llu values, %s: filter %d, after the DC blocker %d\n", (unsigned long long)value, no_hold ? "48 kHz" : "24 kHz", filter_diff_max, diff_max);
    CHECK(value > 1000u);
    CHECK(filter_diff_max <= 1);
    CHECK(diff_max <= 2);       /* the DC blocker is equal to the bit, tested above: a difference of one going in can leave as two */
    if (!no_hold) {
        uint32_t i, same = 1;
        CHECK(captured_n >= 2u * (uint32_t)value);
        for (i = 0; i + 1u < captured_n; i += 2u)
            if (captured[i][0] != captured[i + 1u][0] || captured[i][1] != captured[i + 1u][1])
                same = 0;
        CHECK(same);
    } else {
        CHECK(captured_n >= (uint32_t)value);
    }
}

int main(void)
{
    uint32_t k;
    /* value k at its ceiling, 24,000 for each 53,693,175 master clocks */
    for (k = 1; k < 500000u; k += 37u) {     /* a count; not a game address */
        uint64_t t = ((uint64_t)k * CLK + 23999u) / 24000u;
        CHECK(steps_at(t) >= 256u * (uint64_t)k);
        CHECK(steps_at(t - 1u) < 256u * (uint64_t)k);
    }
    CHECK(steps_at(CLK) == STEP_HZ);

    /* the sum and the clip to the bit, on the mix's own inputs */
    {
        static const int32_t fms[] = {0, 1, -1, 0x1FFF, 0x2000, 0x3FFF, 0xFFF, 0x1000, 0x2FFF, 0x3000};
        static const int32_t psgs[] = {0, 1, 200, 1200, 4800, 0xFFFF, 0x8000, 0x4000};
        static const int32_t dts[] = {0, 1, -1, 32767, -32768, 16000, -16000};
        unsigned a, b, c;
        fresh();
        for (a = 0; a < sizeof(fms) / sizeof(fms[0]); a++)
            for (b = 0; b < sizeof(psgs) / sizeof(psgs[0]); b++)
                for (c = 0; c < sizeof(dts) / sizeof(dts[0]); c++) {
                    int32_t want = ref_input(fms[a], psgs[b], dts[c]);
                    mx.fm[0] = fms[a];
                    mx.psg = psgs[b];
                    mx.dt[0] = dts[c];
                    inputs_changed();
                    CHECK(mx.x[0] == (double)(((int64_t)want * IIR_CX) >> 20));
                }
    }

    /* the DC blocker to the bit, its saturation too */
    {
        RefDc r[2] = {{0, 0}, {0, 0}};
        int high;
        for (high = 0; high < 2; high++) {
            uint32_t i;
            fresh();
            mx.dc_shift_x = high ? 11 : 10;
            mx.dc_shift_y = high ? 10 : 9;
            r[0].x1 = r[0].y = 0;
            for (i = 0; i < 400000u; i++) {     /* a count; not a game address */
                int32_t in = i % 7000u < 3000u ? 32767 : i % 7000u < 3500u ? -32768 : (int32_t)(rnd() % 65536u) - 32768;
                int32_t want = ref_dc(&r[0], in, high), got = dc_block(0, in);
                if (want != got) {
                    CHECK(want == got);
                    break;
                }
            }
            checks++;
        }
    }

    against_the_integers(5, 0);
    against_the_integers(5, 1);

    /* the first values are 0, as the block holds its output after a reset */
    {
        uint32_t i, zero = 1, moved = 0;
        fresh();
        pc_mix_dt(CLK * 8150u / 24000u, 20000, 20000);       /* just before the hold ends, or the DC blocker has taken it away */
        pc_sound_frame_end(CLK);
        CHECK(captured_n >= 48000u - 2u && captured_n <= 48000u);
        for (i = 0; i < 2u * 8192u; i++)
            if (captured[i][0] != 0)
                zero = 0;
        for (i = 2u * 8192u; i < 2u * 8192u + 100u; i++)
            if (captured[i][0] != 0)
                moved = 1;
        CHECK(zero);
        CHECK(moved);
        CHECK(mx.values == 24000u);
    }

    /* with no output asked for, every call does nothing and nothing waits in a queue */
    {
        memset(&mx, 0, sizeof(mx));
        q_fm.head = q_fm.tail = 0;
        CHECK(!pc_mix_on());
        pc_sound_fm(1000, 1, 1);
        pc_sound_psg(1000, 1);
        pc_mix_dt(1000, 1, 1);
        pc_sound_frame_end(2000);
        CHECK(q_fm.tail == 0 && q_psg.tail == q_psg.head && q_dt.tail == q_dt.head && mx.values == 0u);
    }

    printf("%d checks, %d failed\n", checks, fails);
    printf(fails ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    return fails ? 1 : 0;
}
