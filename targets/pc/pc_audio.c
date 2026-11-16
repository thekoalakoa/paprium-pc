/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See pc_audio.h. Three things happen on the device's side, and only there:
 *
 *   FILL     the pull waits until the ring holds its target, three frames plus one device buffer. It then keeps the ring near that. With SDL's buffer of 512 samples that is 2,915 samples, 60.7 ms,
 *            one frame (16.7 ms) more than the two frames this program had before. That frame covers a frame that runs long, such as the one that unpacks the sound bank near the start, and
 *            a few slow presents in a row, after which the game catches up. In test runs, with two frames the bank's unpack made a short gap in 27 runs of 29, and with three in 3 of 29.
 *            The run loop does its part (pc_pace.h): a frame that is a whole frame behind the game's pace is not shown, so slow presents do not slow the game or its sound; and after a longer
 *            stall it makes up the last three frames of the stall at once, unshown but the last, so the ring is full again within those frames' own work. A stall of the game itself
 *            still stops the sound for about as long, less what the ring held.
 *   DELAY    the ring's target is the sound's delay. After a stall the game catches up faster than its own rate, its make-up's frames at once, and a device whose own thread was
 *            held up comes back to a ring the game kept filling; either leaves frames in the ring over its target, one to two and a half after one stall, which the rate control
 *            below drains only over a minute or more. So once all is calm, no stall of the game, no gap and no skip, for 3 seconds, what the ring holds over its target, by its
 *            half-second mean, is let go at once, with a short crossfade and a line in the freeze log, when it is more than half a frame; and what the game is still behind, up to
 *            a frame, with it, as that much more comes while the game catches up. Not sooner: in a run of stalls that extra is the cushion that keeps the next stall from running
 *            the sound dry. A stall here (pc_audio_pace) is a frame that ends two frames or more behind the game's pace, or one that ends half a frame or more further behind
 *            it than the frame before it: a slow frame among frames on time, which empties the ring that much the moment it comes, so a let-go among such frames ran the sound dry at the next, again
 *            and again, in a model of the run loop. So when a stall comes within a minute of a let-go, the next wait is twice as long, up to 48 seconds, and a let-go that holds
 *            for a minute puts it back to 3. More than eight frames over is let go at once, as before. A delay more than a frame over its target for 10 seconds, as in a run of
 *            stalls, gets a line, and so does its end. In a model of the run loop on these files, after one stall of 20 ms to a second and a half, of the present, the work, the
 *            window's events, the device's thread or the whole PC, the delay's two-second mean is back within a frame of its target at most 4.5 seconds after it on a 144 or
 *            240 Hz monitor and 7.5 seconds on a 60 Hz one, where before it stayed over for up to 2 minutes. The price, in the same model: a let-go takes the cushion, so a
 *            stall or slow frames soon after it can run the sound dry where they did not before. That happens in the first minute on some monitors of 120 Hz or less, where
 *            a let-go can come a few seconds after the start with no stall at all; at a second slow stretch soon after a first; where stalls come a few seconds apart, as on a
 *            busy PC; and where a stall is followed by short runs of slow frames whose lateness climbs by less than half a frame a frame and stays under two frames, so that
 *            the rule above calls none of them a stall. On a 60 Hz monitor, where the game catches up only slowly and the fill swings by about a frame, a let-go with no stall
 *            can also come later on. The same model found two more cases on faster monitors. With a device buffer of 480, 1,024 or 2,048 samples in place of SDL's 512, the
 *            start's let-go comes on 144 and 240 Hz monitors as well. And a device whose clock runs about 450 parts in a million or more slower than the PC's timer, 480 on a
 *            144 Hz monitor, gets let-gos of about a frame with no stall: a few in ten minutes near that mark, one every 25 seconds or so at 1,000. Before, such a device made
 *            no jump at all down to 500 parts in a million, and past that a skip of about 133 ms every 5 to 10 minutes.
 *   GAPS     when the ring runs dry the pull plays what it has, holds the last output and fades it to 0 over 64 samples, then gives silence until the ring is back at its target, and fades
 *            in over 64 samples. Each gap and each skip is noted with the PC's timer and the game's frame, for the freeze log.
 *   DRIFT    the game's pace runs off the PC's timer and the device has a clock of its own, so the pull reads the ring at 1 + r samples to a sample, with r kept within 500 parts in a million
 *            by a slow control on the mean fill, through a 16-tap windowed sinc. This is the only place samples are changed. The file output is taken before the ring and never comes here.
 *            By the pace's rule above the game keeps its own rate on any monitor, but on a monitor a little slower than the game its frames come unevenly, a little late for some seconds
 *            and then one left out, and r follows that as well as the two clocks. In a model of the run loop on these files, 30 minutes each: r from -460 to +137 parts in a million
 *            on a 59.865 Hz monitor, and at its limit of -500 for stretches with the device's clock 200 parts in a million fast as well; the sound never ran dry in either.
 *
 * Built with PC_AUDIO_NO_SDL for tests/pc_audio_test.c, which drives the two sides with made-up clocks.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pc_audio.h"

#ifndef PC_AUDIO_NO_SDL
#include <SDL.h>
#include <windows.h>
#endif

#define RING_PAIRS      16384u
#define RING_MASK       (RING_PAIRS - 1u)
#define FRAME_PAIRS     801u
#define DEVICE_PAIRS    512u
#define TARGET_FRAMES   3u
#define FADE_PAIRS      64u
#define OVER_FRAMES     8u
#define SETTLE_OVER     (FRAME_PAIRS / 2u)          /* the half-second mean more than this over the target, with all calm, is let go */
#define SETTLE_CALM     (3u * 48000u)               /* all calm this long, 3 seconds at first: no stall of the game, no gap, no skip */
#define SETTLE_CALM_MOST (48u * 48000u)             /* the wait doubles up to this, 48 seconds, while stalls come soon after a let-go */
#define SETTLE_SOON     (60u * 48000u)              /* soon: within a minute; a let-go that holds this long puts the wait back to SETTLE_CALM */
#define SETTLE_MEAN     24000.0                     /* that mean runs over about half a second, 30 frames, long enough to hide the swing of each push and pull */
#define HIGH_OVER       FRAME_PAIRS                 /* the delay counts as high while the fill's mean is more than this over the target */
#define HIGH_PAIRS      (10u * 48000u)              /* and when it has been high for this long, 10 seconds, the freeze log hears of it */
#define RATE_MAX        500e-6
#define RATE_GAIN       (RATE_MAX / FRAME_PAIRS)    /* one frame away from the target asks for the whole correction */
#define MEAN_PAIRS      96000.0                     /* the fill's mean runs over about 2 seconds */
#define TAPS            16
#define PHASES          256

static int16_t ring[RING_PAIRS][2];
static volatile uint32_t ring_head, ring_tail;      /* head moves only on the machine's side, tail only on the device's */
static volatile int audio_open_now;
static volatile int muted, volume_percent = 100;
static uint64_t pushed, dropped;                    /* the machine's side */
static char device_words[220];

/* ---- what the freeze log hears: the sound's start, each gap and each skip, with the PC's timer and the game's frame. One writer, the device's thread, and one reader, the log's thread; when the
   table is full a note is only counted. */
#define NOTES 64u
enum { NOTE_START, NOTE_GAP, NOTE_PAUSE, NOTE_SKIP, NOTE_GAP_AT_END, NOTE_PAUSE_AT_END, NOTE_SETTLE, NOTE_HIGH, NOTE_HIGH_END };
typedef struct SoundNote { int kind; uint64_t when; uint32_t frame, frame_back; uint64_t pairs; } SoundNote;
static SoundNote notes[NOTES];
static volatile uint32_t notes_head, notes_tail, notes_lost;
static volatile uint32_t frame_now;                 /* the frame the game is making, from the machine's side */
static volatile int paused_now;                     /* the player has paused the game: a ring that runs dry then is no fault */
static volatile int stall_said;                     /* the machine's side: the game has been in a stall since the device last looked (pc_audio_pace) */
static volatile uint32_t behind_now;                /* and how far behind its pace it is now, in samples: sound it still owes, which comes as it catches up */

#ifdef PC_AUDIO_NO_SDL
static uint64_t test_clock;                         /* the tests' made-up timer */
static uint64_t clock_now(void) { return test_clock; }
#else
static uint64_t clock_now(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)t.QuadPart;
}
#endif

static void note_put(int kind, uint64_t when, uint32_t frame, uint64_t pairs)
{
    uint32_t h = notes_head;
    if (h - __atomic_load_n(&notes_tail, __ATOMIC_ACQUIRE) >= NOTES) {
        notes_lost++;
        return;
    }
    notes[h % NOTES].kind = kind;
    notes[h % NOTES].when = when;
    notes[h % NOTES].frame = frame;
    notes[h % NOTES].frame_back = frame_now;
    notes[h % NOTES].pairs = pairs;
    __atomic_store_n(&notes_head, h + 1u, __ATOMIC_RELEASE);
}

void pc_audio_frame(uint32_t frame) { frame_now = frame; }
/* A stall: the frame ended two frames or more behind the game's pace, or half a frame or more further behind it than the frame before it did. A monitor a little slower than the
   game moves the lateness only a little each frame and never makes one. A 60 Hz monitor with vsync on does, now and then, when a frame's work takes 9 ms or more: the game catches
   up on it only slowly, and each time it has, the lateness jumps from nothing to about one frame's work at once, over half a frame. In a model of the run loop that came about once
   every 13 seconds, or once a minute on a 59.94 Hz monitor; it cost no gap, and the delay was still back within a frame of its target at most 7 seconds after a real stall. On a
   75 Hz monitor with 12 ms of work a frame it came more than once a second, so there the delay was not let go after a stall at all, and drained slowly as in the builds before the
   let-go came in. */
void pc_audio_pace(uint32_t behind_pairs)
{
    if (behind_pairs >= 2u * FRAME_PAIRS || behind_pairs >= behind_now + FRAME_PAIRS / 2u)
        stall_said = 1;
    behind_now = behind_pairs;
}
void pc_audio_paused(int paused) { paused_now = paused; }

/* ---- the machine's side */
void pc_audio_push(const int16_t *pairs, uint32_t n)
{
    uint32_t h, t, room, i;
    if (!audio_open_now)
        return;
    h = __atomic_load_n(&ring_head, __ATOMIC_RELAXED);
    t = __atomic_load_n(&ring_tail, __ATOMIC_ACQUIRE);
    room = RING_PAIRS - (h - t);
    pushed += n;
    if (n > room) {
        dropped += n - room;
        n = room;
    }
    for (i = 0; i < n; i++) {
        ring[(h + i) & RING_MASK][0] = pairs[2u * i];
        ring[(h + i) & RING_MASK][1] = pairs[2u * i + 1u];
    }
    __atomic_store_n(&ring_head, h + n, __ATOMIC_RELEASE);
}

/* ---- the device's side */
static struct {
    float table[PHASES + 1][TAPS];
    int table_made;
    uint32_t target, over;
    float history[TAPS][2];         /* the newest input is at the end */
    double phase;                   /* how far past history's middle the output is, 0 to 1 */
    double rate;
    double mean_fill;
    double settle_mean;             /* the fill's mean over about half a second: what the ring holds over its target once all is calm */
    int mean_primed;
    enum { WAITING, PLAYING, FADING } state;
    uint32_t fade;                  /* samples of fade done */
    float hold[2];
    int16_t xfade[FADE_PAIRS][2];   /* samples that come before the ring: a skip's crossfade */
    uint32_t xfade_at, xfade_n;
    uint64_t underruns, skips, gaps_pairs, silent_pairs;
    uint64_t pulled;
    double rate_low, rate_high;
    int started;                    /* the first sound has been played */
    uint64_t start_pairs;           /* the silence before it, while the program started */
    uint64_t gap_when;              /* the gap going on now: when the ring ran dry, the frame then, how long so far, and whether the game was paused then */
    uint32_t gap_frame;
    uint64_t gap_pairs;
    int gap_paused;
    uint64_t gaps_all_pairs, gap_longest;   /* all gaps that are over, and the longest, pauses left out */
    uint32_t gap_longest_frame;
    uint64_t pauses;                /* the times the ring ran dry while the game was paused */
    uint64_t settles;               /* the times the ring's extra delay was let go after a stall, once all was calm */
    uint64_t calm_from;             /* dev.pulled at the last stall of the game, gap or skip */
    uint64_t calm_need;             /* how long all must be calm before the next let-go */
    uint64_t settled_at;            /* dev.pulled at the last let-go, and whether a stall since has been looked at against it */
    int settle_watch;
    int settle_armed;               /* a stall, a gap or a mean more than a frame over has happened since the queue was last at its target */
    uint64_t high_pairs;            /* how long the fill's mean has been high, in samples pulled, and whether the freeze log has heard of it */
    int high_said;
    uint64_t highs;                 /* the times the delay stayed high that long */
} dev;

/* A Kaiser-windowed sinc, phase by phase. tests/pc_audio_test.c holds the response against its limit. */
static double bessel0(double x)
{
    double sum = 1, term = 1;
    int k;
    for (k = 1; k < 30; k++) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
    }
    return sum;
}

static void table_make(void)
{
    const double pi = 3.14159265358979323846, beta = 3.5;
    int p, t;
    for (p = 0; p <= PHASES; p++) {
        double sum = 0, frac = (double)p / PHASES, h[TAPS];
        for (t = 0; t < TAPS; t++) {
            double d = (t - (TAPS / 2 - 1)) - frac;     /* a tap's distance from the output point */
            double w = d / (TAPS / 2);
            double win = fabs(w) < 1 ? bessel0(beta * sqrt(1 - w * w)) / bessel0(beta) : 0;
            double sinc = d == 0 ? 1 : sin(pi * d) / (pi * d);
            h[t] = sinc * win;
            sum += h[t];
        }
        for (t = 0; t < TAPS; t++)
            dev.table[p][t] = (float)(h[t] / sum);
    }
    dev.table_made = 1;
}

static void device_side_reset(uint32_t device_pairs)
{
    if (!dev.table_made)
        table_make();
    memset(dev.history, 0, sizeof(dev.history));
    dev.phase = 0;
    dev.rate = 0;
    dev.mean_fill = 0;
    dev.mean_primed = 0;
    dev.state = WAITING;
    dev.fade = 0;
    dev.xfade_n = dev.xfade_at = 0;
    dev.underruns = dev.skips = dev.gaps_pairs = dev.silent_pairs = dev.pulled = 0;
    dev.rate_low = dev.rate_high = 0;
    dev.started = dev.gap_paused = 0;
    dev.start_pairs = dev.gap_pairs = dev.gaps_all_pairs = dev.gap_longest = dev.pauses = 0;
    dev.settles = dev.high_pairs = dev.highs = 0;
    dev.calm_from = dev.settled_at = 0;
    dev.calm_need = SETTLE_CALM;
    dev.settle_watch = 0;
    dev.settle_armed = 0;
    dev.high_said = 0;
    stall_said = 0;
    behind_now = 0;
    dev.gap_when = 0;
    dev.gap_frame = dev.gap_longest_frame = 0;
    notes_head = notes_tail = notes_lost = 0;
    dev.target = TARGET_FRAMES * FRAME_PAIRS + device_pairs;
    dev.over = dev.target + OVER_FRAMES * FRAME_PAIRS;
}

/* one input sample into the history. Returns 0 when none is to be had. */
static int input_next(void)
{
    float l, r;
    if (dev.xfade_at < dev.xfade_n) {
        l = dev.xfade[dev.xfade_at][0];
        r = dev.xfade[dev.xfade_at][1];
        dev.xfade_at++;
    } else {
        uint32_t t = __atomic_load_n(&ring_tail, __ATOMIC_RELAXED);
        uint32_t h = __atomic_load_n(&ring_head, __ATOMIC_ACQUIRE);
        if (h == t)
            return 0;
        l = ring[t & RING_MASK][0];
        r = ring[t & RING_MASK][1];
        __atomic_store_n(&ring_tail, t + 1u, __ATOMIC_RELEASE);
    }
    memmove(&dev.history[0], &dev.history[1], sizeof(dev.history[0]) * (TAPS - 1));
    dev.history[TAPS - 1][0] = l;
    dev.history[TAPS - 1][1] = r;
    return 1;
}

static uint32_t ring_fill(void)
{
    uint32_t h = __atomic_load_n(&ring_head, __ATOMIC_ACQUIRE);
    uint32_t t = __atomic_load_n(&ring_tail, __ATOMIC_RELAXED);
    return (h - t) + (dev.xfade_n - dev.xfade_at);
}

/* A stall of the game, a gap or a skip: the calm starts again. The first after a let-go, when within SETTLE_SOON of it, shows the let-go came too early for these stalls: the next
   wait is twice as long. */
static void calm_broken(void)
{
    dev.calm_from = dev.pulled;
    if (dev.settle_watch) {
        dev.settle_watch = 0;
        if (dev.pulled - dev.settled_at < SETTLE_SOON && dev.calm_need < SETTLE_CALM_MOST)
            dev.calm_need *= 2u;
    }
}

/* the ring is over: back to aim, the old stream and the new one crossfaded over a short stretch. note: NOTE_SKIP when the device fell far behind, NOTE_SETTLE when the queue's
   extra delay is let go after a calm spell. Returns 1 when it skipped. The crossfade reads FADE_PAIRS samples from the tail and as many from the aim on, and the tail ends past them: so the
   skip must be FADE_PAIRS or more, or it is not worth it, and the aim too, or the crossfade would read past the ring's head and leave the tail beyond it. A skip back to the target always
   has room; a let-go's aim, the fill less what is let go, is checked here. */
static int skip_to(uint32_t fill, uint32_t aim, int note)
{
    uint32_t t = __atomic_load_n(&ring_tail, __ATOMIC_RELAXED);
    uint32_t skip = fill - aim, i;
    if (aim < FADE_PAIRS || fill < aim + FADE_PAIRS || dev.xfade_at < dev.xfade_n)
        return 0;
    for (i = 0; i < FADE_PAIRS; i++) {
        int c;
        for (c = 0; c < 2; c++) {
            int32_t a = ring[(t + i) & RING_MASK][c], b = ring[(t + skip + i) & RING_MASK][c];
            dev.xfade[i][c] = (int16_t)((a * (int32_t)(FADE_PAIRS - i) + b * (int32_t)i) / (int32_t)FADE_PAIRS);
        }
    }
    dev.xfade_at = 0;
    dev.xfade_n = FADE_PAIRS;
    __atomic_store_n(&ring_tail, t + skip + FADE_PAIRS, __ATOMIC_RELEASE);
    if (note == NOTE_SKIP) {
        calm_broken();
        dev.skips++;
    } else {
        dev.calm_from = dev.settled_at = dev.pulled;
        dev.settle_watch = 1;
        dev.settles++;
    }
    dev.mean_fill = dev.settle_mean = dev.target;
    note_put(note, clock_now(), frame_now, skip);
    return 1;
}

/* the slow control, at each pull: it reads the ring faster when it is over the target, slower when under. */
static void rate_control(uint32_t fill, uint32_t n)
{
    double alpha = (double)n / MEAN_PAIRS;
    if (!dev.mean_primed) {
        dev.mean_fill = dev.settle_mean = fill;
        dev.mean_primed = 1;
    }
    dev.mean_fill += alpha * ((double)fill - dev.mean_fill);
    dev.settle_mean += (double)n / SETTLE_MEAN * ((double)fill - dev.settle_mean);
    /* A ring more than a frame over with no stall of the game said, as a device whose own thread was held up leaves it: let go once all is calm for calm_need from now. */
    if (!dev.settle_armed && dev.settle_mean > (double)(dev.target + FRAME_PAIRS)) {
        dev.settle_armed = 1;
        dev.calm_from = dev.pulled;
    }
    /* A delay that stays more than a frame over its target for 10 seconds is said once, and its end too. Nothing here changes the sound. */
    if (dev.mean_fill > (double)(dev.target + HIGH_OVER)) {
        dev.high_pairs += n;
        if (dev.high_pairs >= HIGH_PAIRS && !dev.high_said) {
            dev.high_said = 1;
            dev.highs++;
            note_put(NOTE_HIGH, clock_now(), frame_now, (uint64_t)(dev.mean_fill - dev.target));
        }
    } else if (dev.mean_fill < (double)(dev.target + HIGH_OVER / 2u)) {
        if (dev.high_said)
            note_put(NOTE_HIGH_END, clock_now(), frame_now, dev.high_pairs);
        dev.high_pairs = 0;
        dev.high_said = 0;
    }
    dev.rate = (dev.mean_fill - dev.target) * RATE_GAIN;
    if (dev.rate > RATE_MAX)
        dev.rate = RATE_MAX;
    if (dev.rate < -RATE_MAX)
        dev.rate = -RATE_MAX;
    if (dev.rate < dev.rate_low)
        dev.rate_low = dev.rate;
    if (dev.rate > dev.rate_high)
        dev.rate_high = dev.rate;
}

static inline void resampled(float out[2])
{
    double where = dev.phase * PHASES;
    int p = (int)where, t;
    float f = (float)(where - p);
    const float *h0 = dev.table[p], *h1 = dev.table[p + 1];
    float l = 0, r = 0;
    for (t = 0; t < TAPS; t++) {
        float c = h0[t] + f * (h1[t] - h0[t]);
        l += c * dev.history[t][0];
        r += c * dev.history[t][1];
    }
    out[0] = l;
    out[1] = r;
}

static inline int16_t to_s16(float v)
{
    int32_t i = (int32_t)lrintf(v);
    return (int16_t)(i > 32767 ? 32767 : i < -32768 ? -32768 : i);
}

void pc_audio_pull(int16_t *pairs, uint32_t n)
{
    uint32_t i, fill = ring_fill();
    float gain = muted ? 0.0f : (float)volume_percent / 100.0f;
    if (dev.state == PLAYING) {
        /* After a stall the game makes up its last frames at once, and those that come once the sound has restarted would sit in the ring as extra delay, a frame or two for a minute
           or more (DELAY above). The stall the machine says; a gap, a skip and the device's own holdups this side sees. */
        if (stall_said) {
            stall_said = 0;
            calm_broken();
            dev.settle_armed = 1;
        }
        if (fill > dev.over)
            skip_to(fill, dev.target, NOTE_SKIP);
        rate_control(ring_fill(), n);
        /* After a stall, once all is calm for calm_need, what the ring holds over its target, by its half-second mean through the swing of each push and pull, is let go, and with it
           what the game still owes while it catches up, up to a frame, as on a 60 Hz monitor that lets it catch up only slowly: that much more is still to come. */
        if (dev.settle_watch && dev.pulled - dev.settled_at >= SETTLE_SOON) {
            dev.settle_watch = 0;
            dev.calm_need = SETTLE_CALM;
        }
        if (dev.settle_armed && dev.pulled - dev.calm_from >= dev.calm_need) {
            uint32_t owed = behind_now < FRAME_PAIRS ? behind_now : FRAME_PAIRS;
            double excess = dev.settle_mean - (double)dev.target + (double)owed;
            dev.settle_armed = 0;
            fill = ring_fill();
            if (excess > (double)SETTLE_OVER && fill > (uint32_t)excess)
                skip_to(fill, fill - (uint32_t)excess, NOTE_SETTLE);
        }
    } else {
        stall_said = 0;
        calm_broken();
        dev.settle_armed = 1;
    }
    for (i = 0; i < n; i++) {
        float o[2] = {0, 0};
        float level = 1.0f;
        switch (dev.state) {
        case WAITING:
            if (ring_fill() >= dev.target) {
                memset(dev.history, 0, sizeof(dev.history));
                dev.phase = 0;
                dev.fade = 0;
                dev.mean_primed = 0;
                dev.state = PLAYING;
                if (!dev.started) {
                    dev.started = 1;
                    note_put(NOTE_START, clock_now(), frame_now, dev.start_pairs);
                } else if (dev.gap_paused) {
                    note_put(NOTE_PAUSE, dev.gap_when, dev.gap_frame, dev.gap_pairs);
                } else {
                    note_put(NOTE_GAP, dev.gap_when, dev.gap_frame, dev.gap_pairs);
                    dev.gaps_all_pairs += dev.gap_pairs;
                    if (dev.gap_pairs > dev.gap_longest) {
                        dev.gap_longest = dev.gap_pairs;
                        dev.gap_longest_frame = dev.gap_frame;
                    }
                }
            } else {
                dev.silent_pairs++;
                if (dev.started)
                    dev.gap_pairs++;
                else
                    dev.start_pairs++;
                break;
            }
            /* fall through */
        case PLAYING: {
            int got = 1;
            double next = dev.phase + 1.0 + dev.rate;
            while (next >= 1.0) {
                if (!input_next()) {
                    got = 0;
                    break;
                }
                next -= 1.0;
            }
            if (!got) {
                dev.state = FADING;
                dev.fade = 0;
                dev.underruns++;
                dev.gaps_pairs++;
                dev.gap_when = clock_now();
                dev.gap_frame = frame_now;
                dev.gap_pairs = 1;
                dev.gap_paused = paused_now;
                if (dev.gap_paused)
                    dev.pauses++;
                o[0] = dev.hold[0];
                o[1] = dev.hold[1];
                level = 1.0f - 1.0f / FADE_PAIRS;
                dev.fade = 1;
                break;
            }
            dev.phase = next;
            resampled(o);
            dev.hold[0] = o[0];
            dev.hold[1] = o[1];
            if (dev.fade < FADE_PAIRS) {
                dev.fade++;
                level = (float)dev.fade / FADE_PAIRS;
            }
            break;
        }
        case FADING:
            dev.gaps_pairs++;
            dev.gap_pairs++;
            dev.fade++;
            if (dev.fade >= FADE_PAIRS) {
                dev.state = WAITING;
                break;
            }
            o[0] = dev.hold[0];
            o[1] = dev.hold[1];
            level = 1.0f - (float)dev.fade / FADE_PAIRS;
            break;
        }
        pairs[2u * i] = to_s16(o[0] * level * gain);
        pairs[2u * i + 1u] = to_s16(o[1] * level * gain);
    }
    dev.pulled += n;
}

void pc_audio_toggle_mute(void) { muted = !muted; }
int pc_audio_muted(void) { return muted; }
void pc_audio_set_volume(int volume) { volume_percent = volume < 0 ? 0 : volume > 100 ? 100 : volume; }
int pc_audio_volume(void) { return volume_percent; }

/* What the pull costs the device's thread. These times are the PC's own and stay on this side of the ring: nothing of them reaches the machine. */
#ifndef PC_AUDIO_NO_SDL
static uint64_t pull_ticks, pull_worst_ticks, pull_calls;
static uint64_t ticks_a_second;

static void timed_pull(int16_t *pairs, uint32_t n)
{
    LARGE_INTEGER a, b;
    uint64_t d;
    QueryPerformanceCounter(&a);
    pc_audio_pull(pairs, n);
    QueryPerformanceCounter(&b);
    d = (uint64_t)(b.QuadPart - a.QuadPart);
    pull_ticks += d;
    pull_calls++;
    if (d > pull_worst_ticks)
        pull_worst_ticks = d;
}

static void cost_report(void)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    ticks_a_second = (uint64_t)f.QuadPart;
    if (pull_calls && dev.pulled)
        printf("audio: the pull took %.1f microseconds for each frame's %u samples, worst single pull %.1f microseconds, %llu pulls\n", 1e6 * (double)pull_ticks / (double)ticks_a_second * FRAME_PAIRS / (double)dev.pulled,
               FRAME_PAIRS, 1e6 * (double)pull_worst_ticks / (double)ticks_a_second, (unsigned long long)pull_calls);
}
#else
static void cost_report(void) {}
#endif

void pc_audio_report(void)
{
    if (!audio_open_now)
        return;
    cost_report();
    printf("audio: pushed %llu, dropped %llu, pulled %llu, underruns %llu (%llu samples of fade, %llu of silence), skips %llu, rate from %.0f to %.0f parts in a million, volume %d, %s\n", (unsigned long long)pushed,
           (unsigned long long)dropped, (unsigned long long)dev.pulled, (unsigned long long)dev.underruns, (unsigned long long)dev.gaps_pairs, (unsigned long long)dev.silent_pairs, (unsigned long long)dev.skips, dev.rate_low * 1e6,
           dev.rate_high * 1e6, volume_percent, muted ? "muted" : "not muted");
}

void pc_audio_describe(char *text, size_t size) { snprintf(text, size, "%s", device_words); }

static unsigned long long ms_of_pairs(uint64_t pairs) { return (unsigned long long)((pairs * 1000u + PC_AUDIO_RATE / 2u) / PC_AUDIO_RATE); }

/* A gap is a time the ring ran dry and the output faded out; a skip is a time the device fell far behind and the ring jumped forward. A read of these while the device's thread runs can be a sample out
   of date, which a line of text can take. The silence before the first sound is the program starting, with the device already open: it is counted apart from the gaps. */
void pc_audio_totals(char *text, size_t size)
{
    text[0] = 0;
    if (audio_open_now)
        snprintf(text, size, "sound: %llu gaps, %llu ms in all with the sound stopped, the longest %llu ms at frame %u; %llu pauses; %llu ms of silence before the first sound; %llu skips, "
                 "%llu times the delay was put back after a stall, %llu times it stayed high; %llu samples dropped, %llu samples played; %u notes about the sound never reached this log, their "
                 "table full; lines over the sound's allowances are counted in the run's last line",
                 (unsigned long long)(dev.underruns - dev.pauses), ms_of_pairs(dev.gaps_all_pairs), ms_of_pairs(dev.gap_longest), (unsigned)dev.gap_longest_frame,
                 (unsigned long long)dev.pauses, ms_of_pairs(dev.start_pairs), (unsigned long long)dev.skips, (unsigned long long)dev.settles, (unsigned long long)dev.highs,
                 (unsigned long long)dropped, (unsigned long long)dev.pulled, (unsigned)notes_lost);
}

/* After pc_audio_close, so the device's thread has stopped and this thread is the only writer of the notes: a gap or a pause still going on when the run ended gets its line too, and the gap
   counts in the totals. A silence before the first sound is no gap. */
void pc_audio_end_of_run(void)
{
    if (!dev.started || dev.state == PLAYING)
        return;
    if (dev.gap_paused) {
        note_put(NOTE_PAUSE_AT_END, dev.gap_when, dev.gap_frame, dev.gap_pairs);
        return;
    }
    note_put(NOTE_GAP_AT_END, dev.gap_when, dev.gap_frame, dev.gap_pairs);
    dev.gaps_all_pairs += dev.gap_pairs;
    if (dev.gap_pairs > dev.gap_longest) {
        dev.gap_longest = dev.gap_pairs;
        dev.gap_longest_frame = dev.gap_frame;
    }
}

int pc_audio_take_line(uint64_t *when, char *text, size_t size)
{
    uint32_t t = notes_tail;
    const SoundNote *n;
    if (t == __atomic_load_n(&notes_head, __ATOMIC_ACQUIRE))
        return 0;
    n = &notes[t % NOTES];
    *when = n->when;
    if (n->kind == NOTE_START)
        snprintf(text, size, "sound started at frame %u, %llu ms after the sound device opened", (unsigned)n->frame, ms_of_pairs(n->pairs));
    else if (n->kind == NOTE_GAP)
        snprintf(text, size, "sound gap at frame %u: the sound ran dry and stopped for %llu ms, back at frame %u", (unsigned)n->frame, ms_of_pairs(n->pairs), (unsigned)n->frame_back);
    else if (n->kind == NOTE_PAUSE)
        snprintf(text, size, "sound paused at frame %u with the game, for %llu ms, back at frame %u", (unsigned)n->frame, ms_of_pairs(n->pairs), (unsigned)n->frame_back);
    else if (n->kind == NOTE_GAP_AT_END)
        snprintf(text, size, "sound gap at frame %u: the sound ran dry and was still stopped when the run ended at frame %u, after %llu ms", (unsigned)n->frame, (unsigned)n->frame_back, ms_of_pairs(n->pairs));
    else if (n->kind == NOTE_PAUSE_AT_END)
        snprintf(text, size, "sound paused at frame %u with the game, and still paused when the run ended, after %llu ms", (unsigned)n->frame, ms_of_pairs(n->pairs));
    else if (n->kind == NOTE_SETTLE)
        snprintf(text, size, "sound delay at frame %u: all calm again, so %llu ms of sound was let go, what had built up in the queue and what the game was still behind, to bring the delay back to its %llu ms",
                 (unsigned)n->frame, ms_of_pairs(n->pairs), ms_of_pairs(dev.target));
    else if (n->kind == NOTE_HIGH)
        snprintf(text, size, "sound delay at frame %u: for 10 seconds the queue has held more than a frame over its %llu ms, %llu ms over now; it is let go once all is calm, and the rate control brings it down slowly meanwhile",
                 (unsigned)n->frame, ms_of_pairs(dev.target), ms_of_pairs(n->pairs));
    else if (n->kind == NOTE_HIGH_END)
        snprintf(text, size, "sound delay at frame %u: the queue is back within half a frame of its %llu ms, after %llu s more than a frame over", (unsigned)n->frame, ms_of_pairs(dev.target),
                 (unsigned long long)(n->pairs / PC_AUDIO_RATE));
    else
        snprintf(text, size, "sound skip at frame %u: the sound device fell behind and %llu ms of sound was left out", (unsigned)n->frame, ms_of_pairs(n->pairs));
    __atomic_store_n(&notes_tail, t + 1u, __ATOMIC_RELEASE);
    return n->kind == NOTE_SETTLE || n->kind == NOTE_HIGH || n->kind == NOTE_HIGH_END ? 2 : 1;
}

#ifdef PC_AUDIO_NO_SDL

/* The tests' way in: the ring and the pull with no device behind them. */
int pc_audio_open(const char *device, int volume, char *error, size_t error_size)
{
    (void)device;
    (void)error;
    (void)error_size;
    ring_head = ring_tail = 0;
    pushed = dropped = 0;
    muted = 0;
    volume_percent = volume;
    device_side_reset(DEVICE_PAIRS);
    audio_open_now = 1;
    return 0;
}

/* As in the program, closing leaves the totals and the notes to be read. */
void pc_audio_close(void) {}

#else

/* ---- the devices */
static SDL_AudioDeviceID sdl_device;
static HANDLE test_thread;
static volatile int test_stop;
static enum { TEST_NULL, TEST_STALL, TEST_SLOW } test_kind;

static void SDLCALL sdl_callback(void *user, Uint8 *stream, int bytes)
{
    (void)user;
    timed_pull((int16_t *)(void *)stream, (uint32_t)bytes / 4u);
}

/* A plain generator: nothing here reaches the machine, so its numbers only choose when the test's device gets stuck. */
static uint32_t test_random(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;       /* a textbook generator; not a game address */
    return *state >> 8;
}

static DWORD WINAPI test_device_thread(void *arg)
{
    static int16_t scratch[DEVICE_PAIRS * 2u];
    uint32_t seed = 0x5EED0001u;
    LARGE_INTEGER freq, start, now;
    uint64_t pulled = 0, stall_at = 48000u * 2u;
    (void)arg;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    while (!test_stop) {
        double seconds, due;
        QueryPerformanceCounter(&now);
        seconds = (double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart;
        due = seconds * PC_AUDIO_RATE * (test_kind == TEST_SLOW ? 0.9 : 1.0);
        while ((double)pulled < due) {
            timed_pull(scratch, DEVICE_PAIRS);
            pulled += DEVICE_PAIRS;
        }
        if (test_kind == TEST_STALL && pulled >= stall_at) {
            DWORD ms = 100u + test_random(&seed) % 1900u;
            Sleep(ms);
            stall_at = pulled + (uint64_t)(48000u * (1u + test_random(&seed) % 4u));
        }
        Sleep(5);
    }
    return 0;
}

/* SDL converts to the device's own format behind the callback, so what was opened and what the device itself runs at can differ. Not every SDL audio driver can say the second. */
static void describe_sdl_device(const SDL_AudioSpec *have)
{
    char *name = NULL;
    SDL_AudioSpec own;
    const char *driver = SDL_GetCurrentAudioDriver();
    size_t used;
    SDL_zero(own);
    used = (size_t)snprintf(device_words, sizeof(device_words), "SDL driver %s", driver ? driver : "unknown");
    if (SDL_GetDefaultAudioInfo(&name, &own, 0) == 0) {
        used += (size_t)snprintf(device_words + used, sizeof(device_words) - used, ", device %s at %d Hz", name ? name : "with no name", own.freq);
        SDL_free(name);
    }
    if (used < sizeof(device_words))
        snprintf(device_words + used, sizeof(device_words) - used, ", opened at %d Hz, %d channels, %d samples a buffer", have->freq, have->channels, have->samples);
}

int pc_audio_open(const char *device, int volume, char *error, size_t error_size)
{
    int use_sdl = !strcmp(device, "sdl") || !strcmp(device, "dummy");
    volume_percent = volume < 0 ? 0 : volume > 100 ? 100 : volume;
    device_side_reset(DEVICE_PAIRS);
    if (!strcmp(device, "fail")) {
        snprintf(error, error_size, "this test's device always fails to open");
        return -1;
    }
    if (use_sdl) {
        SDL_AudioSpec want, have;
        if (!strcmp(device, "dummy"))
            SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);     /* so no real device is touched at all */
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            snprintf(error, error_size, "SDL's audio did not start: %s", SDL_GetError());
            return -1;
        }
        SDL_zero(want);
        want.freq = PC_AUDIO_RATE;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = DEVICE_PAIRS;
        want.callback = sdl_callback;
        sdl_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);     /* no changes allowed: SDL converts if the device differs */
        if (!sdl_device) {
            snprintf(error, error_size, "no audio device: %s", SDL_GetError());
            return -1;
        }
        device_side_reset(have.samples);
        describe_sdl_device(&have);
        audio_open_now = 1;
        SDL_PauseAudioDevice(sdl_device, 0);
        return 0;
    }
    if (!strcmp(device, "dead")) {
        snprintf(device_words, sizeof(device_words), "the test device dead, at %d Hz", PC_AUDIO_RATE);
        audio_open_now = 1;
        return 0;
    }
    if (!strcmp(device, "null"))
        test_kind = TEST_NULL;
    else if (!strcmp(device, "stall"))
        test_kind = TEST_STALL;
    else if (!strcmp(device, "slow"))
        test_kind = TEST_SLOW;
    else {
        snprintf(error, error_size, "no audio device is called %s", device);
        return -1;
    }
    snprintf(device_words, sizeof(device_words), "the test device %s, at %d Hz", device, PC_AUDIO_RATE);
    audio_open_now = 1;
    test_stop = 0;
    test_thread = CreateThread(NULL, 0, test_device_thread, NULL, 0, NULL);
    return 0;
}

void pc_audio_close(void)
{
    if (sdl_device) {
        SDL_CloseAudioDevice(sdl_device);
        sdl_device = 0;
    }
    if (test_thread) {
        test_stop = 1;
        WaitForSingleObject(test_thread, 5000);
        CloseHandle(test_thread);
        test_thread = NULL;
    }
}

#endif
