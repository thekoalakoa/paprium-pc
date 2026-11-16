/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The audio output's ring and pull with made-up clocks and no device. targets/pc/pc_audio.c is included whole, built without SDL. Built by tests/test_pc_machine.py, which also plants faults in it
 * and wants each one caught.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define PC_AUDIO_NO_SDL
#include "pc_audio.c"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static void fresh(void)
{
    char e[8];
    pc_audio_open("test", 100, e, sizeof(e));
}

/* the machine's side at the game's pace: a frame's 801.04 samples as a block */
static void push_level(uint32_t n, int16_t v)
{
    static int16_t block[2u * 2048u];
    uint32_t i;
    for (i = 0; i < 2u * n; i++)
        block[i] = v;
    pc_audio_push(block, n);
}

/* The ring brought to x samples over its target as the pull sees it, a push of 512 coming before each pull in hold below. Only by pushes: x is never under where it is. */
static void set_over(uint32_t x)
{
    uint32_t want = dev.target - 512u + x, fill = ring_fill();
    if (fill < want)
        push_level(want - fill, 2000);
}

/* The two sides held for up to seconds, each pull of 512 after a push that brings the ring back to where it was, so the fill at each pull stays put against the rate control's pull.
   The machine says each frame that the game is behind by behind samples, and each stall_every seconds when that is not 0 a frame stall_size behind (0: two frames, a stall). Returns the
   seconds held when the delay was let go, or -1. */
static uint32_t stall_size;

static double hold(double seconds, double stall_every, uint32_t behind)
{
    static int16_t o[2u * 512u];
    uint64_t settles = dev.settles;
    uint32_t level = ring_fill() + 512u, fill;
    double t = 0, next_stall = stall_every;
    while (t < seconds) {
        if (stall_every > 0 && t >= next_stall) {
            pc_audio_pace(stall_size ? stall_size : 2u * 801u);
            next_stall += stall_every;
        } else {
            pc_audio_pace(behind);
        }
        fill = ring_fill();
        push_level(level > fill ? level - fill : 0u, 2000);
        pc_audio_pull(o, 512u);
        t += 512.0 / 48000.0;
        if (dev.settles != settles)
            return t;
    }
    return -1;
}

/* A fresh start, played and held at the target for 4 seconds: the start's own wait counts as a stall, and finds nothing over. */
static void start_playing(void)
{
    static int16_t o[2u * 512u];
    uint32_t i;
    fresh();
    for (i = 0; i < 4u; i++)
        push_level(801u, 2000);
    pc_audio_pull(o, 512u);
    set_over(0u);
    hold(4.0, 0, 0);
}

/* how far the fill, as the pull sees it, is from at */
static int off_from(uint32_t at) { return abs((int)(ring_fill() + 512u) - (int)at); }

/* A run of the two sides on made-up clocks. drift is how fast the machine's 48 kHz runs against the device's, in parts in a million. */
static void steady(double seconds, double drift_ppm, double *fill_low, double *fill_high)
{
    static int16_t out[2u * DEVICE_PAIRS];
    double t_device = 0, t_frame = 0;
    double frame_s = (3420.0 * 262.0 / 53693175.0) / (1.0 + drift_ppm * 1e-6), device_s = (double)DEVICE_PAIRS / 48000.0;
    double carry = 0;
    uint64_t settled_after = (uint64_t)(120.0 / device_s), pulls = 0;
    *fill_low = 1e9;
    *fill_high = 0;
    while (t_device < seconds) {
        if (t_frame <= t_device) {
            /* 48,000 x 3420 x 262 / 53,693,175 samples a frame: the mix's own count */
            double want = 48000.0 * 3420.0 * 262.0 / 53693175.0 + carry;
            uint32_t n = (uint32_t)want;
            carry = want - n;
            push_level(n, 1000);
            t_frame += frame_s;
        } else {
            pc_audio_pull(out, DEVICE_PAIRS);
            t_device += device_s;
            pulls++;
            if (pulls > settled_after) {
                double fill = (double)(ring_head - ring_tail);
                if (fill < *fill_low) *fill_low = fill;
                if (fill > *fill_high) *fill_high = fill;
            }
        }
    }
}

/* the resampler's level at one frequency, at a set rate */
static double level_at(double hz, double rate)
{
    static int16_t in[2u * 4100u];
    uint32_t k, i;
    double sum_out = 0, phase = 0;
    const double pi = 3.14159265358979323846;
    uint64_t n = 0;
    fresh();
    /* play past the target and the fade-in, with no control */
    for (k = 0; k < 40u; k++) {
        for (i = 0; i < 4100u; i++) {
            int16_t v = (int16_t)lrint(20000.0 * sin(phase));
            in[2u * i] = v;
            in[2u * i + 1u] = v;
            phase += 2.0 * pi * hz / 48000.0;
        }
        pc_audio_push(in, 4100u);
        {
            uint32_t pull_now;
            for (pull_now = 0; pull_now < 4096u; pull_now++) {
                int16_t o[2];
                pc_audio_pull(o, 1u);
                dev.rate = rate;    /* the test holds it, against the control */
                if (k > 4u) {
                    sum_out += (double)o[0] * (double)o[0];
                    n++;
                }
            }
        }
    }
    return sqrt(sum_out / (double)n) / (20000.0 / sqrt(2.0));
}

int main(void)
{
    static int16_t out[2u * 8192u];
    uint32_t i;
    double low, high;

    /* a full ring takes what fits and counts the rest; it never waits */
    fresh();
    for (i = 0; i < 100u; i++)
        push_level(801u, 5);
    CHECK(pushed == 100u * 801u);
    CHECK(dropped == 100u * 801u - RING_PAIRS);
    CHECK(ring_head - ring_tail == RING_PAIRS);

    /* silence until the ring holds its target: three frames and one device buffer, 60.7 ms */
    fresh();
    CHECK(dev.target == 3u * 801u + 512u);
    push_level(801u, 10000);
    pc_audio_pull(out, 512u);
    for (i = 0; i < 1024u; i++)
        CHECK(out[i] == 0);
    CHECK(dev.state == WAITING);
    push_level(801u, 10000);
    push_level(801u, 10000);
    pc_audio_pull(out, 1u);
    CHECK(dev.state == WAITING);
    push_level(512u, 10000);
    CHECK(ring_head - ring_tail == 3u * 801u + 512u);

    /* it fades in over 64 samples, past the filter's own delay */
    pc_audio_pull(out, 200u);
    CHECK(dev.state == PLAYING);
    CHECK(out[2u * 63u] != 0 || out[2u * 10u] == 0);
    CHECK(abs(out[2u * 150u] - 10000) <= 2);
    CHECK(abs((int)out[2u * 40u]) < 10000);

    /* then it runs dry: it holds its last output and fades to 0 over 64 samples, then silence */
    {
        uint32_t left = ring_head - ring_tail;
        pc_audio_pull(out, left);       /* all that is there, in the state it left */
        pc_audio_pull(out, 200u);
        CHECK(dev.underruns == 1u);
        CHECK(dev.state == WAITING);
        {
            int fade = 1;
            for (i = 1; i < 64u; i++)
                if (out[2u * i] > out[2u * (i - 1u)])
                    fade = 0;
            CHECK(fade);
            CHECK(out[0] > 9000);
            CHECK(abs((int)out[2u * 32u] - 5000) < 400);
            CHECK(out[2u * 64u] == 0 && out[2u * 199u] == 0);
        }
        /* and starts again only at the target */
        push_level(801u, 10000);
        pc_audio_pull(out, 10u);
        CHECK(dev.state == WAITING);
        push_level(801u, 10000);
        push_level(801u, 10000);
        pc_audio_pull(out, 10u);
        CHECK(dev.state == WAITING);
        push_level(512u, 10000);
        pc_audio_pull(out, 200u);
        CHECK(dev.state == PLAYING);
        CHECK(abs(out[2u * 150u] - 10000) <= 2);
    }

    /* What the freeze log hears: the start, each gap with its time and frames, a pause as a pause, and a skip; then the totals. The time is the made-up timer's. */
    {
        uint64_t when;
        char line[300], totals[400];
        fresh();
        test_clock = 1000;
        pc_audio_frame(10u);
        pc_audio_pull(out, 480u);               /* 10 ms of waiting, while the program starts */
        CHECK(!pc_audio_take_line(&when, line, sizeof(line)));
        for (i = 0; i < 4u; i++)
            push_level(801u, 3000);
        pc_audio_frame(14u);
        test_clock = 2000;
        pc_audio_pull(out, 100u);
        CHECK(pc_audio_take_line(&when, line, sizeof(line)) && when == 2000u);
        CHECK(!strcmp(line, "sound started at frame 14, 10 ms after the sound device opened"));
        /* the ring runs dry at frame 20, at time 3000, and the sound is back at frame 23 after 4,800 samples of waiting */
        pc_audio_frame(20u);
        test_clock = 3000;
        pc_audio_pull(out, ring_fill() + 100u);
        CHECK(dev.state == WAITING);
        pc_audio_pull(out, 4800u - 100u);
        CHECK(!pc_audio_take_line(&when, line, sizeof(line)));
        for (i = 0; i < 4u; i++)
            push_level(801u, 3000);
        pc_audio_frame(23u);
        test_clock = 4000;
        pc_audio_pull(out, 10u);
        CHECK(pc_audio_take_line(&when, line, sizeof(line)) && when == 3000u);
        CHECK(!strcmp(line, "sound gap at frame 20: the sound ran dry and stopped for 100 ms, back at frame 23"));
        /* paused: the same, noted as a pause and left out of the gaps */
        pc_audio_paused(1);
        pc_audio_frame(30u);
        test_clock = 5000;
        pc_audio_pull(out, ring_fill() + 100u);
        pc_audio_pull(out, 2300u);
        for (i = 0; i < 4u; i++)
            push_level(801u, 3000);
        pc_audio_paused(0);
        pc_audio_frame(31u);
        pc_audio_pull(out, 10u);
        CHECK(pc_audio_take_line(&when, line, sizeof(line)) && when == 5000u);
        CHECK(!strcmp(line, "sound paused at frame 30 with the game, for 50 ms, back at frame 31"));
        /* far over: a skip, with how much was left out */
        for (i = 0; i < 14u; i++)
            push_level(801u, 3000);
        pc_audio_frame(40u);
        test_clock = 6000;
        pc_audio_pull(out, 10u);
        {
            const char *skip_words = "sound skip at frame 40: the sound device fell behind and ";
            const char *total_words = "sound: 1 gaps, 100 ms in all with the sound stopped, the longest 100 ms at frame 20; 1 pauses; 10 ms of silence before the first sound; 1 skips, 0 times the delay was put back after a stall, 0 times it stayed high; 0 samples dropped, ";
            CHECK(pc_audio_take_line(&when, line, sizeof(line)) && when == 6000u && !strncmp(line, skip_words, strlen(skip_words)));
            printf("%s\n", line);
            CHECK(!pc_audio_take_line(&when, line, sizeof(line)));
            pc_audio_totals(totals, sizeof(totals));
            CHECK(!strncmp(totals, total_words, strlen(total_words)));
            CHECK(strstr(totals, "; 0 notes about the sound never reached this log, their table full; lines over the sound's allowances are counted in the run's last line") != NULL);
            printf("%s\n", totals);
        }
        /* The run ends in a gap: once the device has stopped, the gap gets its line, with how long it had lasted, and the totals count it. */
        pc_audio_frame(50u);
        test_clock = 7000;
        pc_audio_pull(out, ring_fill() + 100u);
        pc_audio_pull(out, 2300u);
        pc_audio_frame(52u);
        pc_audio_close();
        pc_audio_end_of_run();
        CHECK(pc_audio_take_line(&when, line, sizeof(line)) && when == 7000u);
        CHECK(!strcmp(line, "sound gap at frame 50: the sound ran dry and was still stopped when the run ended at frame 52, after 50 ms"));
        printf("%s\n", line);
        pc_audio_totals(totals, sizeof(totals));
        {
            const char *total_words = "sound: 2 gaps, 150 ms in all with the sound stopped, the longest 100 ms at frame 20; 1 pauses;";
            CHECK(!strncmp(totals, total_words, strlen(total_words)));
        }
    }
    /* A run that ends paused says so; a run that ends playing, or before the first sound, has nothing to add. */
    {
        uint64_t when;
        char line[300], totals[400];
        fresh();
        pc_audio_pull(out, 100u);
        pc_audio_end_of_run();
        CHECK(!pc_audio_take_line(&when, line, sizeof(line)));
        for (i = 0; i < 4u; i++)
            push_level(801u, 3000);
        pc_audio_pull(out, 100u);
        CHECK(pc_audio_take_line(&when, line, sizeof(line)));
        pc_audio_end_of_run();
        CHECK(!pc_audio_take_line(&when, line, sizeof(line)));
        pc_audio_paused(1);
        pc_audio_frame(60u);
        pc_audio_pull(out, ring_fill() + 100u);
        pc_audio_pull(out, 480u);
        pc_audio_end_of_run();
        CHECK(pc_audio_take_line(&when, line, sizeof(line)) && !strncmp(line, "sound paused at frame 60 with the game, and still paused when the run ended, after ", 83));
        pc_audio_paused(0);
        /* More lines than the table holds, with none taken: the rest are counted, and the totals say how many. */
        fresh();
        for (i = 0; i < 70u; i++) {
            push_level(801u, 3000);
            push_level(801u, 3000);
            push_level(801u, 3000);
            push_level(801u, 3000);
            pc_audio_pull(out, ring_fill() + 100u);
        }
        pc_audio_totals(totals, sizeof(totals));
        printf("%s\n", totals);
        CHECK(strstr(totals, "; 6 notes about the sound never reached this log, their table full") != NULL);
    }

    /* far over the target it skips back, with no jump in a steady signal and every sample accounted for */
    fresh();
    for (i = 0; i < 16u; i++)
        push_level(801u, 2000);
    pc_audio_pull(out, 200u);
    CHECK(dev.skips == 0u);
    CHECK(dev.state == PLAYING);
    for (i = 0; i < 16u; i++) {
        uint32_t room = RING_PAIRS - (ring_head - ring_tail);
        push_level(room < 801u ? room : 801u, 2000);
    }
    pc_audio_pull(out, 1024u);
    CHECK(dev.skips == 1u);
    CHECK(ring_fill() <= dev.target + 8u);
    CHECK(ring_fill() + 1024u >= dev.target);
    {
        int steady_out = 1;
        for (i = 0; i < 1024u; i++)
            if (abs(out[2u * i] - 2000) > 2)
                steady_out = 0;
        CHECK(steady_out);
    }

    /* THE DELAY. Seven frames over the target at a pull, as a device whose thread was held up leaves it, is no skip; more than eight is, at once, as before. */
    fresh();
    for (i = 0; i < 4u; i++)
        push_level(801u, 2000);
    pc_audio_pull(out, 512u);
    CHECK(dev.state == PLAYING);
    push_level(dev.target + 7u * 801u - ring_fill(), 2000);
    pc_audio_pull(out, 1u);
    CHECK(dev.skips == 0u && ring_fill() + 1u == dev.target + 7u * 801u);
    push_level(801u + 801u + 64u, 2000);
    pc_audio_pull(out, 1u);
    CHECK(dev.skips == 1u && ring_fill() + 1u == dev.target && dev.settles == 0u);
    /* A skip never aims under the crossfade's length: the crossfade reads that many samples from the aim on, so a lower aim would read past the ring's head and leave its tail beyond it.
       Sixty-four is the least; under it nothing moves. */
    {
        uint32_t fill, tail;
        fresh();
        for (i = 0; i < 6u; i++)
            push_level(801u, 2000);
        pc_audio_pull(out, 512u);
        fill = ring_fill();
        tail = ring_tail;
        CHECK(!skip_to(fill, 63u, NOTE_SETTLE) && !skip_to(fill, 0u, NOTE_SETTLE) && ring_tail == tail && ring_fill() == fill && dev.settles == 0u);
        CHECK(skip_to(fill, 64u, NOTE_SETTLE) && ring_tail == ring_head && ring_fill() == 64u && (int32_t)(ring_head - ring_tail) >= 0 && dev.settles == 1u);
        /* the let-go itself, armed by a stall and calm long enough, asked for all but 30 of what the ring holds, as a mean far out of step with the ring could ask: nothing goes */
        fresh();
        for (i = 0; i < 4u; i++)
            push_level(801u, 2000);
        pc_audio_pull(out, 512u);
        pc_audio_pace(2u * 801u);
        pc_audio_pull(out, 512u);
        pc_audio_pace(0u);
        dev.calm_from = dev.pulled - dev.calm_need;
        dev.settle_mean = (double)(dev.target + ring_fill() - 30u);
        fill = ring_fill();
        tail = ring_tail;
        pc_audio_pull(out, 1u);
        CHECK(dev.settles == 0u && dev.underruns == 0u && (int32_t)(ring_head - ring_tail) >= 0 && ring_tail - tail <= 2u);
    }
    /* After a stall, once there has been no stall, gap or skip for 3 seconds, what the ring holds over its target, by its half-second mean, is let go when that is more than half a
       frame, and noted with how much. Each case from a fresh start. */
    {
        uint64_t when;
        char line[300];
        double t, t2;
        uint32_t skips, k;
        start_playing();
        CHECK(dev.state == PLAYING && dev.settles == 0u);
        while (pc_audio_take_line(&when, line, sizeof(line)))
            ;
        pc_audio_frame(50u);
        pc_audio_pace(2u * 801u);
        set_over(600u);
        t = hold(10.0, 0, 0);
        CHECK(t > 2.95 && t < 3.1 && dev.settles == 1u && off_from(dev.target) <= 40);
        printf("let go %.2f s after the stall, %d samples from the target\n", t, off_from(dev.target));
        CHECK(pc_audio_take_line(&when, line, sizeof(line)) && !strncmp(line, "sound delay at frame 50: all calm again, so ", 44) &&
              strstr(line, " ms of sound was let go, what had built up in the queue and what the game was still behind, to bring the delay back to its 61 ms") != NULL);
        printf("%s\n", line);
        /* not half a frame over: nothing, then or later */
        start_playing();
        pc_audio_pace(2u * 801u);
        set_over(300u);
        CHECK(hold(8.0, 0, 0) < 0 && dev.settles == 0u);
        /* with no stall, a ring under a frame over is the rate control's, however long */
        start_playing();
        set_over(700u);
        CHECK(hold(20.0, 0, 0) < 0 && dev.settles == 0u);
        /* more than a frame over with no stall said, as a device whose own thread was held up leaves it: let go 3 seconds after its half-second mean goes over a frame */
        start_playing();
        set_over(1300u);
        t = hold(10.0, 0, 0);
        CHECK(t > 3.0 && t < 3.5 && dev.settles == 1u && off_from(dev.target) <= 40);
        /* While stalls keep coming the extra stays, the cushion that keeps the next stall from running the sound dry; 3 seconds after the last, it goes. */
        start_playing();
        set_over(1300u);
        CHECK(hold(20.0, 1.0, 0) < 0 && dev.settles == 0u);
        t = hold(10.0, 0, 0);
        CHECK(t > 1.9 && t < 3.1 && dev.settles == 1u && off_from(dev.target) <= 40);
        /* A slow frame among frames on time is a stall too: one that ends half a frame or more further behind the game's pace than the frame before it, here each second, keeps the
           extra. The let-go that once came among such frames left the ring with no cushion, and the next ran it dry. One a little under half a frame further behind is not one. */
        start_playing();
        pc_audio_pace(2u * 801u);
        set_over(600u);
        stall_size = 401u;
        CHECK(hold(20.0, 1.0, 0) < 0 && dev.settles == 0u);
        stall_size = 0;
        t = hold(10.0, 0, 0);
        CHECK(t > 1.9 && t < 3.1 && dev.settles == 1u);
        start_playing();
        pc_audio_pace(2u * 801u);
        set_over(600u);
        stall_size = 399u;
        t = hold(20.0, 1.0, 0);
        stall_size = 0;
        CHECK(t > 2.95 && t < 3.1 && dev.settles == 1u);
        /* Lateness that grows a little each frame, as on a monitor a little slower than the game, is no stall however far it goes: let go 3 seconds after the stall before it. */
        start_playing();
        pc_audio_pace(2u * 801u);
        pc_audio_pace(0u);
        set_over(600u);
        {
            uint32_t k2, late = 0;
            static int16_t o2[2u * 512u];
            uint32_t level = ring_fill() + 512u, fill2;
            for (k2 = 0, t = -1; k2 < 600u && t < 0; k2++) {
                late = late + 300u < 1600u ? late + 300u : 0u;    /* up a little under half a frame each time, past two frames never */
                pc_audio_pace(late);
                fill2 = ring_fill();
                push_level(level > fill2 ? level - fill2 : 0u, 2000);
                pc_audio_pull(o2, 512u);
                if (dev.settles)
                    t = k2 * 512.0 / 48000.0;
            }
            CHECK(t > 2.95 && t < 3.1);
        }
        /* What the game is still behind is sound to come: let go with the rest, up to a frame. */
        start_playing();
        pc_audio_pace(2u * 801u);
        set_over(600u);
        CHECK(hold(10.0, 0, 400u) > 0 && dev.settles == 1u && off_from(dev.target - 400u) <= 40);
        start_playing();
        pc_audio_pace(2u * 801u);
        set_over(600u);
        CHECK(hold(10.0, 0, 1500u) > 0 && dev.settles == 1u && off_from(dev.target - 801u) <= 40);
        /* A gap counts as a stall: what came while the ring was dry and after goes 3 seconds after it. */
        start_playing();
        pc_audio_pull(out, ring_fill() + 100u);
        CHECK(dev.state == WAITING);
        push_level(dev.target, 2000);
        pc_audio_pull(out, 10u);
        CHECK(dev.state == PLAYING);
        set_over(600u);
        t = hold(10.0, 0, 0);
        CHECK(t > 2.85 && t < 3.1 && dev.settles == 1u);
        /* a gap or a skip in the 3 seconds starts them again */
        start_playing();
        pc_audio_pace(2u * 801u);
        set_over(600u);
        CHECK(hold(1.5, 0, 0) < 0);
        pc_audio_pull(out, ring_fill() + 100u);
        push_level(dev.target, 2000);
        pc_audio_pull(out, 10u);
        set_over(600u);
        t = hold(10.0, 0, 0);
        CHECK(t > 2.85 && t < 3.1 && dev.settles == 1u);
        start_playing();
        pc_audio_pace(2u * 801u);
        set_over(600u);
        CHECK(hold(2.0, 0, 0) < 0);
        skips = (uint32_t)dev.skips;
        push_level(dev.target + 8u * 801u + 600u - ring_fill(), 2000);
        pc_audio_pull(out, 10u);
        CHECK(dev.skips == skips + 1u);
        set_over(600u);
        t = hold(10.0, 0, 0);
        CHECK(t > 2.85 && t < 3.1 && dev.settles == 1u);
        /* A stall within a minute of a let-go doubles the next wait: 6, 12, 24, then 48 seconds, no more. A let-go that holds for a minute puts it back to 3. */
        start_playing();
        pc_audio_pace(2u * 801u);
        set_over(600u);
        CHECK(hold(10.0, 0, 0) > 0 && dev.settles == 1u);
        for (k = 0; k < 5u; k++) {
            double want = k < 4u ? 6.0 * (double)(1u << k) : 48.0;
            pc_audio_pace(2u * 801u);
            set_over(600u);
            t2 = hold(100.0, 0, 0);
            CHECK(t2 > want - 0.1 && t2 < want + 0.2 && dev.settles == 2u + k);
            printf("a stall just after a let-go: the next let-go %.2f s after it\n", t2);
        }
        CHECK(hold(61.0, 0, 0) < 0);
        pc_audio_pace(2u * 801u);
        set_over(600u);
        t = hold(10.0, 0, 0);
        CHECK(t > 2.95 && t < 3.1 && dev.settles == 7u);
    }
    /* A delay more than a frame over its target for 10 seconds, as in a run of stalls, gets a line, and so does its end; the totals count it. The machine says a stall each second, so
       the extra is kept, and the two sides here run at one rate, so only the rate control brings it back, at its slow pace. */
    {
        uint64_t when;
        char line[300], totals[500];
        double t_frame = 0, t_device = 0, frame_s = 3420.0 * 262.0 / 53693175.0, device_s = 512.0 / 48000.0, carry = 0;
        int high = 0, high_end = 0;
        fresh();
        for (i = 0; i < 4u; i++)
            push_level(801u, 2000);
        pc_audio_pull(out, 512u);
        push_level(dev.target + 700u - ring_fill(), 2000);    /* with the frames to come, about a frame and a third over */
        while (t_device < 200.0 && !high_end) {
            if (t_frame <= t_device) {
                if (fmod(t_frame, 1.0) < frame_s)
                    pc_audio_pace(2u * 801u);
                double want = 48000.0 * frame_s + carry;
                uint32_t n = (uint32_t)want;
                carry = want - n;
                push_level(n, 2000);
                t_frame += frame_s;
            } else {
                pc_audio_pull(out, 512u);
                t_device += device_s;
            }
            while (pc_audio_take_line(&when, line, sizeof(line))) {
                if (strstr(line, ": for 10 seconds the queue has held more than a frame over its 61 ms, ")) {
                    high++;
                    CHECK(t_device > 9.9 && t_device < 11.0);
                    printf("%s, after %.1f s\n", line, t_device);
                }
                if (strstr(line, ": the queue is back within half a frame of its 61 ms, after ") && strstr(line, " s more than a frame over")) {
                    high_end++;
                    printf("%s, after %.1f s\n", line, t_device);
                }
            }
        }
        CHECK(high == 1 && high_end == 1 && dev.skips == 0u);
        pc_audio_totals(totals, sizeof(totals));
        CHECK(strstr(totals, "; 0 skips, 0 times the delay was put back after a stall, 1 times it stayed high; ") != NULL);
        printf("%s\n", totals);
    }

    /* mute and volume are the device's side's alone */
    fresh();
    for (i = 0; i < 8u; i++)
        push_level(801u, 8000);
    pc_audio_pull(out, 300u);
    CHECK(abs(out[2u * 299u] - 8000) <= 2);
    volume_percent = 50;
    pc_audio_pull(out, 10u);
    CHECK(abs(out[0] - 4000) <= 2);
    volume_percent = 100;
    pc_audio_toggle_mute();
    pc_audio_pull(out, 10u);
    CHECK(out[0] == 0);
    pc_audio_toggle_mute();
    pc_audio_pull(out, 10u);
    CHECK(abs(out[0] - 8000) <= 2);

    /* an hour at each end of a device's clock: the fill stays in its band, nothing is dropped and nothing runs dry once settled */
    {
        static const double drifts[] = {300.0, -300.0, 0.0};
        unsigned d;
        for (d = 0; d < 3u; d++) {
            uint64_t under0;
            fresh();
            steady(3600.0, drifts[d], &low, &high);
            under0 = dev.underruns;
            printf("drift %+.0f: the fill stays %.0f to %.0f around a target of %u; rate %.0f to %.0f parts in a million, %llu underruns, %llu skips, %llu dropped\n", drifts[d], low, high, (unsigned)dev.target,
                   dev.rate_low * 1e6, dev.rate_high * 1e6, (unsigned long long)under0, (unsigned long long)dev.skips, (unsigned long long)dropped);
            CHECK(dropped == 0u);
            CHECK(dev.skips == 0u);
            CHECK(under0 == 0u);
            CHECK(low > (double)DEVICE_PAIRS);
            CHECK(high < (double)dev.over);
            CHECK(fabs(dev.rate_high) < RATE_MAX && fabs(dev.rate_low) < RATE_MAX);
        }
    }

    /* the resampler is flat to 18 kHz within 0.1 dB, at the rate's two ends and at no drift */
    {
        static const double freqs[] = {100.0, 1000.0, 5000.0, 10000.0, 15000.0, 18000.0};
        static const double rates[] = {500e-6, -500e-6};
        unsigned a, b;
        for (b = 0; b < 2u; b++)
            for (a = 0; a < sizeof(freqs) / sizeof(freqs[0]); a++) {
                double db = 20.0 * log10(level_at(freqs[a], rates[b]));
                printf("level at %.0f Hz, rate %+.0f parts in a million: %+.3f dB\n", freqs[a], rates[b] * 1e6, db);
                CHECK(fabs(db) < 0.1);
            }
    }

    printf("%d checks, %d failed\n", checks, fails);
    printf(fails ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    return fails ? 1 : 0;
}
