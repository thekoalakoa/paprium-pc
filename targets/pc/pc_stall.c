/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The freeze log. See pc_stall.h for what it notes and what it must never do.
 *
 * Two threads. The main thread only reads the timer and, when a pass of the loop ran long, fills one entry of a small table. The second thread wakes every 20 ms, empties the table into the
 * file and looks at whether the main thread has been in one pass for too long. All file work and all text are on the second thread.
 */
#include <windows.h>
#include <psapi.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pc_stall.h"

#define WATCH_MS 20
#define RING 64
#define HEADER_LINES 12
#define STACK_WORDS 16384       /* of the main thread's stack, read from its stack pointer upward: far enough to reach the program's own frames */
#define STACK_NAMES 24
#define MODULES 512
#define RESERVED_LINES 10       /* the cap on gap and stack lines leaves room for a vsync switch and the totals */

/* The firmware's turns count as a part in the text, though the loop has no mark for them. */
#define PART_FIRMWARE PC_STALL_PARTS
static const char *const part_names[PC_STALL_PARTS + 1] = {"work", "render", "pacing", "vsync switch", "present", "events", "save", "firmware"};

typedef struct Entry {
    int is_note;            /* 0 a pass over the limit, 1 a note in text, 2 the sound device's close at the run's end, which took total */
    uint32_t frame;
    uint64_t end, total, ticks[PC_STALL_PARTS + 1];
    int vsync, paused;
    char text[PC_STALL_NOTE_CHARS];
} Entry;

/* The main thread fills the slot at ring_head and moves it on. The second thread takes the slot at ring_tail. Neither ever waits for the other: when the table is full, the entry is counted. */
static Entry ring[RING];
static volatile uint32_t ring_head, ring_tail, ring_lost;

static int running;
static HANDLE main_thread, watcher_thread;
static char log_path[1400];
static uint64_t limit_ticks, timer_rate;
static const volatile uint8_t *firmware_flag;
static HWND game_window;
static char header_lines[HEADER_LINES][400];
static int header_n;
static char first_line_words[120], footer_line[PC_STALL_FOOTER_CHARS];

/* The main thread's place in the loop, for the second thread to look at. */
static volatile uint64_t pass_start;
static volatile uint32_t pass_id, current_frame;
static volatile int current_part, state_vsync = -1, state_paused;
static volatile int stop_asked;
static volatile int closing_sound;              /* the run's end: the sound device is being closed, since closing_start */
static volatile uint64_t closing_start;

/* The main thread's own. */
static uint64_t part_start, part_ticks[PC_STALL_PARTS], firmware_base;

/* A test's planted sleeps: each on its frames, once a frame, as the loop enters its part. A player's run has none. */
#define PLANTS 256
static struct { int first, last, part, ms, fired; } plants[PLANTS];
static int plants_n;

static inline uint64_t timer_now(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)t.QuadPart;
}

/* ---- the main thread */
static void plant_check(int part)
{
    int i, frame = (int)current_frame;
    for (i = 0; i < plants_n; i++) {
        if (plants[i].part == part && frame >= plants[i].first && frame <= plants[i].last && plants[i].fired != frame) {
            plants[i].fired = frame;
            Sleep((DWORD)plants[i].ms);
        }
    }
}

void pc_stall_part(int part)
{
    uint64_t now;
    if (!running)
        return;
    now = timer_now();
    part_ticks[current_part] += now - part_start;
    part_start = now;
    current_part = part;
    plant_check(part);
}

static Entry *ring_slot(void)
{
    uint32_t head = ring_head;
    if (head - __atomic_load_n(&ring_tail, __ATOMIC_ACQUIRE) >= RING) {
        ring_lost++;
        return NULL;
    }
    return &ring[head % RING];
}

static void ring_commit(void)
{
    __atomic_store_n(&ring_head, ring_head + 1u, __ATOMIC_RELEASE);
}

void pc_stall_pass_end(uint32_t next_frame, uint64_t firmware_ticks)
{
    uint64_t now;
    if (!running)
        return;
    now = timer_now();
    part_ticks[current_part] += now - part_start;
    if (now - pass_start > limit_ticks) {
        Entry *e = ring_slot();
        if (e) {
            uint64_t firmware = firmware_ticks - firmware_base;
            int i;
            e->is_note = 0;
            e->frame = current_frame;
            e->end = now;
            e->total = now - pass_start;
            for (i = 0; i < PC_STALL_PARTS; i++)
                e->ticks[i] = part_ticks[i];
            /* The firmware's turns ran inside the work. */
            e->ticks[PC_STALL_WORK] = part_ticks[PC_STALL_WORK] > firmware ? part_ticks[PC_STALL_WORK] - firmware : 0;
            e->ticks[PART_FIRMWARE] = firmware;
            e->vsync = state_vsync;
            e->paused = state_paused;
            ring_commit();
        }
    }
    memset(part_ticks, 0, sizeof(part_ticks));
    firmware_base = firmware_ticks;
    part_start = now;
    current_frame = next_frame;
    current_part = PC_STALL_WORK;
    pass_start = now;
    pass_id++;
    plant_check(PC_STALL_WORK);
}

void pc_stall_state(int vsync, int paused)
{
    state_vsync = vsync;
    state_paused = paused;
}

void pc_stall_note(const char *text)
{
    Entry *e;
    if (!running)
        return;
    e = ring_slot();
    if (e) {
        e->is_note = 1;
        e->frame = current_frame;
        e->end = timer_now();
        snprintf(e->text, sizeof(e->text), "%s", text);
        ring_commit();
    }
}

void pc_stall_closing_sound(int closing)
{
    uint64_t now;
    if (!running)
        return;
    now = timer_now();
    if (closing) {
        closing_start = now;
        closing_sound = 1;
        return;
    }
    closing_sound = 0;
    if (now - closing_start > limit_ticks) {
        Entry *e = ring_slot();
        if (e) {
            e->is_note = 2;
            e->frame = current_frame;
            e->end = now;
            e->total = now - closing_start;
            ring_commit();
        }
    }
}

int pc_stall_test_plant(int first, int last, const char *name, int ms)
{
    int i;
    if (plants_n >= PLANTS || last < first || ms < 0)
        return -1;
    for (i = 0; i < PC_STALL_PARTS; i++) {
        if (!strcmp(name, part_names[i])) {
            plants[plants_n].first = first;
            plants[plants_n].last = last;
            plants[plants_n].part = i;
            plants[plants_n].ms = ms;
            plants[plants_n].fired = -1;
            plants_n++;
            return i;
        }
    }
    return -1;
}

void pc_stall_header(const char *line)
{
    if (header_n < HEADER_LINES)
        snprintf(header_lines[header_n++], sizeof(header_lines[0]), "%s", line);
}

void pc_stall_first_line(const char *text) { snprintf(first_line_words, sizeof(first_line_words), "%s, ", text); }
void pc_stall_footer(const char *line) { snprintf(footer_line, sizeof(footer_line), "%s", line); }

void pc_stall_adapters(char *text, size_t size)
{
    DISPLAY_DEVICEA d;
    size_t used = 0;
    int want_primary, i;
    text[0] = 0;
    /* The one that drives the main display first. An adapter is listed once for each of its outputs, so a name is kept only the first time. */
    for (want_primary = 1; want_primary >= 0; want_primary--) {
        for (i = 0; i < 32; i++) {
            memset(&d, 0, sizeof(d));
            d.cb = sizeof(d);
            if (!EnumDisplayDevicesA(NULL, (DWORD)i, &d, 0))
                break;
            if ((d.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) || !d.DeviceString[0] || ((d.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0) != want_primary)
                continue;
            if (strstr(text, d.DeviceString) || used + 1 >= size)
                continue;
            used += (size_t)snprintf(text + used, size - used, "%s%s", used ? "; " : "", d.DeviceString);
            if (used >= size)
                used = size - 1;
        }
    }
    if (!used)
        snprintf(text, size, "the system lists none");
}

/* ---- the second thread: text */
static char out[1u << 16];
static size_t out_n;
/* Three allowances. The sound's lines have their own, so a run whose sound keeps running dry can never use up the room the freeze lines need; and the lines about the sound's delay have a
   third, so a run of stalls, which brings let-gos and a delay that stays high, can never use up the room of the sound's gaps. Each says once, at the time, that it is used up. */
static int lines_written, sound_written, delay_written, cap_said, sound_cap_said, delay_cap_said;
static uint32_t lines_dropped, sound_dropped;
static uint64_t start_timer, start_filetime;

static void out_flush(void)
{
    HANDLE h;
    DWORD done;
    if (!out_n)
        return;
    /* Opened for each batch, so the file is free between stalls and a player can open it while the game runs. A failed write is ignored: this log must never get in the game's way. */
    h = CreateFileA(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        WriteFile(h, out, (DWORD)out_n, &done, NULL);
        CloseHandle(h);
    }
    out_n = 0;
}

static void cap_line(const char *what, int lines, const char *later);

/* One line, in plain characters, counted in no allowance. */
static void put_text(const char *text)
{
    size_t n = strlen(text), i;
    if (out_n + n + 2 > sizeof(out))
        out_flush();
    for (i = 0; i < n && i + 2 < sizeof(out); i++) {
        unsigned char c = (unsigned char)text[i];
        out[out_n++] = c >= 32 && c < 127 ? (char)c : '?';
    }
    out[out_n++] = '\r';
    out[out_n++] = '\n';
}

/* One line on the run's allowance. room: how many lines of it this kind of line may reach. */
static void put_line(const char *text, int room)
{
    if (lines_written >= room) {
        lines_dropped++;
        if (room == PC_STALL_MAX_LINES - RESERVED_LINES && !cap_said) {
            cap_said = 1;
            cap_line("its first lines, gaps, stacks and notes", PC_STALL_MAX_LINES - RESERVED_LINES, "later gaps and stacks");
        }
        return;
    }
    put_text(text);
    lines_written++;
}

/* A line about the sound, on its own allowance, or one about the sound's delay, on the delay's. Both count as the sound's in the run's last line. */
static void put_sound_line(const char *text, int delay)
{
    if (delay && delay_written >= PC_STALL_DELAY_LINES) {
        sound_dropped++;
        if (!delay_cap_said) {
            delay_cap_said = 1;
            cap_line("the sound's delay", PC_STALL_DELAY_LINES, "later ones");
        }
        return;
    }
    if (!delay && sound_written >= PC_STALL_SOUND_LINES) {
        sound_dropped++;
        if (!sound_cap_said) {
            sound_cap_said = 1;
            cap_line("the sound", PC_STALL_SOUND_LINES, "later ones");
        }
        return;
    }
    put_text(text);
    if (delay)
        delay_written++;
    else
        sound_written++;
}

/* The time of day of a reading of the timer, as text. */
static void time_text(uint64_t t, char *text, size_t size)
{
    uint64_t d = t > start_timer ? t - start_timer : 0;
    uint64_t hundreds = start_filetime + d / timer_rate * 10000000u + d % timer_rate * 10000000u / timer_rate;   /* not a game address: 100 ns units in a second */
    FILETIME ft, local;
    SYSTEMTIME st;
    ft.dwLowDateTime = (DWORD)hundreds;
    ft.dwHighDateTime = (DWORD)(hundreds >> 32);
    if (FileTimeToLocalFileTime(&ft, &local) && FileTimeToSystemTime(&local, &st))
        snprintf(text, size, "%02u:%02u:%02u.%03u", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    else
        snprintf(text, size, "time unknown");
}

static unsigned long long ms_of(uint64_t ticks)
{
    return (unsigned long long)(ticks * 1000u / timer_rate);
}

/* The moment an allowance is used up, said once, outside both allowances. The run's allowance holds its first lines and its notes as well as its gaps and stacks; only the last two
   stop at it, as the notes, a switch of vsync for one, and the last lines have the reserve after it. */
static void cap_line(const char *what, int lines, const char *later)
{
    char line[300], clock[32];
    time_text(timer_now(), clock, sizeof(clock));
    snprintf(line, sizeof(line), "%s frame %u: the %d lines this run allows for %s are used up, so %s are only counted, in the last line", clock, (unsigned)current_frame, lines, what, later);
    put_text(line);
}

/* How the run looks: said the same way on a gap's line and on a stack's. */
static void state_text(char *text, size_t size, int vsync, int paused)
{
    size_t used = (size_t)snprintf(text, size, "%s", vsync > 0 ? "vsync on" : vsync == 0 ? "vsync off" : "no window");
    if (game_window)
        used += (size_t)snprintf(text + used, size - used, ", window %s", GetForegroundWindow() == game_window ? "in front" : "not in front");
    if (paused)
        snprintf(text + used, size - used, ", paused");
}

/* ---- the second thread: what the main thread is using, read once a pass, so a gap can say whether it was busy or waiting */
typedef struct Usage { uint64_t cpu, faults; } Usage;

static void usage_read(Usage *u)
{
    FILETIME a, b, kernel, user;
    PROCESS_MEMORY_COUNTERS m;
    u->cpu = u->faults = 0;
    if (GetThreadTimes(main_thread, &a, &b, &kernel, &user)) {
        u->cpu = ((uint64_t)kernel.dwHighDateTime << 32 | kernel.dwLowDateTime) + ((uint64_t)user.dwHighDateTime << 32 | user.dwLowDateTime);
    }
    memset(&m, 0, sizeof(m));
    m.cb = sizeof(m);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &m, sizeof(m)))
        u->faults = m.PageFaultCount;
}

static Usage usage_base;
static uint64_t longest_sleep;
static uint32_t gaps_seen;
static uint64_t gap_worst;
static uint32_t gap_worst_frame;

/* ---- the second thread: lines from another thread's own table, such as the sound's gaps */
static int (*line_source)(uint64_t *when, char *text, size_t size);

void pc_stall_line_source(int (*take)(uint64_t *when, char *text, size_t size)) { line_source = take; }

static void take_source_lines(void)
{
    char text[300], line[360], clock[32];
    uint64_t when;
    int kind;
    while (line_source && (kind = line_source(&when, text, sizeof(text))) != 0) {
        time_text(when, clock, sizeof(clock));
        snprintf(line, sizeof(line), "%s %s", clock, text);
        put_sound_line(line, kind == 2);
    }
}

/* ---- the second thread: the table */
static void take_entries(void)
{
    uint32_t tail = ring_tail, head = __atomic_load_n(&ring_head, __ATOMIC_ACQUIRE);
    char line[900], clock[32], state[80];
    while (tail != head) {
        const Entry *e = &ring[tail % RING];
        time_text(e->end, clock, sizeof(clock));
        if (e->is_note == 2) {
            snprintf(line, sizeof(line), "%s frame %u: closing the sound device took %llu ms; the game had already stopped, so this was no stall", clock, (unsigned)e->frame, ms_of(e->total));
            put_line(line, PC_STALL_MAX_LINES - 1);
        } else if (e->is_note) {
            snprintf(line, sizeof(line), "%s frame %u: %s", clock, (unsigned)e->frame, e->text);
            put_line(line, PC_STALL_MAX_LINES - 1);
        } else {
            Usage now;
            size_t used;
            int i, most = 0;
            gaps_seen++;
            if (e->total > gap_worst) {
                gap_worst = e->total;
                gap_worst_frame = e->frame;
            }
            for (i = 1; i <= PC_STALL_PARTS; i++)
                if (e->ticks[i] > e->ticks[most])
                    most = i;
            state_text(state, sizeof(state), e->vsync, e->paused);
            used = (size_t)snprintf(line, sizeof(line), "%s gap of %llu ms, frame %u, %s, in %s:", clock, ms_of(e->total), (unsigned)e->frame, state, part_names[most]);
            for (i = 0; i <= PC_STALL_PARTS; i++)
                used += (size_t)snprintf(line + used, sizeof(line) - used, "%s %s %llu", i ? "," : "", part_names[i], ms_of(e->ticks[i]));
            usage_read(&now);
            snprintf(line + used, sizeof(line) - used, "; the main thread used about %llu ms of processor, the program took %llu page faults, the watcher's longest sleep was %llu ms", (unsigned long long)((now.cpu - usage_base.cpu) / 10000u),
                     (unsigned long long)(now.faults - usage_base.faults), ms_of(longest_sleep));
            put_line(line, PC_STALL_MAX_LINES - RESERVED_LINES);
        }
        tail++;
    }
    __atomic_store_n(&ring_tail, tail, __ATOMIC_RELEASE);
}

/* ---- the second thread: the main thread's stack */
typedef struct Module { uintptr_t base, size; char name[48]; } Module;
static Module modules[MODULES];
static int modules_n;

/* Read again at each look, because a stall can be the very moment a module is being loaded. */
static void modules_read(void)
{
    static HMODULE list[MODULES];
    DWORD need = 0;
    int n, i;
    modules_n = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), list, sizeof(list), &need))
        return;
    n = (int)(need / sizeof(HMODULE));
    if (n > MODULES)
        n = MODULES;
    for (i = 0; i < n; i++) {
        MODULEINFO info;
        char name[260];
        if (K32GetModuleInformation(GetCurrentProcess(), list[i], &info, sizeof(info)) && K32GetModuleBaseNameA(GetCurrentProcess(), list[i], name, sizeof(name))) {
            modules[modules_n].base = (uintptr_t)info.lpBaseOfDll;
            modules[modules_n].size = info.SizeOfImage;
            lstrcpynA(modules[modules_n].name, name, sizeof(modules[0].name));
            modules_n++;
        }
    }
}

static const Module *module_of(uintptr_t address)
{
    int i;
    for (i = 0; i < modules_n; i++)
        if (address >= modules[i].base && address - modules[i].base < modules[i].size)
            return &modules[i];
    return NULL;
}

static size_t name_module(char *text, size_t size, const Module *m, uintptr_t address, uintptr_t program)
{
    /* The program's own offsets say nothing to a reader, and a system module's can be looked up. */
    if (m->base == program)
        return (size_t)snprintf(text, size, " %s", m->name);
    return (size_t)snprintf(text, size, " %s+%llX", m->name, (unsigned long long)(address - m->base));
}

/* Takes one look. While the main thread is held only plain system calls run here: it may be holding the C library's or the loader's lock, so anything that takes a lock would wait for ever. The text
   is made after it has been let go. A value on the stack that falls inside a module is taken as a return address, which is good enough to name modules and needs no symbols. */
static void look_at_main(uint32_t id, uint64_t since)
{
    static CONTEXT context;
    static uintptr_t stack[STACK_WORDS];
    static char line[1100];
    SIZE_T got = 0;
    uint64_t now;
    int part, ok = 0, in_firmware = 0, vsync, paused, i, shown = 0, closing;
    uint32_t frame;
    size_t used;
    char clock[32], state[80];
    uintptr_t program = (uintptr_t)GetModuleHandleA(NULL);
    const Module *last;
    if (!main_thread)
        return;
    memset(&context, 0, sizeof(context));
    context.ContextFlags = CONTEXT_CONTROL;
    if (SuspendThread(main_thread) == (DWORD)-1)
        return;
    if (GetThreadContext(main_thread, &context)) {
        MEMORY_BASIC_INFORMATION region;
        SIZE_T want = sizeof(stack);
        if (VirtualQuery((void *)context.Rsp, &region, sizeof(region))) {
            SIZE_T left = (SIZE_T)((char *)region.BaseAddress + region.RegionSize - (char *)context.Rsp);
            if (left < want)
                want = left;
        }
        ok = ReadProcessMemory(GetCurrentProcess(), (void *)context.Rsp, stack, want, &got);
    }
    part = current_part;
    frame = current_frame;
    closing = closing_sound;
    if (firmware_flag && *firmware_flag)
        in_firmware = 1;
    vsync = state_vsync;
    paused = state_paused;
    now = timer_now();
    /* It came back just as this look began, so what is on the stack now is not the stall. Or the run is over: the main thread sets stop_asked in pc_stall_stop before it
       waits there for this thread's last lines, so a look that finds it set can only find that wait, which is no stall. Both are read while the main thread is held. */
    if (pass_id != id || stop_asked)
        ok = 0;
    ResumeThread(main_thread);
    if (!ok)
        return;
    modules_read();
    time_text(now, clock, sizeof(clock));
    state_text(state, sizeof(state), vsync, paused);
    if (closing)
        used = (size_t)snprintf(line, sizeof(line), "%s closing the sound device at the run's end, for %llu ms so far, frame %u, %s; the game has stopped:", clock,
                                ms_of(now > closing_start ? now - closing_start : 0), (unsigned)frame, state);
    else
        used = (size_t)snprintf(line, sizeof(line), "%s stuck for %llu ms so far, frame %u, %s, in %s:", clock, ms_of(now - since), (unsigned)frame, state,
                                part_names[in_firmware ? PART_FIRMWARE : part]);
    last = module_of((uintptr_t)context.Rip);
    if (last) {
        used += name_module(line + used, sizeof(line) - used, last, (uintptr_t)context.Rip, program);
        shown++;
    }
    for (i = 0; i < (int)(got / sizeof(uintptr_t)) && shown < STACK_NAMES; i++) {
        const Module *m = module_of(stack[i]);
        if (m && m != last) {
            used += name_module(line + used, sizeof(line) - used, m, stack[i], program);
            last = m;
            shown++;
        }
    }
    put_line(line, PC_STALL_MAX_LINES - RESERVED_LINES);
}

static void block_top(void)
{
    char line[1700], name[1100], when[48], built[48];
    uintptr_t program = (uintptr_t)GetModuleHandleA(NULL);
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(program + (uintptr_t)((const IMAGE_DOS_HEADER *)program)->e_lfanew);
    const char *base = name;
    SYSTEMTIME st;
    uint64_t linked;
    FILETIME ft, local;
    int i;
    GetLocalTime(&st);
    snprintf(when, sizeof(when), "%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    if (GetModuleFileNameA(NULL, name, sizeof(name))) {
        const char *slash = strrchr(name, '\\');
        if (slash)
            base = slash + 1;
    } else {
        name[0] = 0;
    }
    /* The link time tells one build from another, and it is all that differs between two builds of one tree. */
    linked = ((uint64_t)nt->FileHeader.TimeDateStamp + 11644473600ull) * 10000000ull;   /* not a game address: seconds from 1601 to 1970, and 100 ns units in a second */
    ft.dwLowDateTime = (DWORD)linked;
    ft.dwHighDateTime = (DWORD)(linked >> 32);
    built[0] = 0;
    if (FileTimeToLocalFileTime(&ft, &local) && FileTimeToSystemTime(&local, &st))
        snprintf(built, sizeof(built), "%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    snprintf(line, sizeof(line), "==== run started %s, %s, built %s, %sa gap over %llu ms gets a line", when, base, built, first_line_words, ms_of(limit_ticks));
    put_line(line, PC_STALL_MAX_LINES - 1);
    for (i = 0; i < header_n; i++)
        put_line(header_lines[i], PC_STALL_MAX_LINES - 1);
}

static DWORD WINAPI watcher(LPVOID unused)
{
    uint32_t seen = pass_id;
    uint64_t next_look = limit_ticks;
    char line[300], clock[32];
    (void)unused;
    block_top();
    out_flush();
    usage_read(&usage_base);
    while (!stop_asked) {
        uint64_t before = timer_now(), now, since;
        uint32_t id;
        Sleep(WATCH_MS);
        now = timer_now();
        if (now - before > longest_sleep)
            longest_sleep = now - before;
        take_entries();
        take_source_lines();
        id = pass_id;
        since = pass_start;
        if (id != seen) {
            /* A new pass: what a later gap says about the processor and the sleeps starts from here. */
            seen = id;
            next_look = limit_ticks;
            usage_read(&usage_base);
            longest_sleep = 0;
        } else if (now > since && now - since > next_look) {
            look_at_main(id, since);
            next_look *= 3u;
        }
        out_flush();
    }
    take_entries();
    take_source_lines();
    if (footer_line[0])
        put_line(footer_line, PC_STALL_MAX_LINES - 1);
    time_text(timer_now(), clock, sizeof(clock));
    snprintf(line, sizeof(line), "%s run ended at frame %u: %u gaps over %llu ms, the longest %llu ms at frame %u; %u lines not written: %u about stalls, %u about the sound", clock, (unsigned)current_frame,
             (unsigned)gaps_seen, ms_of(limit_ticks), ms_of(gap_worst), (unsigned)gap_worst_frame, (unsigned)(lines_dropped + ring_lost + sound_dropped), (unsigned)(lines_dropped + ring_lost),
             (unsigned)sound_dropped);
    put_line(line, PC_STALL_MAX_LINES);
    out_flush();
    return 0;
}

/* ---- starting and stopping */
void pc_stall_start(const char *path, int limit_ms, const volatile uint8_t *in_firmware, void *window, uint32_t frame)
{
    LARGE_INTEGER rate;
    FILETIME ft;
    uint64_t now;
    snprintf(log_path, sizeof(log_path), "%s", path);
    QueryPerformanceFrequency(&rate);
    timer_rate = (uint64_t)rate.QuadPart;
    limit_ticks = timer_rate * (uint64_t)(limit_ms > 0 ? limit_ms : PC_STALL_LIMIT_MS) / 1000u;
    firmware_flag = in_firmware;
    game_window = (HWND)window;
    current_frame = frame;
    current_part = PC_STALL_WORK;
    GetSystemTimeAsFileTime(&ft);
    now = timer_now();
    start_filetime = (uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
    start_timer = now;
    pass_start = part_start = now;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &main_thread, 0, FALSE, DUPLICATE_SAME_ACCESS))
        main_thread = NULL;
    /* Its working memory is in this file, so a small stack is plenty. Left to the default, it would reserve the 64 MB the machine's own stack is linked with. */
    watcher_thread = CreateThread(NULL, 256u * 1024u, watcher, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    running = watcher_thread != NULL;
}

void pc_stall_stop(void)
{
    if (!running)
        return;
    running = 0;
    stop_asked = 1;
    WaitForSingleObject(watcher_thread, 1000);
}
