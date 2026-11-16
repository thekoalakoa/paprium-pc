/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The freeze log alone, with a made-up run loop: no machine, no SDL, no game. Built by tests/test_pc_machine.py, which also builds it from copies of pc_stall.c with planted bugs.
 *
 * One run of the program holds one run of the log, so the program runs itself once for each scene, by name:
 *   parts     a stall in each part, named right, with its frame, time and stack; nothing for a normal pass; a stall seen in the file while it is still going on; a long note kept whole;
 *             the longest footer kept whole; the sound device's slow close at the run's end called that, not a stall, and a quick one not written at all
 *   cap       thousands of stalls: the file stays within the allowance, and the closing line counts the rest
 *   noisy     the main thread keeps the process heap's lock nearly all the time, while the second thread keeps holding it to look at its stack. A look that took a lock would hang here
 *   stop      the run's end: a look that falls due while the main thread only waits in pc_stall_stop for the log's last lines is not taken, so no "stuck" line follows the close
 *   nowrite   a path that cannot be written: nothing on the console, and the program runs on and ends in good time
 *   locked    the same, with another program holding the file
 * Each scene checks the file it wrote and returns 1 when a check failed.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc_stall.h"

static int checks, failed;
#define CHECK(cond) do { checks++; if (!(cond)) { failed++; printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static char text[1u << 20];

static long read_log(const char *path)
{
    /* The second thread may have it open for a moment: this must share, as a player's text editor would. */
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD got = 0;
    text[0] = 0;
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    ReadFile(h, text, sizeof(text) - 1, &got, NULL);
    CloseHandle(h);
    text[got] = 0;
    return (long)got;
}

static int count_of(const char *word)
{
    int n = 0;
    const char *p = text;
    while ((p = strstr(p, word)) != NULL) {
        n++;
        p += strlen(word);
    }
    return n;
}

/* How many lines hold both words. */
static int lines_with_both(const char *a, const char *b)
{
    int n = 0;
    const char *p = text;
    while ((p = strstr(p, a)) != NULL) {
        const char *s = p, *e = strchr(p, '\n'), *q;
        while (s > text && s[-1] != '\n')
            s--;
        if (!e)
            e = p + strlen(p);
        q = strstr(s, b);
        if (q && q < e)
            n++;
        p = e;
    }
    return n;
}

/* The line holding this word, or NULL, copied so the test can look for more in it. */
static const char *line_with(const char *word, char *out, size_t size)
{
    const char *p = strstr(text, word), *a, *b;
    size_t n;
    if (!p)
        return NULL;
    a = p;
    while (a > text && a[-1] != '\n')
        a--;
    b = strchr(p, '\n');
    n = b ? (size_t)(b - a) : strlen(a);
    if (n >= size)
        n = size - 1;
    memcpy(out, a, n);
    out[n] = 0;
    return out;
}

static uint64_t ticks_of_ms(int ms)
{
    LARGE_INTEGER r;
    QueryPerformanceFrequency(&r);
    return (uint64_t)r.QuadPart * (uint64_t)ms / 1000u;
}

static int plain_text_only(void)
{
    const unsigned char *p = (const unsigned char *)text;
    for (; *p; p++)
        if (!(*p >= 32 && *p < 127) && *p != '\r' && *p != '\n')
            return 0;
    return 1;
}

/* Lines from another thread's table, as the sound's are: two, each with the timer reading it is about. */
static volatile uint64_t source_times[2];
static volatile int source_left;
static char long_note[PC_STALL_NOTE_CHARS], long_footer[PC_STALL_FOOTER_CHARS];

static int source_take(uint64_t *when, char *line, size_t size)
{
    if (!source_left)
        return 0;
    source_left--;
    *when = source_times[1 - source_left];
    snprintf(line, size, "made-up sound line %d", 2 - source_left);
    return 1;
}

static const struct { int part; const char *word; } parts[] = {
    {PC_STALL_WORK, "work"}, {PC_STALL_RENDER, "render"}, {PC_STALL_PACE, "pacing"}, {PC_STALL_VSYNC_SWITCH, "vsync switch"}, {PC_STALL_PRESENT, "present"}, {PC_STALL_EVENTS, "events"}, {PC_STALL_SAVE, "save"},
};

/* Each part in turn: one pass that sticks in it, at a frame of its own. */
static void scene_parts(const char *path)
{
    static volatile uint8_t in_firmware;
    static char line[1200], word[80];
    uint64_t firmware = 0;
    uint32_t frame = 100;
    unsigned i;
    long size_before;

    /* An earlier run's block is kept, as his earlier runs are. */
    {
        FILE *f = fopen(path, "wb");
        fprintf(f, "an earlier run's block\r\n");
        fclose(f);
    }
    pc_stall_header("monitor: made up, at 240 Hz");
    pc_stall_header("renderer: made up");
    pc_stall_header("gpu: made up");
    pc_stall_first_line("renderer made up");
    /* A footer as long as the log keeps, longer than the sound's totals line can be: it must come out whole. */
    memset(long_footer, '-', sizeof(long_footer) - 1);
    memcpy(long_footer, "sound: made up gaps ", 20);
    memcpy(long_footer + sizeof(long_footer) - 1 - 8, " its end", 8);
    long_footer[sizeof(long_footer) - 1] = 0;
    pc_stall_footer(long_footer);
    pc_stall_state(1, 0);
    pc_stall_line_source(source_take);
    pc_stall_start(path, 100, &in_firmware, NULL, frame);

    /* Normal passes, and one under the limit though it has a slow part, get nothing. */
    for (i = 0; i < 10; i++) {
        pc_stall_part(PC_STALL_PRESENT);
        Sleep(5);
        pc_stall_part(PC_STALL_WORK);
        pc_stall_pass_end(++frame, firmware);
    }
    pc_stall_part(PC_STALL_PRESENT);
    Sleep(70);
    pc_stall_pass_end(++frame, firmware);

    for (i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        frame = 200 + i * 10;
        pc_stall_pass_end(frame, firmware);
        pc_stall_part(parts[i].part);
        Sleep(160);
        pc_stall_part(PC_STALL_WORK);
        pc_stall_pass_end(frame + 1, firmware);
    }

    /* A firmware turn stuck inside the work: the flag says so while it sticks, the ticks after it. */
    frame = 300;
    pc_stall_pass_end(frame, firmware);
    in_firmware = 1;
    Sleep(160);
    in_firmware = 0;
    firmware += ticks_of_ms(160);
    pc_stall_pass_end(frame + 1, firmware);

    /* Time shared out: neither part is over the limit, the pass is, and it names the longer. */
    frame = 310;
    pc_stall_pass_end(frame, firmware);
    pc_stall_part(PC_STALL_RENDER);
    Sleep(50);
    pc_stall_part(PC_STALL_PRESENT);
    Sleep(90);
    pc_stall_pass_end(frame + 1, firmware);

    /* Two lines from another thread's table, each with the time of day of its own timer reading, 1.5 s apart. */
    {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        source_times[0] = (uint64_t)t.QuadPart;
        source_times[1] = (uint64_t)t.QuadPart + ticks_of_ms(1500);
        source_left = 2;
        Sleep(80);
    }

    /* The state as it stands, and a note of its own. */
    frame = 320;
    pc_stall_pass_end(frame, firmware);
    pc_stall_state(0, 1);
    pc_stall_note("vsync switched off, made up");
    /* A note as long as the log keeps, longer than a vsync switch's reason: it must come out whole. */
    memset(long_note, '-', sizeof(long_note) - 1);
    memcpy(long_note, "a long made-up note ", 20);
    memcpy(long_note + sizeof(long_note) - 1 - 8, " its end", 8);
    long_note[sizeof(long_note) - 1] = 0;
    pc_stall_note(long_note);
    pc_stall_part(PC_STALL_PRESENT);
    Sleep(160);
    pc_stall_pass_end(frame + 1, firmware);
    pc_stall_state(1, 0);

    /* A pause's passes are normal passes, so a long pause is no gap. A plant goes off once, at its frame and part; one over a run of frames goes off once on each, even if the part is
       entered again in the same frame. */
    frame = 330;
    pc_stall_pass_end(frame, firmware);
    CHECK(pc_stall_test_plant(331, 331, "events", 160) == PC_STALL_EVENTS);
    CHECK(pc_stall_test_plant(331, 331, "no such part", 160) < 0);
    CHECK(pc_stall_test_plant(334, 333, "save", 160) < 0);
    CHECK(pc_stall_test_plant(333, 334, "save", 160) == PC_STALL_SAVE);
    pc_stall_pass_end(331, firmware);
    pc_stall_part(PC_STALL_EVENTS);
    pc_stall_pass_end(332, firmware);
    pc_stall_pass_end(333, firmware);
    pc_stall_part(PC_STALL_SAVE);
    pc_stall_part(PC_STALL_WORK);
    pc_stall_part(PC_STALL_SAVE);
    pc_stall_pass_end(334, firmware);
    pc_stall_part(PC_STALL_SAVE);
    pc_stall_pass_end(335, firmware);
    pc_stall_part(PC_STALL_SAVE);
    pc_stall_pass_end(336, firmware);

    /* Stuck, and not back yet: the file must already say so, or a run that never comes back leaves nothing. */
    frame = 340;
    pc_stall_pass_end(frame, firmware);
    pc_stall_part(PC_STALL_PRESENT);
    Sleep(1100);
    size_before = read_log(path);
    CHECK(size_before > 0);
    CHECK(count_of("frame 340") >= 3);      /* looks at 100, 300 and 900 ms */
    CHECK(strstr(text, "gap of") != NULL);
    CHECK(!strstr(text, "ms, frame 340, vsync on, in present: work"));     /* its gap line comes only when it is over */
    pc_stall_pass_end(frame + 1, firmware);

    /* The run's end: the sound device closed after the last frame. A quick close gets no line; a slow one is called the close, not a stall, while it lasts and when it is over. */
    pc_stall_closing_sound(1);
    pc_stall_closing_sound(0);
    pc_stall_closing_sound(1);
    Sleep(350);
    pc_stall_closing_sound(0);
    pc_stall_stop();
    Sleep(50);
    CHECK(read_log(path) > size_before);

    CHECK(plain_text_only());
    CHECK(!strncmp(text, "an earlier run's block\r\n", 24));
    CHECK(count_of("==== run started") == 1);
    CHECK(strstr(text, ", renderer made up, a gap over 100 ms gets a line\r\nmonitor: made up, at 240 Hz\r\nrenderer: made up\r\ngpu: made up\r\n") != NULL);
    {
        const char *m = strstr(text, long_footer);
        CHECK(m != NULL && m > text && m[-1] == '\n' && !strncmp(m + strlen(long_footer), "\r\n", 2) && m < strstr(text, " run ended at frame"));
    }
    CHECK(strstr(text, "monitor: made up") < strstr(text, "gap of"));

    CHECK(!strstr(text, "frame 105"));      /* a normal pass */
    CHECK(!strstr(text, "frame 110"));      /* one slow part, under the limit */

    for (i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        snprintf(word, sizeof(word), "ms, frame %u, vsync on, in %s:", 200 + i * 10, parts[i].word);
        CHECK(line_with(word, line, sizeof(line)) != NULL);     /* one gap line, in its part */
        snprintf(word, sizeof(word), "so far, frame %u, vsync on, in %s:", 200 + i * 10, parts[i].word);
        CHECK(line_with(word, line, sizeof(line)) != NULL);     /* one stack line, in its part */
        /* Sleep's stack: a system module, then the test program itself. */
        CHECK(strstr(line, "ntdll.dll") != NULL);
        CHECK(strstr(line, "pc_stall_test") != NULL);
    }
    CHECK(line_with("ms, frame 300, vsync on, in firmware:", line, sizeof(line)) != NULL);
    {
        const char *m = strstr(line, "firmware ");
        CHECK(m && atoi(m + 9) >= 150);
        m = strstr(line, " work ");
        CHECK(m && atoi(m + 6) < 150);
    }
    CHECK(line_with("so far, frame 300, vsync on, in firmware:", line, sizeof(line)) != NULL);
    CHECK(line_with("ms, frame 310, vsync on, in present:", line, sizeof(line)) != NULL);
    {
        const char *m = strstr(line, " render ");
        CHECK(m && atoi(m + 8) >= 40 && atoi(m + 8) < 90);
        m = strstr(line, " present ");
        CHECK(m && atoi(m + 9) >= 80);
    }
    CHECK(line_with("frame 320: vsync switched off, made up", line, sizeof(line)) != NULL);
    CHECK(line_with(long_note, line, sizeof(line)) != NULL && strstr(line, "frame 320: a long made-up note ") != NULL);
    {
        int h1, m1, s1, ms1, h2, m2, s2, ms2;
        long apart;
        CHECK(line_with(" made-up sound line 1", line, sizeof(line)) != NULL);
        CHECK(sscanf(line, "%d:%d:%d.%d made-up sound line 1", &h1, &m1, &s1, &ms1) == 4);
        CHECK(line_with(" made-up sound line 2", line, sizeof(line)) != NULL);
        CHECK(sscanf(line, "%d:%d:%d.%d made-up sound line 2", &h2, &m2, &s2, &ms2) == 4);
        apart = ((h2 * 60L + m2) * 60L + s2) * 1000L + ms2 - (((h1 * 60L + m1) * 60L + s1) * 1000L + ms1);
        if (apart < 0)
            apart += 24L * 3600L * 1000L;
        CHECK(apart >= 1495 && apart <= 1505);
        CHECK(strstr(text, "made-up sound line 1") < strstr(text, "made-up sound line 2"));
    }
    CHECK(line_with("ms, frame 320, vsync off, paused, in present:", line, sizeof(line)) != NULL);
    CHECK(line_with("frame 331, vsync on, in events:", line, sizeof(line)) != NULL);
    CHECK(line_with("ms, frame 333, vsync on, in save:", line, sizeof(line)) != NULL);
    {
        const char *m = strstr(line, "gap of ");
        CHECK(m && atoi(m + 7) >= 150 && atoi(m + 7) < 300);     /* once in the frame, though the part came twice */
    }
    CHECK(line_with("ms, frame 334, vsync on, in save:", line, sizeof(line)) != NULL);
    CHECK(!strstr(text, "ms, frame 335, vsync on, in save:"));
    CHECK(line_with("gap of", line, sizeof(line)) != NULL);
    {
        const char *m = strstr(text, "ms, frame 340");
        const char *a = m;
        while (a > text && a[-1] != '\n')
            a--;
        CHECK(m != NULL);
        m = strstr(a, "gap of ");
        CHECK(m && atoi(m + 7) >= 1050);
        CHECK(strstr(a, "main thread used about ") != NULL);
    }
    /* The close: its looks name it, with its stack, and no look calls it stuck; one line says how long it took, before the run's last line. */
    CHECK(line_with("closing the sound device at the run's end, for ", line, sizeof(line)) != NULL);
    CHECK(strstr(line, "ms so far, frame 341, vsync on; the game has stopped:") != NULL);
    CHECK(strstr(line, "ntdll.dll") != NULL && strstr(line, "pc_stall_test") != NULL);
    CHECK(lines_with_both("stuck for", "frame 341,") == 0);
    CHECK(count_of("closing the sound device took ") == 1);
    CHECK(line_with("closing the sound device took ", line, sizeof(line)) != NULL);
    {
        const char *m = strstr(line, " took ");
        CHECK(strstr(line, " frame 341: closing the sound device took ") != NULL);
        CHECK(m && atoi(m + 6) >= 340 && atoi(m + 6) < 2000);
        CHECK(strstr(text, "closing the sound device took ") < strstr(text, " run ended at frame"));
    }
    CHECK(line_with("run ended at frame 341", line, sizeof(line)) != NULL);
    {
        const char *m = strstr(line, ": ");
        CHECK(m && atoi(m + 2) >= 12);          /* 7 parts, firmware, shared out, state, plant, stuck; a busy test PC can add one of its own */
        CHECK(strstr(line, "the longest 1") != NULL);
        CHECK(strstr(line, "at frame 340") != NULL);
        CHECK(strstr(line, "; 0 lines not written") != NULL);
    }
}

/* A flood of the sound's lines, as a run whose sound keeps running dry makes; every third one about the sound's delay, as a run of stalls brings. */
#define FLOOD 500
static volatile int flood_left;

static int flood_take(uint64_t *when, char *line, size_t size)
{
    LARGE_INTEGER t;
    int k;
    if (flood_left <= 0)
        return 0;
    flood_left--;
    k = FLOOD - flood_left;
    QueryPerformanceCounter(&t);
    *when = (uint64_t)t.QuadPart;
    if (k % 3 == 0) {
        snprintf(line, size, "made-up delay line %d", k);
        return 2;
    }
    snprintf(line, size, "made-up sound line %d", k);
    return 1;
}

/* Far more stalls than the file may hold, after far more of the sound's lines than it may hold: the sound's lines use only their own allowance, so the freeze lines still get all of theirs; and
   the lines about the sound's delay use only theirs, so the sound's gaps still get all of their own. */
static void scene_cap(const char *path)
{
    static char line[1200];
    char want[160];
    uint64_t one_and_a_bit = ticks_of_ms(2);
    LARGE_INTEGER a, b;
    int i, lines = 0;
    const char *p;
    DeleteFileA(path);
    pc_stall_header("monitor: made up");
    pc_stall_state(1, 0);
    flood_left = FLOOD;
    pc_stall_line_source(flood_take);
    pc_stall_start(path, 1, NULL, NULL, 0);
    for (i = 0; flood_left > 0 && i < 500; i++)
        Sleep(10);                                          /* the second thread takes the sound's lines first */
    for (i = 0; i < 3000; i++) {
        QueryPerformanceCounter(&a);
        do
            QueryPerformanceCounter(&b);
        while ((uint64_t)(b.QuadPart - a.QuadPart) < one_and_a_bit);
        pc_stall_pass_end((uint32_t)i + 1, 0);
    }
    pc_stall_note("a note after the cap");
    pc_stall_stop();
    Sleep(50);
    CHECK(read_log(path) > 0);
    for (p = text; (p = strchr(p, '\n')) != NULL; p++)
        lines++;
    CHECK(lines <= PC_STALL_MAX_LINES + PC_STALL_SOUND_LINES + PC_STALL_DELAY_LINES + 3);
    CHECK(count_of("made-up sound line") == PC_STALL_SOUND_LINES);
    CHECK(count_of("made-up delay line") == PC_STALL_DELAY_LINES);
    snprintf(want, sizeof(want), "the %d lines this run allows for the sound's delay are used up", PC_STALL_DELAY_LINES);
    CHECK(count_of(want) == 1);
    /* All the room for gaps and stacks went to gaps and stacks: the top two lines, then these, up to the reserve kept for vsync switches and the last lines. */
    CHECK(count_of("gap of ") + count_of("stuck for") == PC_STALL_MAX_LINES - 10 - 2);
    snprintf(want, sizeof(want), "the %d lines this run allows for the sound are used up", PC_STALL_SOUND_LINES);
    CHECK(count_of(want) == 1 && strstr(text, want) < strstr(text, "gap of "));
    snprintf(want, sizeof(want), "the %d lines this run allows for its first lines, gaps, stacks and notes are used up, so later gaps and stacks are only counted", PC_STALL_MAX_LINES - 10);
    CHECK(count_of(want) == 1);
    CHECK(strstr(text, "a note after the cap") != NULL);    /* a vsync switch always gets its line */
    CHECK(line_with("run ended at frame 3000", line, sizeof(line)) != NULL);
    p = strstr(line, "; ");
    CHECK(p && atoi(p + 2) > 0);                            /* the rest are counted */
    snprintf(want, sizeof(want), " about stalls, %d about the sound", FLOOD - PC_STALL_SOUND_LINES - PC_STALL_DELAY_LINES);
    CHECK(strstr(line, want) != NULL);
}

/* Two threads on one heap: the second keeps stopping the first at random moments. The scene goes on until the second thread has looked at the stuck first one 20 times, which on a PC with one
   processor for the two takes about three times as many passes as on one with two or more, as the second thread waits for the first to be switched out. */
static void scene_noisy(const char *path)
{
    int i, j = 0, looks = 0, processors = 0;
    static char sink[256];
    char want[64];
    uint64_t pass = ticks_of_ms(60);
    HANDLE heap = GetProcessHeap();
    DWORD_PTR mine = 0, system = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &mine, &system))
        for (; mine; mine &= mine - 1)
            processors++;
    DeleteFileA(path);
    pc_stall_state(1, 0);
    pc_stall_start(path, 1, NULL, NULL, 0);
    for (i = 0; i < 300 && looks < 20; i++) {
        LARGE_INTEGER a, b;
        QueryPerformanceCounter(&a);
        do {
            int k;
            HeapLock(heap);
            for (k = 0; k < 200; k++) {
                void *p = malloc(64 + (size_t)(++j & 1023));
                snprintf(sink, sizeof(sink), "%d %d", i, j);
                free(p);
            }
            HeapUnlock(heap);
            QueryPerformanceCounter(&b);
        } while ((uint64_t)(b.QuadPart - a.QuadPart) < pass);
        pc_stall_pass_end((uint32_t)i + 1, 0);
        if (i >= 39 && i % 10 == 9 && read_log(path) > 0)
            looks = count_of("stuck for");
    }
    pc_stall_stop();
    Sleep(50);
    CHECK(read_log(path) > 0);
    looks = count_of("stuck for");
    printf("noisy: %d looks at the stuck thread in %d passes of 60 ms, on %d processors\n", looks, i, processors);
    CHECK(looks >= 20);
    snprintf(want, sizeof(want), "run ended at frame %d:", i);
    CHECK(strstr(text, want) != NULL);
}

/* The run's end, after the sound device's close: the program asks the log to stop, and the main thread then only waits in pc_stall_stop while the second thread writes its last
   lines. A look that falls due in that wait must not be taken. Left to chance, one falls due there only when the stop comes within one of the second thread's sleeps before a look,
   a few runs in a hundred. The second thread reads the time once a round, just after its sleep, and decides on a look with that reading after it has taken the lines of the round.
   So the made-up line source holds it twice. The first hold begins before the last pass does and lasts until 150 ms into it, so the round after it reads a time past the 100 ms
   look. The second hold is in that round, after the reading: the main thread ends the close and asks for the stop only once it has begun, and it lasts 300 ms more, so the look
   falls due while the main thread waits in pc_stall_stop, every time. */
static volatile LONG hold_armed, holds_begun, holds_over;
static volatile uint64_t hold_until[2];

static int stop_take(uint64_t *when, char *line, size_t size)
{
    LARGE_INTEGER t;
    LONG n;
    (void)when;
    (void)line;
    (void)size;
    if (InterlockedExchange(&hold_armed, 0)) {
        n = InterlockedIncrement(&holds_begun) - 1;
        do {
            Sleep(1);
            QueryPerformanceCounter(&t);
        } while ((uint64_t)t.QuadPart < hold_until[n & 1]);
        InterlockedIncrement(&holds_over);
    }
    return 0;
}

static void scene_stop(const char *path)
{
    static char line[1200];
    LARGE_INTEGER start, asked, back;
    DeleteFileA(path);
    hold_until[0] = hold_until[1] = UINT64_MAX;
    pc_stall_state(1, 0);
    pc_stall_line_source(stop_take);
    pc_stall_start(path, 100, NULL, NULL, 1);
    /* A pass stuck in the present for 250 ms: its look at 100 ms is written, so looks are taken in this scene. */
    pc_stall_part(PC_STALL_PRESENT);
    Sleep(250);
    pc_stall_pass_end(2, 0);
    /* The first hold. Once it has begun, the last pass, frame 3: the sound device's close begins, and the first hold is set to end 150 ms into the pass. */
    hold_armed = 1;
    while (holds_begun < 1)
        Sleep(1);
    pc_stall_pass_end(3, 0);
    QueryPerformanceCounter(&start);
    pc_stall_closing_sound(1);
    hold_armed = 1;
    hold_until[0] = (uint64_t)start.QuadPart + ticks_of_ms(150);
    /* The second hold begins on the next round. The close ends, it has taken 150 ms or more, and the program asks the log to stop, as at a quit. */
    while (holds_begun < 2)
        Sleep(1);
    pc_stall_closing_sound(0);
    QueryPerformanceCounter(&asked);
    hold_until[1] = (uint64_t)asked.QuadPart + ticks_of_ms(300);
    pc_stall_stop();
    QueryPerformanceCounter(&back);
    Sleep(50);
    CHECK(read_log(path) > 0);
    CHECK(holds_over == 2);                                                     /* both holds came and ended */
    CHECK((uint64_t)(back.QuadPart - asked.QuadPart) >= ticks_of_ms(250));      /* and the main thread waited in pc_stall_stop all through the second */
    CHECK(line_with("so far, frame 1, vsync on, in present:", line, sizeof(line)) != NULL && strstr(line, "stuck for ") != NULL);
    CHECK(count_of("closing the sound device took ") == 1);
    CHECK(line_with("closing the sound device took ", line, sizeof(line)) != NULL && strstr(line, " frame 3: closing the sound device took ") != NULL);
    CHECK(lines_with_both("stuck for", "frame 3,") == 0);
    CHECK(line_with("run ended at frame 3:", line, sizeof(line)) != NULL);
    CHECK(strstr(line, "; 0 lines not written") != NULL);
    CHECK(strstr(text, "closing the sound device took ") < strstr(text, " run ended at frame"));
}

/* A log that cannot be written. The main thread must not care, and the console must hear nothing. */
static void scene_nowrite(const char *path)
{
    uint64_t start = GetTickCount64();
    pc_stall_state(1, 0);
    pc_stall_start(path, 100, NULL, NULL, 0);
    pc_stall_part(PC_STALL_PRESENT);
    Sleep(160);
    pc_stall_pass_end(1, 0);
    pc_stall_note("a note");
    pc_stall_stop();
    CHECK(GetTickCount64() - start < 1500);
}

static void scene_locked(const char *path)
{
    HANDLE held = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER size;
    CHECK(held != INVALID_HANDLE_VALUE);
    scene_nowrite(path);
    CHECK(GetFileSizeEx(held, &size) && size.QuadPart == 0);
    CloseHandle(held);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("use: pc_stall_test SCENE PATH\n");
        return 2;
    }
    if (!strcmp(argv[1], "parts"))
        scene_parts(argv[2]);
    else if (!strcmp(argv[1], "cap"))
        scene_cap(argv[2]);
    else if (!strcmp(argv[1], "noisy"))
        scene_noisy(argv[2]);
    else if (!strcmp(argv[1], "stop"))
        scene_stop(argv[2]);
    else if (!strcmp(argv[1], "nowrite"))
        scene_nowrite(argv[2]);
    else if (!strcmp(argv[1], "locked"))
        scene_locked(argv[2]);
    else
        return 2;
    if (failed)
        printf("%d of %d checks failed\n", failed, checks);
    return failed ? 1 : 0;
}
